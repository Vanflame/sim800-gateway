// ============================================================================
// SMS Parsing and Queue Management Implementation
// ============================================================================

#include "sms.h"
#include "sim800.h"
#include "config.h"
#include "mux.h"
#include "calls.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include "webui.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// External persistent storage functions (defined in main .ino)
extern unsigned long persistentReceived;
extern unsigned long persistentForwarded;
extern unsigned long persistentFailed;
extern void savePersistentStats();
extern void appendSmsToFile(const char* time, int simSlot, const char* number, const char* sender, const char* message);

// -----------------------------------------------------------------------------
// Pending SMS Queue
// -----------------------------------------------------------------------------

static PendingSms pendingQueue[MAX_PENDING_SMS];
static int pendingCount = 0;
static unsigned long lastRetryMs = 0;
static unsigned long lastSmsHttpsEndMs = 0;
extern volatile bool httpsBusy;

static void waitBetweenSmsForwards() {
    if (lastSmsHttpsEndMs == 0) return;
    const unsigned long gapEnd = lastSmsHttpsEndMs + SMS_FORWARD_GAP_MS;
    while (millis() < gapEnd) {
        handleWebRequests();
        yield();
        delay(10);
    }
}

static void endSmsHttpsSession() {
    lastSmsHttpsEndMs = millis();
    deferHeartbeat(SMS_POST_FORWARD_HEARTBEAT_DEFER_MS);
    httpsBusy = false;
}

// Statistics
static unsigned long totalReceived = 0;
static unsigned long totalForwarded = 0;
static unsigned long totalFailed = 0;

// Polling state
static int currentPollSim = 0;
static unsigned long pollingPauseUntil = 0;
static unsigned long lastPollMs = 0;
static unsigned long lastSlotPollMs[SIM_COUNT];

static bool smsSlotPollDue(int slot) {
    if (slot < 0 || slot >= SIM_COUNT) return false;
    return (millis() - lastSlotPollMs[slot]) >= (unsigned long)SMS_SLOT_POLL_COOLDOWN_MS;
}

static void cooperativeDelayMs(unsigned long ms) {
    const unsigned long until = millis() + ms;
    while ((long)(until - millis()) > 0) {
        handleWebRequests();
        yield();
        delay(5);
    }
}

static bool smsSlotEligibleForPoll(int slot) {
    if (slot < 0 || slot >= SIM_COUNT) return false;
    if (!simStates[slot].userDisabled && !simStates[slot].enabled) return false;
    if (!simStates[slot].responsive) return false;
    return true;
}

static void pollOneSimSlot(int slot) {
    selectSIM(slot);

    sendATCapture("AT", 300);
    if (!strstr(getSimBuffer(), "OK")) {
        logMsgInt("[SMS] SIM not responding during poll:", slot + 1);
        simMarkSlotOffline(slot, "no AT during poll");
        return;
    }

    if (!simStates[slot].basicInitDone) {
        sendATCapture("AT", 500);
        sendATCapture("AT+CMGF=1", 800);
        sendATCapture("AT+CPMS=\"SM\",\"SM\",\"SM\"", 800);
        sendATCapture("AT+CNMI=2,1,0,0,0", 800);
        if (missedCallForwardEnabled) {
            configureModemForMissedCallDetect();
        } else {
            sendATCapture("AT+CLIP=0", 800);
            sendATCapture("AT+GSMBUSY=1", 800);
        }
        sendATCapture("AT+CSCS=\"GSM\"", 800);
        simStates[slot].basicInitDone = true;
        logMsgInt("[SMS] SIM initialized:", slot + 1);
    }

    sendATCapture("AT+CMGL=\"ALL\"", SMS_CMGL_TIMEOUT_MS);

    char* buf = getSimBuffer();
    if (missedCallForwardEnabled) {
        processCallUrcFromBuffer(slot, buf);
#if MISSED_CALL_URC_LISTEN_MS > 0
        listenForCallUrc(slot, MISSED_CALL_URC_LISTEN_MS);
#endif
    }

    if (strstr(buf, "+CMGL:") != NULL) {
        simStates[slot].consecutiveErrors = 0;
        simStates[slot].lastSuccessfulPoll = millis();

        static SmsMessage messages[5];
        const int count = parseSMSList(buf, messages, 5);
        char line[56];
        snprintf(line, sizeof(line), "[SMS] Poll SIM %d: %d message(s)", slot + 1, count);
        logMsg(line);
        for (int j = 0; j < count; j++) {
            messages[j].simSlot = slot;
            processIncomingSMS(slot, &messages[j]);
        }
    } else if (strstr(buf, "OK") != NULL) {
        simStates[slot].consecutiveErrors = 0;
        simStates[slot].lastSuccessfulPoll = millis();
        char line[40];
        snprintf(line, sizeof(line), "[SMS] Poll SIM %d: empty", slot + 1);
        logMsg(line);
    } else {
        char line[40];
        snprintf(line, sizeof(line), "[SMS] Poll SIM %d: error", slot + 1);
        logMsg(line);
        simStates[slot].consecutiveErrors++;
        if (simStates[slot].consecutiveErrors >= SIM_POLL_DISABLE_THRESHOLD) {
            simMarkSlotOffline(slot, "CMGL errors");
        }
    }

    cooperativeDelayMs(80);
}

