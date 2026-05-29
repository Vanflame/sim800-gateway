#pragma once

#include <Arduino.h>
#include <cstdint>

// ============================================================================
// SIM800 Gateway Configuration
// All constants, pins, and settings in one place
// ============================================================================

// -----------------------------------------------------------------------------
// Hardware Serial Pins (ESP32)
// -----------------------------------------------------------------------------
// Set to 1 to use two independent SIM800L modules on two UARTs (no mux).
// Set to 0 to use the CD74HC4067 mux (single UART shared across SIM slots).
#define USE_DUAL_UART   0

#define UART_BAUD_RATE  115200

// Mux mode (single UART)
#define UART_RX_PIN     4       // GPIO4 - RX from multiplexer
#define UART_TX_PIN     5       // GPIO5 - TX to all SIM RX (shared)

// Dual-UART mode (two independent UARTs)
// SIM800L #1
#define UART1_RX_PIN    4
#define UART1_TX_PIN    5
// SIM800L #2
#define UART2_RX_PIN    16
#define UART2_TX_PIN    17

// -----------------------------------------------------------------------------
// Multiplexer Control Pins (CD74HC4067)
// -----------------------------------------------------------------------------
#if !USE_DUAL_UART
#define MUX_S0          16      // GPIO16
#define MUX_S1          17      // GPIO17
#define MUX_S2          18      // GPIO18
#define MUX_S3          19      // GPIO19
#endif

// -----------------------------------------------------------------------------
// Reset Pin (for SIM reset via secondary multiplexer)
// -----------------------------------------------------------------------------
#define RESET_PIN       23      // GPIO23

// -----------------------------------------------------------------------------
// Firmware version (shown in web UI; bump when releasing OTA builds)
// -----------------------------------------------------------------------------
#define FIRMWARE_VERSION    "1.0.9"

// -----------------------------------------------------------------------------
// Over-the-air updates (ESP32 HTTPS OTA from GitHub Releases or custom URL)
// Requires partition scheme with OTA slots (e.g. "Minimal SPIFFS (1.9MB APP with OTA)")
// -----------------------------------------------------------------------------
// 0 = fits default 1.25MB partition (Check updates OK; Install needs OTA_ENABLED 1 + large partition).
// 1 = full HTTPS OTA install (~1.32MB). REQUIRED partition: Tools → Custom → partitions.csv
//     OR "Minimal SPIFFS (1.9MB APP with OTA)" — then set OTA_ENABLED to 1.
#ifndef OTA_ENABLED
#define OTA_ENABLED         1
#endif
// 1 = HEAD request before GET (two TLS sessions; GET often fails with -1 on ESP32). 0 = one GET only.
#ifndef OTA_PREFLIGHT_HEAD
#define OTA_PREFLIGHT_HEAD  0
#endif
// Allow POST /firmware-update from the device web UI. Off = no wireless install (USB flash only).
#ifndef OTA_WEB_INSTALL_ENABLED
#define OTA_WEB_INSTALL_ENABLED  1
#endif
// OTA download throughput (larger = faster HTTPS read; needs ~OTA_DL_BUFFER_SIZE free heap)
#ifndef OTA_DL_BUFFER_SIZE
#define OTA_DL_BUFFER_SIZE          8192
#endif
// 0 = mbedTLS default buffers (most reliable connect). 8192+ can speed download if heap allows.
#ifndef OTA_TLS_RX_BUFFER_SIZE
#define OTA_TLS_RX_BUFFER_SIZE      0
#endif
#ifndef OTA_TLS_TX_BUFFER_SIZE
#define OTA_TLS_TX_BUFFER_SIZE      512
#endif

// Default partition = 1.25MB app slot → sketch too big for HTTPS OTA. Auto-disable OTA so Verify still works.
#if OTA_ENABLED && defined(ARDUINO_PARTITION_default)
#warning SIM800: Partition Scheme is still Default. OTA install disabled for this build. Set Tools -> Partition Scheme -> Minimal SPIFFS (1.9MB APP with OTA), then rebuild for full OTA.
#undef OTA_ENABLED
#define OTA_ENABLED         0
#endif
#define OTA_FIRMWARE_URL_DEFAULT \
    "https://trfifrcfdtaxyuvsbfql.supabase.co/storage/v1/object/public/app/firmware.bin"
#define OTA_VERSION_URL \
    "https://raw.githubusercontent.com/Vanflame/sim800-gateway/main/firmware/version.txt"
