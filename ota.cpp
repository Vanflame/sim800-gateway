// ============================================================================
// HTTPS OTA firmware updates (GitHub Releases or custom URL)
// ============================================================================

#include "ota.h"
#include "logger.h"
#include "utils.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>

char otaFirmwareUrl[256] = "";

extern void appendMonitorLog(const char* msg);
extern void appendMonitorLogVal(const char* msg, const char* val);
extern volatile bool httpsBusy;
extern bool smsPollingPaused;
extern bool heartbeatPaused;

static bool fetchTextUrl(const char* url, char* out, size_t outSize) {
    if (!out || outSize < 2 || charBufIsEmpty(url)) return false;
    out[0] = '\0';

    if (!WiFi.isConnected()) return false;

    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    const bool isHttps = (strncmp(url, "https://", 8) == 0);

    if (isHttps) {
        clientSecure.setInsecure();
        clientSecure.setTimeout(15000);
        if (!http.begin(clientSecure, url)) return false;
    } else {
        if (!http.begin(client, url)) return false;
    }

    http.setTimeout(15000);
    const int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    body.trim();
    if (body.length() == 0 || body.length() >= (int)outSize) return false;
    charBufSet(out, outSize, body.c_str());
    return true;
}

bool otaBuildDefaultUrl(char* buf, size_t bufSize) {
    if (!buf || bufSize < 32) return false;

#if !OTA_ENABLED
    return false;
#else
    if (charBufIsEmpty(OTA_GITHUB_OWNER) || charBufIsEmpty(OTA_GITHUB_REPO)) {
        return false;
    }
    snprintf(buf, bufSize,
        "https://github.com/%s/%s/releases/latest/download/%s",
        OTA_GITHUB_OWNER,
        OTA_GITHUB_REPO,
        OTA_FIRMWARE_BIN);
    return true;
#endif
}

void otaLoadUrlFromPreferences() {
    Preferences prefs;
    prefs.begin("ota", true);
    prefs.getString("url", otaFirmwareUrl, sizeof(otaFirmwareUrl));
    prefs.end();

    if (charBufIsEmpty(otaFirmwareUrl)) {
        otaBuildDefaultUrl(otaFirmwareUrl, sizeof(otaFirmwareUrl));
    }
}

void otaSaveUrlToPreferences(const char* url) {
    if (!url) return;
    charBufSet(otaFirmwareUrl, sizeof(otaFirmwareUrl), url);
    Preferences prefs;
    prefs.begin("ota", false);
    prefs.putString("url", otaFirmwareUrl);
    prefs.end();
}

bool otaFetchRemoteVersion(char* version, size_t versionSize) {
    if (!version || versionSize < 2) return false;

#if !OTA_ENABLED
    return false;
#else
    if (!charBufIsEmpty(OTA_VERSION_URL)) {
        return fetchTextUrl(OTA_VERSION_URL, version, versionSize);
    }
    if (!charBufIsEmpty(OTA_GITHUB_OWNER) && !charBufIsEmpty(OTA_GITHUB_REPO)) {
        static char url[192];
        snprintf(url, sizeof(url),
            "https://raw.githubusercontent.com/%s/%s/main/firmware/version.txt",
            OTA_GITHUB_OWNER,
            OTA_GITHUB_REPO);
        return fetchTextUrl(url, version, versionSize);
    }
    return false;
#endif
}

static int parseVersionPart(const char** p) {
    if (!p || !*p) return 0;
    while (**p == ' ' || **p == '\t' || **p == 'v' || **p == 'V') (*p)++;
    int v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    if (**p == '.') (*p)++;
    return v;
}

static bool versionNewer(const char* remote, const char* local) {
    if (!remote || !local) return false;
    const char* r = remote;
    const char* l = local;
    for (int i = 0; i < 4; i++) {
        const int rv = parseVersionPart(&r);
        const int lv = parseVersionPart(&l);
        if (rv > lv) return true;
        if (rv < lv) return false;
    }
    return false;
}

bool otaCheckForUpdate(bool* updateAvailable, char* remoteVersion, size_t remoteSize) {
    if (updateAvailable) *updateAvailable = false;
    if (!remoteVersion || remoteSize < 2) return false;

    remoteVersion[0] = '\0';
    if (!otaFetchRemoteVersion(remoteVersion, remoteSize)) {
        return false;
    }

    if (updateAvailable) {
        *updateAvailable = versionNewer(remoteVersion, FIRMWARE_VERSION);
    }
    return true;
}

bool otaPerformUpdate(const char* url, char* errorOut, size_t errorOutSize) {
    if (errorOut && errorOutSize > 0) errorOut[0] = '\0';

#if !OTA_ENABLED
    if (errorOut && errorOutSize > 0) {
        snprintf(errorOut, errorOutSize, "OTA disabled in build");
    }
    return false;
#else

    static char updateUrl[256];
    if (url && url[0]) {
        charBufSet(updateUrl, sizeof(updateUrl), url);
    } else if (!charBufIsEmpty(otaFirmwareUrl)) {
        charBufSet(updateUrl, sizeof(updateUrl), otaFirmwareUrl);
    } else if (!otaBuildDefaultUrl(updateUrl, sizeof(updateUrl))) {
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "No firmware URL configured");
        }
        return false;
    }

    if (!WiFi.isConnected()) {
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "WiFi not connected");
        }
        return false;
    }

    logMsg2Val("[OTA] Starting update from", updateUrl, "", "");
    appendMonitorLog("[OTA] Download started");

    smsPollingPaused = true;
    heartbeatPaused = true;
    httpsBusy = true;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(120000);

    httpUpdate.rebootOnUpdate(true);
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    const t_httpUpdate_return ret = httpUpdate.update(client, updateUrl);

    httpsBusy = false;
    smsPollingPaused = false;
    heartbeatPaused = false;

    if (ret == HTTP_UPDATE_OK) {
        appendMonitorLog("[OTA] Success, rebooting");
        return true;  // rebootOnUpdate should restart before return
    }

    const char* err = httpUpdate.getLastErrorString().c_str();
    logMsgVal("[OTA] Failed", err);
    appendMonitorLogVal("[OTA] Failed", err);
    if (errorOut && errorOutSize > 0) {
        snprintf(errorOut, errorOutSize, "%s", err);
    }
    return false;

#endif // OTA_ENABLED
}
