#pragma once

// ============================================================================
// Utility Functions
// Phone normalization, string helpers, JSON escaping
// NO String class - uses char buffers only
// ============================================================================

#include <Arduino.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
// String Buffer Helpers
// -----------------------------------------------------------------------------

// Clear a char buffer
inline void charBufClear(char* buf, size_t size) {
    if (buf && size > 0) buf[0] = '\0';
}

// Check if buffer is empty
inline bool charBufIsEmpty(const char* buf) {
    return !buf || buf[0] == '\0';
}

// Copy string to buffer with null termination
inline void charBufSet(char* buf, size_t size, const char* src) {
    if (!buf || size == 0 || !src) return;
    strncpy(buf, src, size - 1);
    buf[size - 1] = '\0';
}

// Append to buffer
inline void charBufAppend(char* buf, size_t size, const char* src) {
    if (!buf || !src || size == 0) return;
    size_t len = strlen(buf);
    if (len >= size - 1) return;
    strncat(buf, src, size - len - 1);
}

// Trim whitespace from both ends (in place)
void charBufTrim(char* buf);

// -----------------------------------------------------------------------------
// Phone Number Normalization (Philippines format)
// -----------------------------------------------------------------------------

// Normalize to +639XXXXXXXX format
// Returns length of normalized number, or 0 if invalid
// Output buffer must be at least 14 bytes
int normalizePhNumber(const char* input, char* output, size_t outputSize);

// Check if string looks like a phone number
bool isPhoneNumber(const char* str);

// -----------------------------------------------------------------------------
// JSON Escaping
// -----------------------------------------------------------------------------

// Escape string for JSON (adds quotes)
// Returns bytes written (excluding null terminator)
size_t jsonEscape(const char* input, char* output, size_t outputSize);

// Escape string for JSON without quotes
size_t jsonEscapeNoQuotes(const char* input, char* output, size_t outputSize);

// HTML escape
size_t htmlEscape(const char* input, char* output, size_t outputSize);

// -----------------------------------------------------------------------------
// URL Helpers
// -----------------------------------------------------------------------------

// Normalize base URL (ensure http:// or https:// prefix, no trailing slash)
void normalizeBaseUrl(char* buf, size_t size);

// Normalize API path (ensure leading slash)
void normalizeApiPath(char* buf, size_t size);

// URL encode a string
size_t urlEncode(const char* input, char* output, size_t outputSize);

// -----------------------------------------------------------------------------
// Parsing Helpers
// -----------------------------------------------------------------------------

// Extract operator name from +COPS response
// Example: +COPS: 0,0,"SMART Gold" -> "SMART Gold"
// Returns length of extracted name
int extractOperatorName(const char* cops, char* output, size_t outputSize);

// Check if CREG response indicates registered
// Returns true if stat=1 (home) or stat=5 (roaming)
bool cregIndicatesRegistered(const char* creg);

// Extract signal quality from +CSQ response
// Returns -1 if invalid
int extractSignalQuality(const char* csq);

// Extract network type from COPS or CNMP response
// SIM800L only supports 2G: "2G", "EDGE", "GPRS", or "UNKNOWN"
void extractNetworkType(const char* response, char* output, size_t outputSize);

// Extract phone number from +CNUM response
int extractPhoneNumber(const char* cnum, char* output, size_t outputSize);

// -----------------------------------------------------------------------------
// Time Helpers
// -----------------------------------------------------------------------------

// Get current timestamp as ISO string
void getIsoTimestamp(char* buf, size_t size);

// Parse SIM800 time format to ISO
void parseSimTimeToIso(const char* simTime, char* output, size_t outputSize);

// -----------------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------------

inline void charBufTrim(char* buf) {
    if (!buf) return;
    
    // Trim trailing
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\r' || buf[len-1] == '\n' || buf[len-1] == '\t')) {
        buf[--len] = '\0';
    }
    
    // Trim leading
    size_t start = 0;
    while (start < len && (buf[start] == ' ' || buf[start] == '\r' || buf[start] == '\n' || buf[start] == '\t')) {
        start++;
    }
    if (start > 0) {
        memmove(buf, buf + start, len - start + 1);
    }
}

