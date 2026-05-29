// ============================================================================
// HTTPS OTA firmware updates (GitHub Releases or custom URL)
// ============================================================================

#include "ota.h"
#include "sms.h"
#include "logger.h"
#include "utils.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <string.h>
#if OTA_ENABLED
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_err.h>
#endif

char otaFirmwareUrl[256] = "";
volatile bool otaInProgress = false;

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

// GitHub has two common URL shapes for "latest release asset":
// - correct: https://github.com/<owner>/<repo>/releases/latest/download/<asset>
// - common wrong: https://github.com/<owner>/<repo>/releases/download/latest/<asset>
// Normalize the wrong one so OTA keeps working even if a device has an old URL saved in NVS.
static bool normalizeGithubLatestAssetUrl(char* url, size_t urlSize) {
    if (!url || urlSize < 2 || !url[0]) return false;

    const char* needle = "/releases/download/latest/";
    const char* repl   = "/releases/latest/download/";

    char* pos = strstr(url, needle);
    if (!pos) return false;

    const char* suffix = pos + strlen(needle);
    const size_t prefixLen = (size_t)(pos - url);

    char buf[256];
    if (prefixLen + strlen(repl) + strlen(suffix) + 1 >= sizeof(buf)) return false;

    // prefix + repl + suffix
    snprintf(buf, sizeof(buf), "%.*s%s%s",
             (int)prefixLen, url, repl, suffix);

    if (strlen(buf) >= urlSize) return false;
    strncpy(url, buf, urlSize);
    url[urlSize - 1] = '\0';
    return true;
}

// Fix a common GitHub URL typo:
// wrong:   /releases/<tag>/download/<asset>
// correct: /releases/download/<tag>/<asset>
//
// Do NOT touch the "latest release" shape:
// correct: /releases/latest/download/<asset>
static bool normalizeGithubTagDownloadAssetUrl(char* url, size_t urlSize) {
    if (!url || urlSize < 2 || !url[0]) return false;
    // If it already uses the correct "download/<tag>" segment, nothing to do.
    if (strstr(url, "/releases/download/") != nullptr) return false;

    const char* rel = strstr(url, "/releases/");
    if (!rel) return false;

    const char* afterRel = rel + strlen("/releases/");
    const char* downloadSeg = strstr(afterRel, "/download/");
    if (!downloadSeg) return false;

    const size_t tagLen = (size_t)(downloadSeg - afterRel);
    if (tagLen == 0) return false;

    char tag[64];
    if (tagLen >= sizeof(tag)) return false;
    snprintf(tag, sizeof(tag), "%.*s", (int)tagLen, afterRel);

    // Keep "latest" special-cased (it's /releases/latest/download/<asset>).
    if (strcmp(tag, "latest") == 0) return false;

    const char* asset = downloadSeg + strlen("/download/");
    if (!asset || !asset[0]) return false;

    // Build:
    // prefix: up to "/releases/"
    const size_t prefixLen = (size_t)(rel - url) + strlen("/releases/");

    char buf[256];
    if (prefixLen + strlen("download/") + strlen(tag) + 1 + strlen(asset) + 1 >= sizeof(buf)) return false;
    snprintf(buf, sizeof(buf), "%.*sdownload/%s/%s",
             (int)prefixLen, url, tag, asset);

    if (strlen(buf) >= urlSize) return false;
    strncpy(url, buf, urlSize);
    url[urlSize - 1] = '\0';
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

#if OTA_ENABLED
static size_t otaGetUpdatePartitionBytes() {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    return part ? part->size : 0;
}

// Returns false if firmware cannot fit the inactive OTA app slot.
static bool otaFirmwareFitsUpdateSlot(int firmwareBytes, char* errorOut, size_t errorOutSize) {
    if (errorOut && errorOutSize > 0) {
        errorOut[0] = '\0';
    }
    if (firmwareBytes <= 0) {
        return true;
    }

    const size_t slotBytes = otaGetUpdatePartitionBytes();
    const esp_partition_t* running = esp_ota_get_running_partition();
    char info[160];
    snprintf(
        info,
        sizeof(info),
        "[OTA] running=%s fw=%d slot=%u",
        running ? running->label : "?",
        firmwareBytes,
        (unsigned)slotBytes
    );
    logMsg(info);
    appendMonitorLog(info);

    if (slotBytes == 0) {
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "No OTA app partition on device");
        }
        return false;
    }

    if ((size_t)firmwareBytes > slotBytes) {
        if (errorOut && errorOutSize > 0) {
            snprintf(
                errorOut,
                errorOutSize,
                "Firmware %d bytes > OTA slot %u. Flash with Tools -> Minimal SPIFFS (1.9MB APP with OTA), then upload a smaller .bin or rebuild.",
                firmwareBytes,
                (unsigned)slotBytes
            );
        }
        logMsgInt("[OTA] Firmware too large for OTA slot", firmwareBytes);
        return false;
    }

    return true;
}
#endif

