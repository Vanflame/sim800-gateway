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
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
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

#ifdef OTA_FIRMWARE_URL_DEFAULT
    snprintf(buf, bufSize, "%s", OTA_FIRMWARE_URL_DEFAULT);
    return true;
#elif !OTA_ENABLED
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
        if (!charBufIsEmpty(otaFirmwareUrl)) {
            otaSaveUrlToPreferences(otaFirmwareUrl);
        }
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

    if (!charBufIsEmpty(OTA_VERSION_URL)) {
        return fetchTextUrl(OTA_VERSION_URL, version, versionSize);
    }
#if OTA_ENABLED
    if (!charBufIsEmpty(OTA_GITHUB_OWNER) && !charBufIsEmpty(OTA_GITHUB_REPO)) {
        static char url[192];
        snprintf(url, sizeof(url),
            "https://raw.githubusercontent.com/%s/%s/main/firmware/version.txt",
            OTA_GITHUB_OWNER,
            OTA_GITHUB_REPO);
        return fetchTextUrl(url, version, versionSize);
    }
#endif
    return false;
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

// ESP32 application image (from Export compiled Binary) starts with 0xE9
static const uint8_t ESP_APP_IMAGE_MAGIC = 0xE9;
static const size_t OTA_MIN_APP_BYTES = 100000;  // real builds are usually 800KB+

static bool httpGetRangePrefix(const char* url, uint8_t* buf, size_t bufLen, size_t* outRead, int* outCode) {
    if (!url || !buf || bufLen < 1 || !outRead) return false;
    *outRead = 0;
    if (outCode) *outCode = -1;

    if (!WiFi.isConnected()) return false;

    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    const bool isHttps = (strncmp(url, "https://", 8) == 0);

    if (isHttps) {
        clientSecure.setInsecure();
        clientSecure.setTimeout(20000);
        if (!http.begin(clientSecure, url)) return false;
    } else {
        if (!http.begin(client, url)) return false;
    }

    http.setTimeout(20000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("Range", "bytes=0-15");

    const int code = http.GET();
    if (outCode) *outCode = code;

    if (code != 200 && code != 206) {
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (stream) {
        while (*outRead < bufLen && stream->available()) {
            int c = stream->read();
            if (c < 0) break;
            buf[(*outRead)++] = (uint8_t)c;
        }
    }

    http.end();
    return *outRead > 0;
}

// Validate URL serves a real ESP32 app .bin (not partitions/bootloader/HTML)
static bool validateOtaFirmwareUrl(const char* url, char* errorOut, size_t errorOutSize) {
    if (errorOut && errorOutSize > 0) errorOut[0] = '\0';

    uint8_t header[16];
    size_t read = 0;
    int code = -1;

    if (!httpGetRangePrefix(url, header, sizeof(header), &read, &code)) {
        if (errorOut && errorOutSize > 0) {
            if (code == 404) {
                snprintf(errorOut, errorOutSize, "Firmware URL not found (404). Check release asset name.");
            } else if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                snprintf(
                    errorOut,
                    errorOutSize,
                    "Redirect not followed (HTTP %d). Use direct .bin URL or reflash with redirect fix",
                    code
                );
            } else {
                snprintf(errorOut, errorOutSize, "Cannot download firmware (HTTP %d)", code);
            }
        }
        return false;
    }

    if (header[0] != ESP_APP_IMAGE_MAGIC) {
        if (errorOut && errorOutSize > 0) {
            snprintf(
                errorOut,
                errorOutSize,
                "Wrong file type (not ESP32 app .bin). Upload sim800_gateway.ino.bin from Export compiled Binary — not partitions, bootloader, or merged.bin"
            );
        }
        logMsgInt("[OTA] Bad magic byte", header[0]);
        return false;
    }

    // Full size check via HEAD
    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    const bool isHttps = (strncmp(url, "https://", 8) == 0);

    if (isHttps) {
        clientSecure.setInsecure();
        if (!http.begin(clientSecure, url)) return true;  // magic ok, skip size
    } else {
        if (!http.begin(client, url)) return true;
    }

    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    const int headCode = http.sendRequest("HEAD");
    int contentLen = http.getSize();
    http.end();

    if (headCode > 0 && contentLen > 0 && (size_t)contentLen < OTA_MIN_APP_BYTES) {
        if (errorOut && errorOutSize > 0) {
            snprintf(
                errorOut,
                errorOutSize,
                "File too small (%d bytes). Use sim800_gateway.ino.bin (~1MB), not partitions/bootloader",
                contentLen
            );
        }
        logMsgInt("[OTA] Firmware too small", contentLen);
        return false;
    }

    return true;
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

    static char validateErr[160];
    if (!validateOtaFirmwareUrl(updateUrl, validateErr, sizeof(validateErr))) {
        logMsgVal("[OTA] Preflight failed", validateErr);
        appendMonitorLogVal("[OTA] Preflight failed", validateErr);
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "%s", validateErr);
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
        if (strstr(err, "Verify Bin Header") != nullptr) {
            snprintf(
                errorOut,
                errorOutSize,
                "Verify Bin Header Failed — release must be sim800_gateway.ino.bin (Export compiled Binary), not partitions/merged"
            );
        } else {
            snprintf(errorOut, errorOutSize, "%s", err);
        }
    }
    return false;

#endif // OTA_ENABLED
}
