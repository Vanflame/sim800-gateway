// ============================================================================
// SIM800 Modem Communication Implementation
// ============================================================================

#include "sim800.h"
#include "mux.h"
#include "logger.h"
#include "utils.h"
#include <Arduino.h>
#include <WebServer.h>

// External web server for handleClient() during AT commands
extern WebServer server;

static bool gYieldToWebServer = true;

// Hardware serial instance (UART2 on ESP32)
HardwareSerial sim800(2);

// Response buffer (static to avoid heap allocation)
static char simBuffer[SIM_BUFFER_SIZE];
static size_t simBufferLen = 0;

// Busy state (defined in main .ino, declared extern in config.h)
// static volatile bool simBusy = false;

// -----------------------------------------------------------------------------
// Serial Interface
// -----------------------------------------------------------------------------

void initSIM800Serial() {
    sim800.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    delay(100);
    flushSimInput();
    logMsg("[SIM800] Serial initialized");
}

// -----------------------------------------------------------------------------
// AT Command Functions
// -----------------------------------------------------------------------------

void clearSimBuffer() {
    simBuffer[0] = '\0';
    simBufferLen = 0;
}

char* getSimBuffer() {
    return simBuffer;
}

void sendATCapture(const char* cmd, unsigned long timeoutMs) {
    setSimBusy(true);
    clearSimBuffer();
    
    // Flush any pending unsolicited bytes so they don't mix with this command response
    for (int i = 0; i < 200 && sim800.available(); i++) {
        (void)sim800.read();
        delay(1);
    }
    
    // Send command
    sim800.println(cmd);
    
    // Read response with timeout
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        int readCount = 0;
        while (sim800.available() && readCount < 100) {
            readCount++;
            char c = (char)sim800.read();
            if (simBufferLen < SIM_BUFFER_SIZE - 1) {
                simBuffer[simBufferLen++] = c;
                simBuffer[simBufferLen] = '\0';
            }
        }
        
        // Check if response is complete (ends with OK or ERROR)
        charBufTrim(simBuffer);
        simBufferLen = strlen(simBuffer);  // Update length after trim
        
        if (simBufferLen >= 2) {
            // Check for "OK" at end
            if (strcmp(simBuffer + simBufferLen - 2, "OK") == 0) {
                break;
            }
            // Check for ERROR at end
            if (simBufferLen >= 5 && strcmp(simBuffer + simBufferLen - 5, "ERROR") == 0) {
                break;
            }
            // Check for prompt character
            if (simBuffer[simBufferLen - 1] == '>') {
                break;
            }
        }
        
        // Yield to web server to keep UI responsive (can be disabled to prevent re-entrancy)
        if (gYieldToWebServer) {
            server.handleClient();
        }
        delay(2);
    }
    
    charBufTrim(simBuffer);
    simBufferLen = strlen(simBuffer);
    setSimBusy(false);
}

void setSimYieldToWebServer(bool enabled) {
    gYieldToWebServer = enabled;
}

bool sendAT(const char* cmd, unsigned long timeoutMs) {
    sendATCapture(cmd, timeoutMs);
    return responseIsOK();
}

int readLine(char* buf, size_t bufSize, unsigned long timeoutMs) {
    if (!buf || bufSize < 1) return 0;
    
    buf[0] = '\0';
    size_t len = 0;
    unsigned long start = millis();
    
    while (millis() - start < timeoutMs && len < bufSize - 1) {
        if (sim800.available()) {
            char c = (char)sim800.read();
            
            if (c == '\n') {
                if (len > 0) {
                    buf[len] = '\0';
                    return len;
                }
            } else if (c != '\r') {
                buf[len++] = c;
            }
        }
        delay(1);
    }
    
    buf[len] = '\0';
    return len;
}

void readAllAvailable(char* buf, size_t bufSize) {
    if (!buf || bufSize < 1) return;
    
    size_t len = 0;
    while (sim800.available() && len < bufSize - 1) {
        buf[len++] = (char)sim800.read();
        delay(1);
    }
    buf[len] = '\0';
}

void flushSimInput() {
    for (int i = 0; i < UART_FLUSH_ITER && sim800.available(); i++) {
        sim800.read();
        delay(1);
    }
}

// -----------------------------------------------------------------------------
// Response Parsing
// -----------------------------------------------------------------------------