inline int normalizePhNumber(const char* input, char* output, size_t outputSize) {
    if (!input || !output || outputSize < 14) return 0;
    
    // Copy and trim
    char temp[32];
    strncpy(temp, input, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    charBufTrim(temp);
    
    // Remove spaces and dashes
    size_t j = 0;
    for (size_t i = 0; temp[i] && j < sizeof(temp) - 1; i++) {
        if (temp[i] != ' ' && temp[i] != '-') {
            temp[j++] = temp[i];
        }
    }
    temp[j] = '\0';
    
    // Already in correct format: +639XXXXXXXX (13 chars)
    if (strncmp(temp, "+639", 4) == 0 && strlen(temp) == 13) {
        strcpy(output, temp);
        return 13;
    }
    
    // Convert 09XX to +639XX
    if (strncmp(temp, "09", 2) == 0 && strlen(temp) == 11) {
        strcpy(output, "+639");
        strcat(output, temp + 2);
        return 13;
    }
    
    // Convert 9XX to +639XX
    if (temp[0] == '9' && strlen(temp) == 10) {
        strcpy(output, "+639");
        strcat(output, temp);
        return 13;
    }
    
    // Convert +09XX to +639XX
    if (strncmp(temp, "+09", 3) == 0 && strlen(temp) == 12) {
        strcpy(output, "+639");
        strcat(output, temp + 3);
        return 13;
    }
    
    // Convert +63XX (missing 9) to +639XX
    if (strncmp(temp, "+63", 3) == 0 && strlen(temp) == 12) {
        strcpy(output, "+639");
        strcat(output, temp + 4);
        return 13;
    }
    
    // Already has +63
    if (strncmp(temp, "+63", 3) == 0) {
        strncpy(output, temp, outputSize - 1);
        output[outputSize - 1] = '\0';
        return strlen(output);
    }
    
    // Add + if missing
    if (temp[0] != '+' && strlen(temp) >= 10) {
        output[0] = '+';
        strncpy(output + 1, temp, outputSize - 2);
        output[outputSize - 1] = '\0';
        return strlen(output);
    }
    
    // Return as-is
    strncpy(output, temp, outputSize - 1);
    output[outputSize - 1] = '\0';
    return strlen(output);
}

inline bool isPhoneNumber(const char* str) {
    if (!str || !str[0]) return false;
    
    // Skip leading +
    if (str[0] == '+') str++;
    
    // Must be all digits
    while (*str) {
        if (*str < '0' || *str > '9') {
            if (*str != ' ' && *str != '-') return false;
        }
        str++;
    }
    return true;
}

inline size_t jsonEscape(const char* input, char* output, size_t outputSize) {
    if (!output || outputSize < 3) return 0;
    
    size_t j = 0;
    output[j++] = '"';
    
    if (input) {
        for (size_t i = 0; input[i] && j < outputSize - 2; i++) {
            char c = input[i];
            if (c == '"') {
                if (j < outputSize - 3) { output[j++] = '\\'; output[j++] = '"'; }
            } else if (c == '\\') {
                if (j < outputSize - 3) { output[j++] = '\\'; output[j++] = '\\'; }
            } else if (c == '\n') {
                if (j < outputSize - 3) { output[j++] = '\\'; output[j++] = 'n'; }
            } else if (c == '\r') {
                if (j < outputSize - 3) { output[j++] = '\\'; output[j++] = 'r'; }
            } else if (c == '\t') {
                if (j < outputSize - 3) { output[j++] = '\\'; output[j++] = 't'; }
            } else if ((unsigned char)c >= 0x20) {
                output[j++] = c;
            }
            // Skip other control chars
        }
    }
    
    output[j++] = '"';
    output[j] = '\0';
    return j;
}

inline size_t jsonEscapeNoQuotes(const char* input, char* output, size_t outputSize) {
    if (!output || outputSize < 1) return 0;
    
    size_t j = 0;
    if (input) {
        for (size_t i = 0; input[i] && j < outputSize - 1; i++) {
            char c = input[i];
            if (c == '"') {
                if (j < outputSize - 2) { output[j++] = '\\'; output[j++] = '"'; }
            } else if (c == '\\') {
                if (j < outputSize - 2) { output[j++] = '\\'; output[j++] = '\\'; }
            } else if (c == '\n') {
                if (j < outputSize - 2) { output[j++] = '\\'; output[j++] = 'n'; }
            } else if (c == '\r') {
                if (j < outputSize - 2) { output[j++] = '\\'; output[j++] = 'r'; }
            } else if (c == '\t') {
                if (j < outputSize - 2) { output[j++] = '\\'; output[j++] = 't'; }
            } else if ((unsigned char)c >= 0x20) {
                output[j++] = c;
            }
        }
    }
    output[j] = '\0';
    return j;
}

inline size_t htmlEscape(const char* input, char* output, size_t outputSize) {
    if (!output || outputSize < 1) return 0;
    
    size_t j = 0;
    if (input) {
        for (size_t i = 0; input[i] && j < outputSize - 1; i++) {
            char c = input[i];
            if (c == '&') {
                if (j < outputSize - 6) { strcpy(output + j, "&amp;"); j += 5; }
            } else if (c == '<') {
                if (j < outputSize - 5) { strcpy(output + j, "&lt;"); j += 4; }
            } else if (c == '>') {
                if (j < outputSize - 5) { strcpy(output + j, "&gt;"); j += 4; }
            } else if (c == '"') {
                if (j < outputSize - 7) { strcpy(output + j, "&quot;"); j += 6; }
            } else if (c == '\'') {
                if (j < outputSize - 6) { strcpy(output + j, "&#39;"); j += 5; }
            } else {
                output[j++] = c;
            }
        }
    }
    output[j] = '\0';
    return j;
}

inline bool cregIndicatesRegistered(const char* creg) {
    if (!creg) return false;
    
    // Find +CREG:
    const char* p = strstr(creg, "+CREG:");
    if (!p) return false;
    
    // Find the stat value after comma
    const char* comma = strchr(p, ',');
    if (!comma) return false;
    
    // Parse stat
    int stat = atoi(comma + 1);
    return (stat == 1 || stat == 5);  // 1=home, 5=roaming
}

inline int extractSignalQuality(const char* csq) {
    if (!csq) return -1;
    
    // Format: +CSQ: <rssi>,<ber>
    const char* p = strstr(csq, "+CSQ:");
    if (!p) return -1;
    
    p += 5;  // Skip "+CSQ:"
    while (*p == ' ') p++;  // Skip spaces
    
    return atoi(p);
}

inline void extractNetworkType(const char* response, char* output, size_t outputSize) {
    if (!output || outputSize < 2) return;
    output[0] = '\0';
    
    if (!response) {
        strncpy(output, "UNKNOWN", outputSize - 1);
        output[outputSize - 1] = '\0';
        return;
    }
    
    // SIM800L only supports 2G networks
    // Check for CNMP response: +CNMP: 2 (GSM only)
    const char* cnmp = strstr(response, "+CNMP:");
    if (cnmp) {
        cnmp += 7;
        while (*cnmp == ' ') cnmp++;
        int mode = atoi(cnmp);
        // SIM800L modes: 2=GSM, 13=GSM only, 38=GPRS, 48=EDGE
        if (mode == 48) {
            strncpy(output, "EDGE", outputSize - 1);
        } else if (mode == 38) {
            strncpy(output, "GPRS", outputSize - 1);
        } else {
            strncpy(output, "2G", outputSize - 1);
        }
        output[outputSize - 1] = '\0';
        return;
    }
    
    // Default to 2G for SIM800L (it doesn't support 3G/4G)
    strncpy(output, "2G", outputSize - 1);
    output[outputSize - 1] = '\0';
}

inline int extractOperatorName(const char* cops, char* output, size_t outputSize) {
    if (!output || outputSize < 1) return 0;
    output[0] = '\0';
    
    if (!cops) return 0;
    
    // Format: +COPS: 0,0,"SMART Gold"
    const char* q1 = strchr(cops, '"');
    if (!q1) return 0;
    
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return 0;
    
    size_t len = q2 - q1 - 1;
    if (len >= outputSize) len = outputSize - 1;
    
    strncpy(output, q1 + 1, len);
    output[len] = '\0';
    
    return len;
}
