// ============================================================================
// Missed call → Viber-style SMS forward
// ============================================================================

#include "calls.h"
#include "sim800.h"
#include "mux.h"
#include "sms.h"
#include "logger.h"
#include "utils.h"
#include <Arduino.h>
#include <string.h>
#include <time.h>

extern void getFallbackTimeStamp(char* buf, size_t bufSize);
extern void appendSmsToFile(const char* time, int simSlot, const char* number, const char* sender, const char* message);
extern ActiveSession activeSessions[];
extern int activeSessionCount;

extern SimState simStates[];
extern CallLogItem callLog[];
extern int callLogCount;

static struct {
    char last6[7];
    unsigned long atMs;
} lastMissedFwd[SIM_COUNT];

static bool isViberAppName(const char* name) {
    if (!name || !name[0]) return false;
    char lower[32];
    size_t n = strlen(name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        lower[i] = c;
    }
    lower[n] = '\0';
    return strstr(lower, "viber") != NULL;
}

static bool isSessionStillActive(const ActiveSession* sess) {
    if (!sess || !sess->sessionId[0]) return false;
    if (sess->expiresAtMs == 0) return true;

    time_t nowSecs = time(nullptr);
    if (nowSecs > 1600000000L) {
        return sess->expiresAtMs > (unsigned long)nowSecs * 1000UL;
    }
    return true;
}

static int sessionSlotToSimIdx(int sessionSlot) {
    if (sessionSlot < 0) return -1;
    if (sessionSlot < SIM_COUNT) return sessionSlot;
    if (sessionSlot >= 1 && sessionSlot <= SIM_COUNT) return sessionSlot - 1;
    return -1;
}

int getPriorityMissedCallSlot() {
    if (missedCallWatchSlot >= 0 && missedCallWatchSlot < SIM_COUNT) {
        if (simStates[missedCallWatchSlot].enabled && simStates[missedCallWatchSlot].responsive) {
            return missedCallWatchSlot;
        }
    }

    int bestSlot = -1;
    unsigned long bestExpiry = 0;

    for (int i = 0; i < activeSessionCount; i++) {
        const ActiveSession* sess = &activeSessions[i];
        if (!isSessionStillActive(sess)) continue;
        if (!isViberAppName(sess->appName)) continue;

        const int slot = sessionSlotToSimIdx(sess->slot);
        if (slot < 0 || slot >= SIM_COUNT) continue;
        if (!simStates[slot].enabled || !simStates[slot].responsive) continue;

        if (bestSlot < 0 || (sess->expiresAtMs > 0 && (bestExpiry == 0 || sess->expiresAtMs < bestExpiry))) {
            bestSlot = slot;
            bestExpiry = sess->expiresAtMs;
        }
    }
    return bestSlot;
}

static void buildViberMissedCallMessage(const char* last6, char* out, size_t outSize) {
    snprintf(out, outSize,
        "Viber: Your verification code is %s. "
        "Missed call — the last 6 digits of the caller number are %s.",
        last6, last6);
}

static bool parseClipNumber(const char* buf, char* callerOut, size_t callerOutSize) {
    if (!buf || !callerOut || callerOutSize < 2) return false;
    callerOut[0] = '\0';

    const char* clip = strstr(buf, "+CLIP:");
    if (!clip) return false;

    const char* q1 = strchr(clip, '"');
    if (!q1) return false;
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return false;

    size_t len = (size_t)(q2 - q1 - 1);
    if (len >= callerOutSize) len = callerOutSize - 1;
    strncpy(callerOut, q1 + 1, len);
    callerOut[len] = '\0';
    return callerOut[0] != '\0';
}

static void appendCallLogEntry(int slot, const char* caller) {
    if (callLogCount >= MAX_CALL_LOG) {
        memmove(&callLog[0], &callLog[1], (size_t)(MAX_CALL_LOG - 1) * sizeof(CallLogItem));
        callLogCount = MAX_CALL_LOG - 1;
    }
    CallLogItem* e = &callLog[callLogCount++];
    charBufSet(e->caller, sizeof(e->caller), caller);
    if (slot >= 0 && slot < SIM_COUNT && simStates[slot].cops[0]) {
        extractOperatorName(simStates[slot].cops, e->network, sizeof(e->network));
    } else {
        charBufClear(e->network, sizeof(e->network));
    }
    if (!getLocalTimeStamp(e->ts, sizeof(e->ts))) {
        getFallbackTimeStamp(e->ts, sizeof(e->ts));
    }
    e->durationMs = 0;
}

