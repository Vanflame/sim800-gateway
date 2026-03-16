#pragma once

// ============================================================================
// Safe Logging Functions
// NO String class usage - avoids heap fragmentation
// Uses only char buffers and PROGMEM strings
// ============================================================================

#include <Arduino.h>
#include <time.h>
#include "config.h"

// -----------------------------------------------------------------------------
// Time Helpers
// -----------------------------------------------------------------------------

// Get formatted timestamp (returns false if NTP not ready)
bool getLocalTimeStamp(char* buf, size_t bufSize);

// Get fallback timestamp from millis()
void getFallbackTimeStamp(char* buf, size_t bufSize);

// Ensure NTP is configured (call once at startup)
void ensureNtpConfigured();

// -----------------------------------------------------------------------------
// Core Logging Functions
// -----------------------------------------------------------------------------

// Print timestamp prefix to Serial
void logPrintPrefix();

// Log a simple message (from PROGMEM or static string)
void logMsg(const char* msg);

// Log message with one string value
// Usage: logMsgVal("SIM reset", "SIM1")
void logMsgVal(const char* msg, const char* val);

// Log message with integer value
// Usage: logMsgInt("Slot", 5)
void logMsgInt(const char* msg, int val);

// Log message with two values
// Usage: logMsg2Val("SIM", "1", "Status", "OK")
void logMsg2Val(const char* label1, const char* val1, const char* label2, const char* val2);

// Log message with integer and string
void logMsgIntVal(const char* label, int val, const char* label2, const char* val2);

// Log hex dump of data (for debugging)
void logHexDump(const char* label, const uint8_t* data, size_t len);

// -----------------------------------------------------------------------------
// Monitor Log (for web UI display)
// -----------------------------------------------------------------------------

// Append to monitor log buffer (circular buffer)
void appendMonitorLog(const char* msg);

// Append with integer value
void appendMonitorLogInt(const char* msg, int val);

// Append with string value
void appendMonitorLogVal(const char* msg, const char* val);

// Get monitor log as text (for web UI)
void getMonitorLogText(char* buf, size_t bufSize);

// Clear monitor log
void clearMonitorLog();

// -----------------------------------------------------------------------------
// Implementation (inline for performance)
// -----------------------------------------------------------------------------

inline void logPrintPrefix() {
    char ts[32];
    if (getLocalTimeStamp(ts, sizeof(ts))) {
        unsigned long ms = millis() % 1000;
        Serial.print('[');
        Serial.print(ts);
        Serial.print('.');
        if (ms < 100) Serial.print('0');
        if (ms < 10) Serial.print('0');
        Serial.print(ms);
        Serial.print("] ");
    } else {
        getFallbackTimeStamp(ts, sizeof(ts));
        Serial.print('[');
        Serial.print(ts);
        Serial.print("] ");
    }
}

inline void logMsg(const char* msg) {
    logPrintPrefix();
    Serial.println(msg);
}

inline void logMsgVal(const char* msg, const char* val) {
    logPrintPrefix();
    Serial.print(msg);
    Serial.print(": ");
    Serial.println(val);
}

inline void logMsgInt(const char* msg, int val) {
    logPrintPrefix();
    Serial.print(msg);
    Serial.print(": ");
    Serial.println(val);
}

inline void logMsg2Val(const char* label1, const char* val1, const char* label2, const char* val2) {
    logPrintPrefix();
    Serial.print(label1);
    Serial.print("=");
    Serial.print(val1);
    Serial.print(" ");
    Serial.print(label2);
    Serial.print("=");
    Serial.println(val2);
}

inline void logMsgIntVal(const char* label, int val, const char* label2, const char* val2) {
    logPrintPrefix();
    Serial.print(label);
    Serial.print("=");
    Serial.print(val);
    Serial.print(" ");
    Serial.print(label2);
    Serial.print("=");
    Serial.println(val2);
}

inline void logHexDump(const char* label, const uint8_t* data, size_t len) {
    logPrintPrefix();
    Serial.print(label);
    Serial.print(" (");
    Serial.print(len);
    Serial.println(" bytes):");
    
    for (size_t i = 0; i < len; i++) {
        if (data[i] < 16) Serial.print('0');
        Serial.print(data[i], HEX);
        Serial.print(' ');
        if ((i + 1) % 16 == 0) Serial.println();
    }
    if (len % 16 != 0) Serial.println();
}