// Multipart SMS queue
static MultipartSms multipartQueue[MAX_MULTIPART_QUEUE];
static unsigned long multipartTimeoutMs = 15000; // 15 seconds to collect all parts

// -----------------------------------------------------------------------------
// Multipart SMS Functions
// -----------------------------------------------------------------------------

// Forward declaration
static void forwardSms(int simSlot, const char* senderDisplay, const SmsMessage* msg);

// Check if a message looks like a continuation (starts with lowercase or common continuation patterns)
static bool looksLikeContinuation(const char* msg) {
    if (!msg || msg[0] == '\0') return false;
    
    // Check if starts with lowercase letter (likely continuation)
    if (msg[0] >= 'a' && msg[0] <= 'z') return true;
    
    // Check common continuation patterns
    if (strncmp(msg, "l allow", 7) == 0) return true;  // "full allow" -> "l allow"
    if (strncmp(msg, "r (+", 4) == 0) return true;     // "number (+632)" -> "r (+632)"
    if (strncmp(msg, "DO NOT", 6) == 0) return false;  // This is a new message
    
    return false;
}

// Check if a message looks truncated/incomplete (ends mid-sentence)
static bool looksTruncated(const char* msg) {
    if (!msg || msg[0] == '\0') return false;
    
    size_t len = strlen(msg);
    if (len < 20) return false;  // Too short to determine
    
    // Check if ends with lowercase letter (likely truncated)
    const char* end = msg + len - 1;
    if (*end >= 'a' && *end <= 'z') return true;
    
    // Check for specific truncation patterns
    if (strstr(msg, "DO NOT SHARE") != nullptr && 
        strstr(msg, "this") != nullptr &&
        strstr(msg, "account") == nullptr) {
        return true;
    }
    
    return false;
}

// Find existing multipart SMS for this sender
static MultipartSms* findMultipartSms(int simSlot, const char* sender) {
    for (int i = 0; i < MAX_MULTIPART_QUEUE; i++) {
        if (multipartQueue[i].active && 
            multipartQueue[i].simSlot == simSlot && 
            strcmp(multipartQueue[i].sender, sender) == 0) {
            return &multipartQueue[i];
        }
    }
    return nullptr;
}

// Create new multipart SMS entry
static MultipartSms* createMultipartSms(int simSlot, const char* sender) {
    // Find free slot
    for (int i = 0; i < MAX_MULTIPART_QUEUE; i++) {
        if (!multipartQueue[i].active) {
            MultipartSms* mp = &multipartQueue[i];
            mp->simSlot = simSlot;
            charBufSet(mp->sender, sizeof(mp->sender), sender);
            mp->firstPartTime = millis();
            mp->receivedParts = 0;
            mp->combined[0] = '\0';
            memset(mp->partIndices, 0, sizeof(mp->partIndices));
            mp->active = true;
            return mp;
        }
    }
    return nullptr;
}

