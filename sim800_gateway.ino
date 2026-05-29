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
#include "calls.h"
#include "ussd.h"
#include "maintenance.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_err.h>
#include <stdio.h>
#include <time.h>

extern "C" {
#include "esp_littlefs.h"
}

#ifndef ESP_PARTITION_SUBTYPE_DATA_LITTLEFS
#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS ((esp_partition_subtype_t)0x83)
#endif

#define LFS_MOUNT_PATH     "/littlefs"
#define LFS_MESSAGES_PATH  LFS_MOUNT_PATH "/messages.log"
#define LFS_ERRORS_PATH    LFS_MOUNT_PATH "/errors.log"

// Bump when LittleFS init logic changes (forces one raw erase on next boot).
static const uint32_t LFS_STORE_MAGIC = 4;

static bool g_littlefsReady = false;

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
unsigned long wifiUserSetupUntilMs = 0;  // Pause STA reconnect while user scans/saves WiFi
bool modemGatewayRunning = false;
volatile bool modemStartRequested = false;

bool isModemGatewayRunning() {
    return modemGatewayRunning;
}

bool isModemGatewayStartQueued() {
    return modemStartRequested;
}

void requestModemGatewayStart() {
    if (modemGatewayRunning || modemStartRequested) {
        return;
    }
    modemStartRequested = true;
    appendMonitorLog("[MODEM] Start queued (web UI)");
    logMsg("[MODEM] Start queued");
}

void stopModemGateway() {
    modemStartRequested = false;
    if (!modemGatewayRunning) {
        return;
    }
    modemGatewayRunning = false;
    resetAgentInventoryHeartbeat();
    pauseSmsPolling(60000);
    appendMonitorLog("[MODEM] Stopped (web UI)");
    logMsg("[MODEM] Stopped");
}

void modemGatewayTick() {
    if (!modemStartRequested || modemGatewayRunning) {
        return;
    }
    if (httpsBusy || isSimBusy() || otaInProgress) {
        return;
    }
    if (millis() < wifiUserSetupUntilMs) {
        return;
    }

    modemStartRequested = false;
    appendMonitorLog("[MODEM] SIM init starting...");
    logMsg("[MODEM] SIM init starting...");
    checkAllSIMsOnStartup();
    resetAgentInventoryHeartbeat();
    modemGatewayRunning = true;
    modemGatewayStableUntilMs = millis() + MODEM_GATEWAY_STABLE_MS;
    deferHeartbeat(MODEM_GATEWAY_STABLE_MS);
    appendMonitorLog("[MODEM] Running — SMS poll active");
    logMsg("[MODEM] Running");
    logMsg("[HEARTBEAT] Deferred until modem/SIM stable");
}

void armWifiUserSetupMs(unsigned long durationMs) {
    const unsigned long until = millis() + durationMs;
    if (until > wifiUserSetupUntilMs) {
        wifiUserSetupUntilMs = until;
    }
}

void ensureGatewaySoftAp() {
    wifi_mode_t mode = WiFi.getMode();
    if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
        WiFi.mode(WIFI_AP_STA);
    }
    if (WiFi.softAPIP()[0] == 0) {
        char apSsid[32];
        snprintf(apSsid, sizeof(apSsid), AP_SSID_PREFIX "%06X", (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));
        WiFi.softAP(apSsid, AP_PASSWORD, AP_CHANNEL);
        logMsg2Val("[WIFI] AP (re)started", apSsid, "IP", WiFi.softAPIP().toString().c_str());
        appendMonitorLogVal("[WIFI] AP up", WiFi.softAPIP().toString().c_str());
    }
}

static void wifiWaitStaSettle(uint32_t maxMs) {
    const unsigned long t0 = millis();
    while (millis() - t0 < maxMs) {
        handleWebRequests();
        const wl_status_t st = WiFi.status();
        if (st == WL_DISCONNECTED || st == WL_CONNECTION_LOST ||
            st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
            st == WL_IDLE_STATUS) {
            return;
        }
        delay(20);
    }
}

/** Block until STA linked or timeout; keeps AP web UI responsive. */
static bool wifiWaitStaConnected(uint32_t maxMs) {
    const unsigned long t0 = millis();
    while (millis() - t0 < maxMs) {
        handleWebRequests();
        yield();
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0) {
            delay(150);
            wifiFixStaNetworkIfNeeded();
            return WiFi.isConnected() && wifiStaNetworkLooksValid();
        }
        delay(50);
    }
    return WiFi.isConnected() && WiFi.localIP()[0] != 0;
}

static bool wifiConnectStaWithRetries(int maxAttempts, uint32_t attemptTimeoutMs, bool bootLog) {
    if (charBufIsEmpty(wifiSsid)) {
        return false;
    }

    WiFi.setAutoReconnect(false);
    WiFi.setSleep(WIFI_PS_NONE);
    ensureGatewaySoftAp();

    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        if (attempt > 1) {
            WiFi.disconnect(true, false);
            wifiWaitStaSettle(1200);
            ensureGatewaySoftAp();
            if (bootLog) {
                logMsgInt("[WIFI] Boot connect retry", attempt);
                appendMonitorLogInt("[WIFI] Boot connect retry", attempt);
            }
        } else if (bootLog) {
            logMsg2Val("[WIFI] Connecting to", wifiSsid, "", "");
            appendMonitorLogVal("[WIFI] Connecting to", wifiSsid);
        }

        WiFi.begin(wifiSsid, wifiPassword);

        if (wifiWaitStaConnected(attemptTimeoutMs)) {
            wifiLogStaNetworkDetails();
            if (bootLog) {
                logMsgInt("[WIFI] Connected on try", attempt);
                appendMonitorLogInt("[WIFI] Connected on try", attempt);
            }
            return true;
        }

        if (bootLog) {
            char detail[48];
            snprintf(detail, sizeof(detail), "status=%d", (int)WiFi.status());
            logMsg2Val("[WIFI] Connect attempt failed", detail, "", "");
        }
    }

    if (bootLog) {
        logMsg("[WIFI] Boot connect failed — use AP web UI");
        appendMonitorLog("[WIFI] Boot connect failed");
    }
    return false;
}

void wifiPrepareForUserConfig() {
    armWifiUserSetupMs(WIFI_USER_SETUP_ARM_MS);
    WiFi.setAutoReconnect(false);
    ensureGatewaySoftAp();
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true, false);
        wifiWaitStaSettle(800);
    }
    WiFi.mode(WIFI_AP_STA);
    ensureGatewaySoftAp();
}

void wifiPrepareForScan() {
    armWifiUserSetupMs(WIFI_USER_SETUP_ARM_MS);
    WiFi.setAutoReconnect(false);
    ensureGatewaySoftAp();
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        WiFi.disconnect(true, false);
        wifiWaitStaSettle(600);
    }
}

bool wifiStaNetworkLooksValid() {
    if (!WiFi.isConnected()) {
        return false;
    }
    const IPAddress ip = WiFi.localIP();
    const IPAddress gw = WiFi.gatewayIP();
    const IPAddress dns = WiFi.dnsIP();
    return ip[0] != 0 && gw[0] != 0 && dns[0] != 0;
}

void wifiLogStaNetworkDetails() {
    if (!WiFi.isConnected()) {
        return;
    }
    char line[96];
    snprintf(line, sizeof(line), "[WIFI] IP %s GW %s",
             WiFi.localIP().toString().c_str(),
             WiFi.gatewayIP().toString().c_str());
    logMsg(line);
    appendMonitorLog(line);
    snprintf(line, sizeof(line), "[WIFI] DNS %s MASK %s",
             WiFi.dnsIP().toString().c_str(),
             WiFi.subnetMask().toString().c_str());
    logMsg(line);
    appendMonitorLog(line);
}

