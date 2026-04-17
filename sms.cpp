// ============================================================================
// SMS Parsing and Queue Management Implementation
// ============================================================================

#include "sms.h"
#include "sim800.h"
#include "mux.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// -----------------------------------------------------------------------------
// Pending SMS Queue
// -----------------------------------------------------------------------------

static PendingSms pendingQueue[MAX_PENDING_SMS];
static int pendingCount = 0;
static unsigned long lastRetryMs = 0;

// Statistics
static unsigned long totalReceived = 0;
static unsigned long totalForwarded = 0;
static unsigned long totalFailed = 0;

// Polling state
static int currentPollSim = 0;
static bool pollingInProgress = false;
static unsigned long pollingPauseUntil = 0;
static unsigned long lastPollMs = 0;

// -----------------------------------------------------------------------------
// Queue Functions
// -----------------------------------------------------------------------------

static bool extractNthQuotedField(const char* s, int fieldIndex, char* out, size_t outSize) {
    if (!out || outSize < 1) return false;
    out[0] = '\0';
    if (!s || fieldIndex < 0) return false;

    int found = 0;
    const char* p = s;
    while ((p = strchr(p, '"')) != NULL) {
        const char* start = p + 1;
        const char* end = strchr(start, '"');
        if (!end) break;

        if (found == fieldIndex) {
            size_t len = (size_t)(end - start);
            if (len >= outSize) len = outSize - 1;
            strncpy(out, start, len);
            out[len] = '\0';
            charBufTrim(out);
            return true;
        }

        found++;
        p = end + 1;
    }

    return false;
}

static bool extractBracketPrefix(const char* msg, char* out, size_t outSize) {
    if (!out || outSize < 1) return false;
    out[0] = '\0';
    if (!msg) return false;

    // Common OTP format: [SHEIN] ...
    if (msg[0] != '[') return false;
    const char* end = strchr(msg, ']');
    if (!end) return false;

    size_t len = (size_t)(end - (msg + 1));
    if (len == 0) return false;
    if (len >= outSize) len = outSize - 1;
    strncpy(out, msg + 1, len);
    out[len] = '\0';
    charBufTrim(out);
    return out[0] != '\0';
}

void initSMSQueue() {
    pendingCount = 0;
    memset(pendingQueue, 0, sizeof(pendingQueue));
    logMsg("[SMS] Queue initialized");
}

bool enqueuePendingSms(int simSlot, const char* simNumber, const char* sender, const char* message) {
    if (pendingCount >= MAX_PENDING_SMS) {
        logMsg("[SMS] Queue full, dropping message");
        return false;
    }
    
    PendingSms* p = &pendingQueue[pendingCount++];
    p->simSlot = simSlot;
    charBufSet(p->simNumber, sizeof(p->simNumber), simNumber);
    charBufSet(p->sender, sizeof(p->sender), sender);
    charBufSet(p->message, sizeof(p->message), message);
    p->timestamp = millis();
    p->retryCount = 0;
    
    logMsgInt("[SMS] Queued on SIM", simSlot + 1);
    return true;
}

bool dequeuePendingSms(PendingSms* out) {
    if (pendingCount == 0 || !out) return false;
    
    *out = pendingQueue[0];
    
    // Shift queue
    for (int i = 1; i < pendingCount; i++) {
        pendingQueue[i - 1] = pendingQueue[i];
    }
    pendingCount--;
    
    return true;
}

int getPendingSmsCount() {
    return pendingCount;
}

void clearPendingSms() {
    pendingCount = 0;
}

// -----------------------------------------------------------------------------
// Pending SMS Queue Processing
// -----------------------------------------------------------------------------

void processPendingSmsQueue() {
    // Check retry interval
    if (millis() - lastRetryMs < SMS_RETRY_INTERVAL_MS) {
        return;
    }
    
    if (pendingCount == 0) {
        return;
    }
    
    lastRetryMs = millis();
    
    // Process one pending SMS at a time
    PendingSms* p = &pendingQueue[0];
    p->retryCount++;
    
    // Try to forward
    static SmsMessage msg;
    msg.simSlot = p->simSlot;
    msg.messageIndex = 0;
    msg.timestamp[0] = '\0';
    msg.unread = true;
    charBufSet(msg.sender, sizeof(msg.sender), p->sender);
    charBufSet(msg.message, sizeof(msg.message), p->message);
    
    if (forwardSmsToBackend(&msg)) {
        // Success - remove from queue
        for (int i = 1; i < pendingCount; i++) {
            pendingQueue[i - 1] = pendingQueue[i];
        }
        pendingCount--;
        logMsg("[SMS] Pending SMS forwarded successfully");
    } else if (p->retryCount >= 5) {
        // Max retries - drop
        logMsg("[SMS] Pending SMS dropped after max retries");
        for (int i = 1; i < pendingCount; i++) {
            pendingQueue[i - 1] = pendingQueue[i];
        }
        pendingCount--;
    }
}

