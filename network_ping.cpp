#include "network_ping.h"
#include "logger.h"

#include <WiFi.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#if defined(ESP_PLATFORM) && __has_include(<ping/ping_sock.h>)
#include <ping/ping_sock.h>
#define GATEWAY_ICMP_PING 1
#elif defined(ESP_PLATFORM) && __has_include("ping/ping_sock.h")
#include "ping/ping_sock.h"
#define GATEWAY_ICMP_PING 1
#endif

namespace {

constexpr uint8_t kPingCount = 4;
constexpr uint32_t kPingIntervalMs = 500;
constexpr uint32_t kPingTimeoutMs = 1500;
constexpr uint32_t kDnsTimeoutMs = 3000;
constexpr uint32_t kPingTaskStack = 8192;

static void pingLog(const char* line) {
    if (!line) return;
    logMsg(line);
    appendMonitorLog(line);
}

static void pingLogf(const char* fmt, ...) {
    char line[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    pingLog(line);
}

struct DnsResolveCtx {
    const char* host = nullptr;
    IPAddress ip;
    volatile bool done = false;
    volatile bool ok = false;
};

static DnsResolveCtx s_dnsJob;
static volatile bool s_dnsTaskActive = false;

struct PingRunCtx {
    char* output = nullptr;
    size_t outputSize = 0;
    size_t outLen = 0;
    char targetIp[20] = {};
    uint8_t recvCount = 0;
    uint8_t timeoutCount = 0;
    uint32_t rttMs[8] = {};
    EventGroupHandle_t doneEvent = nullptr;
};

static char s_pingHost[80];
static char s_pingOutput[384];
static char s_pingSummary[128];
static volatile bool s_pingSuccess = false;
static volatile bool s_pingWorkerRunning = false;
static SemaphoreHandle_t s_pingDoneSem = nullptr;

static void pingSleepMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void pingAppend(PingRunCtx* ctx, const char* fmt, ...) {
    if (!ctx || !ctx->output || ctx->outLen >= ctx->outputSize - 1) return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(ctx->output + ctx->outLen, ctx->outputSize - ctx->outLen, fmt, args);
    va_end(args);
    if (n > 0) {
        ctx->outLen += (size_t)n;
        if (ctx->outLen >= ctx->outputSize) ctx->outLen = ctx->outputSize - 1;
    }
}

static bool staRouteReady() {
    if (!WiFi.isConnected()) return false;
    return WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

static void dnsResolveTask(void* param) {
    (void)param;
    if (s_dnsJob.host) {
        s_dnsJob.ok = (WiFi.hostByName(s_dnsJob.host, s_dnsJob.ip) == 1) && s_dnsJob.ip[0] != 0;
    }
    s_dnsJob.done = true;
    s_dnsTaskActive = false;
    vTaskDelete(nullptr);
}

static void dnsWaitForIdle() {
    const unsigned long deadline = millis() + 8000;
    while (s_dnsTaskActive && (long)(deadline - millis()) > 0) {
        pingSleepMs(20);
    }
}

static bool resolveHostToIp(const char* host, IPAddress& outIp, unsigned long* dnsMs) {
    if (!host || !host[0]) return false;
    if (outIp.fromString(host)) {
        if (dnsMs) *dnsMs = 0;
        pingLogf("[NET] Ping using IP %s", outIp.toString().c_str());
        return true;
    }

    dnsWaitForIdle();

    const unsigned long t0 = millis();
    s_dnsJob.host = host;
    s_dnsJob.ip = IPAddress();
    s_dnsJob.done = false;
    s_dnsJob.ok = false;
    s_dnsTaskActive = true;

    pingLogf("[NET] Ping DNS lookup %s", host);

    if (xTaskCreate(dnsResolveTask, "gw_dns", 4096, nullptr, 1, nullptr) != pdPASS) {
        s_dnsTaskActive = false;
        const bool ok = (WiFi.hostByName(host, outIp) == 1) && outIp[0] != 0;
        if (dnsMs) *dnsMs = millis() - t0;
        return ok;
    }

    const unsigned long deadline = t0 + kDnsTimeoutMs;
    while (!s_dnsJob.done && (long)(deadline - millis()) > 0) {
        pingSleepMs(20);
    }

    if (dnsMs) *dnsMs = millis() - t0;

    if (!s_dnsJob.done) {
        pingLogf("[NET] Ping DNS timeout for %s", host);
        dnsWaitForIdle();
        return false;
    }

    outIp = s_dnsJob.ip;
    if (s_dnsJob.ok) {
        pingLogf("[NET] Ping DNS OK %s -> %s (%lu ms)", host, outIp.toString().c_str(), (unsigned long)(dnsMs ? *dnsMs : 0));
    } else {
        pingLogf("[NET] Ping DNS failed for %s", host);
    }
    return s_dnsJob.ok;
}

#ifdef GATEWAY_ICMP_PING
static void onIcmpPingSuccess(esp_ping_handle_t hdl, void* args) {
    auto* ctx = static_cast<PingRunCtx*>(args);
    if (!ctx) return;

    uint32_t dur = 0;
    uint32_t ttl = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &dur, sizeof(dur));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));

