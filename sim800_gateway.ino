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

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Global State Definitions (declared in config.h)
// -----------------------------------------------------------------------------

SimState simStates[SIM_COUNT];
PendingSms pendingSmsQueue[MAX_PENDING_SMS];
CallLogItem callLog[MAX_CALL_LOG];
int pendingSmsCount = 0;
int callLogCount = 0;
int activeSim = 0;
int currentMuxSim = 0;
volatile bool simBusy = false;
volatile bool httpsBusy = false;  // Prevent concurrent HTTPS
bool smsPollingPaused = false;
bool deviceRegistered = false;
bool simRegistered = false;

// Monitor log
char monitorLog[MONITOR_LOG_MAX_SIZE][160];
int monitorLogCount = 0;

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
        charBufClear(simStates[i].number, sizeof(simStates[i].number));
        charBufClear(simStates[i].creg, sizeof(simStates[i].creg));
        charBufClear(simStates[i].cops, sizeof(simStates[i].cops));
        charBufClear(simStates[i].csq, sizeof(simStates[i].csq));
    }
    
    // Load settings
    loadSettings();
    appendMonitorLog("[BOOT] Settings loaded");

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
    if (now - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs = now;
        performHeartbeat();
    }
    
    // Battery info update
    static unsigned long lastBatteryMs = 0;
    if (now - lastBatteryMs > BATTERY_INFO_INTERVAL_MS) {
        lastBatteryMs = now;
        updateBatteryInfo();
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

    // Backend expects { device_id, sims:[{number,slot,status}] }
    static char body[2048];
    size_t pos = 0;
    pos += snprintf(body + pos, sizeof(body) - pos,
        "{\"device_id\":\"%s\",\"sims\":[",
        agentDeviceId
    );

    bool first = true;
    for (int i = 0; i < SIM_COUNT && pos < sizeof(body) - 200; i++) {
        if (!simStates[i].enabled) continue;
        if (!simStates[i].responsive) continue;
        if (charBufIsEmpty(simStates[i].number)) continue;

        if (!first) pos += snprintf(body + pos, sizeof(body) - pos, ",");
        first = false;

        // status: ACTIVE when enabled, DISABLED otherwise (we only send enabled sims)
        char numEsc[64];
        jsonEscape(simStates[i].number, numEsc, sizeof(numEsc));
        pos += snprintf(body + pos, sizeof(body) - pos,
            "{\"number\":%s,\"slot\":%d,\"status\":\"ACTIVE\"}",
            numEsc,
            i + 1
        );
    }

    pos += snprintf(body + pos, sizeof(body) - pos, "]}");

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

                    if (slot >= 1 && slot <= SIM_COUNT && status[0] != '\0') {
                        int idx = slot - 1;
                        bool shouldEnable = (strcmp(status, "ACTIVE") == 0 || strcmp(status, "IN_USE") == 0);
                        simStates[idx].enabled = shouldEnable;
                        if (!shouldEnable) {
                            appendMonitorLogVal("[HEARTBEAT] Backend disabled SIM", String(slot).c_str());
                            simStates[idx].responsive = false;
                            simStates[idx].registered = false;
                            simStates[idx].backendRegistered = false;
                            simStates[idx].basicInitDone = false;
                            charBufClear(simStates[idx].number, sizeof(simStates[idx].number));
                            charBufClear(simStates[idx].creg, sizeof(simStates[idx].creg));
                            charBufClear(simStates[idx].cops, sizeof(simStates[idx].cops));
                            charBufClear(simStates[idx].csq, sizeof(simStates[idx].csq));
                        }
                    }

                    p = objEnd + 1;
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