#define OTA_GITHUB_OWNER    "Vanflame"
#define OTA_GITHUB_REPO     "sim800-gateway"
#define OTA_FIRMWARE_BIN    "firmware.bin"

// -----------------------------------------------------------------------------
// SIM Configuration
// -----------------------------------------------------------------------------
#if USE_DUAL_UART
#define SIM_COUNT       2       // Two SIM800L modules
#else
#define SIM_COUNT       16      // Total number of SIM slots

// UI slot 0..15 (SIM 1..16) -> CD74HC4067 mux channel (S0-S3)
#define LOGICAL_TO_MUX_INIT { \
    12, 11, 10,  3,  4,  5, \
    15, 14, 13,  0,  1,  2, \
     9,  8,  7,  6 \
}
#endif
#define MUX_SETTLE_MS   500     // Delay after switching MUX channel (ms) - increased for stability
#define MUX_VERIFY_RETRIES 3    // Number of retries to verify SIM after switch
#define UART_FLUSH_ITER 200    // Max iterations when flushing UART - increased
#define UART_FLUSH_WAIT_MS 50  // Wait time during UART flush

// -----------------------------------------------------------------------------
// Timing Intervals (ms)
// -----------------------------------------------------------------------------
#define SMS_POLL_INTERVAL_MS        400     // Min gap between each SIM slot poll (one slot per loop tick)
#define SMS_SLOT_POLL_COOLDOWN_MS   3000    // Min gap before re-polling the same slot
#define SMS_CMGL_TIMEOUT_MS         2500    // List SMS; empty SIMs return OK in <1s
#define SMS_POLL_TIMEOUT_MS         15000   // Safety timeout
#define SMS_RETRY_INTERVAL_MS       30000   // 30s between retry batches
#define HEARTBEAT_INTERVAL_MS       60000   // 60s heartbeat (ping after first full sync)
#define HEARTBEAT_PING_PAUSE_MS     8000UL   // SMS poll pause during lightweight ping
#define HEARTBEAT_FULL_PAUSE_MS     5000UL   // brief SMS poll pause during full inventory sync
#define HEARTBEAT_PING_TIMEOUT_MS   12000   // HTTP timeout for /api/agent/ping
#define HEARTBEAT_FULL_TIMEOUT_MS   25000   // HTTP timeout for full /api/agent/heartbeat
#define HEARTBEAT_POST_LOGIN_DEFER_MS   90000UL   // keep UI responsive after sign-in
#define HEARTBEAT_POST_FAIL_COOLDOWN_MS 120000UL  // after conn error, backoff before next HB
#define SMS_FORWARD_GAP_MS                  3000UL   // pause between back-to-back SMS HTTPS posts
#define HTTPS_POST_SETTLE_MS                6000UL   // wait after any HTTPS (ping/HB/SMS) before next POST
#define SMS_PRIORITY_SETTLE_MS              1500UL   // short TLS settle when OTP seen (priority forward)
#define SMS_PRIORITY_HTTPS_WAIT_MS          8000UL   // max wait for ping/HB to finish before OTP POST
#define SMS_PRIORITY_RETRY_GAP_MS           6000UL   // min gap between urgent OTP retries
#define SMS_POST_FORWARD_HEARTBEAT_DEFER_MS 20000UL  // defer heartbeat after successful SMS forward
// Second POST after heartbeat (activeSessions + announcements). Costs another WiFi block; off by default.
#ifndef HEARTBEAT_FOLLOWUP_ENABLED
#define HEARTBEAT_FOLLOWUP_ENABLED  0
#endif
#if HEARTBEAT_FOLLOWUP_ENABLED
#define HEARTBEAT_FOLLOWUP_DELAY_MS 15000UL  // gap before followup when enabled
#endif
#define PENDING_SMS_PROCESS_GAP_MS  3000UL   // retry queued SMS while modem polling continues
#define FIRMWARE_CHECK_INTERVAL_MS  (12UL * 60UL * 60UL * 1000UL)  // 12h OTA version check
// Periodic POST to /api/agent/device-maintenance (~every 55s). Blocks loop() during HTTPS — off by default.
#ifndef MAINTENANCE_BACKEND_POLL_ENABLED
#define MAINTENANCE_BACKEND_POLL_ENABLED  0
#endif
#define MAINTENANCE_POLL_MIN_INTERVAL_MS  55000   // min gap between maintenance API polls (when enabled)
#define MAINTENANCE_HTTP_TIMEOUT_MS       8000    // keep maint poll short so SMS poll gaps stay small
#define SCHEDULED_RESTART_SESSION_BUFFER_MS  (5UL * 60UL * 1000UL)  // defer restart 5m after last session
#define BATTERY_INFO_INTERVAL_MS    60000   // 60s battery check
#define SIM_BACKEND_REG_RETRY_MS    30000   // 30s backend reg retry
#define SYNC_RETRY_MS               30000   // 30s sync retry
#define NO_SMS_LOG_THROTTLE_MS      10000   // 10s throttle for "no SMS" logs
#define OUTGOING_MAX_DURATION_MS    15000   // 15s max call duration
#define MISSED_CALL_TIMEOUT_MS      30000   // 30s missed call timeout