    if (ctx->recvCount < sizeof(ctx->rttMs) / sizeof(ctx->rttMs[0])) {
        ctx->rttMs[ctx->recvCount] = dur;
        ctx->recvCount++;
    }
    pingAppend(ctx, "Reply from %s: bytes=32 time=%lu ms TTL=%lu\n",
        ctx->targetIp, (unsigned long)dur, (unsigned long)ttl);
}

static void onIcmpPingTimeout(esp_ping_handle_t hdl, void* args) {
    (void)hdl;
    auto* ctx = static_cast<PingRunCtx*>(args);
    if (!ctx) return;
    ctx->timeoutCount++;
    pingAppend(ctx, "Request timed out.\n");
}

static void onIcmpPingEnd(esp_ping_handle_t hdl, void* args) {
    auto* ctx = static_cast<PingRunCtx*>(args);
    if (hdl) {
        esp_ping_stop(hdl);
        esp_ping_delete_session(hdl);
    }
    if (ctx && ctx->doneEvent) {
        xEventGroupSetBits(ctx->doneEvent, BIT0);
    }
}

static bool icmpPingHost(const char* host, const IPAddress& ip, PingRunCtx* ctx) {
    if (!ctx) return false;

    ip_addr_t target{};
    target.type = IPADDR_TYPE_V4;
    IP4_ADDR(ip_2_ip4(&target), ip[0], ip[1], ip[2], ip[3]);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = kPingCount;
    cfg.interval_ms = kPingIntervalMs;
    cfg.timeout_ms = kPingTimeoutMs;
    cfg.task_stack_size = 4096;
    cfg.task_prio = 2;

    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = onIcmpPingSuccess;
    cbs.on_ping_timeout = onIcmpPingTimeout;
    cbs.on_ping_end = onIcmpPingEnd;
    cbs.cb_args = ctx;

    ctx->doneEvent = xEventGroupCreate();
    if (!ctx->doneEvent) return false;

    esp_ping_handle_t ping = nullptr;
    const esp_err_t err = esp_ping_new_session(&cfg, &cbs, &ping);
    if (err != ESP_OK || !ping) {
        vEventGroupDelete(ctx->doneEvent);
        ctx->doneEvent = nullptr;
        return false;
    }

    pingLog("[NET] Ping ICMP probes...");
    pingAppend(ctx, "PING %s (%s): 32 data bytes (ICMP)\n", host, ip.toString().c_str());

    if (esp_ping_start(ping) != ESP_OK) {
        esp_ping_delete_session(ping);
        vEventGroupDelete(ctx->doneEvent);
        ctx->doneEvent = nullptr;
        return false;
    }

    const uint32_t waitMs = kPingCount * (kPingIntervalMs + kPingTimeoutMs) + 2000;
    const unsigned long deadline = millis() + waitMs;
    while (millis() < deadline) {
        if (xEventGroupGetBits(ctx->doneEvent) & BIT0) {
            break;
        }
        pingSleepMs(20);
    }

    vEventGroupDelete(ctx->doneEvent);
    ctx->doneEvent = nullptr;
    return ctx->recvCount > 0;
}
#endif