#if OTA_ENABLED
// HTTPClient + WiFiClientSecure must NOT live on loopTask stack (~8KB) — use static globals.
static HTTPClient gOtaHttp;
static WiFiClientSecure gOtaTls;
static unsigned long gOtaProgLastMs = 0;

static void otaReleaseSession() {
    gOtaHttp.end();
    gOtaTls.stop();
    delay(100);
}

static void otaCooldownMs(unsigned long ms) {
    WiFi.setSleep(WIFI_PS_NONE);
    const unsigned long until = millis() + ms;
    while ((long)(until - millis()) > 0) {
        yield();
        delay(10);
    }
    if (WiFi.isConnected()) {
        char buf[40];
        snprintf(buf, sizeof(buf), "[OTA] WiFi RSSI=%d", WiFi.RSSI());
        logMsg(buf);
    } else {
        logMsg("[OTA] WiFi disconnected");
    }
}

static void otaLogDnsForUrl(const char* url) {
    if (!url || strncmp(url, "https://", 8) != 0) {
        return;
    }
    const char* hostStart = url + 8;
    const char* pathStart = strchr(hostStart, '/');
    char host[96];
    if (pathStart) {
        const size_t hostLen = (size_t)(pathStart - hostStart);
        if (hostLen == 0 || hostLen >= sizeof(host)) {
            return;
        }
        memcpy(host, hostStart, hostLen);
        host[hostLen] = '\0';
    } else {
        charBufSet(host, sizeof(host), hostStart);
    }
    IPAddress ip;
    if (WiFi.hostByName(host, ip)) {
        char buf[80];
        snprintf(
            buf,
            sizeof(buf),
            "[OTA] DNS %s -> %u.%u.%u.%u",
            host,
            (unsigned)ip[0],
            (unsigned)ip[1],
            (unsigned)ip[2],
            (unsigned)ip[3]
        );
        logMsg(buf);
    } else {
        logMsgVal("[OTA] DNS failed for", host);
    }
}

static void otaLogGetFailure(int code) {
    logMsgInt("[OTA] HTTP GET failed", code);
    if (code == -1) {
        logMsg("[OTA] GET detail: connection error");
    } else if (code == -2) {
        logMsg("[OTA] GET detail: timeout");
    } else if (code == -11) {
        logMsg("[OTA] GET detail: read timeout");
    }
}

static void otaOnProgress(size_t current, size_t total) {
    const unsigned long now = millis();
    if (now - gOtaProgLastMs < 3000UL) {
        return;
    }
    gOtaProgLastMs = now;
    const unsigned pct = (total > 0) ? (unsigned)((current * 100UL) / total) : 0UL;
    char pbuf[80];
    snprintf(pbuf, sizeof(pbuf), "[OTA] %u / %u (%u%%)", (unsigned)current, (unsigned)total, pct);
    logMsg(pbuf);
}

static void otaPrepareTlsClient() {
    gOtaTls.setInsecure();
    // Per-read timeout (ms) — not connect timeout.
    gOtaTls.setTimeout(60000);
}

static void otaConfigureHttpClient() {
    gOtaHttp.setTimeout(600000);
    gOtaHttp.setConnectTimeout(30000);
    gOtaHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    gOtaHttp.setReuse(false);
    gOtaHttp.addHeader("Connection", "close");
    gOtaHttp.setUserAgent("sim800-gateway-ota/1.0");
}

