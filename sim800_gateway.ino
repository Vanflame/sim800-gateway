// ============================================================================
// SIM800 Gateway - Main Program
// 16-SIM ESP32 Gateway with Multiplexer
// ============================================================================
//
// Architecture:
//   ESP32 TX ──────────────> all SIM RX pins (shared)
//   ESP32 RX <──── MUX <──── SIM TX pins (multiplexed)
//
// This modular structure prevents heap fragmentation and makes debugging
// 10x easier by isolating functionality into separate modules.
// ============================================================================

#include "config.h"
#include "mux.h"
#include "sim800.h"
#include "sms.h"
#include "webui.h"
#include "logger.h"
#include "utils.h"
#include "ota.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Global State Definitions (declared in config.h)
// -----------------------------------------------------------------------------

SimState simStates[SIM_COUNT];
PendingSms pendingSmsQueue[MAX_PENDING_SMS];
CallLogItem callLog[MAX_CALL_LOG];
int pendingSmsCount = 0;
int callLogCount = 0;
ActiveSession activeSessions[8];  // Max 8 concurrent sessions
int activeSessionCount = 0;
int activeSim = 0;
int currentMuxSim = 0;
volatile bool simBusy = false;
volatile bool httpsBusy = false;  // Prevent concurrent HTTPS
bool smsPollingPaused = false;
bool heartbeatPaused = false;
bool deviceRegistered = false;
bool simRegistered = false;

// Monitor log
char monitorLog[MONITOR_LOG_MAX_SIZE][160];
int monitorLogCount = 0;

// Error log (persistent, longer messages)
char errorLog[ERROR_LOG_MAX_SIZE][ERROR_LOG_LINE_SIZE];
int errorLogCount = 0;

// Timing
unsigned long lastHeartbeatMs = 0;
unsigned long lastSmsPollMs = 0;
unsigned long smsPollingPauseUntil = 0;

// Agent config
char agentBaseUrl[128] = "";
char agentDeviceId[64] = "";
char agentBearerToken[1024] = "";  // JWT tokens can be 700+ chars
char agentRefreshToken[512] = "";
char agentSimNumber[PHONE_BUFFER_SIZE] = "";
int agentSimSlot = 0;
char agentApiPath[64] = DEFAULT_API_PATH;
bool agentUseAuth = true;

// WiFi
char wifiSsid[64] = "";
char wifiPassword[64] = "";

// Battery
int batteryPercent = 0;
int batteryMv = 0;

// NTP
bool ntpConfigured = false;

// -----------------------------------------------------------------------------
// Preferences
// -----------------------------------------------------------------------------

Preferences preferences;

// Persistent statistics (stored in NVS)
unsigned long persistentReceived = 0;
unsigned long persistentForwarded = 0;
unsigned long persistentFailed = 0;

// -----------------------------------------------------------------------------
// Persistent Storage Functions
// -----------------------------------------------------------------------------

void loadPersistentStats() {
    preferences.begin("gateway", true);  // read-only
    persistentReceived = preferences.getULong("received", 0);
    persistentForwarded = preferences.getULong("forwarded", 0);
    persistentFailed = preferences.getULong("failed", 0);
    preferences.end();
    
    logMsgInt("[STORE] Loaded stats - Received:", (int)persistentReceived);
}

void savePersistentStats() {
    preferences.begin("gateway", false);  // read-write
    preferences.putULong("received", persistentReceived);
    preferences.putULong("forwarded", persistentForwarded);
    preferences.putULong("failed", persistentFailed);
    preferences.end();
}

// Append SMS to persistent file
void appendSmsToFile(const char* time, int simSlot, const char* number, const char* sender, const char* message) {
    File file = LittleFS.open("/messages.log", "a");
    if (!file) {
        logMsg("[STORE] Failed to open messages.log");
        return;
    }
    
    // Format: timestamp|sim|number|sender|message\n
    file.print(time);
    file.print("|");
    file.print(simSlot);
    file.print("|");
    file.print(number);
    file.print("|");
    file.print(sender);
    file.print("|");
    file.println(message);
    file.close();
}

// Read SMS messages from file (returns count, fills buffer)
int readSmsFromFile(char* buf, size_t bufSize, int maxMessages) {
    if (!LittleFS.exists("/messages.log")) {
        return 0;
    }
    
    File file = LittleFS.open("/messages.log", "r");
    if (!file) return 0;
    
    // Read file in chunks, keeping only last maxMessages
    int count = 0;
    size_t pos = 0;
    buf[0] = '\0';
    
    // Simple approach: read and count, then seek back
    file.seek(0, SeekEnd);
    size_t fileSize = file.position();
    file.seek(0, SeekSet);
    
    // If file is small enough, just read it all
    if (fileSize < bufSize) {
        while (file.available() && pos < bufSize - 1) {
            buf[pos++] = file.read();
        }
        buf[pos] = '\0';
        
        // Count newlines
        for (size_t i = 0; buf[i]; i++) {
            if (buf[i] == '\n') count++;
        }
    } else {
        // For large files, read from end
        // Find start of last maxMessages lines
        int linesFound = 0;
        size_t readPos = fileSize;
        char chunk[128];
        
        while (readPos > 0 && linesFound < maxMessages) {
            size_t chunkSize = (readPos > sizeof(chunk)) ? sizeof(chunk) : readPos;
            readPos -= chunkSize;
            file.seek(readPos, SeekSet);
            file.readBytes(chunk, chunkSize);
            
            for (int i = chunkSize - 1; i >= 0 && linesFound < maxMessages; i--) {
                if (chunk[i] == '\n') linesFound++;
            }
        }
        
        // Now read from that position
        file.seek(readPos, SeekSet);
        while (file.available() && pos < bufSize - 1) {
            buf[pos++] = file.read();
        }
        buf[pos] = '\0';
        count = linesFound;
    }
    
    file.close();
    return count;
}