bool wifiFixStaNetworkIfNeeded() {
    if (!WiFi.isConnected()) {
        return false;
    }

    IPAddress ip = WiFi.localIP();
    IPAddress gw = WiFi.gatewayIP();
    IPAddress mask = WiFi.subnetMask();
    IPAddress dns = WiFi.dnsIP();
    const IPAddress dns2(1, 1, 1, 1);

    if (ip[0] != 0 && gw[0] != 0 && dns[0] != 0) {
        return true;
    }

    if (mask[0] == 0) {
        mask = IPAddress(255, 255, 255, 0);
    }
    if (gw[0] == 0 && ip[0] != 0) {
        gw = IPAddress(ip[0], ip[1], ip[2], 1);
        appendMonitorLog("[WIFI] No gateway from DHCP — using x.x.x.1");
        logMsg("[WIFI] GW fallback x.x.x.1");
    }
    if (dns[0] == 0) {
        dns = IPAddress(8, 8, 8, 8);
        appendMonitorLog("[WIFI] No DNS from DHCP — using 8.8.8.8");
        logMsg("[WIFI] DNS fallback 8.8.8.8");
    }

    logMsg2Val("[WIFI] STA config fix", ip.toString().c_str(), "GW", gw.toString().c_str());
    const bool applied = WiFi.config(ip, gw, mask, dns, dns2);
    delay(150);
    wifiLogStaNetworkDetails();
    if (!wifiStaNetworkLooksValid()) {
        appendMonitorLog("[WIFI] STA has IP but no route/DNS — check router (guest/isolation?)");
        logMsg("[WIFI] STA route/DNS still invalid");
    }
    return applied && wifiStaNetworkLooksValid();
}

bool wifiStaBackgroundReconnectAllowed() {
    if (millis() < wifiUserSetupUntilMs) {
        return false;
    }
    if (httpsBusy || otaInProgress) {
        return false;
    }
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        return false;
    }
    // Do not stack reconnect on top of an in-progress association (causes "cannot set config")
    if (st != WL_DISCONNECTED && st != WL_CONNECTION_LOST &&
        st != WL_CONNECT_FAILED && st != WL_NO_SSID_AVAIL) {
        return false;
    }
    return !charBufIsEmpty(wifiSsid);
}
bool smsPollingPaused = false;
bool heartbeatPaused = false;
bool pendingHeartbeatFollowup = false;
static unsigned long heartbeatFinishedMs = 0;
static wifi_ps_type_t wifiPsBeforeHttps = WIFI_PS_NONE;
bool missedCallForwardEnabled = MISSED_CALL_FORWARD_DEFAULT;
int missedCallWatchSlot = -1;
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
unsigned long heartbeatNotBeforeMs = 0;
unsigned long lastHttpsEndMs = 0;
static bool agentInventoryHeartbeatDone = false;

void markHttpsSessionEnded() {
    lastHttpsEndMs = millis();
}
unsigned long modemGatewayStableUntilMs = 0;
unsigned long lastSmsPollMs = 0;
unsigned long smsPollingPauseUntil = 0;

// Agent config
char agentBaseUrl[128] = "";
char agentDeviceId[64] = "";
char agentBearerToken[AGENT_BEARER_TOKEN_SIZE] = "";
char agentRefreshToken[AGENT_REFRESH_TOKEN_SIZE] = "";
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

static const esp_partition_t* getLittleFsPartition() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, nullptr);
    if (part) {
        return part;
    }
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

static uint32_t getLittleFsPartitionBytes() {
    const esp_partition_t* part = getLittleFsPartition();
    return part ? part->size : 0;
}

static bool eraseLittleFsPartitionRaw(const esp_partition_t* part) {
    if (!part) {
        return false;
    }
    if (esp_littlefs_partition_mounted(part)) {
        esp_vfs_littlefs_unregister_partition(part);
    }
    const esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    Serial.printf("[STORE] Raw erase %u bytes: %s\n",
                  (unsigned)part->size, esp_err_to_name(err));
    return err == ESP_OK;
}

// Arduino LittleFS.begin() sets grow_on_mount=true and can report success when mount failed.
static bool mountLittleFSStorage(bool formatFirst) {
    const esp_partition_t* part = getLittleFsPartition();
    if (!part) {
        return false;
    }

    g_littlefsReady = false;
    if (esp_littlefs_partition_mounted(part)) {
        esp_vfs_littlefs_unregister_partition(part);
    }

    if (formatFirst) {
        eraseLittleFsPartitionRaw(part);
        const esp_err_t ferr = esp_littlefs_format_partition(part);
        Serial.printf("[STORE] format_partition: %s\n", esp_err_to_name(ferr));
        if (ferr != ESP_OK) {
            return false;
        }
    }

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = LFS_MOUNT_PATH;
    conf.partition = part;
    conf.partition_label = nullptr;
    conf.format_if_mount_failed = false;
    conf.read_only = false;
    conf.dont_mount = false;
    conf.grow_on_mount = false;

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    Serial.printf("[STORE] vfs register: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) {
        return false;
    }

    if (!esp_littlefs_partition_mounted(part)) {
        return false;
    }

    FILE* probe = fopen(LFS_MOUNT_PATH "/.ok", "w");
    if (!probe) {
        return false;
    }
    fclose(probe);
    remove(LFS_MOUNT_PATH "/.ok");
    g_littlefsReady = true;
    return true;
}

static void initLittleFS() {
    const esp_partition_t* part = getLittleFsPartition();
    const uint32_t partBytes = part ? part->size : 0;
    if (!part || partBytes == 0) {
        Serial.println("[STORE] No SPIFFS/LittleFS data partition");
        appendMonitorLog("[STORE] No data partition");
        return;
    }

    Preferences storeMeta;
    storeMeta.begin("store", false);
    const uint32_t lastBytes = storeMeta.getUInt("lfs_sz", 0);
    const uint32_t lastMagic = storeMeta.getUInt("lfs_magic", 0);
    const bool needPrep = (lastMagic != LFS_STORE_MAGIC) || (lastBytes != partBytes);
    storeMeta.end();

    if (needPrep) {
        Serial.printf("[STORE] Preparing flash storage (%u bytes partition)\n",
                      (unsigned)partBytes);
    }

    if (!mountLittleFSStorage(needPrep)) {
        Serial.println("[STORE] Mount failed, raw erase and format");
        if (!mountLittleFSStorage(true)) {
            Serial.println("[STORE] LittleFS mount failed");
            appendMonitorLog("[STORE] LittleFS mount failed");
            g_littlefsReady = false;
            return;
        }
    }

    Serial.println("[STORE] LittleFS mounted");
    appendMonitorLog("[STORE] LittleFS mounted");

    storeMeta.begin("store", false);
    storeMeta.putUInt("lfs_sz", partBytes);
    storeMeta.putUInt("lfs_magic", LFS_STORE_MAGIC);
    storeMeta.end();
}

static bool lfsFileExists(const char* path) {
    if (!g_littlefsReady) {
        return false;
    }
    FILE* f = fopen(path, "r");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

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
    if (!g_littlefsReady) {
        return;
    }
    // Cap stored message length to keep history size predictable
    char clipped[201];
    if (message && message[0]) {
        strncpy(clipped, message, 200);
        clipped[200] = '\0';
    } else {
        clipped[0] = '\0';
    }

    FILE* file = fopen(LFS_MESSAGES_PATH, "a");
    if (!file) {
        logMsg("[STORE] Failed to open messages.log");
        return;
    }
    fprintf(file, "%s|%d|%s|%s|%s\n", time, simSlot, number, sender, clipped);
    fclose(file);
}