static bool tcpPingHost(const char* host, const IPAddress& ip, PingRunCtx* ctx) {
    if (!ctx) return false;

    pingLog("[NET] Ping TCP :443 probes...");
    pingAppend(ctx, "PING %s (%s): 32 data bytes (TCP :443)\n", host, ip.toString().c_str());

    for (uint8_t i = 0; i < kPingCount; i++) {
        WiFiClient client;
        client.setTimeout(kPingTimeoutMs);
        const unsigned long t0 = millis();
        const bool ok = client.connect(ip, 443, kPingTimeoutMs);
        const uint32_t rtt = (uint32_t)(millis() - t0);
        client.stop();

        if (ok) {
            if (ctx->recvCount < sizeof(ctx->rttMs) / sizeof(ctx->rttMs[0])) {
                ctx->rttMs[ctx->recvCount] = rtt;
                ctx->recvCount++;
            }
            pingAppend(ctx, "Reply from %s: bytes=32 time=%lu ms\n",
                ip.toString().c_str(), (unsigned long)rtt);
        } else {
            ctx->timeoutCount++;
            pingAppend(ctx, "Request timed out.\n");
        }
        if (i + 1 < kPingCount) {
            pingSleepMs(kPingIntervalMs);
        }
    }
    return ctx->recvCount > 0;
}

static void formatPingStats(const char* host, PingRunCtx* ctx, char* summary, size_t summarySize) {
    if (!ctx || !summary || summarySize < 8) return;

    uint32_t minMs = 0;
    uint32_t maxMs = 0;
    uint64_t sum = 0;
    if (ctx->recvCount > 0) {
        minMs = maxMs = ctx->rttMs[0];
        sum = ctx->rttMs[0];
        for (uint8_t i = 1; i < ctx->recvCount; i++) {
            const uint32_t v = ctx->rttMs[i];
            if (v < minMs) minMs = v;
            if (v > maxMs) maxMs = v;
            sum += v;
        }
    }
    const uint32_t avgMs = ctx->recvCount > 0 ? (uint32_t)(sum / ctx->recvCount) : 0;
    const uint8_t lossPct = (uint8_t)(((kPingCount - ctx->recvCount) * 100) / kPingCount);

    pingAppend(ctx, "\n--- %s ping statistics ---\n", host);
    pingAppend(ctx, "%u packets transmitted, %u received, %u%% packet loss\n",
        (unsigned)kPingCount, (unsigned)ctx->recvCount, (unsigned)lossPct);
    if (ctx->recvCount > 0) {
        pingAppend(ctx, "round-trip min/avg/max = %lu/%lu/%lu ms\n",
            (unsigned long)minMs, (unsigned long)avgMs, (unsigned long)maxMs);
    }

    if (ctx->recvCount == kPingCount) {
        snprintf(summary, summarySize, "OK — %u/%u replies, avg %lu ms", ctx->recvCount, kPingCount, (unsigned long)avgMs);
    } else if (ctx->recvCount > 0) {
        snprintf(summary, summarySize, "Partial — %u/%u replies, %u%% loss", ctx->recvCount, kPingCount, (unsigned)lossPct);
    } else {
        snprintf(summary, summarySize, "No reply from %s", host);
    }
}