// Append error to persistent file
void appendErrorToFile(const char* time, const char* error) {
    File file = LittleFS.open("/errors.log", "a");
    if (!file) return;
    
    file.print(time);
    file.print("|");
    file.println(error);
    file.close();
}

// Read errors from persistent file
int readErrorsFromFile(char* buf, size_t bufSize, int maxErrors) {
    if (!LittleFS.exists("/errors.log")) {
        return 0;
    }
    
    File file = LittleFS.open("/errors.log", "r");
    if (!file) return 0;
    
    int count = 0;
    size_t pos = 0;
    buf[0] = '\0';
    
    file.seek(0, SeekEnd);
    size_t fileSize = file.position();
    file.seek(0, SeekSet);
    
    // If file is small enough, just read it all
    if (fileSize < bufSize) {
        while (file.available() && pos < bufSize - 1) {
            buf[pos++] = file.read();
        }
        buf[pos] = '\0';
        
        for (size_t i = 0; buf[i]; i++) {
            if (buf[i] == '\n') count++;
        }
    } else {
        // For large files, read from end
        int linesFound = 0;
        size_t readPos = fileSize;
        char chunk[128];
        
        while (readPos > 0 && linesFound < maxErrors) {
            size_t chunkSize = (readPos > sizeof(chunk)) ? sizeof(chunk) : readPos;
            readPos -= chunkSize;
            file.seek(readPos, SeekSet);
            file.readBytes(chunk, chunkSize);
            
            for (int i = chunkSize - 1; i >= 0 && linesFound < maxErrors; i--) {
                if (chunk[i] == '\n') linesFound++;
            }
        }
        
        file.seek(readPos, SeekSet);
        while (file.available() && pos < bufSize - 1) {
            buf[pos++] = file.read();
        }
        buf[pos] = '\0';
        count = linesFound;
    }
    
    file.close();
    return count;
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
    // Initialize Serial for debugging
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n========================================");
    Serial.println("SIM800 Gateway Starting...");
    Serial.println("========================================");

    appendMonitorLog("[BOOT] Starting...");
    
    // Logger is header-only (no init function)
    for (int i = 0; i < SIM_COUNT; i++) {
        simStates[i].enabled = true;
        simStates[i].basicInitDone = false;
        simStates[i].responsive = false;
        simStates[i].registered = false;
        simStates[i].backendRegistered = false;
        simStates[i].errorCount = 0;
        simStates[i].consecutiveErrors = 0;
        simStates[i].signalStrength = -1;
        simStates[i].networkType[0] = '\0';
        simStates[i].lastSuccessfulPoll = 0;
        charBufClear(simStates[i].number, sizeof(simStates[i].number));
        charBufClear(simStates[i].creg, sizeof(simStates[i].creg));
        charBufClear(simStates[i].cops, sizeof(simStates[i].cops));
        charBufClear(simStates[i].csq, sizeof(simStates[i].csq));
    }
    
    // Load settings
    loadSettings();
    otaLoadUrlFromPreferences();
    appendMonitorLog("[BOOT] Settings loaded");
    
    // Initialize LittleFS for persistent storage
    if (!LittleFS.begin(true)) {
        Serial.println("[STORE] LittleFS mount failed, formatting...");
        appendMonitorLog("[STORE] LittleFS format needed");
    } else {
        Serial.println("[STORE] LittleFS mounted");
    }
    
    // Load persistent statistics
    loadPersistentStats();
    
    // Initialize WiFi
    initWiFi();
    appendMonitorLog(WiFi.isConnected() ? "[WIFI] Connected" : "[WIFI] AP mode");
    
    // Initialize hardware modules
    // Initialize SIM800 serial
    initSIM800Serial();
    appendMonitorLog("[SIM800] Serial init");

    // Initialize SIM multiplexor
    initMux();
    appendMonitorLog("[MUX] Initialized");
    initSMSQueue();
    initSMSPolling();
    
    appendMonitorLog("[SMS] Init");
    
    // Initialize web UI
    initWebUI();
    
    // Initial SIM check on startup
    logMsg("[SETUP] Performing initial SIM check...");
    appendMonitorLog("[SETUP] SIM probe start");
    checkAllSIMsOnStartup();
    appendMonitorLog("[SETUP] SIM probe done");
    
    logMsg("========================================");
    logMsg("Setup complete!");
    logMsg("========================================");

    appendMonitorLog("[BOOT] Ready");
}

// -----------------------------------------------------------------------------
// Main Loop
// -----------------------------------------------------------------------------

void loop() {
    // Handle web requests (must be called frequently)
    handleWebRequests();
    
    // Poll SIMs for new SMS
    if (!isSimBusy() && !smsPollingPaused) {
        pollSIMsForSMS();
    }
    
    // Process pending SMS queue
    processPendingSmsQueue();
    
    // Periodic tasks
    unsigned long now = millis();
    
    // Heartbeat / sync with backend
    if (!heartbeatPaused && (now - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS)) {
        lastHeartbeatMs = now;
        performHeartbeat();
    }
    
    // Battery info update
    static unsigned long lastBatteryMs = 0;
    if (now - lastBatteryMs > BATTERY_INFO_INTERVAL_MS) {
        lastBatteryMs = now;
        updateBatteryInfo();
    }
    
    // SIM watchdog - check for unresponsive SIMs and attempt recovery
    static unsigned long lastWatchdogMs = 0;
    if (now - lastWatchdogMs > 30000) {  // Check every 30 seconds
        lastWatchdogMs = now;
        checkAndRecoverUnresponsiveSims();
    }
    
    // Periodic signal strength update for all SIMs
    static unsigned long lastSignalUpdateMs = 0;
    static int signalUpdateSimIdx = 0;
    if (now - lastSignalUpdateMs > 5000) {  // Update one SIM every 5 seconds
        lastSignalUpdateMs = now;
        updateSimSignalStrength(signalUpdateSimIdx);
        signalUpdateSimIdx = (signalUpdateSimIdx + 1) % SIM_COUNT;
    }
    
    // Small delay to prevent tight loops
    delay(10);
}