// Read SMS messages from file (returns count, fills buffer)
int readSmsFromFile(char* buf, size_t bufSize, int maxMessages) {
    if (!g_littlefsReady || !lfsFileExists(LFS_MESSAGES_PATH)) {
        return 0;
    }

    FILE* file = fopen(LFS_MESSAGES_PATH, "r");
    if (!file) {
        return 0;
    }

    int count = 0;
    size_t pos = 0;
    buf[0] = '\0';

    fseek(file, 0, SEEK_END);
    const long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fileSize <= 0) {
        fclose(file);
        return 0;
    }

    if ((size_t)fileSize < bufSize) {
        while (pos < bufSize - 1) {
            const int ch = fgetc(file);
            if (ch == EOF) {
                break;
            }
            buf[pos++] = (char)ch;
        }
        buf[pos] = '\0';
        for (size_t i = 0; buf[i]; i++) {
            if (buf[i] == '\n') {
                count++;
            }
        }
    } else {
        int linesFound = 0;
        long readPos = fileSize;
        char chunk[128];

        while (readPos > 0 && linesFound < maxMessages) {
            const long chunkSize = (readPos > (long)sizeof(chunk)) ? (long)sizeof(chunk) : readPos;
            readPos -= chunkSize;
            fseek(file, readPos, SEEK_SET);
            const size_t got = fread(chunk, 1, (size_t)chunkSize, file);

            for (int i = (int)got - 1; i >= 0 && linesFound < maxMessages; i--) {
                if (chunk[i] == '\n') {
                    linesFound++;
                }
            }
        }

        fseek(file, readPos, SEEK_SET);
        while (pos < bufSize - 1) {
            const int ch = fgetc(file);
            if (ch == EOF) {
                break;
            }
            buf[pos++] = (char)ch;
        }
        buf[pos] = '\0';
        count = linesFound;
    }

    fclose(file);
    return count;
}

// Append error to persistent file
void appendErrorToFile(const char* time, const char* error) {
    if (!g_littlefsReady) {
        return;
    }
    FILE* file = fopen(LFS_ERRORS_PATH, "a");
    if (!file) {
        return;
    }
    fprintf(file, "%s|%s\n", time, error);
    fclose(file);
}

// Read errors from persistent file
int readErrorsFromFile(char* buf, size_t bufSize, int maxErrors) {
    if (!g_littlefsReady || !lfsFileExists(LFS_ERRORS_PATH)) {
        return 0;
    }

    FILE* file = fopen(LFS_ERRORS_PATH, "r");
    if (!file) {
        return 0;
    }

    int count = 0;
    size_t pos = 0;
    buf[0] = '\0';

    fseek(file, 0, SEEK_END);
    const long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fileSize <= 0) {
        fclose(file);
        return 0;
    }

    if ((size_t)fileSize < bufSize) {
        while (pos < bufSize - 1) {
            const int ch = fgetc(file);
            if (ch == EOF) {
                break;
            }
            buf[pos++] = (char)ch;
        }
        buf[pos] = '\0';
        for (size_t i = 0; buf[i]; i++) {
            if (buf[i] == '\n') {
                count++;
            }
        }
    } else {
        int linesFound = 0;
        long readPos = fileSize;
        char chunk[128];

        while (readPos > 0 && linesFound < maxErrors) {
            const long chunkSize = (readPos > (long)sizeof(chunk)) ? (long)sizeof(chunk) : readPos;
            readPos -= chunkSize;
            fseek(file, readPos, SEEK_SET);
            const size_t got = fread(chunk, 1, (size_t)chunkSize, file);

            for (int i = (int)got - 1; i >= 0 && linesFound < maxErrors; i--) {
                if (chunk[i] == '\n') {
                    linesFound++;
                }
            }
        }

        fseek(file, readPos, SEEK_SET);
        while (pos < bufSize - 1) {
            const int ch = fgetc(file);
            if (ch == EOF) {
                break;
            }
            buf[pos++] = (char)ch;
        }
        buf[pos] = '\0';
        count = linesFound;
    }

    fclose(file);
    return count;
}

bool webLittleFsReady() {
    return g_littlefsReady;
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
        simStates[i].userDisabled = false;
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
        simStates[i].ussdStatus = 0;
        simStates[i].ussdLastDurationSec = 0;
        simStates[i].ussdLastCheckMs = 0;
        charBufClear(simStates[i].ussdLastResult, sizeof(simStates[i].ussdLastResult));
    }
    
    // Load settings
    loadSettings();
#if OTA_ENABLED
    otaLoadUrlFromPreferences();
#endif
    appendMonitorLog("[BOOT] Settings loaded");
    
    initLittleFS();
    
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
    
    logMsg("========================================");
    logMsg("Setup complete!");
    logMsg("========================================");

    appendMonitorLog("[BOOT] Ready — press Run in web UI to init SIM800");
}

static void fetchHeartbeatFollowup();

void wifiPrepareForHttps() {
    wifiPsBeforeHttps = WiFi.getSleep();
    WiFi.setSleep(WIFI_PS_NONE);
}

static void wifiRestoreAfterHttps() {
    WiFi.setSleep(wifiPsBeforeHttps);
}

void deferHeartbeat(unsigned long delayMs) {
    const unsigned long now = millis();
    if (delayMs > heartbeatNotBeforeMs - now) {
        heartbeatNotBeforeMs = now + delayMs;
    }
}

static void logHeartbeatWaitThrottled(const char* reason) {
    static unsigned long lastLogMs = 0;
    const unsigned long now = millis();
    if (now - lastLogMs < 30000UL) {
        return;
    }
    lastLogMs = now;
    char buf[72];
    snprintf(buf, sizeof(buf), "[HEARTBEAT] Waiting: %s", reason);
    logMsg(buf);
    appendMonitorLog(buf);
}

void wifiRecoverAfterHttps() {
    wifiRestoreAfterHttps();
    ensureGatewaySoftAp();
    WiFi.setSleep(WIFI_PS_NONE);

    if (!WiFi.isConnected() && wifiSsid[0] != '\0') {
        logMsg("[WIFI] Recovering STA after HTTPS");
        appendMonitorLog("[WIFI] Recovering STA");
        WiFi.setAutoReconnect(false);
        WiFi.begin(wifiSsid, wifiPassword);
        for (int i = 0; i < 25; i++) {
            handleWebRequests();
            yield();
            if (WiFi.status() == WL_CONNECTED) {
                delay(150);
                wifiFixStaNetworkIfNeeded();
                break;
            }
            delay(100);
        }
    }

    if (WiFi.isConnected()) {
        wifiLogStaNetworkDetails();
    } else {
        logMsg("[WIFI] STA down — web UI on AP http://192.168.4.1");
        appendMonitorLog("[WIFI] STA down — use AP");
    }

    markHttpsSessionEnded();
}

bool ensureWifiForHttps() {
    if (WiFi.isConnected()) {
        return true;
    }
    if (!wifiStaBackgroundReconnectAllowed()) {
        return false;
    }
    logMsg("[WIFI] Reconnecting for HTTPS");
    appendMonitorLog("[WIFI] Reconnecting");
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.disconnect(true, false);
    wifiWaitStaSettle(2000);
    ensureGatewaySoftAp();
    WiFi.begin(wifiSsid, wifiPassword);
    for (int i = 0; i < 16; i++) {
        if (WiFi.status() == WL_CONNECTED) {
            delay(200);
            wifiFixStaNetworkIfNeeded();
            return WiFi.isConnected();
        }
        delay(250);
        yield();
        handleWebRequests();
    }
    return false;
}