// WiFi STA reconnect (when disconnected) — slow backoff; never fight AP setup UI
#define WIFI_RECONNECT_INTERVAL_MS      (3UL * 60UL * 1000UL)
#define WIFI_RECONNECT_MAX_ATTEMPTS     3
#define WIFI_RECONNECT_COOLDOWN_MS      (10UL * 60UL * 1000UL)
#define WIFI_STA_DISCONNECT_LOG_MS      60000UL
#define WIFI_USER_SETUP_ARM_MS          (2UL * 60UL * 1000UL)
#define WIFI_BOOT_CONNECT_MAX_ATTEMPTS  5
#define WIFI_BOOT_CONNECT_ATTEMPT_MS    8000UL   // max wait per boot connect try

// Backend login rate limit (per device, in handleLogin)
#define AUTH_LOGIN_MIN_INTERVAL_MS      10000UL
#define AUTH_LOGIN_MAX_FAILS            3
#define AUTH_LOGIN_COOLDOWN_MS          60000UL

// Missed call → forward to backend as Viber-style SMS (last 6 digits of caller)
#define MISSED_CALL_FORWARD_DEFAULT true    // NVS default; toggle in web UI
#define MISSED_CALL_VIBER_SENDER    "Viber"
#define MISSED_CALL_URC_LISTEN_MS   0       // Extra listen after SMS poll (0 = off, use fast scan)
#define MISSED_CALL_SCAN_INTERVAL_MS 200    // Fast scan tick (non-priority SIMs)
#define MISSED_CALL_SCAN_LISTEN_MS  120     // Quick peek per non-priority SIM
#define MISSED_CALL_PRIORITY_LISTEN_MS 4000  // Dwell on SIM with active Viber OTP session
#define MISSED_CALL_PRIORITY_INTERVAL_MS 500 // Re-visit priority SIM this often
#define MISSED_CALL_RING_WAIT_MS    8000    // After RING on priority SIM, wait for +CLIP
#define MISSED_CALL_PENDING_TTL_MS  600000UL // Keep queued missed calls 10 min (before Viber session)
#define MISSED_CALL_PENDING_MAX     16
extern bool missedCallForwardEnabled;
// -1 = auto from Viber OTP session; 0..15 = always watch this slot when missed-call ON
extern int missedCallWatchSlot;

// -----------------------------------------------------------------------------
// Buffer Sizes
// -----------------------------------------------------------------------------
// Supabase JWTs with Google metadata can exceed 1KB; keep headroom for Bearer header.
#define AGENT_BEARER_TOKEN_SIZE   2048
#define AGENT_REFRESH_TOKEN_SIZE  768
#define AGENT_AUTH_HDR_SIZE       (AGENT_BEARER_TOKEN_SIZE + 32)
#define AGENT_AUTH_RESP_SIZE      2560
#define SIM_BUFFER_SIZE     2048    // Main UART response buffer (increased for SMS list responses)
#define PHONE_BUFFER_SIZE   24      // Phone number buffer
#define CREG_BUFFER_SIZE    32      // CREG response buffer
#define COPS_BUFFER_SIZE    48      // COPS response buffer
#define CSQ_BUFFER_SIZE     48      // CSQ response buffer
#define SMS_MESSAGE_SIZE    320     // SMS message max size
#define CALLER_BUFFER_SIZE  32      // Caller ID buffer

