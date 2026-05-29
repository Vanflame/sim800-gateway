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
// Fixed BSS buffer — use readBytes() to fill each chunk (faster than polling available()).
static uint8_t otaDlBuffer[OTA_DL_BUFFER_SIZE];
static const size_t OTA_DL_CHUNK = sizeof(otaDlBuffer);
static const unsigned long OTA_DL_STALL_MS = 120000;

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

static void otaLogGetFailure(HTTPClient& http, int code) {
    logMsgInt("[OTA] HTTP GET failed", code);
    const String err = http.errorToString(code);
    if (err.length() > 0) {
        logMsgVal("[OTA] GET detail", err.c_str());
        appendMonitorLogVal("[OTA] GET detail", err.c_str());
    }
}

static void otaLogDlProgress(size_t written, int contentLen, unsigned long dlStartMs) {
    const unsigned long elapsedMs = millis() - dlStartMs;
    unsigned kbPerSec = 0;
    if (elapsedMs >= 200UL) {
        kbPerSec = (unsigned)((written * 1000UL) / elapsedMs / 1024UL);
    }
    const unsigned pct =
        (contentLen > 0) ? (unsigned)((written * 100UL) / (size_t)contentLen) : 0UL;
    char pbuf[96];
    snprintf(
        pbuf,
        sizeof(pbuf),
        "[OTA] %u / %d (%u%%, %u KB/s)",
        (unsigned)written,
        contentLen,
        pct,
        kbPerSec
    );
    logMsg(pbuf);
}

static void otaPrepareTlsClient(WiFiClientSecure& client) {
    client.setInsecure();
    client.setTimeout(300000);
#if defined(ESP32) && defined(ARDUINO_ARCH_ESP32)
    if (OTA_TLS_RX_BUFFER_SIZE > 0) {
        const size_t largest = ESP.getMaxAllocHeap();
        const size_t need = (size_t)OTA_TLS_RX_BUFFER_SIZE + 24576UL;
        if (largest >= need &&
            client.setBufferSizes(OTA_TLS_RX_BUFFER_SIZE, OTA_TLS_TX_BUFFER_SIZE)) {
            char buf[56];
            snprintf(buf, sizeof(buf), "[OTA] TLS RX=%d TX=%d", OTA_TLS_RX_BUFFER_SIZE, OTA_TLS_TX_BUFFER_SIZE);
            logMsg(buf);
        } else {
            logMsg("[OTA] TLS default buffers (heap or setBufferSizes declined)");
        }
    }
#endif
}