// One blocking HTTPS job per loop() — all WiFi/TLS from loop() (not thread-safe on ESP32).
static bool scheduleBackgroundNet(unsigned long now) {
    if (otaInProgress || httpsBusy) {
        return false;
    }
    if (isSmsForwardPriorityActive()) {
        return false;
    }

#if HEARTBEAT_FOLLOWUP_ENABLED
    if (pendingHeartbeatFollowup && heartbeatFinishedMs > 0 &&
        (now - heartbeatFinishedMs) >= HEARTBEAT_FOLLOWUP_DELAY_MS) {
        pendingHeartbeatFollowup = false;
        logMsg("[HEARTBEAT] Followup starting");
        appendMonitorLog("[HEARTBEAT] Followup starting");
        fetchHeartbeatFollowup();
        pruneExpiredActiveSessions();
        maintenanceOnSessionsUpdated();
        if (!isSimBusy()) {
            flushPendingMissedCallsAfterHeartbeat();
        }
        return true;
    }
#endif

    if (millis() < heartbeatNotBeforeMs) {
        logHeartbeatWaitThrottled("cloud pause after SMS/modem");
        return false;
    }
    if (millis() < wifiUserSetupUntilMs) {
        return false;
    }
    if (millis() < modemGatewayStableUntilMs) {
        logHeartbeatWaitThrottled("modem stabilizing");
        return false;
    }
    if (isSimBusy()) {
        return false;
    }

    const int pendingSms = getPendingSmsCount();
    const bool needFullSync = !agentInventoryHeartbeatDone;
    if (needFullSync && pendingSms > 0) {
        logHeartbeatWaitThrottled("pending SMS (full sync)");
        return false;
    }

    if (!heartbeatPaused && !pendingHeartbeatFollowup && agentIsSignedIn() &&
        !charBufIsEmpty(agentBaseUrl) &&
        !isSimBusy() && ensureWifiForHttps() &&
        (now - lastHeartbeatMs) >= (unsigned long)HEARTBEAT_INTERVAL_MS) {
        pruneExpiredActiveSessions();
        performHeartbeat();
        heartbeatFinishedMs = millis();
        return true;
    }

    if (agentInventoryHeartbeatDone && agentIsSignedIn() &&
        !charBufIsEmpty(agentBaseUrl) &&
        (now - lastHeartbeatMs) < (unsigned long)HEARTBEAT_INTERVAL_MS) {
        const unsigned long nextPingMs = lastHeartbeatMs + (unsigned long)HEARTBEAT_INTERVAL_MS;
        const unsigned long remainSec = (nextPingMs - now) / 1000UL;
        static unsigned long lastNextPingLogMs = 0;
        if (remainSec > 5 && (now - lastNextPingLogMs) >= 30000UL) {
            lastNextPingLogMs = now;
            char buf[56];
            snprintf(buf, sizeof(buf), "[HEARTBEAT] Next ping in %lus", remainSec);
            logMsg(buf);
        }
    }

    return false;
}

// -----------------------------------------------------------------------------
// Main Loop
// -----------------------------------------------------------------------------

static void runBusyWatchdog() {
    static unsigned long simBusySince = 0;
    static unsigned long httpsBusySince = 0;

    if (isSimBusy()) {
        if (simBusySince == 0) {
            simBusySince = millis();
        } else if (millis() - simBusySince > 15000UL) {
            logMsg("[WATCHDOG] Force clear simBusy (15s)");
            appendErrorLog("[WATCHDOG] Force clear simBusy (15s)");
            setSimBusy(false);
            initMux(); // recover mux state if it got stuck mid-switch
            simBusySince = 0;
        }
    } else {
        simBusySince = 0;
    }

    if (httpsBusy) {
        if (httpsBusySince == 0) {
            httpsBusySince = millis();
        } else if (millis() - httpsBusySince > 90000UL) {
            logMsg("[WATCHDOG] Force clear httpsBusy");
            httpsBusy = false;
            httpsBusySince = 0;
            resumeSmsPolling();
        }
    } else {
        httpsBusySince = 0;
    }
}

static void logHeapIfLow() {
    static unsigned long lastHeapLogMs = 0;
    const unsigned long now = millis();
    if (now - lastHeapLogMs < 300000UL) {
        return;
    }
    lastHeapLogMs = now;
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t largest = ESP.getMaxAllocHeap();
    if (freeHeap < 25000 || largest < 12000) {
        char buf[64];
        snprintf(buf, sizeof(buf), "[SYS] Low heap free=%u largest=%u", (unsigned)freeHeap, (unsigned)largest);
        logMsg(buf);
        appendMonitorLog(buf);
    }
}

void loop() {
    // Handle web requests (must be called frequently)
    handleWebRequests();
    runBusyWatchdog();
    logHeapIfLow();

    // During OTA only service HTTP; skip SIM/heartbeat/maintenance so WiFi/CPU focus on download.
    if (otaInProgress) {
        static unsigned long lastOtaPauseLogMs = 0;
        const unsigned long nowMs = millis();
        if (nowMs - lastOtaPauseLogMs >= 30000UL) {
            lastOtaPauseLogMs = nowMs;
            logMsg("[OTA] SMS/heartbeat paused for firmware download");
        }
        yield();
        return;
    }

    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    static bool lastWifiConnected = true;
    static unsigned long lastReconnectTryMs = 0;
    static unsigned long reconnectCooldownUntilMs = 0;
    static uint8_t reconnectFailStreak = 0;

    if (wifiConnected != lastWifiConnected) {
        lastWifiConnected = wifiConnected;
        if (wifiConnected) {
            reconnectFailStreak = 0;
            reconnectCooldownUntilMs = 0;
            logMsg("[WIFI] Connected - background tasks resumed");
            appendMonitorLog("[WIFI] Connected - tasks resumed");
        } else {
            logMsg("[WIFI] Disconnected - background tasks paused");
            appendMonitorLog("[WIFI] Disconnected - tasks paused");
        }
    }

    // Pause background modem/backend work while WiFi is offline so AP setup stays responsive.
    // User can still use web UI to scan/connect WiFi.
    if (!wifiConnected) {
        ensureGatewaySoftAp();

        const unsigned long nowMs = millis();

        if (wifiStaBackgroundReconnectAllowed() &&
            nowMs >= reconnectCooldownUntilMs &&
            (nowMs - lastReconnectTryMs) > WIFI_RECONNECT_INTERVAL_MS) {
            lastReconnectTryMs = nowMs;
            WiFi.setAutoReconnect(false);
            WiFi.setSleep(WIFI_PS_NONE);
            ensureGatewaySoftAp();
            logMsg2Val("[WIFI] STA reconnect try", wifiSsid, "", "");
            appendMonitorLogVal("[WIFI] Reconnect try", wifiSsid);
            WiFi.begin(wifiSsid, wifiPassword);

            reconnectFailStreak++;
            if (reconnectFailStreak >= WIFI_RECONNECT_MAX_ATTEMPTS) {
                reconnectFailStreak = 0;
                reconnectCooldownUntilMs = nowMs + WIFI_RECONNECT_COOLDOWN_MS;
                logMsg("[WIFI] Reconnect paused 10 min — use AP web UI to fix WiFi");
                appendMonitorLog("[WIFI] Reconnect paused 10m");
            }
        }
        delay(10);
        return;
    }

    modemGatewayTick();

    if (modemGatewayRunning) {
        // USSD / refresh-all before SMS poll — avoids CMGL during *143#
        if (!httpsBusy && !isSimBusy()) {
            ussdTick();
        }

        const bool blockSmsPoll = ussdBlocksSmsPoll() || isWebRefreshAllInProgress();
        static unsigned long simBusySkipSinceMs = 0;
        static unsigned long lastSimBusyLogMs = 0;
        // Keep polling during HTTPS — modem UART is separate; stopping poll caused 30–40s gaps.
        if (!isSimBusy() && !smsPollingPaused && !isSmsPollingPaused() && !blockSmsPoll) {
            simBusySkipSinceMs = 0;
            pollSIMsForSMS();
        } else if (isSimBusy() && modemGatewayRunning) {
            const unsigned long nowMs = millis();
            if (simBusySkipSinceMs == 0) {
                simBusySkipSinceMs = nowMs;
            } else if ((nowMs - simBusySkipSinceMs) > 8000UL &&
                       (nowMs - lastSimBusyLogMs) > 15000UL) {
                lastSimBusyLogMs = nowMs;
                logMsg("[SMS] Poll waiting (modem busy)");
            }
        } else {
            simBusySkipSinceMs = 0;
        }

        if (missedCallForwardEnabled && !smsPollingPaused && !isSimBusy() && !blockSmsPoll &&
            !isSmsForwardPriorityActive()) {
            pollMissedCallsFast();
        }
    }

    unsigned long now = millis();
    const bool authReady = !charBufIsEmpty(agentBearerToken) && !charBufIsEmpty(agentRefreshToken);

    if (!cloudBackendDeferred() && authReady && !httpsBusy && getPendingSmsCount() > 0) {
        processPendingSmsQueueNow();
    }

    const bool netJobRan = (authReady && !isSmsForwardPriorityActive())
        ? scheduleBackgroundNet(now)
        : false;

    if (!cloudBackendDeferred() && authReady && !netJobRan && !httpsBusy && !isSimBusy()) {
        maintenanceTick(now);
    }
    
    if (modemGatewayRunning) {
        static unsigned long lastBatteryMs = 0;
        if (now - lastBatteryMs > BATTERY_INFO_INTERVAL_MS) {
            lastBatteryMs = now;
            updateBatteryInfo();
        }
        static unsigned long lastWatchdogMs = 0;
        if (now - lastWatchdogMs > 30000) {
            lastWatchdogMs = now;
            checkAndRecoverUnresponsiveSims();
        }

        static unsigned long lastSignalUpdateMs = 0;
        static int signalUpdateSimIdx = 0;
        if (now - lastSignalUpdateMs > 5000) {
            lastSignalUpdateMs = now;
            updateSimSignalStrength(signalUpdateSimIdx);
            signalUpdateSimIdx = (signalUpdateSimIdx + 1) % SIM_COUNT;
        }
    }
    
    // Small delay to prevent tight loops
    delay(10);
}

