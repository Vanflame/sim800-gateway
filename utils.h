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

// Default device_id for new devices: gw-<6 hex digits from chip MAC>, e.g. gw-a4f2b1
inline void generateDefaultDeviceId(char* out, size_t outSize) {
    if (!out || outSize < 10) {
        return;
    }
    const uint32_t chipId = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFUL);
    snprintf(out, outSize, "gw-%06x", chipId);
}

// -----------------------------------------------------------------------------
// Phone Number Normalization (Philippines format)
// -----------------------------------------------------------------------------

// Normalize to +639XXXXXXXX format (+639 + 9 subscriber digits = 13 chars).
// Returns 13 on success, 0 if invalid. Output buffer must be at least 14 bytes.
int normalizePhNumber(const char* input, char* output, size_t outputSize);

// True when number is canonical +639XXXXXXXX (13 chars).
bool isNormalizedPhMobile(const char* number);

// Normalize in place; returns true when buf holds +639XXXXXXXX.
bool applyPhMobileNormalization(char* buf, size_t bufSize);

// Check if string looks like a phone number
bool isPhoneNumber(const char* str);

// Extract the last N digits from a phone string (digits only). Returns digit count or 0.
int extractLastDigits(const char* input, int count, char* output, size_t outputSize);

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

// Build "Bearer <token>" without Arduino String (avoids heap fragmentation)
inline void formatBearerHeader(char* out, size_t outSize, const char* token) {
    if (!out || outSize < 8) return;
    snprintf(out, outSize, "Bearer %s", (token && token[0]) ? token : "");
}

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

// Decode SIM800 UCS2 hex payload (e.g. "00310032..." -> "12") into UTF-8 out buffer.
bool decodeUcs2HexMessage(const char* hexIn, char* out, size_t outSize);

// True when body looks like UCS2 hex from modem (not plain text).
bool looksLikeUcs2HexPayload(const char* s);

// Decode UCS2 hex in place when detected; returns false only if hex-like but undecodable.
bool normalizeSmsBodyFromModem(char* message, size_t messageSize);

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

// True when output is +639XXXXXXXX (13 chars).
inline bool isNormalizedPhMobile(const char* number) {
    return number && strlen(number) == 13 && strncmp(number, "+639", 4) == 0;
}

inline int normalizePhNumber(const char* input, char* output, size_t outputSize) {
    if (!input || !output || outputSize < 14) return 0;

    // Already canonical (+639 + 9 subscriber digits).
    if (strncmp(input, "+639", 4) == 0 && strlen(input) == 13) {
        charBufSet(output, outputSize, input);
        return 13;
    }

    // Repair legacy double-9 corruption: +6399XXXXXXXXX (14 chars).
    if (strncmp(input, "+6399", 5) == 0 && strlen(input) == 14) {
        snprintf(output, outputSize, "+639%s", input + 5);
        return 13;
    }

    char digits[16];
    int nd = 0;
    for (const char* p = input; *p && nd < 15; p++) {
        if (*p >= '0' && *p <= '9') {
            digits[nd++] = *p;
        }
    }
    digits[nd] = '\0';
    if (nd < 10) {
        output[0] = '\0';
        return 0;
    }

    // PH mobile is always 10 digits starting with 9; take the last 10 digit run.
    const char* tail = digits + nd - 10;
    if (tail[0] != '9') {
        output[0] = '\0';
        return 0;
    }

    snprintf(output, outputSize, "+639%s", tail + 1);
    return 13;
}

inline bool applyPhMobileNormalization(char* buf, size_t bufSize) {
    if (!buf || bufSize < 14 || charBufIsEmpty(buf)) return false;
    char normalized[16];
    if (normalizePhNumber(buf, normalized, sizeof(normalized)) != 13) return false;
    charBufSet(buf, bufSize, normalized);
    return true;
}