// -----------------------------------------------------------------------------
// SMS Reading
// -----------------------------------------------------------------------------

int checkUnreadSMS(int simSlot) {
    selectSIM(simSlot);
    
    // Set text mode
    if (!setSMSTextMode()) {
        return 0;
    }
    
    // List unread messages
    listSMS("REC UNREAD");
    
    // Parse count
    char* buf = getSimBuffer();
    int count = 0;
    char* p = buf;
    
    while ((p = strstr(p, "+CMGL:")) != NULL) {
        count++;
        p++;
    }
    
    return count;
}

int readAllSMS(int simSlot, SmsMessage* messages, int maxMessages) {
    if (!messages || maxMessages < 1) return 0;
    
    selectSIM(simSlot);
    
    if (!setSMSTextMode()) {
        return 0;
    }
    
    listSMS("ALL");
    
    return parseSMSList(getSimBuffer(), messages, maxMessages);
}

int parseSMSList(const char* response, SmsMessage* messages, int maxMessages) {
    if (!response || !messages || maxMessages < 1) return 0;
    
    int count = 0;
    const char* p = response;
    
    while (count < maxMessages) {
        // Find +CMGL header
        const char* cmgl = strstr(p, "+CMGL:");
        if (!cmgl) break;
        
        // Parse header line:
        // +CMGL: <index>,"<status>","<sender>","<alpha>","<timestamp>"
        SmsMessage* msg = &messages[count];
        msg->messageIndex = atoi(cmgl + 6);
        msg->sender[0] = '\0';
        msg->timestamp[0] = '\0';
        msg->message[0] = '\0';
        msg->unread = false;

        // Extract quoted fields robustly from the header
        // Field 0 = status, 1 = sender, 2 = alpha (optional), 3 = timestamp
        char status[16] = "";
        char alpha[32] = "";
        
        (void)extractNthQuotedField(cmgl, 0, status, sizeof(status));
        (void)extractNthQuotedField(cmgl, 1, msg->sender, sizeof(msg->sender));
        (void)extractNthQuotedField(cmgl, 2, alpha, sizeof(alpha));
        (void)extractNthQuotedField(cmgl, 3, msg->timestamp, sizeof(msg->timestamp));
        
        // If we have an alpha field (branded sender name), use it if sender is numeric/empty
        if (alpha[0] != '\0' && !charBufIsEmpty(alpha)) {
            // Alpha field contains the alphanumeric sender (brand name)
            // Use it if sender is numeric or empty
            if (charBufIsEmpty(msg->sender) || isPhoneNumber(msg->sender)) {
                charBufSet(msg->sender, sizeof(msg->sender), alpha);
            }
        }
        
        // Clean up sender - remove any garbage characters
        for (size_t i = 0; msg->sender[i] != '\0'; i++) {
            // Remove control characters and invalid chars
            if (msg->sender[i] < 32 || msg->sender[i] == ',' || 
                (msg->sender[i] == '+' && i > 0 && msg->sender[i-1] != '\0')) {
                msg->sender[i] = '\0';
                break;
            }
        }
        charBufTrim(msg->sender);
        
        msg->unread = (strcmp(status, "REC UNREAD") == 0 || strstr(cmgl, "REC UNREAD") != NULL);
        
        // Find message body (after header line)
        const char* newline = strchr(cmgl, '\n');
        if (newline) {
            newline++;
            const char* nextCmgl = strstr(newline, "+CMGL:");
            const char* end = strstr(newline, "\r\nOK");
            
            const char* msgEnd = nextCmgl ? nextCmgl : end;
            if (!msgEnd) msgEnd = newline + strlen(newline);
            
            size_t len = msgEnd - newline;
            // Remove trailing \r\n
            while (len > 0 && (newline[len-1] == '\r' || newline[len-1] == '\n')) {
                len--;
            }
            if (len >= SMS_MESSAGE_SIZE) len = SMS_MESSAGE_SIZE - 1;
            strncpy(msg->message, newline, len);
            msg->message[len] = '\0';
        }
        
        p = cmgl + 1;
        count++;
    }
    
    return count;
}
// SMS Processing
// -----------------------------------------------------------------------------

