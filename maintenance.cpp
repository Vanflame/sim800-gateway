// Scheduled restart + periodic firmware update check (via backend maintenance API)

#include "maintenance.h"
#include "config.h"
#include "sms.h"
#include "ota.h"
#include "logger.h"
#include "utils.h"
#include "webui.h"

#include <WiFi.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>

extern ActiveSession activeSessions[8];
extern int activeSessionCount;

extern char agentBaseUrl[128];
extern char agentDeviceId[64];
extern volatile bool httpsBusy;
extern bool heartbeatPaused;
extern bool smsPollingPaused;

static unsigned long scheduledRestartAtMs = 0;
static unsigned long lastMaintenancePollMs = 0;
static unsigned long maintenanceDeferUntilMs = 0;
static unsigned long lastFirmwareCheckMs = 0;
static bool sessionsUpdatedSincePoll = false;
static bool restartInProgress = false;

static char cachedFwRemote[32] = "";
static bool cachedFwUpdateAvailable = false;

void maintenanceRecordFirmwareCheck(const char* remoteVersion, bool updateAvailable) {
    cachedFwUpdateAvailable = updateAvailable;
    if (remoteVersion && remoteVersion[0]) {
        charBufSet(cachedFwRemote, sizeof(cachedFwRemote), remoteVersion);
    } else {
        cachedFwRemote[0] = '\0';
    }
    lastFirmwareCheckMs = millis();
}

void maintenanceGetFirmwareCache(char* remoteOut, size_t remoteSize, bool* updateAvailableOut,
    unsigned long* lastCheckMsOut) {
    if (remoteOut && remoteSize > 0) {
        charBufSet(remoteOut, remoteSize, cachedFwRemote);
    }
    if (updateAvailableOut) {
        *updateAvailableOut = cachedFwUpdateAvailable;
    }
    if (lastCheckMsOut) {
        *lastCheckMsOut = lastFirmwareCheckMs;
    }
}

static unsigned long parseIso8601UtcToMs(const char* iso) {
    if (!iso || !iso[0]) return 0;
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) < 6) {
        return 0;
    }
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
    const time_t ts = days * 86400L + hour * 3600L + min * 60L + sec;
    if (ts <= 0) return 0;
    return (unsigned long)ts * 1000UL;
}

static void extractJsonStringValue(const char* json, const char* key, char* out, size_t outSize) {
    if (!out || outSize < 1) return;
    out[0] = '\0';
    if (!json || !key) return;

    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') {
        p++;
        size_t j = 0;
        while (*p && *p != '"' && j < outSize - 1) {
            out[j++] = *p++;
        }
        out[j] = '\0';
        return;
    }
    if (strncmp(p, "null", 4) == 0) {
        return;
    }
    size_t j = 0;
    while (*p && *p != ',' && *p != '}' && *p != ']' && j < outSize - 1) {
        out[j++] = *p++;
    }
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\n')) j--;
    out[j] = '\0';
}

static bool extractJsonBool(const char* json, const char* key, bool defaultVal) {
    char buf[12];
    extractJsonStringValue(json, key, buf, sizeof(buf));
    if (!buf[0]) return defaultVal;
    return (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0);
}

static unsigned long nowUtcMs() {
    const time_t nowSecs = time(nullptr);
    if (nowSecs <= 1600000000L) return 0;
    return (unsigned long)nowSecs * 1000UL;
}

static unsigned long latestActiveSessionExpiryMs() {
    pruneExpiredActiveSessions();
    unsigned long latest = 0;
    for (int i = 0; i < activeSessionCount; i++) {
        if (activeSessions[i].expiresAtMs > latest) {
            latest = activeSessions[i].expiresAtMs;
        }
    }
    return latest;
}