// Parse AT+CNUM response and store +639XXXXXXXX when possible.
inline bool parseCnumResponseNumber(const char* cnumResponse, char* out, size_t outSize) {
    if (!cnumResponse || !out || outSize < 2) return false;
    out[0] = '\0';

    const char* numStart = strstr(cnumResponse, ",\"");
    if (!numStart) return false;
    numStart += 2;
    const char* numEnd = strchr(numStart, '"');
    if (!numEnd || numEnd <= numStart) return false;

    char raw[32];
    const int len = (int)(numEnd - numStart);
    if (len <= 0 || len >= (int)sizeof(raw)) return false;
    strncpy(raw, numStart, (size_t)len);
    raw[len] = '\0';

    if (normalizePhNumber(raw, out, outSize) == 13) {
        return true;
    }
    charBufSet(out, outSize, raw);
    applyPhMobileNormalization(out, outSize);
    return isNormalizedPhMobile(out);
}

inline int extractPhoneNumber(const char* cnum, char* output, size_t outputSize) {
    if (!parseCnumResponseNumber(cnum, output, outputSize)) return 0;
    return isNormalizedPhMobile(output) ? 13 : (int)strlen(output);
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

inline int extractLastDigits(const char* input, int count, char* output, size_t outputSize) {
    if (!input || !output || outputSize < 2 || count < 1) return 0;

    char digits[32];
    int n = 0;
    for (const char* p = input; *p && n < (int)sizeof(digits) - 1; p++) {
        if (*p >= '0' && *p <= '9') {
            digits[n++] = *p;
        }
    }
    digits[n] = '\0';
    if (n < count) return 0;

    const char* start = digits + n - count;
    strncpy(output, start, outputSize - 1);
    output[outputSize - 1] = '\0';
    return count;
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

inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

inline bool looksLikeUcs2HexPayload(const char* s) {
    if (!s) return false;
    int hexCount = 0;
    int total = 0;
    for (const char* p = s; *p; p++) {
        const char c = *p;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        total++;
        if (hexNibble(c) >= 0) hexCount++;
    }
    return total >= 8 && (total % 4 == 0) && (hexCount * 100 / total) >= 95;
}

inline bool decodeUcs2HexMessage(const char* hexIn, char* out, size_t outSize) {
    if (!hexIn || !out || outSize < 2) return false;

    char compact[640];
    size_t clen = 0;
    for (const char* p = hexIn; *p && clen < sizeof(compact) - 1; p++) {
        const char c = *p;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        if (hexNibble(c) < 0) return false;
        compact[clen++] = c;
    }
    compact[clen] = '\0';
    if (clen < 4 || (clen % 4) != 0) return false;

    size_t outPos = 0;
    for (size_t i = 0; i + 3 < clen; i += 4) {
        const int h0 = hexNibble(compact[i]);
        const int h1 = hexNibble(compact[i + 1]);
        const int h2 = hexNibble(compact[i + 2]);
        const int h3 = hexNibble(compact[i + 3]);
        if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;
        const uint16_t codeUnit =
            (uint16_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
        if (codeUnit == 0) break;

        if (codeUnit < 0x80) {
            if (outPos + 1 >= outSize) return false;
            out[outPos++] = (char)codeUnit;
        } else if (codeUnit < 0x800) {
            if (outPos + 2 >= outSize) return false;
            out[outPos++] = (char)(0xC0 | (codeUnit >> 6));
            out[outPos++] = (char)(0x80 | (codeUnit & 0x3F));
        } else {
            if (outPos + 3 >= outSize) return false;
            out[outPos++] = (char)(0xE0 | (codeUnit >> 12));
            out[outPos++] = (char)(0x80 | ((codeUnit >> 6) & 0x3F));
            out[outPos++] = (char)(0x80 | (codeUnit & 0x3F));
        }
    }
    out[outPos] = '\0';
    return outPos > 0;
}

inline bool normalizeSmsBodyFromModem(char* message, size_t messageSize) {
    if (!message || messageSize < 2 || charBufIsEmpty(message)) return true;
    if (!looksLikeUcs2HexPayload(message)) return true;

    char decoded[320];
    if (!decodeUcs2HexMessage(message, decoded, sizeof(decoded))) {
        return false;
    }
    charBufSet(message, messageSize, decoded);
    return true;
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
