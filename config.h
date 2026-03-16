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
#define UART_RX_PIN     4       // GPIO4 - RX from multiplexer
#define UART_TX_PIN     5       // GPIO5 - TX to all SIM RX (shared)
#define UART_BAUD_RATE  115200

// -----------------------------------------------------------------------------
// Multiplexer Control Pins (CD74HC4067)
// -----------------------------------------------------------------------------
#define MUX_S0          16      // GPIO16
#define MUX_S1          17      // GPIO17
#define MUX_S2          18      // GPIO18
#define MUX_S3          19      // GPIO19

// -----------------------------------------------------------------------------
// Reset Pin (for SIM reset via secondary multiplexer)
// -----------------------------------------------------------------------------
#define RESET_PIN       23      // GPIO23

// -----------------------------------------------------------------------------
// SIM Configuration
// -----------------------------------------------------------------------------
#define SIM_COUNT       16      // Total number of SIM slots
#define MUX_SETTLE_MS   300     // Delay after switching MUX channel (ms)
#define UART_FLUSH_ITER 100    // Max iterations when flushing UART

// -----------------------------------------------------------------------------
// Timing Intervals (ms)
// -----------------------------------------------------------------------------
#define SMS_POLL_INTERVAL_MS        2000    // 2s between poll cycles
#define SMS_POLL_TIMEOUT_MS         15000   // Safety timeout
#define SMS_RETRY_INTERVAL_MS       30000   // 30s between retry batches
#define HEARTBEAT_INTERVAL_MS       60000   // 60s heartbeat
#define BATTERY_INFO_INTERVAL_MS    60000   // 60s battery check
#define SIM_BACKEND_REG_RETRY_MS    30000   // 30s backend reg retry
#define SYNC_RETRY_MS               30000   // 30s sync retry
#define NO_SMS_LOG_THROTTLE_MS      10000   // 10s throttle for "no SMS" logs
#define OUTGOING_MAX_DURATION_MS    15000   // 15s max call duration
#define MISSED_CALL_TIMEOUT_MS      30000   // 30s missed call timeout

// -----------------------------------------------------------------------------
// Buffer Sizes
// -----------------------------------------------------------------------------
#define SIM_BUFFER_SIZE     512     // Main UART response buffer
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
#define MONITOR_LOG_MAX_SIZE     40      // Monitor log entries (reduced for heap - SSL needs RAM)

// -----------------------------------------------------------------------------
// Error Thresholds
// -----------------------------------------------------------------------------
#define SIM_ERROR_THRESHOLD    3   // Reset SIM after this many errors

// -----------------------------------------------------------------------------
// WiFi AP Configuration
// -----------------------------------------------------------------------------
#define AP_SSID_PREFIX    "SIM800-Gateway-"
#define AP_PASSWORD       ""        // Open AP (or set a password)
#define AP_CHANNEL        1

// -----------------------------------------------------------------------------
// Default WiFi Credentials (used if not saved)
// -----------------------------------------------------------------------------
#define DEFAULT_WIFI_SSID       "3G"
#define DEFAULT_WIFI_PASSWORD   "Xqwerty11"

// -----------------------------------------------------------------------------
// Default Backend Configuration
// -----------------------------------------------------------------------------
#define DEFAULT_BASE_URL        "https://www.otpocket.app"
#define DEFAULT_API_PATH        "/api/agent/incoming-sms"

// -----------------------------------------------------------------------------
// Debug Flags
// -----------------------------------------------------------------------------
#define SMS_HEX_DEBUG     false    // Enable hex dump of SMS data

// -----------------------------------------------------------------------------
// Type Definitions
// -----------------------------------------------------------------------------

// SIM state tracking
typedef struct {
    bool enabled;
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
    uint8_t errorCount;
    unsigned long lastBackendRegAttempt;
    unsigned long lastNoSmsLog;
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

// -----------------------------------------------------------------------------
// Global State Extern Declarations (defined in main .ino)
// -----------------------------------------------------------------------------
extern SimState simStates[SIM_COUNT];
extern PendingSms pendingSmsQueue[MAX_PENDING_SMS];
extern CallLogItem callLog[MAX_CALL_LOG];
extern int pendingSmsCount;
extern int callLogCount;
extern int activeSim;
extern int currentMuxSim;
extern volatile bool simBusy;
extern bool smsPollingPaused;
extern bool deviceRegistered;
extern bool simRegistered;

// Monitor log
extern char monitorLog[MONITOR_LOG_MAX_SIZE][160];
extern int monitorLogCount;

// Timing
extern unsigned long lastHeartbeatMs;
extern unsigned long lastSmsPollMs;
extern unsigned long smsPollingPauseUntilMs;

// Agent config
extern char agentBaseUrl[128];
extern char agentDeviceId[64];
extern char agentBearerToken[1024];  // JWT tokens can be 700+ chars
extern char agentRefreshToken[512];
extern char agentSimNumber[PHONE_BUFFER_SIZE];
extern int agentSimSlot;
extern char agentApiPath[64];

// Refresh access token using refresh token (updates agentBearerToken + preferences)
bool refreshAgentToken();
extern bool agentUseAuth;

// Perform heartbeat to backend
void performHeartbeat();

// HTTPS busy flag to prevent concurrent connections
extern volatile bool httpsBusy;

// WiFi
extern char wifiSsid[64];
extern char wifiPassword[64];