// -----------------------------------------------------------------------------
// WiFi Initialization
// -----------------------------------------------------------------------------

void initWiFi() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.setHostname(agentDeviceId[0] ? agentDeviceId : "sim800-gw");

    static bool wifiEventRegistered = false;
    if (!wifiEventRegistered) {
        wifiEventRegistered = true;
        WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
            (void)info;
            static unsigned long lastStaDiscLogMs = 0;
            if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
                appendMonitorLog("[WIFI] STA connected");
                logMsg("[WIFI] STA connected");
            } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
                wifiLogStaNetworkDetails();
                if (!wifiFixStaNetworkIfNeeded()) {
                    appendMonitorLog("[WIFI] WiFi linked but internet route may fail");
                    logMsg("[WIFI] Check GW/DNS on router");
                }
                logMsg2Val("[WIFI] STA IP", WiFi.localIP().toString().c_str(), "SSID", WiFi.SSID().c_str());
            } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
                WiFi.setAutoReconnect(false);
                ensureGatewaySoftAp();
                const unsigned long nowMs = millis();
                if (nowMs - lastStaDiscLogMs >= WIFI_STA_DISCONNECT_LOG_MS) {
                    lastStaDiscLogMs = nowMs;
                    appendMonitorLog("[WIFI] STA disconnected (AP still up)");
                    logMsg("[WIFI] STA disconnected (AP still up)");
                }
            }
        });
    }

    // Start in AP mode
    char apSsid[32];
    snprintf(apSsid, sizeof(apSsid), AP_SSID_PREFIX "%06X", (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));
    
    WiFi.softAP(apSsid, AP_PASSWORD, AP_CHANNEL);
    logMsg2Val("[WIFI] AP started", apSsid, "IP", WiFi.softAPIP().toString().c_str());
    
  // Try saved STA with retries (router/DHCP often needs a few seconds at boot).
    if (!charBufIsEmpty(wifiSsid)) {
        wifiConnectStaWithRetries(
            WIFI_BOOT_CONNECT_MAX_ATTEMPTS,
            WIFI_BOOT_CONNECT_ATTEMPT_MS,
            true);
    } else {
        logMsg("[WIFI] No saved network — connect via web UI (AP mode)");
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
    missedCallForwardEnabled = preferences.getBool("mcall", MISSED_CALL_FORWARD_DEFAULT);
    missedCallWatchSlot = preferences.getInt("mcall_slot", -1);
    preferences.end();

    loadSimUserDisabledMask();
    maintenanceOnBoot();
    
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

// Normalize SIM number for backend sync (strict PH mobile format only)
static void normalizeSimNumber(const char* raw, char* out, size_t outSize) {
    if (!raw || !raw[0]) {
        out[0] = '\0';
        return;
    }
    char normalized[PHONE_BUFFER_SIZE];
    normalized[0] = '\0';
    const int n = normalizePhNumber(raw, normalized, sizeof(normalized));
    if (n == 13 && strncmp(normalized, "+639", 4) == 0) {
        charBufSet(out, outSize, normalized);
        return;
    }
    out[0] = '\0';
}

void pruneExpiredActiveSessions() {  // called from maintenance + heartbeat
    time_t nowSecs = time(nullptr);
    if (nowSecs <= 1600000000L) return;
    const unsigned long nowMs = (unsigned long)nowSecs * 1000UL;
    int out = 0;
    for (int i = 0; i < activeSessionCount; i++) {
        if (activeSessions[i].expiresAtMs > 0 && activeSessions[i].expiresAtMs <= nowMs) {
            continue;
        }
        if (out != i) {
            activeSessions[out] = activeSessions[i];
        }
        out++;
    }
    activeSessionCount = out;
}

static void loadSimUserDisabledMask() {
    preferences.begin("agent", true);
    const uint32_t mask = preferences.getUInt("sim_off", 0);
    preferences.end();
    for (int i = 0; i < SIM_COUNT; i++) {
        simStates[i].userDisabled = (mask & (1UL << i)) != 0;
        if (simStates[i].userDisabled) {
            simStates[i].enabled = false;
        }
    }
}

static void saveSimUserDisabledMask() {
    uint32_t mask = 0;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (simStates[i].userDisabled) {
            mask |= (1UL << i);
        }
    }
    preferences.begin("agent", false);
    preferences.putUInt("sim_off", mask);
    preferences.end();
}

void setSimUserDisabled(int simIdx, bool disabled) {
    if (simIdx < 0 || simIdx >= SIM_COUNT) return;
    simStates[simIdx].userDisabled = disabled;
    if (disabled) {
        simStates[simIdx].enabled = false;
    }
    saveSimUserDisabledMask();
}

static void applyBackendSimStatusFromJson(const char* s) {
    if (!s || !s[0]) return;
    const char* simsKey = strstr(s, "\"sims\"");
    if (!simsKey) return;
    const char* p = strchr(simsKey, '[');
    if (!p) return;
    p++;

    while (*p && *p != ']') {
        const char* objStart = strchr(p, '{');
        if (!objStart) break;
        const char* objEnd = strchr(objStart, '}');
        if (!objEnd) break;

        int slot = 0;
        const char* slotKey = strstr(objStart, "\"slot\"");
        if (slotKey && slotKey < objEnd) {
            const char* colon = strchr(slotKey, ':');
            if (colon && colon < objEnd) {
                slot = atoi(colon + 1);
            }
        }

        char status[16];
        status[0] = '\0';
        const char* stKey = strstr(objStart, "\"status\":\"");
        if (stKey && stKey < objEnd) {
            const char* v = stKey + strlen("\"status\":\"");
            const char* endQuote = strchr(v, '"');
            if (endQuote && endQuote < objEnd) {
                const int len = (int)(endQuote - v);
                if (len > 0 && len < (int)sizeof(status)) {
                    strncpy(status, v, (size_t)len);
                    status[len] = '\0';
                }
            }
        }

        if (slot >= 0 && slot < SIM_COUNT && status[0] != '\0' &&
            !simStates[slot].userDisabled) {
            const bool shouldEnable =
                (strcmp(status, "ACTIVE") == 0 || strcmp(status, "IN_USE") == 0);
            simStates[slot].enabled = shouldEnable;
            if (!shouldEnable) {
                char slotBuf[8];
                snprintf(slotBuf, sizeof(slotBuf), "%d", slot);
                appendMonitorLogVal("[HEARTBEAT] Backend disabled SIM", slotBuf);
            }
        }

        p = objEnd + 1;
    }
}