// -----------------------------------------------------------------------------
// WiFi Initialization
// -----------------------------------------------------------------------------

void initWiFi() {
    // Start in AP mode
    char apSsid[32];
    snprintf(apSsid, sizeof(apSsid), AP_SSID_PREFIX "%06X", (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));
    
    WiFi.softAP(apSsid, AP_PASSWORD, AP_CHANNEL);
    logMsg2Val("[WIFI] AP started", apSsid, "IP", WiFi.softAPIP().toString().c_str());
    
    // Try to connect to saved WiFi
    if (!charBufIsEmpty(wifiSsid)) {
        logMsg2Val("[WIFI] Connecting to", wifiSsid, "", "");
        WiFi.begin(wifiSsid, wifiPassword);
        
        // Wait for connection with timeout
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 30) {
            delay(500);
            attempts++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            logMsg2Val("[WIFI] Connected", WiFi.localIP().toString().c_str(), "SSID", wifiSsid);
            
            // Configure NTP
            configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
            ntpConfigured = true;
        } else {
            logMsg("[WIFI] Connection failed, staying in AP mode");
        }
    }
}

// -----------------------------------------------------------------------------
// Settings Load/Save
// -----------------------------------------------------------------------------

void loadSettings() {
    preferences.begin("wifi", true);
    preferences.getString("ssid", wifiSsid, sizeof(wifiSsid));
    preferences.getString("pw", wifiPassword, sizeof(wifiPassword));
    preferences.end();
    
    // Use default WiFi if not saved
    if (charBufIsEmpty(wifiSsid)) {
        charBufSet(wifiSsid, sizeof(wifiSsid), DEFAULT_WIFI_SSID);
        charBufSet(wifiPassword, sizeof(wifiPassword), DEFAULT_WIFI_PASSWORD);
        logMsg("[CONFIG] Using default WiFi credentials");
    }
    
    preferences.begin("agent", true);
    preferences.getString("base", agentBaseUrl, sizeof(agentBaseUrl));
    preferences.getString("dev", agentDeviceId, sizeof(agentDeviceId));
    preferences.getString("tok", agentBearerToken, sizeof(agentBearerToken));
    preferences.getString("rtok", agentRefreshToken, sizeof(agentRefreshToken));
    preferences.getString("sim", agentSimNumber, sizeof(agentSimNumber));
    agentSimSlot = preferences.getInt("slot", 0);
    preferences.getString("path", agentApiPath, sizeof(agentApiPath));
    agentUseAuth = preferences.getBool("auth", true);
    heartbeatPaused = preferences.getBool("hb_pause", false);
    preferences.end();
    
    // Use default base URL if not saved
    if (charBufIsEmpty(agentBaseUrl)) {
        charBufSet(agentBaseUrl, sizeof(agentBaseUrl), DEFAULT_BASE_URL);
        charBufSet(agentApiPath, sizeof(agentApiPath), DEFAULT_API_PATH);
        logMsg2Val("[CONFIG] Using default base URL", agentBaseUrl, "path", agentApiPath);
    }
    
    // Generate device ID if not set
    if (charBufIsEmpty(agentDeviceId)) {
        snprintf(agentDeviceId, sizeof(agentDeviceId), "SIM800-%06X", (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));
        preferences.begin("agent", false);
        preferences.putString("dev", agentDeviceId);
        preferences.end();
        logMsg2Val("[CONFIG] Generated device ID", agentDeviceId, "", "");
    }
    
    logMsg("[CONFIG] Settings loaded");
}

// -----------------------------------------------------------------------------
// Heartbeat / Backend Sync
// -----------------------------------------------------------------------------

// Normalize SIM number to include + prefix for PH numbers
static void normalizeSimNumber(const char* raw, char* out, size_t outSize) {
    if (!raw || !raw[0]) {
        out[0] = '\0';
        return;
    }
    // If already starts with +, copy as-is
    if (raw[0] == '+') {
        charBufSet(out, outSize, raw);
        return;
    }
    // Add + prefix
    snprintf(out, outSize, "+%s", raw);
}