static bool maintenanceHttpPost(const char* body, char* respOut, size_t respSize) {
    if (!respOut || respSize < 2) return false;
    respOut[0] = '\0';
    if (charBufIsEmpty(agentBaseUrl) || charBufIsEmpty(agentDeviceId)) return false;
    if (!agentIsSignedIn()) return false;
    if (!WiFi.isConnected() || httpsBusy) return false;
    if (isSmsForwardPriorityActive()) return false;

    static char url[280];
    snprintf(url, sizeof(url), "%s/api/agent/device-maintenance", agentBaseUrl);

    const int code = agentHttpsPostJson(
        url, body, MAINTENANCE_HTTP_TIMEOUT_MS, true, respOut, respSize, "maint-poll");
    resumeSmsPolling();

    return code >= 200 && code < 300;
}

static void applyMaintenancePollResponse(const char* resp) {
    if (!resp || !resp[0]) return;

    char restartIso[40];
    extractJsonStringValue(resp, "scheduled_restart_at", restartIso, sizeof(restartIso));
    if (restartIso[0]) {
        const unsigned long ms = parseIso8601UtcToMs(restartIso);
        if (ms != scheduledRestartAtMs) {
            scheduledRestartAtMs = ms;
            logMsg2Val("[MAINT] Scheduled restart at", restartIso, "", "");
            appendMonitorLogVal("[MAINT] Scheduled restart", restartIso);
        }
    } else if (strstr(resp, "\"scheduled_restart_at\":null") != nullptr ||
               strstr(resp, "\"scheduled_restart_at\": null") != nullptr) {
        if (scheduledRestartAtMs != 0) {
            scheduledRestartAtMs = 0;
            appendMonitorLog("[MAINT] Scheduled restart cleared");
            logMsg("[MAINT] Scheduled restart cleared");
        }
    }

    const bool fwAvail = extractJsonBool(resp, "firmware_update_available", false);
    if (fwAvail) {
        static bool lastFwAvail = false;
        static char lastFwRemote[32] = "";
        static unsigned long lastFwLogMs = 0;

        char remoteVer[32];
        extractJsonStringValue(resp, "firmware_remote_version", remoteVer, sizeof(remoteVer));
        const bool changed = !lastFwAvail || strcmp(remoteVer, lastFwRemote) != 0;
        const unsigned long nowMs = millis();
        if (changed || (nowMs - lastFwLogMs) >= FIRMWARE_CHECK_INTERVAL_MS) {
            char detail[80];
            snprintf(detail, sizeof(detail), "%s (local %s)", remoteVer[0] ? remoteVer : "?",
                FIRMWARE_VERSION);
            logMsg2Val("[MAINT] Firmware update available", detail, "", "");
            appendMonitorLogVal("[MAINT] Update available", detail);
            lastFwAvail = true;
            charBufSet(lastFwRemote, sizeof(lastFwRemote), remoteVer);
            lastFwLogMs = nowMs;
        }
    }
}

static void maintenancePollBackend() {
    static char body[192];
    static char resp[768];

    snprintf(body, sizeof(body),
        "{\"device_id\":\"%s\",\"firmware_version\":\"%s\",\"action\":\"poll\"}",
        agentDeviceId, FIRMWARE_VERSION);
    logMsg("[MAINT] Backend poll starting");
    appendMonitorLog("[MAINT] Poll start");

    const unsigned long t0 = millis();
    const bool ok = maintenanceHttpPost(body, resp, sizeof(resp));
    const unsigned long elapsedMs = millis() - t0;

    if (ok) {
        applyMaintenancePollResponse(resp);
        lastMaintenancePollMs = millis();
        char doneBuf[56];
        snprintf(doneBuf, sizeof(doneBuf), "[MAINT] Backend poll OK (%lums)", elapsedMs);
        logMsg(doneBuf);
    } else {
        char failBuf[56];
        snprintf(failBuf, sizeof(failBuf), "[MAINT] Backend poll failed (%lums)", elapsedMs);
        logMsg(failBuf);
    }
}