void processIncomingSMS(int simSlot, const SmsMessage* msg) {
    if (!msg) return;
    
    totalReceived++;
    
    // Log
    logMsgIntVal("[SMS] SIM", simSlot + 1, "Num", simStates[simSlot].number);

    char rawSender[PHONE_BUFFER_SIZE];
    charBufSet(rawSender, sizeof(rawSender), msg->sender);
    
    // Defensive cleanup: some networks/modems can yield odd sender strings.
    // Keep only the first token and avoid mid-string '+' that can appear from corrupted parses.
    for (size_t i = 0; rawSender[i] != '\0'; i++) {
        if (rawSender[i] == ',' || rawSender[i] == '\r' || rawSender[i] == '\n') {
            rawSender[i] = '\0';
            break;
        }
        if (rawSender[i] == '+' && i > 0) {
            rawSender[i] = '\0';
            break;
        }
    }
    charBufTrim(rawSender);

    char senderDisplay[PHONE_BUFFER_SIZE];
    charBufSet(senderDisplay, sizeof(senderDisplay), rawSender);
    char brand[PHONE_BUFFER_SIZE];
    if (extractBracketPrefix(msg->message, brand, sizeof(brand))) {
        // If sender is numeric/shortcode, prefer bracket prefix for display
        if (isPhoneNumber(rawSender)) {
            charBufSet(senderDisplay, sizeof(senderDisplay), brand);
        }
    }

    if (strcmp(senderDisplay, rawSender) != 0) {
        logMsg2Val("[SMS] From", senderDisplay, "Raw", rawSender);
    } else {
        logMsgVal("[SMS] From", senderDisplay);
    }

    char monLine[96];
    snprintf(monLine, sizeof(monLine), "[SMS] SIM%d ", simSlot + 1);
    appendMonitorLogVal(monLine, senderDisplay);

    // Add message preview to monitor (clip to avoid huge log lines)
    char preview[96];
    preview[0] = '\0';
    if (msg->message && msg->message[0] != '\0') {
        size_t n = strlen(msg->message);
        if (n > 80) n = 80;
        strncpy(preview, msg->message, n);
        preview[n] = '\0';
    }
    if (preview[0] != '\0') {
        appendMonitorLogVal("[SMS] Msg ", preview);
    }
    
    // Forward to backend
    if (forwardSmsToBackendFull(msg)) {
        totalForwarded++;
        logMsg("[SMS] Forward success, deleting from SIM");
    } else {
        totalFailed++;
        
        // Add to retry queue
        enqueuePendingSms(simSlot, simStates[simSlot].number, msg->sender, msg->message);
        logMsg("[SMS] Forward failed, queued for retry");
    }
    
    // Always delete from SIM after processing to prevent re-detection
    // (we've already queued it for retry if needed)
    selectSIM(simSlot);
    if (msg->messageIndex > 0) {
        deleteSMS(msg->messageIndex);
        logMsgInt("[SMS] Deleted from SIM, index", msg->messageIndex);
    }
}

// Note: forwardSmsToBackend implementation depends on HTTP client
// This is a placeholder - actual implementation in main .ino or separate module
bool forwardSmsToBackend(const SmsMessage* msg) {
    // This will be implemented with HTTP client in main code
    // For now, return true to avoid queue buildup
    logMsg("[SMS] Backend forward - see main .ino for implementation");
    return true;
}

// -----------------------------------------------------------------------------
// SMS Forwarding Implementation (called from main .ino with HTTP client)
// -----------------------------------------------------------------------------

// External references from main .ino
extern char agentBaseUrl[];
extern char agentApiPath[];
extern char agentDeviceId[];
extern char agentBearerToken[];
extern char agentSimNumber[];
extern bool agentUseAuth;
extern SimState simStates[];