bool responseContains(const char* str) {
    return strstr(simBuffer, str) != NULL;
}

bool responseIsOK() {
    return strstr(simBuffer, "OK") != NULL;
}

bool extractValue(const char* prefix, char* value, size_t valueSize) {
    const char* p = strstr(simBuffer, prefix);
    if (!p) return false;
    
    p += strlen(prefix);
    while (*p == ' ' || *p == ':') p++;
    
    // Copy until newline or end
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < valueSize - 1) {
        value[i++] = *p++;
    }
    value[i] = '\0';
    
    return i > 0;
}

// -----------------------------------------------------------------------------
// Modem Status Functions
// -----------------------------------------------------------------------------

bool modemResponds() {
    return sendAT("AT", 500);
}

bool checkNetworkRegistration() {
    sendATCapture("AT+CREG?", 1000);
    return cregIndicatesRegistered(simBuffer);
}

int getSignalQuality() {
    sendATCapture("AT+CSQ", 1000);
    return extractSignalQuality(simBuffer);
}

void getOperatorName(char* buf, size_t bufSize) {
    sendATCapture("AT+COPS?", 1000);
    extractOperatorName(simBuffer, buf, bufSize);
}

void getPhoneNumber(char* buf, size_t bufSize) {
    if (!buf || bufSize < 1) return;
    buf[0] = '\0';
    
    // Try AT+CNUM first
    sendATCapture("AT+CNUM", 1000);
    if (extractPhoneNumber(simBuffer, buf, bufSize) > 0) {
        return;
    }
    
    // Try reading from SIM with AT+CIMI (IMSI)
    // This gives IMSI, not phone number - but can be useful
    // Phone number is usually stored by network
}

void getBatteryInfo(int* percent, int* mv) {
    if (percent) *percent = 0;
    if (mv) *mv = 0;
    
    sendATCapture("AT+CBC", 1000);
    
    // Parse: +CBC: <bcl>,<bcs>,<v>
    const char* p = strstr(simBuffer, "+CBC:");
    if (!p) return;
    
    p += 5;
    while (*p == ' ') p++;

    // Expected: <bcs>,<bcl>,<v>
    // Example: +CBC: 0,55,3846  -> 55%
    const char* comma1 = strchr(p, ',');
    if (!comma1) return;
    const char* comma2 = strchr(comma1 + 1, ',');
    if (!comma2) return;

    if (percent) *percent = atoi(comma1 + 1);
    if (mv) *mv = atoi(comma2 + 1);
}

void getNetworkTime(char* buf, size_t bufSize) {
    if (!buf || bufSize < 1) return;
    buf[0] = '\0';
    
    sendATCapture("AT+CCLK?", 1000);
    
    // Parse: +CCLK: "yy/MM/dd,HH:mm:ss+zz"
    const char* q1 = strchr(simBuffer, '"');
    if (!q1) return;
    
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return;
    
    size_t len = q2 - q1 - 1;
    if (len >= bufSize) len = bufSize - 1;
    
    strncpy(buf, q1 + 1, len);
    buf[len] = '\0';
}

// -----------------------------------------------------------------------------
// Modem Configuration
// -----------------------------------------------------------------------------

bool initModemForSMS() {
    // Core commands - must succeed
    if (!sendAT("AT", 1000)) return false;
    if (!sendAT("AT+CPIN?", 2000)) return false;  // Check SIM ready
    
    // Optional commands - nice to have but not critical
    setSMSTextMode();
    sendAT("AT+CSCS=\"GSM\"", 500);
    sendAT("AT+CPMS=\"SM\",\"SM\",\"SM\"", 1000);  // Set SMS storage
    setSMSIndication();
    disableCallerID();  // Reduce spam
    sendAT("AT+GSMBUSY=1", 500);  // Reject incoming calls
    enableLocalTimestamp();
    
    return true;  // Success if core commands passed
}

bool enableCallerID() {
    return sendAT("AT+CLIP=1", 500);
}

bool disableCallerID() {
    return sendAT("AT+CLIP=0", 500);
}

bool setSMSIndication() {
    return sendAT("AT+CNMI=2,1,0,0,0", 500);
}

bool enableLocalTimestamp() {
    bool ok = true;
    ok &= sendAT("AT+CLTS=1", 500);
    ok &= sendAT("AT+CTZU=1", 500);
    return ok;
}