static bool hasViberSessionOnSlot(int slot) {
    for (int i = 0; i < activeSessionCount; i++) {
        const ActiveSession* sess = &activeSessions[i];
        if (!isSessionStillActive(sess)) continue;
        if (!isViberAppName(sess->appName)) continue;
        const int sessSlot = sessionSlotToSimIdx(sess->slot);
        if (sessSlot == slot) return true;
    }
    return false;
}

typedef struct {
    bool used;
    int slot;
    char caller[CALLER_BUFFER_SIZE];
    char last6[8];
    unsigned long atMs;
} PendingMissedCall;

static PendingMissedCall pendingMissed[MISSED_CALL_PENDING_MAX];

static void pruneExpiredPendingMissed() {
    unsigned long now = millis();
    for (int i = 0; i < MISSED_CALL_PENDING_MAX; i++) {
        if (!pendingMissed[i].used) continue;
        if (now - pendingMissed[i].atMs > MISSED_CALL_PENDING_TTL_MS) {
            pendingMissed[i].used = false;
        }
    }
}

static bool shouldDedupeMissedForward(int slot, const char* last6) {
    if (slot < 0 || slot >= SIM_COUNT) return true;
    unsigned long now = millis();
    if (strcmp(lastMissedFwd[slot].last6, last6) == 0 &&
        (now - lastMissedFwd[slot].atMs) < 60000UL) {
        return true;
    }
    charBufSet(lastMissedFwd[slot].last6, sizeof(lastMissedFwd[slot].last6), last6);
    lastMissedFwd[slot].atMs = now;
    return false;
}

static void forwardMissedCallAsViber(int slot, const char* caller);

static void enqueuePendingMissed(int slot, const char* caller, const char* last6) {
    pruneExpiredPendingMissed();
    for (int i = 0; i < MISSED_CALL_PENDING_MAX; i++) {
        if (pendingMissed[i].used && pendingMissed[i].slot == slot &&
            strcmp(pendingMissed[i].last6, last6) == 0) {
            pendingMissed[i].atMs = millis();
            return;
        }
    }
    for (int i = 0; i < MISSED_CALL_PENDING_MAX; i++) {
        if (!pendingMissed[i].used) {
            pendingMissed[i].used = true;
            pendingMissed[i].slot = slot;
            pendingMissed[i].atMs = millis();
            charBufSet(pendingMissed[i].caller, sizeof(pendingMissed[i].caller), caller);
            charBufSet(pendingMissed[i].last6, sizeof(pendingMissed[i].last6), last6);
            char logLine[80];
            snprintf(logLine, sizeof(logLine), "[CALL] Queued missed call SIM%d ends %s", slot + 1, last6);
            logMsg(logLine);
            appendMonitorLog(logLine);
            return;
        }
    }
    unsigned long oldest = UINT32_MAX;
    int oldestIdx = 0;
    for (int i = 0; i < MISSED_CALL_PENDING_MAX; i++) {
        if (pendingMissed[i].used && pendingMissed[i].atMs < oldest) {
            oldest = pendingMissed[i].atMs;
            oldestIdx = i;
        }
    }
    pendingMissed[oldestIdx].used = true;
    pendingMissed[oldestIdx].slot = slot;
    pendingMissed[oldestIdx].atMs = millis();
    charBufSet(pendingMissed[oldestIdx].caller, sizeof(pendingMissed[oldestIdx].caller), caller);
    charBufSet(pendingMissed[oldestIdx].last6, sizeof(pendingMissed[oldestIdx].last6), last6);
    appendMonitorLog("[CALL] Pending queue full; replaced oldest");
}

void flushPendingMissedCallsAfterHeartbeat() {
    if (!missedCallForwardEnabled) return;
    pruneExpiredPendingMissed();
    for (int i = 0; i < MISSED_CALL_PENDING_MAX; i++) {
        if (!pendingMissed[i].used) continue;
        const int slot = pendingMissed[i].slot;
        if (!hasViberSessionOnSlot(slot)) continue;
        if (shouldDedupeMissedForward(slot, pendingMissed[i].last6)) {
            pendingMissed[i].used = false;
            continue;
        }
        char logLine[96];
        snprintf(logLine, sizeof(logLine), "[CALL] Queued missed -> Viber SMS SIM%d ends %s",
            slot + 1, pendingMissed[i].last6);
        logMsg(logLine);
        appendMonitorLog(logLine);
        forwardMissedCallAsViber(slot, pendingMissed[i].caller);
        pendingMissed[i].used = false;
    }
}

static void handleMissedCallDetected(int slot, const char* caller) {
    if (!missedCallForwardEnabled) return;
    if (slot < 0 || slot >= SIM_COUNT || !simStates[slot].enabled) return;

    char last6[8];
    if (extractLastDigits(caller, 6, last6, sizeof(last6)) < 6) {
        logMsgVal("[CALL] Missed call, no 6-digit caller ID", caller);
        appendMonitorLog("[CALL] Missed call (hidden/short number)");
        return;
    }

    if (hasViberSessionOnSlot(slot)) {
        if (shouldDedupeMissedForward(slot, last6)) return;
        forwardMissedCallAsViber(slot, caller);
        return;
    }

    enqueuePendingMissed(slot, caller, last6);
}