// Add part to multipart SMS
static void addMultipartPart(MultipartSms* mp, const char* message, int msgIndex) {
    if (!mp || mp->receivedParts >= MAX_MULTIPART_PARTS) return;
    
    // Append to combined buffer
    size_t currentLen = strlen(mp->combined);
    size_t addLen = strlen(message);
    
    if (currentLen + addLen < MULTIPART_MSG_SIZE - 1) {
        strcat(mp->combined, message);
    }
    
    mp->partIndices[mp->receivedParts] = msgIndex;
    mp->receivedParts++;
}

// Check and process multipart SMS timeout
static void checkMultipartTimeout() {
    unsigned long now = millis();
    
    for (int i = 0; i < MAX_MULTIPART_QUEUE; i++) {
        MultipartSms* mp = &multipartQueue[i];
        if (mp->active && mp->receivedParts > 0 && 
            (now - mp->firstPartTime > multipartTimeoutMs)) {
            // Timeout - process what we have
            logMsgInt("[SMS] Multipart timeout, parts", mp->receivedParts);
            appendMonitorLogInt("[SMS] Multipart timeout, parts", mp->receivedParts);
            
            // Create a temporary SmsMessage to process
            SmsMessage msg;
            msg.simSlot = mp->simSlot;
            msg.messageIndex = 0;
            msg.unread = true;
            charBufSet(msg.sender, sizeof(msg.sender), mp->sender);
            charBufSet(msg.message, sizeof(msg.message), mp->combined);
            
            // Mark inactive before processing
            mp->active = false;
            
            // Process the combined message
            totalReceived++;
            forwardSms(mp->simSlot, mp->sender, &msg);
        }
    }
}

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

// Check if sender is a shortcode (numeric, 2-6 digits, not a full phone number)
static bool isShortCode(const char* sender) {
    if (!sender || !sender[0]) return false;

    // Count digits
    int digitCount = 0;
    for (const char* p = sender; *p; p++) {
        if (*p >= '0' && *p <= '9') digitCount++;
        else if (*p != '+' && *p != ' ' && *p != '-') return false;  // Non-numeric char
    }

    // Shortcodes are typically 2-6 digits (not full phone numbers which are 10+ digits)
    return digitCount >= 2 && digitCount <= 6;
}