static bool maintenancePostAction(const char* action, const char* extraJsonFields) {
    static char body[320];
    static char resp[256];

    if (extraJsonFields && extraJsonFields[0]) {
        snprintf(body, sizeof(body),
            "{\"device_id\":\"%s\",\"firmware_version\":\"%s\",\"action\":\"%s\",%s}",
            agentDeviceId, FIRMWARE_VERSION, action, extraJsonFields);
    } else {
        snprintf(body, sizeof(body),
            "{\"device_id\":\"%s\",\"firmware_version\":\"%s\",\"action\":\"%s\"}",
            agentDeviceId, FIRMWARE_VERSION, action);
    }

    return maintenanceHttpPost(body, resp, sizeof(resp));
}

static void maintenanceDeferRestart(unsigned long deferredUntilMs) {
    const time_t sec = (time_t)(deferredUntilMs / 1000UL);
    struct tm tmUtc;
    gmtime_r(&sec, &tmUtc);

    char iso[40];
    snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d",
        tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
        tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);

    char fields[96];
    snprintf(fields, sizeof(fields), "\"deferred_until\":\"%s\"", iso);

    if (maintenancePostAction("defer_restart", fields)) {
        scheduledRestartAtMs = deferredUntilMs;
        char logLine[64];
        snprintf(logLine, sizeof(logLine), "until %s (active session)", iso);
        logMsg2Val("[MAINT] Restart deferred", logLine, "", "");
        appendMonitorLogVal("[MAINT] Restart deferred", iso);
    }
}

static void maintenancePrepareScheduledRestart() {
    Preferences prefs;
    prefs.begin("agent", false);
    prefs.putBool("hb_pause", false);
    prefs.putBool("maint_boot", true);
    prefs.end();

    heartbeatPaused = false;
    smsPollingPaused = false;
}

static void maintenancePerformRestart() {
    if (restartInProgress) return;
    restartInProgress = true;

    logMsg("[MAINT] Scheduled restart — acknowledging backend");
    appendMonitorLog("[MAINT] Scheduled restart");

    maintenancePostAction("restart_ack", nullptr);
    maintenancePrepareScheduledRestart();

    delay(300);
    ESP.restart();
}

static void maintenanceMaybeRunScheduledRestart() {
    if (restartInProgress || scheduledRestartAtMs == 0) return;

    const unsigned long nowMs = nowUtcMs();
    if (nowMs == 0) return;
    if (nowMs < scheduledRestartAtMs) return;

    pruneExpiredActiveSessions();
    const unsigned long latestExpiry = latestActiveSessionExpiryMs();
    if (latestExpiry > nowMs) {
        const unsigned long deferredUntil = latestExpiry + (5UL * 60UL * 1000UL);
        if (deferredUntil > scheduledRestartAtMs) {
            maintenanceDeferRestart(deferredUntil);
        }
        return;
    }

    maintenancePerformRestart();
}

static void maintenanceLocalFirmwareCheck() {
#if OTA_ENABLED
    bool updateAvailable = false;
    char remoteVer[32];
    remoteVer[0] = '\0';
    if (otaCheckForUpdate(&updateAvailable, remoteVer, sizeof(remoteVer))) {
        maintenanceRecordFirmwareCheck(remoteVer, updateAvailable);
        if (updateAvailable) {
            static bool lastLocalFwAvail = false;
            static unsigned long lastLocalFwLogMs = 0;
            const bool changed = !lastLocalFwAvail;
            if (changed || (millis() - lastLocalFwLogMs) >= FIRMWARE_CHECK_INTERVAL_MS) {
                char detail[72];
                snprintf(detail, sizeof(detail), "%s (local %s)", remoteVer, FIRMWARE_VERSION);
                logMsg2Val("[MAINT] Firmware update available", detail, "", "");
                appendMonitorLogVal("[MAINT] Update available", detail);
                lastLocalFwAvail = true;
                lastLocalFwLogMs = millis();
            }
        }
    } else {
        maintenanceRecordFirmwareCheck("", false);
    }
#else
    maintenanceRecordFirmwareCheck("", false);
#endif
}