static void otaConfigureHttpClient(HTTPClient& http) {
    http.setTimeout(600000);
    http.setConnectTimeout(45000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.addHeader("Connection", "close");
    http.setUserAgent("sim800-gateway-ota/1.0");
}

// Stream download + esp_ota (HTTPUpdate is unreliable on arduino-esp32).
static bool otaStreamDownloadAndFlash(const char* url, char* errorOut, size_t errorOutSize) {
    if (errorOut && errorOutSize > 0) {
        errorOut[0] = '\0';
    }

    const wifi_ps_type_t prevWifiPs = WiFi.getSleep();
    WiFi.setSleep(WIFI_PS_NONE);

    uint8_t* const buff = otaDlBuffer;

    if (!WiFi.isConnected()) {
        WiFi.setSleep(prevWifiPs);
        if (errorOut && errorOutSize > 0) {
            snprintf(errorOut, errorOutSize, "WiFi not connected");
        }
        return false;
    }

    otaCooldownMs(800);
    otaLogDnsForUrl(url);
    logMsg("[OTA] HTTP GET (streaming)...");

    int code = -1;

    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt > 0) {
            logMsgInt("[OTA] GET retry", attempt + 1);
            otaCooldownMs(4000);
        }

        WiFiClientSecure client;
        HTTPClient http;
        otaPrepareTlsClient(client);
        if (!http.begin(client, url)) {
            logMsg("[OTA] http.begin failed");
            continue;
        }
        otaConfigureHttpClient(http);

        logMsg("[OTA] Connecting to firmware URL...");
        code = http.GET();
        if (code != HTTP_CODE_OK) {
            otaLogGetFailure(http, code);
            http.end();
            client.stop();
            continue;
        }

        const int contentLen = http.getSize();
        if (contentLen <= 0) {
            logMsg("[OTA] Missing Content-Length, retrying");
            http.end();
            client.stop();
            continue;
        }

        {
            char buf[96];
            snprintf(buf, sizeof(buf), "[OTA] Download size=%d bytes", contentLen);
            logMsg(buf);
            appendMonitorLog(buf);
        }

        if ((size_t)contentLen < OTA_MIN_APP_BYTES) {
            if (errorOut && errorOutSize > 0) {
                snprintf(
                    errorOut,
                    errorOutSize,
                    "File too small (%d bytes). Wrong .bin?",
                    contentLen
                );
            }
            http.end();
            client.stop();
            WiFi.setSleep(prevWifiPs);
            return false;
        }

        if (!otaFirmwareFitsUpdateSlot(contentLen, errorOut, errorOutSize)) {
            http.end();
            client.stop();
            WiFi.setSleep(prevWifiPs);
            return false;
        }

        const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
        if (!part) {
            if (errorOut && errorOutSize > 0) {
                snprintf(errorOut, errorOutSize, "No OTA update partition");
            }
            http.end();
            WiFi.setSleep(prevWifiPs);
            return false;
        }

        esp_ota_handle_t ota_handle = 0;
        esp_err_t ota_err = esp_ota_begin(part, (size_t)contentLen, &ota_handle);
        if (ota_err != ESP_OK) {
            if (errorOut && errorOutSize > 0) {
                snprintf(
                    errorOut,
                    errorOutSize,
                    "esp_ota_begin failed: %s (%d bytes)",
                    esp_err_to_name(ota_err),
                    contentLen
                );
            }
            http.end();
            client.stop();
            WiFi.setSleep(prevWifiPs);
            return false;
        }

        WiFiClient* stream = http.getStreamPtr();
        if (!stream) {
            esp_ota_abort(ota_handle);
            http.end();
            client.stop();
            WiFi.setSleep(prevWifiPs);
            if (errorOut && errorOutSize > 0) {
                snprintf(errorOut, errorOutSize, "No HTTP stream");
            }
            return false;
        }

        uint8_t magic = 0;
        unsigned long magicWait = millis();
        while (stream->available() < 1 && http.connected() && (millis() - magicWait) < 30000) {
            yield();
        }
        if (stream->read(&magic, 1) != 1 || magic != ESP_APP_IMAGE_MAGIC) {
            logMsg("[OTA] Bad firmware header, retrying");
            esp_ota_abort(ota_handle);
            http.end();
            client.stop();
            continue;
        }

        size_t written = 1;
        ota_err = esp_ota_write(ota_handle, &magic, 1);
        if (ota_err != ESP_OK) {
            if (errorOut && errorOutSize > 0) {
                snprintf(errorOut, errorOutSize, "esp_ota_write header: %s", esp_err_to_name(ota_err));
            }
            esp_ota_abort(ota_handle);
            http.end();
            client.stop();
            WiFi.setSleep(prevWifiPs);
            return false;
        }
        const unsigned long dlStartMs = millis();
        unsigned long lastLogMs = dlStartMs;
        unsigned long lastDataMs = dlStartMs;

        logMsg("[OTA] Downloading (blocking read, 8 KB chunks)...");

        while (written < (size_t)contentLen) {
            const size_t remaining = (size_t)contentLen - written;
            size_t want = remaining;
            if (want > OTA_DL_CHUNK) {
                want = OTA_DL_CHUNK;
            }

            const int got = stream->readBytes(buff, want);
            if (got > 0) {
                ota_err = esp_ota_write(ota_handle, buff, (size_t)got);
                if (ota_err != ESP_OK) {
                    if (errorOut && errorOutSize > 0) {
                        snprintf(
                            errorOut,
                            errorOutSize,
                            "esp_ota_write at %u: %s",
                            (unsigned)written,
                            esp_err_to_name(ota_err)
                        );
                    }
                    esp_ota_abort(ota_handle);
                    http.end();
                    client.stop();
                    WiFi.setSleep(prevWifiPs);
                    return false;
                }
                written += (size_t)got;
                lastDataMs = millis();

                if (millis() - lastLogMs >= 3000) {
                    otaLogDlProgress(written, contentLen, dlStartMs);
                    lastLogMs = millis();
                }
                continue;
            }

            if (!http.connected()) {
                break;
            }
            if (millis() - lastDataMs > OTA_DL_STALL_MS) {
                if (errorOut && errorOutSize > 0) {
                    snprintf(errorOut, errorOutSize, "Download stalled");
                }
                esp_ota_abort(ota_handle);
                http.end();
                client.stop();
                WiFi.setSleep(prevWifiPs);
                return false;
            }
            delay(1);
            yield();
        }

        http.end();
        client.stop();

        if (written != (size_t)contentLen) {
            logMsg("[OTA] Incomplete download, retrying");
            esp_ota_abort(ota_handle);
            continue;
        }

        otaLogDlProgress(written, contentLen, dlStartMs);
        logMsg("[OTA] Verifying flash...");
        appendMonitorLog("[OTA] Download complete, verifying...");

        ota_err = esp_ota_end(ota_handle);
        if (ota_err != ESP_OK) {
            if (errorOut && errorOutSize > 0) {
                snprintf(errorOut, errorOutSize, "esp_ota_end: %s", esp_err_to_name(ota_err));
            }
            WiFi.setSleep(prevWifiPs);
            return false;
        }

        ota_err = esp_ota_set_boot_partition(part);
        if (ota_err != ESP_OK) {
            if (errorOut && errorOutSize > 0) {
                snprintf(errorOut, errorOutSize, "esp_ota_set_boot: %s", esp_err_to_name(ota_err));
            }
            WiFi.setSleep(prevWifiPs);
            return false;
        }

        WiFi.setSleep(prevWifiPs);
        logMsg("[OTA] Flash OK, rebooting");
        appendMonitorLog("[OTA] Flash OK, rebooting");
        delay(500);
        ESP.restart();
        return true;
    }

    if (errorOut && errorOutSize > 0) {
        snprintf(errorOut, errorOutSize, "HTTP GET failed (%d)", code);
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