// -----------------------------------------------------------------------------
// Queue Limits
// -----------------------------------------------------------------------------
#define MAX_PENDING_SMS     20      // Pending SMS queue size
#define MAX_CALL_LOG        15      // Call log entries
#define MONITOR_LOG_MAX_SIZE     20      // Monitor log entries (reduced for RAM)
#define ERROR_LOG_MAX_SIZE       15      // Error log entries (reduced for RAM)
#define ERROR_LOG_LINE_SIZE      128     // Reduced from 160

// -----------------------------------------------------------------------------
// Error Thresholds
// -----------------------------------------------------------------------------
#define SIM_ERROR_THRESHOLD    3   // Reset SIM after this many errors
#define SIM_CONSECUTIVE_ERROR_THRESHOLD 5  // Legacy threshold (watchdog)
#define SIM_POLL_DISABLE_THRESHOLD    3    // CMGL/poll failures → slot auto-OFF
#define MODEM_GATEWAY_STABLE_MS       45000UL  // No heartbeat until modem/SIM settled
// Watchdog timeout: must be long enough to survive a full heartbeat+maintenance cycle
// (~25s heartbeat + ~30s maintenance + 12 SIMs × 3s cooldown ≈ 95s max idle time per slot)
#define SIM_WATCHDOG_TIMEOUT_MS 300000  // 5 minutes: only trigger on genuinely dead SIMs

// Load balance USSD (Philippines — Globe/Smart etc.)
#define USSD_BALANCE_CODE       "*143#"
#define USSD_RESULT_SIZE        48       // Short balance summary only (UI + JSON)
#define USSD_RAW_SIZE           160      // Temp buffer for modem USSD text
#define USSD_CHECK_TIMEOUT_MS   25000
#define USSD_BULK_GAP_MS        300      // Short gap after USSD before next slot
#define USSD_SLOT_PAUSE_MS      18000UL  // pause SMS poll per *143# (modem busy up to ~11s)
#define USSD_BULK_SLOT_PAUSE_MS 20000UL  // per-slot budget during bulk *143#
#define USSD_AT_GAP_MS          400      // Cancel/close USSD session commands

// -----------------------------------------------------------------------------
// WiFi AP Configuration
// -----------------------------------------------------------------------------
#define AP_SSID_PREFIX    "SIM800-Gateway-"
#define AP_PASSWORD       ""        // Open AP (or set a password)
#define AP_CHANNEL        1

// -----------------------------------------------------------------------------
// Default Backend Configuration
// -----------------------------------------------------------------------------
#define DEFAULT_BASE_URL        "https://seller.otpocket.app"
#define DEFAULT_API_PATH        "/api/agent/incoming-sms"

// -----------------------------------------------------------------------------
// Debug Flags
// -----------------------------------------------------------------------------
#define SMS_HEX_DEBUG     false    // Enable hex dump of SMS data

#define HEARTBEAT_DEBUG   false    // Enable verbose heartbeat request/response logging

// -----------------------------------------------------------------------------
// Type Definitions
// -----------------------------------------------------------------------------

// SIM state tracking
typedef struct {
    bool enabled;
    bool userDisabled;  // Manual heartbeat OFF (web UI) — still poll/SMS; excluded from HB
    bool basicInitDone;
    bool responsive;
    bool registered;
    bool backendRegistered;
    char number[PHONE_BUFFER_SIZE];
    char creg[CREG_BUFFER_SIZE];
    char cops[COPS_BUFFER_SIZE];
    char csq[CSQ_BUFFER_SIZE];
    int batteryPercent;
    int batteryMv;
    int signalStrength;       // ASU 0-31, -1 if unknown
    char networkType[8];      // "2G", "EDGE", "GPRS", "UNKNOWN"
    uint8_t errorCount;
    uint8_t consecutiveErrors; // Track consecutive failures for recovery
    unsigned long lastBackendRegAttempt;
    unsigned long lastNoSmsLog;
    unsigned long lastSuccessfulPoll; // For watchdog detection
    char ussdLastResult[USSD_RESULT_SIZE];
    uint8_t ussdStatus;             // 0=never, 1=ok, 2=error, 3=checking
    uint8_t ussdLastDurationSec;    // Last *143# round-trip time (seconds)
    unsigned long ussdLastCheckMs;
} SimState;

// Pending SMS for retry queue
typedef struct {
    int simSlot;
    char simNumber[PHONE_BUFFER_SIZE];
    char sender[PHONE_BUFFER_SIZE];
    char message[SMS_MESSAGE_SIZE];
    unsigned long timestamp;
    int retryCount;
} PendingSms;