void performHeartbeat() {
    if (charBufIsEmpty(agentBaseUrl)) {
        return;  // No backend configured
    }
    
    if (!WiFi.isConnected()) {
        return;  // No internet
    }
    
    // Skip if another HTTPS operation is in progress
    if (httpsBusy) {
        return;
    }
    
    httpsBusy = true;
    
    // Ensure NTP for TLS
    if (!ntpConfigured) {
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        ntpConfigured = true;
        delay(500);
    }
    
    // Send heartbeat to backend
    char url[256];
    snprintf(url, sizeof(url), "%s/api/agent/heartbeat", agentBaseUrl);
    
    bool isHttps = (strncmp(url, "https://", 8) == 0);
    
    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    
    if (isHttps) {
        clientSecure.setInsecure();
        clientSecure.setTimeout(25000);  // 25 second timeout - backend does many DB ops
        if (!http.begin(clientSecure, url)) {
            logMsg("[HEARTBEAT] HTTPS begin failed");
            appendMonitorLog("[HEARTBEAT] HTTPS begin failed");
            httpsBusy = false;
            return;
        }
    } else {
        if (!http.begin(client, url)) {
            logMsg("[HEARTBEAT] HTTP begin failed");
            appendMonitorLog("[HEARTBEAT] HTTP begin failed");
            httpsBusy = false;
            return;
        }
    }
    
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(25000);  // 25 second timeout
    
    if (!charBufIsEmpty(agentBearerToken)) {
        http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
    }

    if (HEARTBEAT_DEBUG) {
        http.addHeader("x-heartbeat-debug", "1");
    }

    // Backend expects { device_id, battery_level, sims:[{number,slot,status,signal_strength,network_type}] }
    static char body[3072];  // Reduced from 4096
    
    // Find lowest battery percentage among all responsive SIMs
    int lowestBatteryPercent = 100;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!simStates[i].enabled) continue;
        if (!simStates[i].responsive) continue;
        if (simStates[i].batteryPercent > 0 && simStates[i].batteryPercent < lowestBatteryPercent) {
            lowestBatteryPercent = simStates[i].batteryPercent;
        }
    }
    // If no SIM has battery info, use ESP32's battery (if available)
    if (lowestBatteryPercent == 100 && batteryPercent > 0) {
        lowestBatteryPercent = batteryPercent;
    }
    
    size_t pos = 0;
    pos += snprintf(body + pos, sizeof(body) - pos,
        "{\"device_id\":\"%s\",\"battery_level\":%d,\"sims\":[",
        agentDeviceId,
        lowestBatteryPercent
    );

    bool first = true;
    for (int i = 0; i < SIM_COUNT && pos < sizeof(body) - 300; i++) {
        if (!simStates[i].enabled) continue;
        if (!simStates[i].responsive) continue;
        if (charBufIsEmpty(simStates[i].number)) continue;

        if (!first) pos += snprintf(body + pos, sizeof(body) - pos, ",");
        first = false;

        // Normalize number with + prefix for backend consistency
        char numEsc[64];
        char normalizedNum[PHONE_BUFFER_SIZE];
        normalizeSimNumber(simStates[i].number, normalizedNum, sizeof(normalizedNum));
        jsonEscape(normalizedNum, numEsc, sizeof(numEsc));
        
        // Get signal strength from CSQ
        int signal = extractSignalQuality(simStates[i].csq);
        
        // Get network type (SIM800L is 2G only)
        const char* netType = simStates[i].networkType[0] ? simStates[i].networkType : "2G";
        
        pos += snprintf(body + pos, sizeof(body) - pos,
            "{\"number\":%s,\"slot\":%d,\"status\":\"ACTIVE\",\"signal_strength\":%d,\"network_type\":\"%s\"}",
            numEsc,
            i,
            signal,
            netType
        );
    }

    pos += snprintf(body + pos, sizeof(body) - pos, "]}");

    if (HEARTBEAT_DEBUG) {
        logMsg("[HEARTBEAT][DBG] Request body:");
        appendMonitorLog("[HEARTBEAT][DBG] Request body:");
        logMsg(body);
        appendMonitorLog(body);
    }

    // Try up to 2 times for connection errors
    int attempt = 0;
    int code = -1;
    String resp = "";
    
    while (attempt < 2) {
        code = http.POST(body);
        if (code > 0) {
            resp = http.getString();
        }
        http.end();
        
        // Success or non-connection error - don't retry
        if (code >= 0 && code != -1 && code != -11) break;
        
        // Connection error - retry once
        if (code < 0 && attempt == 0) {
            logMsgInt("[HEARTBEAT] Conn error, retrying", code);
            clientSecure.stop();
            client.stop();
            delay(500);  // Wait before retry
            
            // Reconnect
            if (isHttps) {
                clientSecure.setInsecure();
                clientSecure.setTimeout(25000);
                http.begin(clientSecure, url);
            } else {
                http.begin(client, url);
            }
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(25000);
            if (!charBufIsEmpty(agentBearerToken)) {
                http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
            }
        }
        attempt++;
    }

    if (code < 0) {
        logMsgInt("[HEARTBEAT] Failed, code", code);
        appendMonitorLogInt("[HEARTBEAT] Failed", code);
        appendErrorLogInt("[HEARTBEAT] Connection failed", code);
        clientSecure.stop();
        client.stop();
        httpsBusy = false;
        return;
    }
    
    if (code == 401) {
        // Stop previous connection before refresh
        http.end();
        clientSecure.stop();
        client.stop();
        delay(100);  // Let TCP fully close
        
        if (refreshAgentToken()) {
            if (isHttps) {
                clientSecure.setInsecure();
                http.begin(clientSecure, url);
            } else {
                http.begin(client, url);
            }
            http.addHeader("Content-Type", "application/json");
            if (!charBufIsEmpty(agentBearerToken)) {
                http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
            }
            code = http.POST(body);
            resp = "";
            if (code > 0) {
                resp = http.getString();
            }
            http.end();
        }
    }
    
    if (code >= 200 && code < 300) {
        logMsg("[HEARTBEAT] OK");
        appendMonitorLog("[HEARTBEAT] OK");
    } else {
        logMsgInt("[HEARTBEAT] Failed, code", code);
        appendMonitorLogInt("[HEARTBEAT] Failed", code);
        appendErrorLogInt("[HEARTBEAT] HTTP error", code);
    }

    if (HEARTBEAT_DEBUG) {
        appendMonitorLogInt("[HEARTBEAT][DBG] HTTP code", code);
        if (resp.length() > 0) {
            String r = resp;
            if (r.length() > 900) r = r.substring(0, 900);
            appendMonitorLog("[HEARTBEAT][DBG] Response (trunc):");
            appendMonitorLog(r.c_str());
            logMsg("[HEARTBEAT][DBG] Response (trunc):");
            logMsg(r.c_str());
        } else {
            appendMonitorLog("[HEARTBEAT][DBG] Empty response body");
        }
    }
    
    // Always close connections fully
    http.end();
    clientSecure.stop();
    client.stop();
    delay(50);  // Let connection fully close
    
    httpsBusy = false;

    // Apply backend SIM status (source of truth) from response.sims[]
    if (resp.length() > 0) {
        const char* s = resp.c_str();
        const char* simsKey = strstr(s, "\"sims\"");
        if (simsKey) {
            const char* p = strchr(simsKey, '[');
            if (p) {
                p++;
                while (*p && *p != ']') {
                    const char* objStart = strchr(p, '{');
                    if (!objStart) break;
                    const char* objEnd = strchr(objStart, '}');
                    if (!objEnd) break;

                    // Extract slot
                    int slot = 0;
                    const char* slotKey = strstr(objStart, "\"slot\"");
                    if (slotKey && slotKey < objEnd) {
                        const char* colon = strchr(slotKey, ':');
                        if (colon && colon < objEnd) {
                            slot = atoi(colon + 1);
                        }
                    }

                    // Extract status value
                    // Expected: "status":"ACTIVE" (or IN_USE / DISABLED)
                    char status[16];
                    status[0] = '\0';
                    const char* stKey = strstr(objStart, "\"status\":\"");
                    if (stKey && stKey < objEnd) {
                        const char* v = stKey + strlen("\"status\":\"");
                        const char* endQuote = strchr(v, '"');
                        if (endQuote && endQuote < objEnd) {
                            int len = (int)(endQuote - v);
                            if (len > 0 && len < (int)sizeof(status)) {
                                strncpy(status, v, (size_t)len);
                                status[len] = '\0';
                            }
                        }
                    }

                    if (slot >= 0 && slot < SIM_COUNT && status[0] != '\0') {
                        bool shouldEnable = (strcmp(status, "ACTIVE") == 0 || strcmp(status, "IN_USE") == 0);

                        if (HEARTBEAT_DEBUG) {
                            char dbgLine[96];
                            snprintf(dbgLine, sizeof(dbgLine), "[HEARTBEAT][DBG] slot=%d status=%s => enable=%d", slot, status, shouldEnable ? 1 : 0);
                            appendMonitorLog(dbgLine);
                            logMsg(dbgLine);
                        }

                        simStates[slot].enabled = shouldEnable;
                        if (!shouldEnable) {
                            appendMonitorLogVal("[HEARTBEAT] Backend disabled SIM", String(slot).c_str());
                            // Do NOT clear state - keep number so SIM can be re-enabled and resent in next heartbeat
                            // simStates[idx].responsive remains true so it will be included in next heartbeat
                        }
                    }

                    p = objEnd + 1;
                }
            }
        }

        // Parse activeSessions from response
        activeSessionCount = 0;
        const char* sessionsKey = strstr(s, "\"activeSessions\"");
        if (sessionsKey) {
            const char* p2 = strchr(sessionsKey, '[');
            if (p2) {
                p2++;
                while (*p2 && *p2 != ']' && activeSessionCount < 8) {
                    const char* objStart = strchr(p2, '{');
                    if (!objStart) break;
                    const char* objEnd = strchr(objStart, '}');
                    if (!objEnd) break;

                    ActiveSession* sess = &activeSessions[activeSessionCount];
                    memset(sess, 0, sizeof(ActiveSession));

                    // Extract sessionId
                    const char* idKey = strstr(objStart, "\"sessionId\"");
                    if (idKey && idKey < objEnd) {
                        const char* colon = strchr(idKey, ':');
                        if (colon && colon < objEnd) {
                            const char* vStart = strchr(colon, '"');
                            if (vStart && vStart < objEnd) {
                                vStart++;
                                const char* vEnd = strchr(vStart, '"');
                                if (vEnd && vEnd < objEnd) {
                                    int len = (int)(vEnd - vStart);
                                    if (len > 0 && len < 64) {
                                        strncpy(sess->sessionId, vStart, len);
                                    }
                                }
                            }
                        }
                    }

                    // Extract simNumber
                    const char* numKey = strstr(objStart, "\"simNumber\"");
                    if (numKey && numKey < objEnd) {
                        const char* colon = strchr(numKey, ':');
                        if (colon && colon < objEnd) {
                            const char* vStart = strchr(colon, '"');
                            if (vStart && vStart < objEnd) {
                                vStart++;
                                const char* vEnd = strchr(vStart, '"');
                                if (vEnd && vEnd < objEnd) {
                                    int len = (int)(vEnd - vStart);
                                    if (len > 0 && len < PHONE_BUFFER_SIZE) {
                                        strncpy(sess->simNumber, vStart, len);
                                    }
                                }
                            }
                        }
                    }

                    // Extract appName
                    const char* appKey = strstr(objStart, "\"appName\"");
                    if (appKey && appKey < objEnd) {
                        const char* colon = strchr(appKey, ':');
                        if (colon && colon < objEnd) {
                            const char* vStart = strchr(colon, '"');
                            if (vStart && vStart < objEnd) {
                                vStart++;
                                const char* vEnd = strchr(vStart, '"');
                                if (vEnd && vEnd < objEnd) {
                                    int len = (int)(vEnd - vStart);
                                    if (len > 0 && len < 32) {
                                        strncpy(sess->appName, vStart, len);
                                    }
                                }
                            }
                        }
                    }

                    // Extract slot
                    const char* slotKey = strstr(objStart, "\"slot\"");
                    if (slotKey && slotKey < objEnd) {
                        const char* colon = strchr(slotKey, ':');
                        if (colon && colon < objEnd) {
                            sess->slot = atoi(colon + 1);
                        }
                    }

                    // Extract messageCount
                    const char* msgKey = strstr(objStart, "\"messageCount\"");
                    if (msgKey && msgKey < objEnd) {
                        const char* colon = strchr(msgKey, ':');
                        if (colon && colon < objEnd) {
                            sess->messageCount = atoi(colon + 1);
                        }
                    }

                    // Extract expiresAt (ISO format: 2026-05-07T16:30:00.000Z or +00:00)
                    const char* expKey = strstr(objStart, "\"expiresAt\"");
                    if (expKey && expKey < objEnd) {
                        const char* colon = strchr(expKey, ':');
                        if (colon && colon < objEnd) {
                            const char* vStart = strchr(colon, '"');
                            if (vStart && vStart < objEnd) {
                                vStart++;
                                const char* vEnd = strchr(vStart, '"');
                                if (vEnd && vEnd < objEnd) {
                                    // Parse ISO timestamp: YYYY-MM-DDTHH:MM:SS
                                    int year, month, day, hour, min, sec;
                                    if (sscanf(vStart, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) >= 6) {
                                        // Manual UTC to Unix timestamp conversion
                                        // Days per month (non-leap year)
                                        static const int daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};

                                        // Calculate days since 1970-01-01
                                        long days = 0;

                                        // Add days for complete years
                                        for (int y = 1970; y < year; y++) {
                                            days += 365;
                                            // Leap year check
                                            if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
                                                days += 1;
                                            }
                                        }

                                        // Add days for complete months in current year
                                        for (int m = 1; m < month; m++) {
                                            days += daysInMonth[m - 1];
                                            // February in leap year
                                            if (m == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
                                                days += 1;
                                            }
                                        }

                                        // Add days in current month
                                        days += day - 1;

                                        // Convert to Unix timestamp (seconds)
                                        time_t ts = days * 86400L + hour * 3600L + min * 60L + sec;
                                        sess->expiresAtMs = ts * 1000L;
                                    }
                                }
                            }
                        }
                    }

                    activeSessionCount++;
                    p2 = objEnd + 1;
                }
            }
        }
    }

    if (code > 0 && code < 400) {
        deviceRegistered = true;
        logMsgInt("[HEARTBEAT] OK, code", code);
    } else {
        logMsgInt("[HEARTBEAT] Failed, code", code);
    }
}