// Extract brand name from message content using common patterns
// Patterns: "Your <brand> verification code", "<brand> code", "is your <brand> sign-in code"
static bool extractBrandFromMessage(const char* msg, char* out, size_t outSize) {
    if (!out || outSize < 1) return false;
    out[0] = '\0';
    if (!msg) return false;

    // Pattern 1: "Your <brand> verification code"
    const char* yourPos = strstr(msg, "Your ");
    if (yourPos) {
        const char* brandStart = yourPos + 5;  // Skip "Your "
        const char* spaceAfter = strchr(brandStart, ' ');
        if (spaceAfter) {
            size_t len = spaceAfter - brandStart;
            if (len > 0 && len < outSize) {
                strncpy(out, brandStart, len);
                out[len] = '\0';
                charBufTrim(out);
                if (out[0] != '\0') return true;
            }
        }
    }

    // Pattern 2: "<brand> sign-in code" or "<brand> verification code"
    const char* patterns[] = {
        " sign-in code",
        " verification code",
        " verification",
        " code is",
        " OTP code",
    };
    for (int i = 0; i < 5; i++) {
        const char* patPos = strstr(msg, patterns[i]);
        if (patPos) {
            // Look backwards for the brand name
            const char* brandEnd = patPos;
            const char* brandStart = brandEnd;
            // Find start of word (look for space or start of string)
            while (brandStart > msg && *(brandStart - 1) != ' ') {
                brandStart--;
            }
            // Skip if we're at the start or if the word is too long
            if (brandStart < brandEnd) {
                size_t len = brandEnd - brandStart;
                if (len > 0 && len < outSize && len <= 20) {  // Max 20 chars for brand name
                    strncpy(out, brandStart, len);
                    out[len] = '\0';
                    charBufTrim(out);
                    if (out[0] != '\0') return true;
                }
            }
        }
    }

    return false;
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
    if (cloudBackendDeferred()) {
        return;
    }

    // Check retry interval
    if (millis() - lastRetryMs < PENDING_SMS_PROCESS_GAP_MS) {
        return;
    }
    
    if (pendingCount == 0) {
        return;
    }
    
    lastRetryMs = millis();
    
    // Process one pending SMS at a time
    PendingSms* p = &pendingQueue[0];
    p->retryCount++;
    
    // Try to forward with the actual implementation
    static SmsMessage msg;
    msg.simSlot = p->simSlot;
    msg.messageIndex = 0;
    msg.timestamp[0] = '\0';
    msg.unread = true;
    charBufSet(msg.sender, sizeof(msg.sender), p->sender);
    charBufSet(msg.message, sizeof(msg.message), p->message);
    
    char fwdError[64] = "";
    if (forwardSmsToBackendWithSender(&msg, p->sender, fwdError, sizeof(fwdError))) {
        // Success - remove from queue
        for (int i = 1; i < pendingCount; i++) {
            pendingQueue[i - 1] = pendingQueue[i];
        }
        pendingCount--;
        logMsg("[SMS] Pending SMS forwarded successfully");
        appendMonitorLog("[SMS] Retry success, SMS forwarded");
    } else if (p->retryCount >= 5) {
        // Max retries - drop and log to error log
        logMsg("[SMS] Pending SMS dropped after max retries");
        appendMonitorLog("[SMS] Retry failed, SMS dropped after 5 attempts");
        
        // Log to error log with reason
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "[SMS] Dropped SIM%d after 5 retries: %s", p->simSlot + 1, fwdError[0] ? fwdError : "unknown");
        appendErrorLog(errBuf);
        appendErrorLogVal("[SMS] Sender", p->sender);
        
        for (int i = 1; i < pendingCount; i++) {
            pendingQueue[i - 1] = pendingQueue[i];
        }
        pendingCount--;
    } else {
        // Still retrying - log error with reason
        logMsgInt("[SMS] Retry failed, attempt", p->retryCount);
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "[SMS] Retry %d failed SIM%d: %s", p->retryCount, p->simSlot + 1, fwdError[0] ? fwdError : "unknown");
        appendErrorLog(errBuf);
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
    
    // Extract sender first (needed for both paths)
    char rawSender[PHONE_BUFFER_SIZE];
    charBufSet(rawSender, sizeof(rawSender), msg->sender);
    
    // Defensive cleanup
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

    // Try to extract brand name
    if (isShortCode(rawSender)) {
        if (extractBracketPrefix(msg->message, brand, sizeof(brand))) {
            charBufSet(senderDisplay, sizeof(senderDisplay), brand);
        }
        else if (extractBrandFromMessage(msg->message, brand, sizeof(brand))) {
            charBufSet(senderDisplay, sizeof(senderDisplay), brand);
        }
    } else if (isPhoneNumber(rawSender)) {
        if (extractBracketPrefix(msg->message, brand, sizeof(brand))) {
            charBufSet(senderDisplay, sizeof(senderDisplay), brand);
        }
    }

    // Check if this looks like a continuation of a previous message
    if (looksLikeContinuation(msg->message)) {
        MultipartSms* mp = findMultipartSms(simSlot, msg->sender);
        if (mp) {
            // Add as continuation
            addMultipartPart(mp, msg->message, msg->messageIndex);
            logMsgInt("[SMS] Multipart continuation, part", mp->receivedParts);
            appendMonitorLogInt("[SMS] Multipart part", mp->receivedParts);
            
            // Delete from SIM
            selectSIM(simSlot);
            if (msg->messageIndex > 0) {
                deleteSMS(msg->messageIndex);
            }
            
            // Check if we have enough parts (heuristic: 2 parts is usually enough)
            if (mp->receivedParts >= 2) {
                // Assume complete - forward combined
                logMsg("[SMS] Multipart complete, forwarding combined");
                appendMonitorLog("[SMS] Multipart combined");
                
                // Create combined message
                SmsMessage combinedMsg;
                combinedMsg.simSlot = mp->simSlot;
                combinedMsg.messageIndex = 0;
                combinedMsg.unread = true;
                charBufSet(combinedMsg.sender, sizeof(combinedMsg.sender), mp->sender);
                charBufSet(combinedMsg.message, sizeof(combinedMsg.message), mp->combined);
                
                // Mark inactive
                mp->active = false;
                
                // Log the combined message (same format as normal messages for web UI)
                char simLogBuf[64];
                snprintf(simLogBuf, sizeof(simLogBuf), "[SMS] SIM%d Num=%s", mp->simSlot + 1, simStates[mp->simSlot].number);
                logMsgIntVal("[SMS] SIM", mp->simSlot + 1, "Num", simStates[mp->simSlot].number);
                appendMonitorLog(simLogBuf);
                appendMonitorLogVal("[SMS] From", mp->sender);
                appendMonitorLogVal("[SMS] Msg", mp->combined);
                
                // Process the combined message
                totalReceived++;
                forwardSms(mp->simSlot, mp->sender, &combinedMsg);
            }
            return;
        }
        // Part 2+ with no part 1 in buffer — still forward (OTP/code often in this fragment)
        logMsg("[SMS] Orphan continuation, queued for delayed forward");
        appendMonitorLog("[SMS] Orphan continuation queued");
        totalReceived++;
        persistentReceived++;
        savePersistentStats();
        logMsgIntVal("[SMS] SIM", simSlot + 1, "Num", simStates[simSlot].number);
        appendMonitorLogVal("[SMS] From", senderDisplay);
        if (msg->message && msg->message[0] != '\0') {
            appendMonitorLogVal("[SMS] Msg", msg->message);
        }
        enqueuePendingSms(simSlot, simStates[simSlot].number, senderDisplay, msg->message);
        selectSIM(simSlot);
        if (msg->messageIndex > 0) {
            deleteSMS(msg->messageIndex);
            logMsgInt("[SMS] Deleted from SIM, index", msg->messageIndex);
        }
        return;
    }
    
    // Check if message looks truncated - start multipart collection
    if (looksTruncated(msg->message)) {
        MultipartSms* mp = createMultipartSms(simSlot, senderDisplay);
        if (mp) {
            addMultipartPart(mp, msg->message, msg->messageIndex);
            logMsg("[SMS] Message truncated, waiting for more parts");
            appendMonitorLog("[SMS] Truncated, collecting parts");
            appendMonitorLogInt("[SMS] Part", mp->receivedParts);
            
            // Delete from SIM
            selectSIM(simSlot);
            if (msg->messageIndex > 0) {
                deleteSMS(msg->messageIndex);
            }
            return;  // Don't forward yet, wait for continuation
        }
    }
    
    // Normal message processing
    totalReceived++;
    persistentReceived++;
    savePersistentStats();
    
    // Log to serial
    logMsgIntVal("[SMS] SIM", simSlot + 1, "Num", simStates[simSlot].number);
    
    // Log to web monitor
    char monBuf[128];
    snprintf(monBuf, sizeof(monBuf), "[SMS] SIM%d Num=%s", simSlot + 1, simStates[simSlot].number);
    appendMonitorLog(monBuf);

    if (strcmp(senderDisplay, rawSender) != 0) {
        logMsg2Val("[SMS] From", senderDisplay, "Raw", rawSender);
    } else {
        logMsgVal("[SMS] From", senderDisplay);
    }
    
    // Log sender to web monitor
    appendMonitorLogVal("[SMS] From", senderDisplay);

    // Add message to monitor
    if (msg->message && msg->message[0] != '\0') {
        appendMonitorLogVal("[SMS] Msg", msg->message);
    }

    // Forward the message
    forwardSms(simSlot, senderDisplay, msg);
}