static bool gatewayRunPingWorker(const char* host, char* output, size_t outputSize, char* summary, size_t summarySize) {
    if (!host || !host[0] || !output || outputSize < 32 || !summary || summarySize < 8) {
        if (summary && summarySize > 0) summary[0] = '\0';
        if (output && outputSize > 0) output[0] = '\0';
        return false;
    }

    output[0] = '\0';
    summary[0] = '\0';

    if (!WiFi.isConnected()) {
        snprintf(summary, summarySize, "WiFi not connected");
        snprintf(output, outputSize, "WiFi is not connected.\nConnect STA in Settings first.");
        pingLog("[NET] Ping aborted: WiFi not connected");
        return false;
    }

    if (!staRouteReady()) {
        snprintf(summary, summarySize, "No IP on WiFi");
        snprintf(output, outputSize, "WiFi has no IP address yet.\nConnect to a network in Settings first.");
        pingLog("[NET] Ping aborted: no STA IP");
        return false;
    }

    PingRunCtx ctx;
    ctx.output = output;
    ctx.outputSize = outputSize;

    IPAddress ip;
    unsigned long dnsMs = 0;
    if (!resolveHostToIp(host, ip, &dnsMs)) {
        snprintf(summary, summarySize, "DNS failed for %s", host);
        snprintf(output, outputSize,
            "DNS lookup failed for \"%s\" (timeout %lu ms).\n"
            "Check WiFi gateway/DNS in Settings.",
            host, (unsigned long)kDnsTimeoutMs);
        return false;
    }

    if (dnsMs > 0) {
        pingAppend(&ctx, "DNS: %s → %s (%lu ms)\n\n", host, ip.toString().c_str(), (unsigned long)dnsMs);
    }

    snprintf(ctx.targetIp, sizeof(ctx.targetIp), "%s", ip.toString().c_str());

    bool ok = false;
#ifdef GATEWAY_ICMP_PING
    ok = icmpPingHost(host, ip, &ctx);
    if (!ok) {
        pingLog("[NET] Ping ICMP failed — trying TCP :443");
        pingAppend(&ctx, "\nICMP unavailable or blocked — trying TCP :443…\n\n");
        ctx.recvCount = 0;
        ctx.timeoutCount = 0;
        memset(ctx.rttMs, 0, sizeof(ctx.rttMs));
        ok = tcpPingHost(host, ip, &ctx);
    }
#else
    ok = tcpPingHost(host, ip, &ctx);
#endif

    formatPingStats(host, &ctx, summary, summarySize);
    pingLogf("[NET] Ping finished %s — %s", host, summary);
    return ok;
}

static void pingWorkerTask(void* param) {
    (void)param;
    s_pingSuccess = gatewayRunPingWorker(s_pingHost, s_pingOutput, sizeof(s_pingOutput),
        s_pingSummary, sizeof(s_pingSummary));
    s_pingWorkerRunning = false;
    if (s_pingDoneSem) {
        xSemaphoreGive(s_pingDoneSem);
    }
    vTaskDelete(nullptr);
}

}  // namespace

bool gatewayPingIsRunning() {
    return s_pingWorkerRunning;
}

const char* gatewayPingGetHost() {
    return s_pingHost;
}

const char* gatewayPingGetOutput() {
    return s_pingOutput;
}

const char* gatewayPingGetSummary() {
    return s_pingSummary;
}

bool gatewayPingLastSuccess() {
    return s_pingSuccess;
}

bool gatewayPingRunBlocking(const char* host, uint32_t timeoutMs) {
    if (!host) {
        return false;
    }
    if (s_pingWorkerRunning) {
        pingLog("[NET] Ping rejected: already running");
        return false;
    }

    if (!s_pingDoneSem) {
        s_pingDoneSem = xSemaphoreCreateBinary();
        if (!s_pingDoneSem) {
            pingLog("[NET] Ping failed: no semaphore");
            return false;
        }
    }

    strncpy(s_pingHost, host, sizeof(s_pingHost) - 1);
    s_pingHost[sizeof(s_pingHost) - 1] = '\0';
    s_pingOutput[0] = '\0';
    s_pingSummary[0] = '\0';
    s_pingSuccess = false;
    s_pingWorkerRunning = true;

    pingLogf("[NET] Ping starting host=%s", s_pingHost);

    xSemaphoreTake(s_pingDoneSem, 0);

    if (xTaskCreate(pingWorkerTask, "gw_ping", kPingTaskStack, nullptr, 1, nullptr) != pdPASS) {
        s_pingWorkerRunning = false;
        pingLog("[NET] Ping failed: could not start task");
        return false;
    }

    if (xSemaphoreTake(s_pingDoneSem, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        pingLogf("[NET] Ping timed out after %lu ms", (unsigned long)timeoutMs);
        return false;
    }

    return s_pingSuccess;
}