// Standard Arduino OTA: HTTPClient GET + Update.writeStream (same as ESP32 docs / examples).
static bool otaStreamDownloadAndFlash(const char* url, char* errorOut, size_t errorOutSize) {
    if (errorOut && errorOutSize > 0) {
        errorOut[0] = '\0';
    }

    const wifi_ps_type_t prevWifiPs = WiFi.getSleep();
    WiFi.setSleep(WIFI_PS_NONE);

    if (!WiFi.isConnected()) {
        WiFi.setSleep(prevWifiPs);
        if (errorOut) {
            snprintf(errorOut, errorOutSize, "WiFi not connected");
        }
        return false;
    }

    otaCooldownMs(500);
    otaLogDnsForUrl(url);
    logMsg("[OTA] HTTP GET + Update.writeStream");

    int lastCode = -1;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            logMsgInt("[OTA] Retry", attempt + 1);
            Update.abort();
            otaCooldownMs(3000);
        }

        otaReleaseSession();
        otaPrepareTlsClient();

        if (!gOtaHttp.begin(gOtaTls, url)) {
            logMsg("[OTA] http.begin failed");
            continue;
        }
        otaConfigureHttpClient();

        logMsg("[OTA] Connecting...");
        const int code = gOtaHttp.GET();
        lastCode = code;
        if (code != HTTP_CODE_OK) {
            otaLogGetFailure(code);
            otaReleaseSession();
            continue;
        }

        const int contentLen = gOtaHttp.getSize();
        if (contentLen <= 0) {
            logMsg("[OTA] No Content-Length");
            otaReleaseSession();
            continue;
        }

        {
            char buf[80];
            snprintf(buf, sizeof(buf), "[OTA] Firmware size=%d bytes", contentLen);
            logMsg(buf);
        }

        if ((size_t)contentLen < OTA_MIN_APP_BYTES) {
            if (errorOut) {
                snprintf(errorOut, errorOutSize, "File too small (%d bytes)", contentLen);
            }
            otaReleaseSession();
            WiFi.setSleep(prevWifiPs);
            return false;
        }

        if (!otaFirmwareFitsUpdateSlot(contentLen, errorOut, errorOutSize)) {
            otaReleaseSession();
            WiFi.setSleep(prevWifiPs);
            return false;
        }

        if (!Update.begin(contentLen, U_FLASH)) {
            logMsgVal("[OTA] Update.begin failed", Update.errorString());
            if (errorOut) {
                snprintf(errorOut, errorOutSize, "Update.begin: %s", Update.errorString());
            }
            otaReleaseSession();
            continue;
        }

        WiFiClient* stream = gOtaHttp.getStreamPtr();
        if (!stream) {
            Update.abort();
            otaReleaseSession();
            if (errorOut) {
                snprintf(errorOut, errorOutSize, "No HTTP stream");
            }
            continue;
        }

        gOtaProgLastMs = 0;
        Update.onProgress(otaOnProgress);
        logMsg("[OTA] Downloading...");
        appendMonitorLog("[OTA] Downloading");

        const size_t written = Update.writeStream(*stream);
        otaReleaseSession();

        if (written != (size_t)contentLen) {
            logMsgInt("[OTA] writeStream short", (int)written);
            Update.abort();
            continue;
        }

        logMsg("[OTA] Verifying...");
        if (!Update.end(true)) {
            logMsgVal("[OTA] Update.end failed", Update.errorString());
            if (errorOut) {
                snprintf(errorOut, errorOutSize, "Update.end: %s", Update.errorString());
            }
            Update.abort();
            continue;
        }

        WiFi.setSleep(prevWifiPs);
        logMsg("[OTA] Success, rebooting");
        appendMonitorLog("[OTA] Flash OK, rebooting");
        delay(500);
        ESP.restart();
        return true;
    }

    if (errorOut) {
        snprintf(errorOut, errorOutSize, "HTTP GET failed (%d)", lastCode);
    }
    WiFi.setSleep(prevWifiPs);
    return false;
}
#endif

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

    // HEAD only (skip extra Range GET — saves RAM/time; magic checked on download stream).
    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    const bool isHttps = (strncmp(url, "https://", 8) == 0);

    if (isHttps) {
        clientSecure.setInsecure();
        if (!http.begin(clientSecure, url)) {
            // Magic ok (Range read succeeded), but we can't do HEAD for size.
            logMsg("[OTA] Preflight size check skipped (http.begin HEAD failed)");
            return true;
        }
    } else {
        if (!http.begin(client, url)) {
            logMsg("[OTA] Preflight size check skipped (http.begin HEAD failed)");
            return true;
        }
    }

    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    const int headCode = http.sendRequest("HEAD");
    int contentLen = http.getSize();
    http.end();

    if (headCode == 404) {
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "Firmware URL not found (404)");
        }
        return false;
    }

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

    // For debugging: show we reached the size check.
    if (headCode == 302 || headCode == 301 || headCode == 307 || headCode == 308) {
        if (errorOut && errorOutSize > 0) {
            snprintf(
                errorOut,
                errorOutSize,
                "HEAD redirect (%d). Use direct HTTPS .bin URL (e.g. Supabase), not GitHub HTML redirect",
                headCode
            );
        }
        return false;
    }

    if (headCode != 200 || contentLen <= 0) {
        if (errorOut && errorOutSize > 0) {
            snprintf(
                errorOut,
                errorOutSize,
                "HEAD failed (%d) or unknown size. Use direct firmware.bin URL",
                headCode
            );
        }
        return false;
    }

    if (headCode >= 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "[OTA] Preflight OK HEAD=%d len=%d", headCode, contentLen);
        logMsg(buf);
        appendMonitorLog(buf);
    }