// -----------------------------------------------------------------------------
// Call Functions
// -----------------------------------------------------------------------------

bool dialNumber(const char* number) {
    if (!number || !number[0]) return false;
    
    flushSimInput();
    
    sim800.print("ATD");
    sim800.print(number);
    sim800.println(";");
    
    // Wait for response
    sendATCapture("", 5000);
    
    return responseIsOK() || responseContains("OK");
}

void hangupCall() {
    simBusy = true;
    sim800.println("ATH");
    delay(100);
    flushSimInput();
    simBusy = false;
}

bool isCallInProgress() {
    sendATCapture("AT+CLCC", 500);
    return strstr(simBuffer, "+CLCC:") != NULL;
}

bool checkIncomingCall(char* callerBuf, size_t bufSize) {
    if (!callerBuf || bufSize < 1) return false;
    callerBuf[0] = '\0';
    
    // Check for RING
    if (!responseContains("RING")) return false;
    
    // Try to extract caller from +CLIP
    const char* clip = strstr(simBuffer, "+CLIP:");
    if (!clip) return true;  // RING but no caller ID
    
    // Parse: +CLIP: "+639xxx",...
    const char* q1 = strchr(clip, '"');
    if (!q1) return true;
    
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return true;
    
    size_t len = q2 - q1 - 1;
    if (len >= bufSize) len = bufSize - 1;
    
    strncpy(callerBuf, q1 + 1, len);
    callerBuf[len] = '\0';
    
    return true;
}

// -----------------------------------------------------------------------------
// SMS Functions
// -----------------------------------------------------------------------------

bool setSMSTextMode() {
    return sendAT("AT+CMGF=1", 500);
}

void listSMS(const char* filter) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGL=\"%s\"", filter);
    sendATCapture(cmd, 2000);
}

bool readSMS(int index, char* sender, size_t senderSize, char* message, size_t messageSize) {
    if (sender) sender[0] = '\0';
    if (message) message[0] = '\0';
    
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
    sendATCapture(cmd, 1000);
    
    if (!responseIsOK()) return false;
    
    // Parse: +CMGR: "REC READ","+639xxx",,"yy/MM/dd,HH:mm:ss+zz"
    // Followed by message text
    const char* cmgr = strstr(simBuffer, "+CMGR:");
    if (!cmgr) return false;
    
    // Extract sender
    if (sender && senderSize > 0) {
        const char* q1 = strchr(cmgr, '"');
        if (q1) {
            q1 = strchr(q1 + 1, '"');  // Skip first quote pair
            if (q1) {
                q1++;  // Move past opening quote
                const char* q2 = strchr(q1, '"');
                if (q2) {
                    size_t len = q2 - q1;
                    if (len >= senderSize) len = senderSize - 1;
                    strncpy(sender, q1, len);
                    sender[len] = '\0';
                }
            }
        }
    }
    
    // Extract message (after second line)
    const char* newline = strchr(cmgr, '\n');
    if (newline && message && messageSize > 0) {
        newline++;  // Skip newline
        const char* end = strstr(newline, "\r\nOK");
        if (end) {
            size_t len = end - newline;
            if (len >= messageSize) len = messageSize - 1;
            strncpy(message, newline, len);
            message[len] = '\0';
            charBufTrim(message);
        }
    }
    
    return true;
}

bool deleteSMS(int index) {
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    return sendAT(cmd, 1000);
}

bool deleteAllSMS() {
    return sendAT("AT+CMGDA=\"DEL ALL\"", 2000);
}

bool sendSMS(const char* number, const char* message) {
    if (!number || !number[0] || !message) return false;
    
    // Set text mode
    if (!setSMSTextMode()) return false;
    
    // Start SMS
    sim800.print("AT+CMGS=\"");
    sim800.print(number);
    sim800.println("\"");
    
    delay(500);
    
    // Wait for ">" prompt
    unsigned long start = millis();
    bool gotPrompt = false;
    while (millis() - start < 2000) {
        if (sim800.available()) {
            char c = (char)sim800.read();
            if (c == '>') {
                gotPrompt = true;
                break;
            }
        }
        delay(10);
    }
    
    if (!gotPrompt) {
        logMsg("[SMS] No prompt for SMS send");
        return false;
    }
    
    // Send message
    sim800.print(message);
    sim800.write(0x1A);  // Ctrl+Z
    
    // Wait for response
    sendATCapture("", 5000);
    
    if (responseContains("+CMGS:")) {
        logMsgVal("[SMS] Sent to", number);
        return true;
    }
    
    logMsgVal("[SMS] Failed to send to", number);
    return false;
}