// -----------------------------------------------------------------------------
// Battery Info
// -----------------------------------------------------------------------------

void updateBatteryInfo() {
    if (isSimBusy()) return;
    
    setSimBusy(true);
    selectSIM(activeSim);
    getBatteryInfo(&batteryPercent, &batteryMv);
    setSimBusy(false);
}

// -----------------------------------------------------------------------------
// SIM Watchdog - Detect and recover unresponsive SIMs
// -----------------------------------------------------------------------------

void checkAndRecoverUnresponsiveSims() {
    if (isSimBusy()) return;
    
    unsigned long now = millis();
    
    for (int i = 0; i < SIM_COUNT; i++) {
        // Skip disabled SIMs
        if (!simStates[i].enabled) continue;
        
        // For unresponsive SIMs, retry every 5 minutes
        if (!simStates[i].responsive) {
            // Check if we should retry this unresponsive SIM
            static unsigned long lastUnresponsiveRetry[SIM_COUNT] = {0};
            if (now - lastUnresponsiveRetry[i] < 300000) {  // 5 minutes
                continue;
            }
            lastUnresponsiveRetry[i] = now;
            
            // Try to recover unresponsive SIM
            appendMonitorLogInt("[WATCHDOG] Retrying unresponsive SIM", i + 1);
            logMsgInt("[WATCHDOG] Retrying unresponsive SIM", i + 1);
            
            setSimBusy(true);
            selectSIM(i);
            delay(500);
            
            // Try aggressive recovery
            initMux();
            delay(100);
            flushSimInput();
            delay(100);
            selectSIM(i);
            delay(500);
            
            bool recovered = false;
            for (int retry = 0; retry < 5 && !recovered; retry++) {
                flushSimInput();
                sendATCapture("AT", 2000);
                if (strstr(getSimBuffer(), "OK") != NULL) {
                    recovered = true;
                } else {
                    selectSIM(i);
                    delay(300);
                }
            }
            
            if (recovered) {
                // Full re-initialization
                sendATCapture("AT", 1000);
                sendATCapture("AT+CPIN?", 2000);
                sendATCapture("AT+CMGF=1", 800);
                sendATCapture("AT+CSCS=\"GSM\"", 800);
                sendATCapture("AT+CPMS=\"SM\",\"SM\",\"SM\"", 1000);
                sendATCapture("AT+CNMI=2,1,0,0,0", 800);
                
                sendATCapture("AT+CSQ", 2000);
                charBufSet(simStates[i].csq, sizeof(simStates[i].csq), getSimBuffer());
                simStates[i].signalStrength = extractSignalQuality(simStates[i].csq);
                
                simStates[i].consecutiveErrors = 0;
                simStates[i].errorCount = 0;
                simStates[i].lastSuccessfulPoll = millis();
                simStates[i].responsive = true;
                simStates[i].basicInitDone = true;
                
                appendMonitorLogInt("[WATCHDOG] Unresponsive SIM recovered", i + 1);
                logMsgInt("[WATCHDOG] Unresponsive SIM recovered", i + 1);
            }
            
            setSimBusy(false);
            delay(100);
            continue;
        }
        
        // Check if SIM has too many consecutive errors or hasn't responded in a while
        bool needsRecovery = false;
        
        // Condition 1: Too many consecutive errors
        if (simStates[i].consecutiveErrors >= SIM_CONSECUTIVE_ERROR_THRESHOLD) {
            needsRecovery = true;
            appendMonitorLogInt("[WATCHDOG] SIM has consecutive errors", i + 1);
            appendErrorLogInt("[WATCHDOG] SIM consecutive errors", i + 1);
        }
        
        // Condition 2: No successful poll for too long (watchdog timeout)
        if (simStates[i].lastSuccessfulPoll > 0 && 
            (now - simStates[i].lastSuccessfulPoll) > SIM_WATCHDOG_TIMEOUT_MS) {
            needsRecovery = true;
            appendMonitorLogInt("[WATCHDOG] SIM watchdog timeout", i + 1);
            appendErrorLogInt("[WATCHDOG] SIM watchdog timeout", i + 1);
        }
        
        if (!needsRecovery) continue;
        
        // Attempt recovery
        appendMonitorLogInt("[WATCHDOG] Attempting recovery for SIM", i + 1);
        logMsgInt("[WATCHDOG] Recovering SIM", i + 1);
        
        setSimBusy(true);
        selectSIM(i);
        delay(MUX_SETTLE_MS);
        
        // Try to ping the SIM multiple times
        bool recovered = false;
        for (int retry = 0; retry < 3 && !recovered; retry++) {
            sendATCapture("AT", 1000);
            if (strstr(getSimBuffer(), "OK") != NULL) {
                recovered = true;
            } else {
                delay(500);
            }
        }
        
        if (recovered) {
            // Re-initialize the SIM
            sendATCapture("AT+CPIN?", 2000);
            sendATCapture("AT+CMGF=1", 800);
            sendATCapture("AT+CSCS=\"GSM\"", 800);
            
            // Update signal and network info
            sendATCapture("AT+CSQ", 2000);
            charBufSet(simStates[i].csq, sizeof(simStates[i].csq), getSimBuffer());
            simStates[i].signalStrength = extractSignalQuality(simStates[i].csq);
            
            // Reset error counters
            simStates[i].consecutiveErrors = 0;
            simStates[i].errorCount = 0;
            simStates[i].lastSuccessfulPoll = now;
            
            appendMonitorLogInt("[WATCHDOG] SIM recovered (soft)", i + 1);
            logMsgInt("[WATCHDOG] SIM recovered successfully", i + 1);
        } else {
            // Software recovery failed - try aggressive ESP32-side recovery
            appendMonitorLogInt("[WATCHDOG] Soft recovery failed, trying aggressive recovery", i + 1);
            logMsgInt("[WATCHDOG] Aggressive recovery for SIM", i + 1);
            
            // 1. Re-initialize MUX (toggle all control pins)
            initMux();
            delay(100);
            
            // 2. Flush serial buffer thoroughly
            flushSimInput();
            delay(100);
            
            // 3. Re-select the SIM with extra settling time
            selectSIM(i);
            delay(500);  // Extra settling time
            
            // 4. Try multiple AT commands with longer timeouts
            bool recoveredAggressive = false;
            for (int retry = 0; retry < 5 && !recoveredAggressive; retry++) {
                // Longer timeout and multiple flushes
                flushSimInput();
                sendATCapture("AT", 2000);  // 2 second timeout
                if (strstr(getSimBuffer(), "OK") != NULL) {
                    recoveredAggressive = true;
                } else {
                    // Re-select and wait longer
                    selectSIM(i);
                    delay(300);
                }
            }
            
            if (recoveredAggressive) {
                // Full re-initialization
                sendATCapture("AT", 1000);
                sendATCapture("AT+CPIN?", 2000);
                sendATCapture("AT+CMGF=1", 800);
                sendATCapture("AT+CSCS=\"GSM\"", 800);
                sendATCapture("AT+CPMS=\"SM\",\"SM\",\"SM\"", 1000);
                sendATCapture("AT+CNMI=2,1,0,0,0", 800);
                
                // Update signal
                sendATCapture("AT+CSQ", 2000);
                charBufSet(simStates[i].csq, sizeof(simStates[i].csq), getSimBuffer());
                simStates[i].signalStrength = extractSignalQuality(simStates[i].csq);
                
                // Reset error counters
                simStates[i].consecutiveErrors = 0;
                simStates[i].errorCount = 0;
                simStates[i].lastSuccessfulPoll = millis();
                simStates[i].responsive = true;
                simStates[i].basicInitDone = true;
                
                appendMonitorLogInt("[WATCHDOG] SIM recovered (aggressive)", i + 1);
                logMsgInt("[WATCHDOG] SIM recovered after aggressive recovery", i + 1);
            } else {
                // Mark as unresponsive - will be retried next watchdog cycle
                simStates[i].responsive = false;
                appendMonitorLogInt("[WATCHDOG] SIM marked unresponsive", i + 1);
                logMsgInt("[WATCHDOG] Failed to recover SIM", i + 1);
            }
        }
        
        setSimBusy(false);
        delay(100);  // Small delay between SIMs
    }
}