static void parseActiveSessionsFromJson(const char* s) {
    if (!s || !s[0]) return;

    const char* sessionsKey = strstr(s, "\"activeSessions\"");
    if (!sessionsKey) return;

    const char* p2 = strchr(sessionsKey, '[');
    if (!p2) return;

    const char* afterBracket = p2 + 1;
    while (*afterBracket == ' ' || *afterBracket == '\n' || *afterBracket == '\r') {
        afterBracket++;
    }
    if (*afterBracket == ']') {
        activeSessionCount = 0;
        return;
    }

    activeSessionCount = 0;
    p2++;

    while (*p2 && *p2 != ']' && activeSessionCount < 8) {
        const char* objStart = strchr(p2, '{');
        if (!objStart) break;
        const char* objEnd = strchr(objStart, '}');
        if (!objEnd) break;

        ActiveSession* sess = &activeSessions[activeSessionCount];
        memset(sess, 0, sizeof(ActiveSession));

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
                            strncpy(sess->sessionId, vStart, (size_t)len);
                        }
                    }
                }
            }
        }

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
                            strncpy(sess->simNumber, vStart, (size_t)len);
                        }
                    }
                }
            }
        }

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
                            strncpy(sess->appName, vStart, (size_t)len);
                        }
                    }
                }
            }
        }

        const char* slotKey = strstr(objStart, "\"slot\"");
        if (slotKey && slotKey < objEnd) {
            const char* colon = strchr(slotKey, ':');
            if (colon && colon < objEnd) {
                sess->slot = atoi(colon + 1);
            }
        }

        const char* msgKey = strstr(objStart, "\"messageCount\"");
        if (msgKey && msgKey < objEnd) {
            const char* colon = strchr(msgKey, ':');
            if (colon && colon < objEnd) {
                sess->messageCount = atoi(colon + 1);
            }
        }

        const char* expKey = strstr(objStart, "\"expiresAt\"");
        if (expKey && expKey < objEnd) {
            const char* colon = strchr(expKey, ':');
            if (colon && colon < objEnd) {
                const char* vStart = strchr(colon, '"');
                if (vStart && vStart < objEnd) {
                    vStart++;
                    const char* vEnd = strchr(vStart, '"');
                    if (vEnd && vEnd < objEnd) {
                        int year, month, day, hour, min, sec;
                        if (sscanf(vStart, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) >= 6) {
                            static const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                            long days = 0;
                            for (int y = 1970; y < year; y++) {
                                days += 365;
                                if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) days += 1;
                            }
                            for (int m = 1; m < month; m++) {
                                days += daysInMonth[m - 1];
                                if (m == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) days += 1;
                            }
                            days += day - 1;
                            time_t ts = days * 86400L + hour * 3600L + min * 60L + sec;
                            sess->expiresAtMs = (unsigned long)ts * 1000UL;
                        }
                    }
                }
            }
        }

        activeSessionCount++;
        p2 = objEnd + 1;
    }
}

static bool simInHeartbeatReport(int i) {
    if (i < 0 || i >= SIM_COUNT) return false;
    if (simStates[i].userDisabled) return false;
    if (!simStates[i].enabled) return false;
    if (!simStates[i].responsive) return false;
    if (charBufIsEmpty(simStates[i].number)) return false;
    char normalizedNum[PHONE_BUFFER_SIZE];
    normalizeSimNumber(simStates[i].number, normalizedNum, sizeof(normalizedNum));
    if (charBufIsEmpty(normalizedNum)) {
        simStates[i].enabled = false;  // Turn slot OFF in UI for invalid numbers.
        appendErrorLogInt("[SIM] Disabled invalid number slot", i + 1);
        return false;
    }
    return true;
}

// Heartbeat HTTPS uses large TLS/HTTP objects — must not live on loopTask stack (8KB).
static HTTPClient gHbHttp;
static WiFiClientSecure gHbTls;
static WiFiClient gHbPlain;
static char gHbUrl[280];
static char gHbAuthHdr[AGENT_AUTH_HDR_SIZE];
static char gHbBody[3072];
static char gHbResp[1536];
static char gHbNumEsc[72];
static char gHbNormNum[PHONE_BUFFER_SIZE];

static bool hbHttpBegin(const char* url, int timeoutMs) {
    gHbTls.stop();
    gHbPlain.stop();
    gHbHttp.end();
    const bool isHttps = (strncmp(url, "https://", 8) == 0);
    if (isHttps) {
        gHbTls.setInsecure();
        gHbTls.setTimeout(timeoutMs);
        return gHbHttp.begin(gHbTls, url);
    }
    gHbPlain.setTimeout(timeoutMs);
    return gHbHttp.begin(gHbPlain, url);
}

static void hbHttpEnd() {
    gHbHttp.end();
    gHbTls.stop();
    gHbPlain.stop();
}

int agentHttpsPostJson(const char* url, const char* jsonBody, int timeoutMs, bool addAuth,
    char* respOut, size_t respOutSize, const char* opLabel) {
    if (!url || !jsonBody || timeoutMs < 1000) {
        return -1;
    }
    if (respOut && respOutSize > 0) {
        respOut[0] = '\0';
    }
    if (!ensureWifiForHttps()) {
        return -1;
    }
    if (httpsBusy) {
        return -11;
    }

    const unsigned long t0 = millis();
    if (opLabel && opLabel[0]) {
        char startBuf[56];
        snprintf(startBuf, sizeof(startBuf), "[HTTPS] %s starting", opLabel);
        logMsg(startBuf);
    }

    httpsBusy = true;
    wifiPrepareForHttps();

    if (!hbHttpBegin(url, timeoutMs)) {
        httpsBusy = false;
        wifiRecoverAfterHttps();
        return -1;
    }

    gHbHttp.addHeader("Content-Type", "application/json");
    gHbHttp.setTimeout(timeoutMs);
    if (addAuth && !charBufIsEmpty(agentBearerToken)) {
        formatBearerHeader(gHbAuthHdr, sizeof(gHbAuthHdr), agentBearerToken);
        gHbHttp.addHeader("Authorization", gHbAuthHdr);
    }

    handleWebRequests();
    yield();
    const int code = gHbHttp.POST(jsonBody);

    if (code > 0 && respOut && respOutSize > 1) {
        WiFiClient* stream = gHbHttp.getStreamPtr();
        if (stream) {
            size_t total = 0;
            const unsigned long t0 = millis();
            const unsigned long readLimitMs = (unsigned long)timeoutMs + 2000UL;
            while (millis() - t0 < readLimitMs && total < respOutSize - 1) {
                if (stream->available()) {
                    const int n = stream->read(
                        (uint8_t*)(respOut + total),
                        (int)(respOutSize - 1 - total)
                    );
                    if (n > 0) {
                        total += (size_t)n;
                    }
                } else if (!gHbHttp.connected()) {
                    break;
                } else {
                    handleWebRequests();
                    yield();
                    delay(2);
                }
            }
            respOut[total] = '\0';
        }
    }

    hbHttpEnd();
    httpsBusy = false;
    wifiRecoverAfterHttps();

    if (opLabel && opLabel[0]) {
        const unsigned long elapsedMs = millis() - t0;
        char doneBuf[72];
        snprintf(doneBuf, sizeof(doneBuf), "[HTTPS] %s done %lums code=%d",
            opLabel, elapsedMs, code);
        logMsg(doneBuf);
    }
    return code;
}

void resetAgentInventoryHeartbeat() {
    agentInventoryHeartbeatDone = false;
}

