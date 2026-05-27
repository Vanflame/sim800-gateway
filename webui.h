#pragma once

// ============================================================================
// Web UI Server
// HTTP server for configuration and monitoring
// ============================================================================

#include "config.h"
#include <WebServer.h>

// Web server instance
extern WebServer server;

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

// Initialize web server and register all routes
void initWebUI();

// Handle web requests in main loop
void handleWebRequests();

// -----------------------------------------------------------------------------
// Route Handlers - Status
// -----------------------------------------------------------------------------

// GET / - Main page
void handleRoot();

// GET /status - System status JSON
void handleStatus();

// GET /monitor - Monitor log text
void handleMonitor();

// GET /clear-monitor - Clear monitor log
void handleClearMonitor();

// -----------------------------------------------------------------------------
// Route Handlers - WiFi
// -----------------------------------------------------------------------------

// GET /scan - Scan WiFi networks
void handleScan();

// POST /save-wifi - Save WiFi credentials
void handleSaveWifi();

// GET /disconnect - Disconnect WiFi
void handleDisconnect();

// -----------------------------------------------------------------------------
// Route Handlers - SIM Management
// -----------------------------------------------------------------------------

// GET /sim-config - Get all SIM states
void handleSimConfig();

// GET /check-sim?slot=N - Check specific SIM
void handleCheckSim();

// GET /check-all-sim - Check all SIMs
void handleCheckAllSim();

// POST /sim-enable?slot=N&enabled=1/0 - Enable/disable SIM
void handleSimEnable();

// POST /sim-disable-all - Disable every SIM slot (same as Off per slot)
void handleSimDisableAll();

// POST /ussd-check?slot=N - Start *143# on one SIM (background)
void handleUssdCheck();

// GET /ussd-manual-status - Progress or result for manual *143#
void handleUssdManualStatus();

// POST /ussd-bulk - Start *143# on all enabled SIMs (background)
void handleUssdBulk();

// GET /ussd-bulk-status - Progress or final OK/fail list with numbers
void handleUssdBulkStatus();

// POST /toggle-missed-call?enabled=1/0 - Enable/disable missed call → Viber SMS
void handleToggleMissedCall();

// -----------------------------------------------------------------------------
// Route Handlers - Calls
// -----------------------------------------------------------------------------

// GET /calls - Get call log
void handleCalls();

// GET /clear-calls - Clear call log
void handleClearCalls();

// POST /call?number=XXX&slot=N - Make a call
void handleCall();

// POST /hangup - Hang up current call
void handleHangup();

// -----------------------------------------------------------------------------
// Route Handlers - SMS
// -----------------------------------------------------------------------------

// POST /send-sms?number=XXX&message=XXX&slot=N - Send SMS
void handleSendSms();

// GET /messages - Get message history
void handleMessages();

// GET /clear-messages - Clear message history
void handleClearMessages();

// -----------------------------------------------------------------------------
// Route Handlers - Agent Config
// -----------------------------------------------------------------------------

// POST /agent-config - Save agent configuration
void handleAgentConfig();

// POST /login - Agent login
void handleLogin();

// GET /logout - Logout
void handleLogout();

// POST /refresh-token - Refresh access token
void handleRefreshToken();

// POST /register-device - Register device with backend
void handleRegisterDevice();

// Register device_id with backend (required before heartbeat). Returns true on 2xx.
bool registerDeviceWithBackend();

// POST /register-sim - Register SIM with backend
void handleRegisterSim();

// POST /heartbeat - Manual heartbeat trigger (debug)
void handleHeartbeatManual();

// POST /toggle-polling - Toggle SMS polling pause
void handleTogglePolling();

// POST /toggle-heartbeat - Toggle heartbeat pause
void handleToggleHeartbeat();

// GET /firmware-check - Compare FIRMWARE_VERSION to remote version.txt
void handleFirmwareCheck();

// POST /firmware-update - Download and flash firmware (optional url=)
void handleFirmwareUpdate();

// POST /firmware-config - Save OTA firmware URL to NVS
void handleFirmwareConfig();

// -----------------------------------------------------------------------------
// JSON Response Helpers
// -----------------------------------------------------------------------------

// Send JSON success response
void sendJsonSuccess(const char* message = NULL);

// Send JSON error response
void sendJsonError(const char* error, int code = 400);

// Send JSON response
void sendJson(const char* json);

// -----------------------------------------------------------------------------
// HTML Generation
// -----------------------------------------------------------------------------

// Get HTML page (from PROGMEM)
const char* getHtmlPage();

// Build status JSON
void buildStatusJson(char* buf, size_t bufSize);

// Build SIM config JSON
void buildSimConfigJson(char* buf, size_t bufSize);

// Build call log JSON
void buildCallLogJson(char* buf, size_t bufSize);