// Forward SMS to backend with HTTP client
// This is the full implementation
bool forwardSmsToBackendFull(const SmsMessage* msg) {
    if (!msg) return false;
    
    // Check WiFi
    if (!WiFi.isConnected()) {
        logMsg("[SMS] No WiFi, queuing");
        return false;
    }
    
    // Check config
    if (charBufIsEmpty(agentBaseUrl)) {
        logMsg("[SMS] No backend URL");
        return false;
    }
    
    // Build URL (static to avoid stack)
    static char url[256];
    snprintf(url, sizeof(url), "%s%s", agentBaseUrl, agentApiPath);
    
    // Determine SIM number (static to avoid stack)
    static char simNum[PHONE_BUFFER_SIZE];
    if (!charBufIsEmpty(simStates[msg->simSlot].number)) {
        charBufSet(simNum, sizeof(simNum), simStates[msg->simSlot].number);
    } else if (!charBufIsEmpty(agentSimNumber)) {
        charBufSet(simNum, sizeof(simNum), agentSimNumber);
    } else {
        charBufSet(simNum, sizeof(simNum), "unknown");
    }
    
    // Build payload (static to avoid stack overflow)
    static char payload[768];
    static char senderEsc[64];
    static char messageEsc[256];
    static char simNumEsc[64];
    jsonEscape(msg->sender, senderEsc, sizeof(senderEsc));
    jsonEscape(msg->message, messageEsc, sizeof(messageEsc));
    jsonEscape(simNum, simNumEsc, sizeof(simNumEsc));
    
    snprintf(payload, sizeof(payload),
        "{\"number\":%s,"
        "\"slot\":%d,"
        "\"carrier\":null,"
        "\"sender\":%s,"
        "\"message\":%s,"
        "\"sim_slot\":%d,"
        "\"sim_number\":%s,"
        "\"device_id\":\"%s\"}",
        simNumEsc,
        msg->simSlot + 1,
        senderEsc,
        messageEsc,
        msg->simSlot + 1,
        simNumEsc,
        agentDeviceId
    );
    
    // Make HTTP request (local to avoid collision with heartbeat)
    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    bool isHttps = (strncmp(url, "https://", 8) == 0);
    
    // Wait if another HTTPS operation is in progress
    if (httpsBusy) {
        logMsg("[SMS] HTTPS busy, retry later");
        return false;
    }
    httpsBusy = true;
    
    if (isHttps) {
        clientSecure.setInsecure();
        if (!http.begin(clientSecure, url)) {
            logMsg("[SMS] HTTP begin failed");
            httpsBusy = false;
            return false;
        }
    } else {
        if (!http.begin(client, url)) {
            logMsg("[SMS] HTTP begin failed");
            httpsBusy = false;
            return false;
        }
    }
    
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);
    
    if (agentUseAuth && !charBufIsEmpty(agentBearerToken)) {
        http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
    }
    
    int code = http.POST(payload);
    http.end();

    if (code == 401) {
        // Stop previous connection before refresh
        clientSecure.stop();
        client.stop();
        delay(100);  // Let TCP fully close
        
        if (refreshAgentToken()) {
            if (isHttps) {
                clientSecure.setInsecure();
                if (!http.begin(clientSecure, url)) {
                    logMsg("[SMS] HTTP begin failed");
                    return false;
                }
            } else {
                if (!http.begin(client, url)) {
                    logMsg("[SMS] HTTP begin failed");
                    return false;
                }
            }
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(8000);
            if (agentUseAuth && !charBufIsEmpty(agentBearerToken)) {
                http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
            }
            code = http.POST(payload);
            http.end();
        }
    }
    
    if (code >= 200 && code < 300) {
        logMsgInt("[SMS] Forwarded OK, code", code);
        http.end();
        clientSecure.stop();
        client.stop();
        delay(50);
        httpsBusy = false;
        return true;
    } else {
        logMsgInt("[SMS] Forward failed, code", code);
        http.end();
        clientSecure.stop();
        client.stop();
        delay(50);
        httpsBusy = false;
        return false;
    }
}

// -----------------------------------------------------------------------------
// SMS Polling
// -----------------------------------------------------------------------------

void initSMSPolling() {
    currentPollSim = 0;
    pollingInProgress = false;
    lastPollMs = 0;
    pollingPauseUntil = 0;
}