static void forwardMissedCallAsViber(int slot, const char* caller) {
    if (!missedCallForwardEnabled) return;
    if (slot < 0 || slot >= SIM_COUNT || !simStates[slot].enabled) return;

    char last6[8];
    if (extractLastDigits(caller, 6, last6, sizeof(last6)) < 6) {
        return;
    }

    if (shouldDedupeMissedForward(slot, last6)) {
        return;
    }

    char message[SMS_MESSAGE_SIZE];
    buildViberMissedCallMessage(last6, message, sizeof(message));

    appendCallLogEntry(slot, caller);

    SmsMessage msg = {};
    msg.simSlot = slot;
    msg.messageIndex = 0;
    charBufSet(msg.sender, sizeof(msg.sender), MISSED_CALL_VIBER_SENDER);
    charBufSet(msg.message, sizeof(msg.message), message);
    if (!getLocalTimeStamp(msg.timestamp, sizeof(msg.timestamp))) {
        getFallbackTimeStamp(msg.timestamp, sizeof(msg.timestamp));
    }

    char err[64] = "";
    appendSmsToFile(msg.timestamp, slot + 1, simStates[slot].number, MISSED_CALL_VIBER_SENDER, message);

    if (forwardSmsToBackendWithSender(&msg, MISSED_CALL_VIBER_SENDER, err, sizeof(err))) {
        char logLine[96];
        snprintf(logLine, sizeof(logLine), "[CALL] Missed call -> Viber SMS SIM%d ends %s", slot + 1, last6);
        logMsg(logLine);
        appendMonitorLog(logLine);
    } else {
        enqueuePendingSms(slot, simStates[slot].number, MISSED_CALL_VIBER_SENDER, message);
        char logLine[128];
        snprintf(logLine, sizeof(logLine), "[CALL] Missed call forward queued SIM%d: %s", slot + 1, err[0] ? err : "unknown");
        appendMonitorLog(logLine);
    }
}

void configureModemForMissedCallDetect() {
    sendATCapture("AT+CLIP=1", 800);
    sendATCapture("AT+GSMBUSY=0", 800);
    sendATCapture("AT+CRC=1", 800);
}

void configureModemCallBlockOnly() {
    sendATCapture("AT+CLIP=0", 800);
    sendATCapture("AT+GSMBUSY=1", 800);
}

void applyMissedCallModemToAllSims() {
    if (isSimBusy()) {
        logMsg("[CALL] Skip modem apply — SIM busy");
        return;
    }

    pauseSmsPolling(45000);
    setSimBusy(true);
    setSimYieldToWebServer(false);

    for (int i = 0; i < SIM_COUNT; i++) {
        if (!simStates[i].enabled || !simStates[i].responsive) continue;
        selectSIM(i);
        delay(MUX_SETTLE_MS / 2);
        if (missedCallForwardEnabled) {
            configureModemForMissedCallDetect();
        } else {
            configureModemCallBlockOnly();
        }
    }

    setSimBusy(false);
    setSimYieldToWebServer(true);
    resumeSmsPolling();

    logMsg(missedCallForwardEnabled ? "[CALL] Missed-call mode ON (all SIMs)" : "[CALL] Missed-call mode OFF (block calls)");
    appendMonitorLog(missedCallForwardEnabled ? "[CALL] Missed-call forward enabled" : "[CALL] Missed-call forward disabled");
}

void processCallUrcFromBuffer(int slot, const char* buf) {
    if (!missedCallForwardEnabled || !buf || buf[0] == '\0') return;
    if (strstr(buf, "RING") == NULL && strstr(buf, "+CLIP:") == NULL) return;

    char caller[CALLER_BUFFER_SIZE];
    if (!parseClipNumber(buf, caller, sizeof(caller))) {
        if (strstr(buf, "RING") != NULL) {
            logMsgInt("[CALL] RING on SIM", slot + 1);
            appendMonitorLogInt("[CALL] RING SIM", slot + 1);
        }
        return;
    }

    hangupCall();
    handleMissedCallDetected(slot, caller);
}

void processCallUrcDuringAtRead(int slot) {
    if (!missedCallForwardEnabled || slot < 0 || slot >= SIM_COUNT) return;
    processCallUrcFromBuffer(slot, getSimBuffer());
}