// Update signal strength for a single SIM (called periodically)
void updateSimSignalStrength(int simIdx) {
    if (simIdx < 0 || simIdx >= SIM_COUNT) return;
    if (!simStates[simIdx].enabled) return;
    if (!simStates[simIdx].responsive) return;
    if (isSimBusy()) return;
    
    setSimBusy(true);
    selectSIM(simIdx);
    delay(100);  // Settling time
    
    // Get signal strength
    sendATCapture("AT+CSQ", 1000);
    charBufSet(simStates[simIdx].csq, sizeof(simStates[simIdx].csq), getSimBuffer());
    simStates[simIdx].signalStrength = extractSignalQuality(simStates[simIdx].csq);
    
    // Get battery info from SIM800L
    sendATCapture("AT+CBC", 1000);
    // Parse: +CBC: <bcs>,<bcl>,<v>
    const char* p = strstr(getSimBuffer(), "+CBC:");
    if (p) {
        p += 5;
        while (*p == ' ') p++;
        const char* comma1 = strchr(p, ',');
        if (comma1) {
            const char* comma2 = strchr(comma1 + 1, ',');
            if (comma2) {
                simStates[simIdx].batteryPercent = atoi(comma1 + 1);
                simStates[simIdx].batteryMv = atoi(comma2 + 1);
            }
        }
    }
    
    setSimBusy(false);
}