static void fetchHeartbeatFollowup() {
    if (charBufIsEmpty(agentBaseUrl) || charBufIsEmpty(agentBearerToken) || charBufIsEmpty(agentRefreshToken)) return;
    if (httpsBusy || !ensureWifiForHttps()) return;

    snprintf(gHbUrl, sizeof(gHbUrl), "%s/api/agent/heartbeat/followup", agentBaseUrl);

    size_t pos = snprintf(gHbBody, sizeof(gHbBody), "{\"device_id\":\"%s\",\"sim_numbers\":[", agentDeviceId);

    bool firstNum = true;
    for (int i = 0; i < SIM_COUNT && pos < sizeof(gHbBody) - 300; i++) {
        if (!simInHeartbeatReport(i)) continue;
        normalizeSimNumber(simStates[i].number, gHbNormNum, sizeof(gHbNormNum));
        jsonEscape(gHbNormNum, gHbNumEsc, sizeof(gHbNumEsc));
        if (!firstNum) pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos, ",");
        firstNum = false;
        pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos, "%s", gHbNumEsc);
    }

    pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos, "],\"sim_slots\":[");
    bool firstSlot = true;
    for (int i = 0; i < SIM_COUNT && pos < sizeof(gHbBody) - 200; i++) {
        if (!simInHeartbeatReport(i)) continue;
        normalizeSimNumber(simStates[i].number, gHbNormNum, sizeof(gHbNormNum));
        jsonEscape(gHbNormNum, gHbNumEsc, sizeof(gHbNumEsc));
        if (!firstSlot) pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos, ",");
        firstSlot = false;
        pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos, "{\"number\":%s,\"slot\":%d}", gHbNumEsc, i);
    }
    pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos, "]}");

    if (firstNum) {
        return;
    }

    logMsg("[HEARTBEAT] Followup POST");
    httpsBusy = true;
    wifiPrepareForHttps();

    if (!hbHttpBegin(gHbUrl, 25000)) {
        httpsBusy = false;
        wifiRecoverAfterHttps();
        return;
    }

    gHbHttp.addHeader("Content-Type", "application/json");
    gHbHttp.setTimeout(25000);
    formatBearerHeader(gHbAuthHdr, sizeof(gHbAuthHdr), agentBearerToken);
    gHbHttp.addHeader("Authorization", gHbAuthHdr);

    const int code = gHbHttp.POST(gHbBody);

    gHbResp[0] = '\0';
    if (code > 0) {
        WiFiClient* stream = gHbHttp.getStreamPtr();
        if (stream) {
            const unsigned long t0 = millis();
            int total = 0;
            while (millis() - t0 < 6000 && total < (int)sizeof(gHbResp) - 1) {
                if (stream->available()) {
                    const int n = stream->read((uint8_t*)(gHbResp + total), sizeof(gHbResp) - 1 - total);
                    if (n > 0) {
                        total += n;
                    }
                } else if (!gHbHttp.connected()) {
                    break;
                } else {
                    handleWebRequests();
                    yield();
                    delay(2);
                }
            }
            gHbResp[total] = '\0';
        }
    }

    hbHttpEnd();
    httpsBusy = false;
    wifiRecoverAfterHttps();

    if (code >= 200 && code < 300 && gHbResp[0] != '\0') {
        parseActiveSessionsFromJson(gHbResp);
        if (activeSessionCount > 0) {
            logMsgInt("[HEARTBEAT] Followup sessions", activeSessionCount);
        }
        appendMonitorLog("[HEARTBEAT] Followup done");
        pruneExpiredActiveSessions();
        maintenanceOnSessionsUpdated();
    }
}

static int heartbeatLowestBatteryPercent() {
    int lowestBatteryPercent = 100;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!simStates[i].enabled) continue;
        if (!simStates[i].responsive) continue;
        if (simStates[i].batteryPercent > 0 && simStates[i].batteryPercent < lowestBatteryPercent) {
            lowestBatteryPercent = simStates[i].batteryPercent;
        }
    }
    if (lowestBatteryPercent == 100 && batteryPercent > 0) {
        lowestBatteryPercent = batteryPercent;
    }
    return lowestBatteryPercent;
}

static void performHeartbeatPing() {
    if (!ensureWifiForHttps()) {
        logMsg("[HEARTBEAT] Ping skipped (no WiFi)");
        return;
    }
    if (httpsBusy || httpsStackNeedsSettle()) {
        return;
    }
    if (isSmsForwardPriorityActive()) {
        logHeartbeatWaitThrottled("pending SMS forward");
        return;
    }

    lastHeartbeatMs = millis();
    pauseSmsPolling(2000);
    logMsg("[HEARTBEAT] Ping starting");
    appendMonitorLog("[HEARTBEAT] Ping starting");

    if (!ntpConfigured) {
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        ntpConfigured = true;
        delay(200);
    }

    snprintf(gHbUrl, sizeof(gHbUrl), "%s/api/agent/ping", agentBaseUrl);
    snprintf(gHbBody, sizeof(gHbBody),
        "{\"device_id\":\"%s\",\"battery_level\":%d}",
        agentDeviceId,
        heartbeatLowestBatteryPercent());

    const int code = agentHttpsPostJson(
        gHbUrl, gHbBody, HEARTBEAT_PING_TIMEOUT_MS, true, nullptr, 0, "ping");
    resumeSmsPolling();

    if (code >= 200 && code < 300) {
        logMsg("[HEARTBEAT] Ping OK");
        appendMonitorLog("[HEARTBEAT] Ping OK");
        deviceRegistered = true;
    } else if (code < 0) {
        logMsgInt("[HEARTBEAT] Ping failed", code);
        appendMonitorLogInt("[HEARTBEAT] Ping failed", code);
        deferHeartbeat(HEARTBEAT_POST_FAIL_COOLDOWN_MS);
    } else {
        logMsgInt("[HEARTBEAT] Ping HTTP error", code);
        appendMonitorLogInt("[HEARTBEAT] Ping HTTP error", code);
    }
    lastHeartbeatMs = millis();
}