void drainUartForMissedCall(int slot) {
    if (!missedCallForwardEnabled || slot < 0 || slot >= SIM_COUNT) return;

    HardwareSerial& serial = simSerial();
    char acc[384];
    size_t len = 0;
    acc[0] = '\0';

    unsigned long t = millis();
    while (millis() - t < 80) {
        int n = 0;
        while (serial.available() && len < sizeof(acc) - 1 && n < 48) {
            acc[len++] = (char)serial.read();
            acc[len] = '\0';
            n++;
        }
        if (strstr(acc, "+CLIP:") != NULL) {
            processCallUrcFromBuffer(slot, acc);
            return;
        }
        delay(2);
    }
    if (strstr(acc, "RING") != NULL) {
        logMsgInt("[CALL] RING on SIM (pre-AT)", slot + 1);
    }
}

static bool listenUartAccumulate(int slot, char* urcBuf, size_t urcBufSize, unsigned long listenMs, bool extendOnRing) {
    if (!urcBuf || urcBufSize < 8) return false;

    HardwareSerial& serial = simSerial();
    size_t urcLen = strlen(urcBuf);
    bool sawRing = (strstr(urcBuf, "RING") != NULL);
    unsigned long start = millis();
    unsigned long deadline = start + listenMs;
    const unsigned long ringWaitMs = extendOnRing ? (unsigned long)MISSED_CALL_RING_WAIT_MS : 1500UL;

    while (millis() < deadline) {
        int n = 0;
        while (serial.available() && urcLen < urcBufSize - 1 && n < 48) {
            urcBuf[urcLen++] = (char)serial.read();
            urcBuf[urcLen] = '\0';
            n++;
        }
        if (!sawRing && strstr(urcBuf, "RING") != NULL) {
            sawRing = true;
            logMsgInt("[CALL] RING on SIM", slot + 1);
            appendMonitorLogInt("[CALL] RING SIM", slot + 1);
            if (extendOnRing) {
                deadline = millis() + ringWaitMs;
            }
        }
        if (strstr(urcBuf, "+CLIP:") != NULL) {
            processCallUrcFromBuffer(slot, urcBuf);
            return true;
        }
        delay(2);
    }
    return false;
}

void listenForCallUrc(int slot, unsigned long listenMs) {
    if (!missedCallForwardEnabled || listenMs == 0) return;

    const bool priority = (slot == getPriorityMissedCallSlot());
    char urcBuf[384];
    urcBuf[0] = '\0';
    listenUartAccumulate(slot, urcBuf, sizeof(urcBuf), listenMs, priority);
}

static int callScanNextSlot = 0;
static unsigned long lastCallScanMs = 0;
static unsigned long lastPriorityScanMs = 0;
static int lastLoggedPrioritySlot = -2;

static void scanSlotForCalls(int slot, unsigned long listenMs, bool extendOnRing) {
    if (slot < 0 || slot >= SIM_COUNT) return;
    if (!simStates[slot].enabled || !simStates[slot].responsive) return;

    selectSIM(slot);
    delay(50);

    char urcBuf[384];
    urcBuf[0] = '\0';
    listenUartAccumulate(slot, urcBuf, sizeof(urcBuf), listenMs, extendOnRing);
}

void pollMissedCallsFast() {
    if (!missedCallForwardEnabled) return;
    if (isSimBusy()) return;

    const int prioritySlot = getPriorityMissedCallSlot();
    if (prioritySlot != lastLoggedPrioritySlot) {
        lastLoggedPrioritySlot = prioritySlot;
        if (prioritySlot >= 0) {
            appendMonitorLogInt("[CALL] Viber OTP watch SIM", prioritySlot + 1);
            logMsgInt("[CALL] Viber OTP session — priority watch SIM", prioritySlot + 1);
        }
    }

    const unsigned long now = millis();

    if (prioritySlot >= 0 && (now - lastPriorityScanMs) >= (unsigned long)MISSED_CALL_PRIORITY_INTERVAL_MS) {
        lastPriorityScanMs = now;
        lastCallScanMs = now;
        scanSlotForCalls(prioritySlot, MISSED_CALL_PRIORITY_LISTEN_MS, true);
        return;
    }

    if (now - lastCallScanMs < (unsigned long)MISSED_CALL_SCAN_INTERVAL_MS) {
        return;
    }
    lastCallScanMs = now;

    for (int n = 0; n < SIM_COUNT; n++) {
        int slot = (callScanNextSlot + n) % SIM_COUNT;
        callScanNextSlot = (slot + 1) % SIM_COUNT;

        if (slot == prioritySlot) {
            continue;
        }
        if (!simStates[slot].enabled || !simStates[slot].responsive) {
            continue;
        }

        scanSlotForCalls(slot, MISSED_CALL_SCAN_LISTEN_MS, false);
        return;
    }
}