// -----------------------------------------------------------------------------
// Monitor Log Implementation
// -----------------------------------------------------------------------------

void appendMonitorLog(const char* msg) {
    char stamped[192];
    char ts[32];
    
    if (getLocalTimeStamp(ts, sizeof(ts))) {
        snprintf(stamped, sizeof(stamped), "%s %s", ts, msg);
    } else {
        getFallbackTimeStamp(ts, sizeof(ts));
        snprintf(stamped, sizeof(stamped), "%s %s", ts, msg);
    }
    
    if (monitorLogCount < MONITOR_LOG_MAX_SIZE) {
        charBufSet(monitorLog[monitorLogCount], sizeof(monitorLog[0]), stamped);
        monitorLogCount++;
    } else {
        // Shift log
        for (int i = 1; i < MONITOR_LOG_MAX_SIZE; i++) {
            strcpy(monitorLog[i - 1], monitorLog[i]);
        }
        charBufSet(monitorLog[MONITOR_LOG_MAX_SIZE - 1], sizeof(monitorLog[0]), stamped);
    }
}

void appendMonitorLogInt(const char* msg, int val) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: %d", msg, val);
    appendMonitorLog(buf);
}

void appendMonitorLogVal(const char* msg, const char* val) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s: %s", msg, val);
    appendMonitorLog(buf);
}