void performHeartbeat() {
    if (charBufIsEmpty(agentBaseUrl)) {
        return;  // No backend configured
    }
    if (isSimBusy() || millis() < modemGatewayStableUntilMs) {
        return;
    }
    if (!agentInventoryHeartbeatDone && getPendingSmsCount() > 0) {
        logHeartbeatWaitThrottled("pending SMS (full sync)");
        return;
    }
    if (charBufIsEmpty(agentBearerToken) || charBufIsEmpty(agentRefreshToken)) {
        logHeartbeatWaitThrottled("not signed in");
        return;
    }
    
    if (!ensureWifiForHttps()) {
        logMsg("[HEARTBEAT] No WiFi");
        appendMonitorLog("[HEARTBEAT] No WiFi");
        return;
    }

    // Skip if another HTTPS operation is in progress
    if (httpsBusy) {
        return;
    }

    if (agentInventoryHeartbeatDone) {
        performHeartbeatPing();
        return;
    }

    // Mark attempt immediately to prevent tight retry loops on begin/conn failures.
    lastHeartbeatMs = millis();

    httpsBusy = true;
    pauseSmsPolling(HEARTBEAT_FULL_PAUSE_MS);
    wifiPrepareForHttps();
    logMsg("[HEARTBEAT] Full sync starting");
    appendMonitorLog("[HEARTBEAT] Full sync starting");
    
    // Ensure NTP for TLS
    if (!ntpConfigured) {
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        ntpConfigured = true;
        delay(500);
    }
    
    snprintf(gHbUrl, sizeof(gHbUrl), "%s/api/agent/heartbeat", agentBaseUrl);

    if (!hbHttpBegin(gHbUrl, HEARTBEAT_FULL_TIMEOUT_MS)) {
        logMsg("[HEARTBEAT] HTTPS begin failed");
        appendMonitorLog("[HEARTBEAT] HTTPS begin failed");
        httpsBusy = false;
        wifiRecoverAfterHttps();
        resumeSmsPolling();
        return;
    }

    gHbHttp.addHeader("Content-Type", "application/json");
    gHbHttp.setTimeout(HEARTBEAT_FULL_TIMEOUT_MS);

    if (!charBufIsEmpty(agentBearerToken)) {
        formatBearerHeader(gHbAuthHdr, sizeof(gHbAuthHdr), agentBearerToken);
        gHbHttp.addHeader("Authorization", gHbAuthHdr);
    }

    if (HEARTBEAT_DEBUG) {
        gHbHttp.addHeader("x-heartbeat-debug", "1");
    }

    const int lowestBatteryPercent = heartbeatLowestBatteryPercent();

    size_t pos = 0;
    pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos,
        "{\"device_id\":\"%s\",\"battery_level\":%d,\"inventory_sync\":true,\"sims\":[",
        agentDeviceId,
        lowestBatteryPercent
    );

    bool first = true;
    for (int i = 0; i < SIM_COUNT && pos < sizeof(gHbBody) - 300; i++) {
        if (!simInHeartbeatReport(i)) continue;

        if (!first) pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos, ",");
        first = false;

        normalizeSimNumber(simStates[i].number, gHbNormNum, sizeof(gHbNormNum));
        jsonEscape(gHbNormNum, gHbNumEsc, sizeof(gHbNumEsc));

        const int signal = extractSignalQuality(simStates[i].csq);
        const char* netType = simStates[i].networkType[0] ? simStates[i].networkType : "2G";

        pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos,
            "{\"number\":%s,\"slot\":%d,\"status\":\"ACTIVE\",\"signal_strength\":%d,\"network_type\":\"%s\"}",
            gHbNumEsc,
            i,
            signal,
            netType
        );
    }

    pos += snprintf(gHbBody + pos, sizeof(gHbBody) - pos, "]}");

    if (HEARTBEAT_DEBUG) {
        logMsg("[HEARTBEAT][DBG] Request body:");
        appendMonitorLog("[HEARTBEAT][DBG] Request body:");
        logMsg(gHbBody);
        appendMonitorLog(gHbBody);
    }

    gHbResp[0] = '\0';

    int attempt = 0;
    int code = -1;

    while (attempt < 2) {
        handleWebRequests();
        yield();
        code = gHbHttp.POST(gHbBody);
        if (code > 0) {
            WiFiClient* stream = gHbHttp.getStreamPtr();
            if (stream) {
                size_t total = 0;
                const unsigned long t0 = millis();
                while (millis() - t0 < 20000UL && total < sizeof(gHbResp) - 1) {
                    if (stream->available()) {
                        const int n = stream->read(
                            (uint8_t*)(gHbResp + total),
                            (int)(sizeof(gHbResp) - 1 - total)
                        );
                        if (n > 0) {
                            total += (size_t)n;
                        }
                    } else if (!gHbHttp.connected()) {
                        break;
                    } else {
                        handleWebRequests();
                        yield();
                        delay(5);
                    }
                }
                gHbResp[total] = '\0';
            }
        }
        gHbHttp.end();

        if (code > 0) break;

        if (code < 0 && attempt == 0) {
            logMsgInt("[HEARTBEAT] Conn error, retrying", code);
            hbHttpEnd();
            for (int i = 0; i < 5; i++) {
                handleWebRequests();
                delay(30);
            }
            if (!WiFi.isConnected() && !ensureWifiForHttps()) {
                break;
            }
            delay(200);
            if (!hbHttpBegin(gHbUrl, 15000)) {
                break;
            }
            gHbHttp.addHeader("Content-Type", "application/json");
            gHbHttp.setTimeout(15000);
            if (!charBufIsEmpty(agentBearerToken)) {
                formatBearerHeader(gHbAuthHdr, sizeof(gHbAuthHdr), agentBearerToken);
                gHbHttp.addHeader("Authorization", gHbAuthHdr);
            }
        }
        attempt++;
    }

    if (code < 0) {
        logMsgInt("[HEARTBEAT] Failed, code", code);
        appendMonitorLogInt("[HEARTBEAT] Failed", code);
        appendErrorLogInt("[HEARTBEAT] Connection failed", code);
        hbHttpEnd();
        httpsBusy = false;
        wifiRecoverAfterHttps();
        resumeSmsPolling();
        deferHeartbeat(HEARTBEAT_POST_FAIL_COOLDOWN_MS);
        logMsg("[HEARTBEAT] Backoff 2 min before next try");
        appendMonitorLog("[HEARTBEAT] Backoff 2m");
        return;
    }

    if (code == 401) {
        hbHttpEnd();
        delay(100);

        if (refreshAgentToken()) {
            if (hbHttpBegin(gHbUrl, HEARTBEAT_FULL_TIMEOUT_MS)) {
                gHbHttp.addHeader("Content-Type", "application/json");
                gHbHttp.setTimeout(HEARTBEAT_FULL_TIMEOUT_MS);
                if (!charBufIsEmpty(agentBearerToken)) {
                    formatBearerHeader(gHbAuthHdr, sizeof(gHbAuthHdr), agentBearerToken);
                    gHbHttp.addHeader("Authorization", gHbAuthHdr);
                }
                code = gHbHttp.POST(gHbBody);
                gHbResp[0] = '\0';
                if (code > 0) {
                    WiFiClient* stream = gHbHttp.getStreamPtr();
                    if (stream) {
                        size_t total = 0;
                        const unsigned long t0 = millis();
                        while (millis() - t0 < 20000UL && total < sizeof(gHbResp) - 1) {
                            if (stream->available()) {
                                const int n = stream->read(
                                    (uint8_t*)(gHbResp + total),
                                    (int)(sizeof(gHbResp) - 1 - total)
                                );
                                if (n > 0) {
                                    total += (size_t)n;
                                }
                            } else if (!gHbHttp.connected()) {
                                break;
                            } else {
                                handleWebRequests();
                                yield();
                                delay(5);
                            }
                        }
                        gHbResp[total] = '\0';
                    }
                }
                gHbHttp.end();
            }
        }
    }

    if (code >= 200 && code < 300) {
        logMsg("[HEARTBEAT] Full sync OK");
        appendMonitorLog("[HEARTBEAT] Full sync OK");
        agentInventoryHeartbeatDone = true;
    } else {
        logMsgInt("[HEARTBEAT] Failed, code", code);
        appendMonitorLogInt("[HEARTBEAT] Failed", code);
        appendErrorLogInt("[HEARTBEAT] HTTP error", code);
    }

    if (HEARTBEAT_DEBUG) {
        appendMonitorLogInt("[HEARTBEAT][DBG] HTTP code", code);
        if (gHbResp[0]) {
            appendMonitorLog("[HEARTBEAT][DBG] Response (trunc):");
            appendMonitorLog(gHbResp);
        } else {
            appendMonitorLog("[HEARTBEAT][DBG] Empty response body");
        }
    }

    hbHttpEnd();
    delay(50);

    httpsBusy = false;
    wifiRecoverAfterHttps();
    resumeSmsPolling();

    if (code >= 200 && code < 300 && gHbResp[0] != '\0') {
        applyBackendSimStatusFromJson(gHbResp);
#if HEARTBEAT_FOLLOWUP_ENABLED
        pendingHeartbeatFollowup = !charBufIsEmpty(agentBearerToken);
#else
        pendingHeartbeatFollowup = false;
#endif
        pruneExpiredActiveSessions();
        maintenanceOnHeartbeatSuccess();
        if (!pendingHeartbeatFollowup) {
            maintenanceOnSessionsUpdated();
            if (!isSimBusy()) {
                flushPendingMissedCallsAfterHeartbeat();
            }
        }
    } else if (code < 200 || code >= 300) {
        appendMonitorLog("[HEARTBEAT] Keeping local sessions (request failed)");
        pruneExpiredActiveSessions();
        maintenanceOnSessionsUpdated();
    }

    if (code > 0 && code < 400) {
        deviceRegistered = true;
    }

    // lastHeartbeatMs already set at start; keep it updated here too
    lastHeartbeatMs = millis();
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
    if (!modemGatewayRunning) return;
    if (isSimBusy()) return;
    if (httpsBusy || isSmsPollingPaused()) return;

    for (int i = 0; i < SIM_COUNT; i++) {
        if (simStates[i].userDisabled) continue;
        if (!simStates[i].enabled) continue;

        if (!simStates[i].responsive) {
            simMarkSlotOffline(i, "not responsive");
            continue;
        }

        if (simStates[i].consecutiveErrors >= SIM_POLL_DISABLE_THRESHOLD) {
            simMarkSlotOffline(i, "poll errors");
            continue;
        }
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
    if (g_littlefsReady && lfsFileExists(LFS_ERRORS_PATH)) {
        remove(LFS_ERRORS_PATH);
    }
}

void clearMessagesLog() {
    if (g_littlefsReady && lfsFileExists(LFS_MESSAGES_PATH)) {
        remove(LFS_MESSAGES_PATH);
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