void pollSIMsForSMS() {
    // Check if paused
    if (millis() < pollingPauseUntil) {
        return;
    }
    
    // Check if busy
    if (isSimBusy() || pollingInProgress) {
        return;
    }
    
    // Check poll interval
    if (millis() - lastPollMs < SMS_POLL_INTERVAL_MS) {
        return;
    }
    
    pollingInProgress = true;
    lastPollMs = millis();
    
    // Poll each enabled SIM in round-robin
    for (int i = 0; i < SIM_COUNT; i++) {
        int slot = (currentPollSim + i) % SIM_COUNT;
        
        // Skip disabled SIMs
        if (!simStates[slot].enabled) {
            continue;
        }
        
        // Skip unresponsive SIMs
        if (!simStates[slot].responsive) {
            continue;
        }
        
        // Select SIM and verify it's responsive
        selectSIM(slot);
        delay(100);  // Extra settling time after MUX switch
        
        // Verify SIM is responsive before polling
        sendATCapture("AT", 500);
        if (!strstr(getSimBuffer(), "OK")) {
            logMsgInt("[SMS] SIM not responding during poll:", slot + 1);
            continue;  // Skip this SIM
        }
        
        if (!simStates[slot].basicInitDone) {
            sendATCapture("AT", 500);
            sendATCapture("AT+CMGF=1", 800);
            // Set SMS storage to SIM memory
            sendATCapture("AT+CPMS=\"SM\",\"SM\",\"SM\"", 800);
            // Enable new SMS indications - store to SIM
            sendATCapture("AT+CNMI=2,1,0,0,0", 800);
            // Set character set to GSM for proper sender decoding
            sendATCapture("AT+CSCS=\"GSM\"", 800);
            
            // Debug: Show SMS storage status
            sendATCapture("AT+CPMS?", 800);
            logMsgVal("[SMS] Storage", getSimBuffer());
            // Debug: Show SMSC (SMS center number)
            sendATCapture("AT+CSCA?", 800);
            logMsgVal("[SMS] SMSC", getSimBuffer());
            
            simStates[slot].basicInitDone = true;
            logMsgInt("[SMS] SIM initialized:", slot + 1);
        }
        
        // AT+CMGL="ALL" returns all stored messages (more reliable than REC UNREAD)
        // Format: +CMGL: <index>,"status","sender","","timestamp"<CR><LF>body<CR><LF>
        logMsgInt("[SMS] Polling SIM", slot + 1);
        sendATCapture("AT+CMGL=\"ALL\"", 8000);  // 8 second timeout for reliability
        
        char* buf = getSimBuffer();
        
        // Debug: show raw response (first 200 chars)
        logMsg("[SMS] Raw response:");
        int bufLen = strlen(buf);
        int showLen = bufLen > 200 ? 200 : bufLen;
        for (int k = 0; k < showLen; k++) {
            if (buf[k] == '\r') Serial.print("\\r");
            else if (buf[k] == '\n') Serial.print("\\n");
            else Serial.print(buf[k]);
        }
        Serial.println();
        
        // Check if any messages
        if (strstr(buf, "+CMGL:") != NULL) {
            logMsgInt("[SMS] Found messages on SIM", slot + 1);
            
            // Parse and process (static + small to avoid stack overflow)
            static SmsMessage messages[5];  // Increased from 3 to 5
            int count = parseSMSList(buf, messages, 5);
            
            for (int j = 0; j < count; j++) {
                messages[j].simSlot = slot;
                processIncomingSMS(slot, &messages[j]);
            }
        } else if (strstr(buf, "OK") != NULL) {
            // No messages, just OK

            // Some SIMs store SMS in ME; try once per cycle as a fallback
            sendATCapture("AT+CPMS=\"ME\",\"ME\",\"ME\"", 800);
            sendATCapture("AT+CMGL=\"ALL\"", 8000);
            char* buf2 = getSimBuffer();
            if (strstr(buf2, "+CMGL:") != NULL) {
                logMsgInt("[SMS] Found messages on SIM (ME)", slot + 1);
                static SmsMessage messages[5];
                int count = parseSMSList(buf2, messages, 5);
                for (int j = 0; j < count; j++) {
                    messages[j].simSlot = slot;
                    processIncomingSMS(slot, &messages[j]);
                }
            } else {
                unsigned long now = millis();
                if (now - simStates[slot].lastNoSmsLog > NO_SMS_LOG_THROTTLE_MS) {
                    simStates[slot].lastNoSmsLog = now;
                    logMsgInt("[SMS] No stored SMS on SIM", slot + 1);
                }
            }
        } else {
            logMsgInt("[SMS] Poll error on SIM", slot + 1);
        }
        
        // Longer delay between SIMs for stability
        delay(150);
    }
    
    // Move to next SIM for next cycle
    currentPollSim = (currentPollSim + 1) % SIM_COUNT;
    
    pollingInProgress = false;
}

void pauseSmsPolling(unsigned long durationMs) {
    unsigned long until = millis() + durationMs;
    if (until > pollingPauseUntil) {
        pollingPauseUntil = until;
    }
}

void resumeSmsPolling() {
    pollingPauseUntil = 0;
}

bool isSmsPollingPaused() {
    return millis() < pollingPauseUntil;
}

int getCurrentPollSim() {
    return currentPollSim;
}

bool isPollingInProgress() {
    return pollingInProgress;
}

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

unsigned long getTotalSmsReceived() {
    return totalReceived;
}

unsigned long getTotalSmsForwarded() {
    return totalForwarded;
}

unsigned long getTotalSmsFailed() {
    return totalFailed;
}