void getMonitorLogText(char* buf, size_t bufSize) {
    if (!buf || bufSize < 1) return;
    
    size_t pos = 0;
    for (int i = 0; i < monitorLogCount && pos < bufSize - 2; i++) {
        size_t len = strlen(monitorLog[i]);
        if (pos + len + 2 >= bufSize) break;
        
        strcpy(buf + pos, monitorLog[i]);
        pos += len;
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
}

void clearMonitorLog() {
    monitorLogCount = 0;
}

// -----------------------------------------------------------------------------
// Error Log Implementation (persistent, no truncation)
// -----------------------------------------------------------------------------

void appendErrorLog(const char* msg) {
    char stamped[ERROR_LOG_LINE_SIZE];
    char ts[32];
    
    if (getLocalTimeStamp(ts, sizeof(ts))) {
        snprintf(stamped, sizeof(stamped), "%s %s", ts, msg);
    } else {
        getFallbackTimeStamp(ts, sizeof(ts));
        snprintf(stamped, sizeof(stamped), "%s %s", ts, msg);
    }
    
    // Add to RAM buffer
    if (errorLogCount < ERROR_LOG_MAX_SIZE) {
        charBufSet(errorLog[errorLogCount], sizeof(errorLog[0]), stamped);
        errorLogCount++;
    } else {
        // Shift log (FIFO)
        for (int i = 1; i < ERROR_LOG_MAX_SIZE; i++) {
            strcpy(errorLog[i - 1], errorLog[i]);
        }
        charBufSet(errorLog[ERROR_LOG_MAX_SIZE - 1], sizeof(errorLog[0]), stamped);
    }
    
    // Also append to persistent file
    appendErrorToFile(ts, msg);
}

void appendErrorLogInt(const char* msg, int val) {
    char buf[ERROR_LOG_LINE_SIZE - 32];
    snprintf(buf, sizeof(buf), "%s: %d", msg, val);
    appendErrorLog(buf);
}

void appendErrorLogVal(const char* msg, const char* val) {
    char buf[ERROR_LOG_LINE_SIZE - 32];
    snprintf(buf, sizeof(buf), "%s: %s", msg, val);
    appendErrorLog(buf);
}

void getErrorLogText(char* buf, size_t bufSize) {
    if (!buf || bufSize < 1) return;
    
    size_t pos = 0;
    for (int i = 0; i < errorLogCount && pos < bufSize - 2; i++) {
        size_t len = strlen(errorLog[i]);
        if (pos + len + 2 >= bufSize) break;
        
        strcpy(buf + pos, errorLog[i]);
        pos += len;
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
}

void clearErrorLog() {
    errorLogCount = 0;
    // Also delete persistent file
    if (LittleFS.exists("/errors.log")) {
        LittleFS.remove("/errors.log");
    }
}

// -----------------------------------------------------------------------------
// Logger Implementation
// -----------------------------------------------------------------------------

bool getLocalTimeStamp(char* buf, size_t bufSize) {
    if (!buf || bufSize < 20) return false;
    
    if (!ntpConfigured) {
        return false;
    }
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return false;
    }
    
    strftime(buf, bufSize, "%Y-%m-%d %H:%M:%S", &timeinfo);
    return true;
}

void getFallbackTimeStamp(char* buf, size_t bufSize) {
    if (!buf || bufSize < 16) return;
    
    unsigned long ms = millis();
    unsigned long sec = ms / 1000;
    unsigned long rem = ms % 1000;
    
    snprintf(buf, bufSize, "%lu.%03lus", sec, rem);
}

void ensureNtpConfigured() {
    if (ntpConfigured) return;
    
    if (WiFi.isConnected()) {
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        ntpConfigured = true;
    }
}