// -----------------------------------------------------------------------------
// State Tracking
// -----------------------------------------------------------------------------

bool isSimBusy() {
    return simBusy;
}

void setSimBusy(bool busy) {
    simBusy = busy;
}

// Check all SIMs on startup - marks responsive SIMs and initializes them
void checkAllSIMsOnStartup() {
    extern SimState simStates[];

    // Prevent re-entrant web requests while probing SIMs
    bool prevYield = gYieldToWebServer;
    gYieldToWebServer = false;

    // Pass 1: probe all SIMs
    for (int i = 0; i < SIM_COUNT; i++) {
        selectSIM(i);
        sendATCapture("AT", 500);
        bool isResponsive = (strstr(getSimBuffer(), "OK") != NULL);
        simStates[i].responsive = isResponsive;

        if (!isResponsive) {
            simStates[i].enabled = false;
            simStates[i].registered = false;
            charBufClear(simStates[i].number, sizeof(simStates[i].number));
            charBufClear(simStates[i].creg, sizeof(simStates[i].creg));
            charBufClear(simStates[i].cops, sizeof(simStates[i].cops));
            charBufClear(simStates[i].csq, sizeof(simStates[i].csq));
        }

        if (isResponsive) {
            logMsgInt("[SETUP] SIM responsive:", i + 1);
        } else {
            logMsgInt("[SETUP] SIM not responding:", i + 1);
        }

        delay(100);
    }

    // Pass 2: init responsive SIMs
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!simStates[i].responsive) continue;

        selectSIM(i);

        logMsgInt("[SETUP] SIM init begin:", i + 1);
        sendATCapture("AT+CPIN?", 700);
        sendATCapture("AT+CMGF=1", 700);
        sendATCapture("AT+CSCS=\"GSM\"", 700);
        sendATCapture("AT+CPMS=\"SM\",\"SM\",\"SM\"", 700);
        sendATCapture("AT+CNMI=2,1,0,0,0", 700);
        sendATCapture("AT+CLIP=0", 700);
        sendATCapture("AT+GSMBUSY=1", 700);
        
        // Debug: Show SMS storage status
        sendATCapture("AT+CPMS?", 600);
        logMsgVal("[SETUP] SMS Storage", getSimBuffer());
        // Debug: Show SMSC (SMS center number)
        sendATCapture("AT+CSCA?", 600);
        logMsgVal("[SETUP] SMSC", getSimBuffer());
        
        simStates[i].basicInitDone = true;
        logMsgInt("[SETUP] SIM init sms/call cfg OK:", i + 1);

        sendATCapture("AT+CSQ", 3000);
        charBufSet(simStates[i].csq, sizeof(simStates[i].csq), getSimBuffer());

        sendATCapture("AT+CREG?", 1500);
        charBufSet(simStates[i].creg, sizeof(simStates[i].creg), getSimBuffer());
        simStates[i].registered = (strstr(simStates[i].creg, "0,1") != NULL || strstr(simStates[i].creg, "0,5") != NULL);

        sendATCapture("AT+COPS?", 1500);
        charBufSet(simStates[i].cops, sizeof(simStates[i].cops), getSimBuffer());

        sendATCapture("AT+CNUM", 1500);
        char numBuf[64];
        charBufSet(numBuf, sizeof(numBuf), getSimBuffer());
        const char* numStart = strstr(numBuf, ",\"");
        if (numStart) {
            numStart += 2;
            const char* numEnd = strchr(numStart, '"');
            if (numEnd) {
                int len = (int)(numEnd - numStart);
                if (len > 0 && len < (int)sizeof(simStates[i].number)) {
                    strncpy(simStates[i].number, numStart, (size_t)len);
                    simStates[i].number[len] = '\0';
                }
            }
        }

        getBatteryInfo(&simStates[i].batteryPercent, &simStates[i].batteryMv);
        logMsgInt("[SETUP] SIM init complete:", i + 1);

        delay(100);
    }

    gYieldToWebServer = prevYield;
}
