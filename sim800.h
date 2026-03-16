#pragma once

// ============================================================================
// SIM800 Modem Communication
// All AT command handling and modem communication
// ============================================================================

#include "config.h"
#include <HardwareSerial.h>

// -----------------------------------------------------------------------------
// Serial Interface
// -----------------------------------------------------------------------------

// Global serial instance for SIM800
extern HardwareSerial sim800;

// Initialize SIM800 serial
void initSIM800Serial();

// -----------------------------------------------------------------------------
// AT Command Functions
// -----------------------------------------------------------------------------

// Send AT command and wait for response
// Returns true if "OK" found in response
bool sendAT(const char* cmd, unsigned long timeoutMs = 1000);

// Send AT command and capture response to buffer
// Response stored in static buffer (512 bytes)
void sendATCapture(const char* cmd, unsigned long timeoutMs = 1000);

// Get last captured response buffer
char* getSimBuffer();
void clearSimBuffer();

// Read a line from SIM800 (blocking with timeout)
// Returns line length, 0 on timeout
int readLine(char* buf, size_t bufSize, unsigned long timeoutMs = 1000);

// Check all SIMs on startup - marks responsive SIMs
void checkAllSIMsOnStartup();

// Read all available data into buffer
void readAllAvailable(char* buf, size_t bufSize);

// Flush all incoming data
void flushSimInput();

// -----------------------------------------------------------------------------
// Modem Status Functions
// -----------------------------------------------------------------------------

// Check if modem responds to AT
bool modemResponds();

// Check network registration status
// Returns true if registered (home or roaming)
bool checkNetworkRegistration();

// Get signal quality (0-31, -1 on error)
int getSignalQuality();

// Get operator name
void getOperatorName(char* buf, size_t bufSize);

// Get phone number from SIM
void getPhoneNumber(char* buf, size_t bufSize);

// Get battery info
void getBatteryInfo(int* percent, int* mv);

// Get network time
void getNetworkTime(char* buf, size_t bufSize);

// Control whether sendATCapture yields to WebServer.handleClient()
void setSimYieldToWebServer(bool enabled);

// -----------------------------------------------------------------------------
// Modem Configuration
// -----------------------------------------------------------------------------

// Initialize modem for SMS operation
// Sets text mode, GSM charset, SMS storage
bool initModemForSMS();

// Enable caller ID presentation
bool enableCallerID();

// Disable caller ID (to reduce spam)
bool disableCallerID();

// Set SMS indication mode
bool setSMSIndication();

// Enable local timestamp
bool enableLocalTimestamp();

// -----------------------------------------------------------------------------
// Call Functions
// -----------------------------------------------------------------------------

// Make a call
// Returns true if dial command accepted
bool dialNumber(const char* number);

// Hang up call
void hangupCall();

// Check if call is in progress
bool isCallInProgress();

// Check for incoming call (RING)
bool checkIncomingCall(char* callerBuf, size_t bufSize);

// -----------------------------------------------------------------------------
// SMS Functions (Low-level)
// -----------------------------------------------------------------------------

// Set SMS text mode
bool setSMSTextMode();

// List SMS messages
// filter: "ALL", "REC UNREAD", "REC READ"
void listSMS(const char* filter);

// Read specific SMS by index
bool readSMS(int index, char* sender, size_t senderSize, char* message, size_t messageSize);

// Delete SMS by index
bool deleteSMS(int index);

// Delete all SMS
bool deleteAllSMS();

// Send SMS
// Returns true if sent successfully
bool sendSMS(const char* number, const char* message);

// -----------------------------------------------------------------------------
// Response Parsing
// -----------------------------------------------------------------------------

// Check if response contains a specific string
bool responseContains(const char* str);

// Check if last response is OK
bool responseIsOK();

// Extract value from response (e.g., +CREG: 0,1)
// Returns true if found
bool extractValue(const char* prefix, char* value, size_t valueSize);

// -----------------------------------------------------------------------------
// State Tracking
// -----------------------------------------------------------------------------

// Get/set busy state
bool isSimBusy();
void setSimBusy(bool busy);