// Call log entry
typedef struct {
    char caller[PHONE_BUFFER_SIZE];
    char network[COPS_BUFFER_SIZE];
    char ts[32];
    unsigned long durationMs;
} CallLogItem;

// Active session from backend
typedef struct {
    char sessionId[64];
    char simNumber[PHONE_BUFFER_SIZE];
    char appName[32];
    int slot;
    int messageCount;
    unsigned long expiresAtMs;  // Unix timestamp in milliseconds
} ActiveSession;

// -----------------------------------------------------------------------------
// Global State Extern Declarations (defined in main .ino)
// -----------------------------------------------------------------------------
extern SimState simStates[SIM_COUNT];
extern PendingSms pendingSmsQueue[MAX_PENDING_SMS];
extern CallLogItem callLog[MAX_CALL_LOG];
extern ActiveSession activeSessions[8];  // Max 8 concurrent sessions
extern int pendingSmsCount;
extern int callLogCount;
extern int activeSessionCount;
extern int activeSim;
extern int currentMuxSim;
extern volatile bool simBusy;
extern bool smsPollingPaused;
extern bool deviceRegistered;
extern bool simRegistered;

// Monitor log
extern char monitorLog[MONITOR_LOG_MAX_SIZE][160];
extern int monitorLogCount;

// Error log (persistent, longer messages)
extern char errorLog[ERROR_LOG_MAX_SIZE][ERROR_LOG_LINE_SIZE];
extern int errorLogCount;

// Timing
extern unsigned long lastHeartbeatMs;
extern unsigned long heartbeatNotBeforeMs;
void deferHeartbeat(unsigned long delayMs);
inline void deferCloudBackend(unsigned long delayMs) { deferHeartbeat(delayMs); }
inline bool cloudBackendDeferred() { return millis() < heartbeatNotBeforeMs; }
void wifiPrepareForHttps();
bool ensureWifiForHttps();
void wifiRecoverAfterHttps();
// Single shared TLS client (gHbHttp) — use for SMS, ping, maintenance to avoid -1 after ping.
int agentHttpsPostJson(const char* url, const char* jsonBody, int timeoutMs, bool addAuth,
    char* respOut, size_t respOutSize, const char* opLabel = nullptr);
unsigned long getLastSmsPollActivityMs();
extern unsigned long lastHttpsEndMs;
void markHttpsSessionEnded();
inline bool httpsStackNeedsSettle() {
    return lastHttpsEndMs > 0 && (millis() - lastHttpsEndMs) < HTTPS_POST_SETTLE_MS;
}
bool isSmsForwardPriorityActive();
void requestSmsForwardPriority();
void processPendingSmsQueueNow();
extern unsigned long modemGatewayStableUntilMs;
extern unsigned long lastSmsPollMs;
extern unsigned long smsPollingPauseUntilMs;
extern bool heartbeatPaused;

// Agent config
extern char agentBaseUrl[128];
extern char agentDeviceId[64];
extern char agentBearerToken[AGENT_BEARER_TOKEN_SIZE];
extern char agentRefreshToken[AGENT_REFRESH_TOKEN_SIZE];
extern char agentSimNumber[PHONE_BUFFER_SIZE];
extern int agentSimSlot;
extern char agentApiPath[64];

// Refresh access token using refresh token (updates agentBearerToken + preferences)
bool refreshAgentToken();
extern bool agentUseAuth;

inline bool agentIsSignedIn() {
    return agentBearerToken[0] != '\0' && agentRefreshToken[0] != '\0';
}

// Perform heartbeat to backend (full inventory once after init, then ping)
void performHeartbeat();
void resetAgentInventoryHeartbeat();

// Prune expired OTP sessions (needs valid NTP time)
void pruneExpiredActiveSessions();

// HTTPS busy flag to prevent concurrent connections
extern volatile bool httpsBusy;
extern bool modemGatewayRunning;
extern volatile bool modemStartRequested;
extern volatile bool otaInProgress;

// WiFi
extern char wifiSsid[64];
extern unsigned long wifiUserSetupUntilMs;
extern char wifiPassword[64];
void armWifiUserSetupMs(unsigned long durationMs);
void ensureGatewaySoftAp();
void wifiPrepareForUserConfig();
void wifiPrepareForScan();
bool wifiStaBackgroundReconnectAllowed();
bool wifiStaNetworkLooksValid();
void wifiLogStaNetworkDetails();
bool wifiFixStaNetworkIfNeeded();