void maintenanceOnBoot() {
    Preferences prefs;
    prefs.begin("agent", true);
    const bool maintBoot = prefs.getBool("maint_boot", false);
    prefs.end();

    if (!maintBoot) return;

    prefs.begin("agent", false);
    prefs.putBool("maint_boot", false);
    prefs.putBool("hb_pause", false);
    prefs.end();

    heartbeatPaused = false;
    smsPollingPaused = false;

    logMsg("[MAINT] Post-restart defaults restored (HB on, SMS polling on)");
    appendMonitorLog("[MAINT] Defaults restored after scheduled restart");
}

void maintenanceOnHeartbeatSuccess() {
    // Maintenance API poll runs on MAINTENANCE_POLL_MIN_INTERVAL_MS via maintenanceTick()
    // (no extra HTTPS stacked right after heartbeat).
    sessionsUpdatedSincePoll = false;
}

bool maintenanceRunDeferredPollIfNeeded() {
    (void)0;
    return false;
}

void maintenanceOnSessionsUpdated() {
    sessionsUpdatedSincePoll = true;
    maintenanceMaybeRunScheduledRestart();
}

extern volatile bool otaInProgress;

void maintenanceTick(unsigned long nowMs) {
    if (otaInProgress || httpsBusy) return;
    if (cloudBackendDeferred()) return;
    if (isSmsForwardPriorityActive()) return;
    if (nowMs < maintenanceDeferUntilMs) return;

    maintenanceMaybeRunScheduledRestart();

    if (!WiFi.isConnected()) return;

    if (httpsStackNeedsSettle()) {
        maintenanceDeferUntilMs = nowMs + 12000UL;
        return;
    }

    const unsigned long lastPoll = getLastSmsPollActivityMs();
    if (lastPoll > 0 && (nowMs - lastPoll) < 3000UL) {
        maintenanceDeferUntilMs = nowMs + 15000UL;
        static unsigned long lastDeferLogMs = 0;
        if (nowMs - lastDeferLogMs >= 30000UL) {
            lastDeferLogMs = nowMs;
            logMsg("[MAINT] Poll deferred (SMS polling active)");
        }
        return;
    }

    bool ranMaintPoll = false;
#if MAINTENANCE_BACKEND_POLL_ENABLED
    if (agentIsSignedIn() && !charBufIsEmpty(agentBaseUrl) &&
        (nowMs - lastMaintenancePollMs) >= (unsigned long)MAINTENANCE_POLL_MIN_INTERVAL_MS) {
        maintenancePollBackend();
        ranMaintPoll = true;
    }
#else
    static bool maintPollOffLogged = false;
    if (!maintPollOffLogged && agentIsSignedIn()) {
        maintPollOffLogged = true;
        logMsg("[MAINT] Backend poll off (MAINTENANCE_BACKEND_POLL_ENABLED=0)");
    }
#endif

    if (!ranMaintPoll &&
        (lastFirmwareCheckMs == 0 ||
         (nowMs - lastFirmwareCheckMs) >= (unsigned long)FIRMWARE_CHECK_INTERVAL_MS)) {
        maintenanceLocalFirmwareCheck();
    }
}

bool maintenanceHasScheduledRestart() {
    return scheduledRestartAtMs > 0;
}

long maintenanceGetScheduledRestartInSec() {
    if (scheduledRestartAtMs == 0) return -1;
    const time_t nowSecs = time(nullptr);
    if (nowSecs <= 1600000000L) return -1;
    const unsigned long nowMs = (unsigned long)nowSecs * 1000UL;
    if (scheduledRestartAtMs <= nowMs) return 0;
    return (long)((scheduledRestartAtMs - nowMs + 999UL) / 1000UL);
}