// Forward SMS to backend (extracted for reuse by multipart)
static void forwardSms(int simSlot, const char* senderDisplay, const SmsMessage* msg) {
    char fwdError[64] = "";
    extern volatile bool httpsBusy;
    auto isHexUcs2LikeMessage = [](const char* s) -> bool {
        if (!s) return false;
        int hexCount = 0;
        int total = 0;
        for (const char* p = s; *p; p++) {
            const char c = *p;
            if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
            total++;
            const bool isHex = ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'));
            if (isHex) hexCount++;
        }
        return total >= 24 && (hexCount * 100 / total) >= 95 && (total % 4 == 0);
    };

    if (isHexUcs2LikeMessage(msg ? msg->message : nullptr)) {
        appendErrorLogInt("[SMS] Blocked UCS2-hex-like message SIM", simSlot + 1);
        appendMonitorLog("[SMS] Blocked suspicious hex/UCS2 message");
        // Do not forward/queue this payload.
        return;
    }

    if (httpsBusy) {
        enqueuePendingSms(simSlot, simStates[simSlot].number, senderDisplay, msg->message);
        logMsg("[SMS] Queued (HTTPS busy)");
        appendMonitorLog("[SMS] Queued during HTTPS");
    } else if (forwardSmsToBackendWithSender(msg, senderDisplay, fwdError, sizeof(fwdError))) {
        totalForwarded++;
        persistentForwarded++;
        logMsg("[SMS] Forward success, deleting from SIM");
        appendMonitorLog("[SMS] Forward success");
    } else {
        totalFailed++;
        persistentFailed++;

        // Log to error log with detailed reason
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "[SMS] Forward failed SIM%d: %s", simSlot + 1, fwdError[0] ? fwdError : "unknown");
        appendErrorLog(errBuf);
        appendErrorLogVal("[SMS] Sender", senderDisplay);

        // Add to retry queue with normalized sender
        enqueuePendingSms(simSlot, simStates[simSlot].number, senderDisplay, msg->message);
        logMsg("[SMS] Forward failed, queued for retry");
        
        // Log to monitor with reason
        char monBuf[96];
        snprintf(monBuf, sizeof(monBuf), "[SMS] Forward failed: %s", fwdError[0] ? fwdError : "unknown");
        appendMonitorLog(monBuf);
    }
    
    // Save stats to persistent storage
    savePersistentStats();
    
    // Save SMS to file for Messages tab
    char timestamp[32];
    if (!getLocalTimeStamp(timestamp, sizeof(timestamp))) {
        getFallbackTimeStamp(timestamp, sizeof(timestamp));
    }
    appendSmsToFile(timestamp, simSlot + 1, simStates[simSlot].number, senderDisplay, msg->message);
    
    // Always delete from SIM after processing to prevent re-detection
    // (we've already queued it for retry if needed)
    selectSIM(simSlot);
    if (msg->messageIndex > 0) {
        deleteSMS(msg->messageIndex);
        logMsgInt("[SMS] Deleted from SIM, index", msg->messageIndex);
        appendMonitorLogInt("[SMS] Deleted idx", msg->messageIndex);
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
extern char agentRefreshToken[];
extern char agentSimNumber[];
extern bool agentUseAuth;
extern SimState simStates[];

// Forward SMS to backend with HTTP client
// This is the full implementation
bool forwardSmsToBackendFull(const SmsMessage* msg) {
    return forwardSmsToBackendWithSender(msg, nullptr, nullptr, 0);
}

// Forward SMS to backend with optional normalized sender (brand name extracted from message)
// Returns false and sets errorOut if provided
bool forwardSmsToBackendWithSender(const SmsMessage* msg, const char* normalizedSender, char* errorOut, size_t errorOutSize) {
    if (!msg) {
        if (errorOut) snprintf(errorOut, errorOutSize, "Null message");
        return false;
    }

    // Check WiFi
    if (!WiFi.isConnected()) {
        logMsg("[SMS] No WiFi, queuing");
        if (errorOut) snprintf(errorOut, errorOutSize, "No WiFi");
        return false;
    }

    if (cloudBackendDeferred()) {
        if (errorOut) snprintf(errorOut, errorOutSize, "Cloud sync paused");
        return false;
    }

    // Check config
    if (charBufIsEmpty(agentBaseUrl)) {
        logMsg("[SMS] No backend URL");
        if (errorOut) snprintf(errorOut, errorOutSize, "No backend URL");
        return false;
    }

    // Check auth
    if (agentUseAuth && (charBufIsEmpty(agentBearerToken) || charBufIsEmpty(agentRefreshToken))) {
        logMsg("[SMS] No auth/refresh token");
        if (errorOut) snprintf(errorOut, errorOutSize, "No auth/refresh token");
        return false;
    }

    // Build URL (static to avoid stack)
    static char url[256];
    snprintf(url, sizeof(url), "%s%s", agentBaseUrl, agentApiPath);

    // Determine SIM number (static to avoid stack)
    static char simNum[PHONE_BUFFER_SIZE];
    if (!charBufIsEmpty(simStates[msg->simSlot].number)) {
        normalizePhNumber(simStates[msg->simSlot].number, simNum, sizeof(simNum));
    } else if (!charBufIsEmpty(agentSimNumber)) {
        normalizePhNumber(agentSimNumber, simNum, sizeof(simNum));
    } else {
        simNum[0] = '\0';
    }

    if (strlen(simNum) != 13 || strncmp(simNum, "+639", 4) != 0) {
        if (msg->simSlot >= 0 && msg->simSlot < SIM_COUNT) {
            simStates[msg->simSlot].enabled = false;  // Turn off invalid SIM slot in UI.
        }
        logMsgInt("[SMS] Invalid SIM number, slot disabled", msg->simSlot + 1);
        if (errorOut) snprintf(errorOut, errorOutSize, "Invalid SIM number");
        return false;
    }

    // Use normalized sender if provided, otherwise use original
    const char* senderToUse = normalizedSender && normalizedSender[0] != '\0' ? normalizedSender : msg->sender;

    // Build payload (static to avoid stack overflow)
    static char payload[768];
    static char senderEsc[64];
    static char messageEsc[256];
    static char simNumEsc[64];
    jsonEscape(senderToUse, senderEsc, sizeof(senderEsc));
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
        msg->simSlot,
        senderEsc,
        messageEsc,
        msg->simSlot,
        simNumEsc,
        agentDeviceId
    );
    
    // Make HTTP request (local to avoid collision with heartbeat)
    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    bool isHttps = (strncmp(url, "https://", 8) == 0);
    
    // Wait if heartbeat/maintenance holds the TLS stack (common cause of -2)
    const unsigned long busyWait0 = millis();
    while (httpsBusy && (millis() - busyWait0) < 15000UL) {
        handleWebRequests();
        yield();
        delay(10);
    }
    if (httpsBusy) {
        logMsg("[SMS] HTTPS busy — queued for retry");
        if (errorOut) snprintf(errorOut, errorOutSize, "HTTPS busy");
        return false;
    }

    waitBetweenSmsForwards();
    httpsBusy = true;
    
    if (isHttps) {
        clientSecure.setInsecure();
        if (!http.begin(clientSecure, url)) {
            logMsg("[SMS] HTTP begin failed");
            endSmsHttpsSession();
            if (errorOut) snprintf(errorOut, errorOutSize, "HTTP begin failed");
            return false;
        }
    } else {
        if (!http.begin(client, url)) {
            logMsg("[SMS] HTTP begin failed");
            endSmsHttpsSession();
            if (errorOut) snprintf(errorOut, errorOutSize, "HTTP begin failed");
            return false;
        }
    }
    
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);

    static char authHdr[AGENT_AUTH_HDR_SIZE];
    if (agentUseAuth && !charBufIsEmpty(agentBearerToken)) {
        formatBearerHeader(authHdr, sizeof(authHdr), agentBearerToken);
        http.addHeader("Authorization", authHdr);
    }

    // Try up to 2 times for connection errors
    int attempt = 0;
    int code = -1;

    while (attempt < 2) {
        code = http.POST(payload);

        // Success or non-connection error - don't retry
        if (code >= 0 || (code != -1 && code != -11 && code != -2)) break;

        // Connection error - retry once
        if (code < 0 && attempt == 0) {
            logMsgInt("[SMS] Forward conn error, retrying", code);
            http.end();
            clientSecure.stop();
            client.stop();
            delay(100);
            attempt++;

            // Re-initialize connection
            if (isHttps) {
                clientSecure.setInsecure();
                if (!http.begin(clientSecure, url)) {
                    logMsg("[SMS] HTTP begin failed on retry");
                    break;
                }
            } else {
                if (!http.begin(client, url)) {
                    logMsg("[SMS] HTTP begin failed on retry");
                    break;
                }
            }
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(15000);
            if (agentUseAuth && !charBufIsEmpty(agentBearerToken)) {
                formatBearerHeader(authHdr, sizeof(authHdr), agentBearerToken);
                http.addHeader("Authorization", authHdr);
            }
            continue;
        }
        break;
    }

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
                    endSmsHttpsSession();
                    return false;
                }
            } else {
                if (!http.begin(client, url)) {
                    logMsg("[SMS] HTTP begin failed");
                    endSmsHttpsSession();
                    return false;
                }
            }
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(15000);
            if (agentUseAuth && !charBufIsEmpty(agentBearerToken)) {
                formatBearerHeader(authHdr, sizeof(authHdr), agentBearerToken);
                http.addHeader("Authorization", authHdr);
            }
            code = http.POST(payload);
            http.end();
        }
    }

    clientSecure.stop();
    client.stop();
    endSmsHttpsSession();

    if (code >= 200 && code < 300) {
        logMsgInt("[SMS] Forwarded OK, code", code);
        appendMonitorLogInt("[SMS] Forwarded OK, code", code);
        delay(50);
        return true;
    } else {
        // Log detailed error
        if (code < 0) {
            logMsgInt("[SMS] Forward failed, connection error", code);
            if (errorOut) snprintf(errorOut, errorOutSize, "Connection error %d", code);
        } else if (code == 401) {
            logMsg("[SMS] Forward failed, unauthorized (401)");
            if (errorOut) snprintf(errorOut, errorOutSize, "Unauthorized (401)");
        } else if (code == 403) {
            logMsg("[SMS] Forward failed, forbidden (403)");
            if (errorOut) snprintf(errorOut, errorOutSize, "Forbidden (403)");
        } else if (code == 404) {
            logMsg("[SMS] Forward failed, not found (404)");
            if (errorOut) snprintf(errorOut, errorOutSize, "Not found (404)");
        } else if (code == 429) {
            logMsg("[SMS] Forward failed, rate limited (429)");
            if (errorOut) snprintf(errorOut, errorOutSize, "Rate limited (429)");
        } else if (code >= 500) {
            logMsgInt("[SMS] Forward failed, server error", code);
            if (errorOut) snprintf(errorOut, errorOutSize, "Server error %d", code);
        } else {
            logMsgInt("[SMS] Forward failed, HTTP code", code);
            if (errorOut) snprintf(errorOut, errorOutSize, "HTTP %d", code);
        }
        delay(50);
        return false;
    }
}