#if OTA_ENABLED
    if (headCode == 200 && contentLen > 0) {
        static char fitErr[200];
        if (!otaFirmwareFitsUpdateSlot(contentLen, fitErr, sizeof(fitErr))) {
            if (errorOut && errorOutSize > 0) {
                snprintf(errorOut, errorOutSize, "%s", fitErr);
            }
            logMsgVal("[OTA] Preflight failed", fitErr);
            appendMonitorLogVal("[OTA] Preflight failed", fitErr);
            return false;
        }
    }
#endif

    return true;
}

bool otaVersionIsNewer(const char* a, const char* b) {
    return versionNewer(a, b);
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

    otaInProgress = true;
    smsPollingPaused = true;
    heartbeatPaused = true;
    httpsBusy = true;
    logMsg("[OTA] Background tasks paused for download");
    appendMonitorLog("[OTA] Background tasks paused");

#if !OTA_ENABLED
    otaInProgress = false;
    httpsBusy = false;
    smsPollingPaused = false;
    heartbeatPaused = false;
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
        otaInProgress = false;
        httpsBusy = false;
        smsPollingPaused = false;
        heartbeatPaused = false;
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "No firmware URL configured");
        }
        return false;
    }

    // Fix older/stored URLs that use the wrong GitHub "download/latest" shape.
    if (normalizeGithubLatestAssetUrl(updateUrl, sizeof(updateUrl))) {
        logMsgVal("[OTA] Normalized update URL to", updateUrl);
        appendMonitorLogVal("[OTA] Normalized update URL", updateUrl);
    }
    // Fix malformed /releases/<tag>/download/<asset> URLs.
    normalizeGithubTagDownloadAssetUrl(updateUrl, sizeof(updateUrl));

    if (!WiFi.isConnected()) {
        otaInProgress = false;
        httpsBusy = false;
        smsPollingPaused = false;
        heartbeatPaused = false;
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "WiFi not connected");
        }
        return false;
    }

#if OTA_PREFLIGHT_HEAD
    static char validateErr[160];
    if (!validateOtaFirmwareUrl(updateUrl, validateErr, sizeof(validateErr))) {
        otaInProgress = false;
        httpsBusy = false;
        smsPollingPaused = false;
        heartbeatPaused = false;
        logMsgVal("[OTA] Preflight failed", validateErr);
        appendMonitorLogVal("[OTA] Preflight failed", validateErr);
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "%s", validateErr);
        }
        return false;
    }
    otaCooldownMs(800);
#else
    logMsg("[OTA] Single HTTPS GET (no HEAD preflight)");
#endif

    logMsg2Val("[OTA] Starting update from", updateUrl, "", "");
    appendMonitorLog("[OTA] Download started");
    {
        char buf[128];
        snprintf(
            buf,
            sizeof(buf),
            "[OTA] Free heap=%u largest=%u",
            (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap()
        );
        logMsg(buf);
        appendMonitorLog(buf);
    }

    static char otaErr[160];
    const bool ok = otaStreamDownloadAndFlash(updateUrl, otaErr, sizeof(otaErr));

    otaInProgress = false;
    httpsBusy = false;
    smsPollingPaused = false;
    heartbeatPaused = false;
    resumeSmsPolling();

    if (ok) {
        return true;
    }

    logMsgVal("[OTA] Failed", otaErr[0] ? otaErr : "unknown");
    logMsg("[OTA] SMS polling resumed");
    appendMonitorLogVal("[OTA] Failed", otaErr[0] ? otaErr : "unknown");
    if (errorOut && errorOutSize > 0) {
        snprintf(errorOut, errorOutSize, "%s", otaErr[0] ? otaErr : "OTA failed");
    }
    return false;

#endif // OTA_ENABLED
}