// -----------------------------------------------------------------------------
// SMS Polling
// -----------------------------------------------------------------------------

void initSMSPolling() {
    currentPollSim = 0;
    lastPollMs = 0;
    pollingPauseUntil = 0;
    memset(lastSlotPollMs, 0, sizeof(lastSlotPollMs));
}

extern bool modemGatewayRunning;

void pollSIMsForSMS() {
    if (!modemGatewayRunning) {
        return;
    }
    checkMultipartTimeout();

    if (millis() < pollingPauseUntil) {
        return;
    }
    if (isSimBusy()) {
        return;
    }
    if (millis() - lastPollMs < SMS_POLL_INTERVAL_MS) {
        return;
    }

    // Poll exactly one eligible+due SIM per call (round-robin via currentPollSim).
    const int start = currentPollSim;
    int slot = -1;
    for (int i = 0; i < SIM_COUNT; i++) {
        const int trySlot = (start + i) % SIM_COUNT;
        if (!smsSlotEligibleForPoll(trySlot)) {
            continue;
        }
        if (!smsSlotPollDue(trySlot)) {
            continue;
        }
        slot = trySlot;
        break;
    }
    if (slot < 0) {
        currentPollSim = (start + 1) % SIM_COUNT;
        return;
    }
    currentPollSim = (slot + 1) % SIM_COUNT;

    lastPollMs = millis();
    lastSlotPollMs[slot] = millis();
    pollOneSimSlot(slot);
    handleWebRequests();
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
    return isSimBusy();
}

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

unsigned long getTotalSmsReceived() {
    return persistentReceived;
}

unsigned long getTotalSmsForwarded() {
    return persistentForwarded;
}

unsigned long getTotalSmsFailed() {
    return persistentFailed;
}
