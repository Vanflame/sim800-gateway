#include "mux_map.h"

#if !USE_DUAL_UART

#include "mux.h"
#include "logger.h"
#include "utils.h"
#include <Preferences.h>
#include <cstring>

static uint8_t gChannels[SIM_COUNT];
static unsigned long gSettleMs = MUX_SETTLE_MS;

static const uint8_t kDefaultChannels[SIM_COUNT] = LOGICAL_TO_MUX_INIT;
static const uint32_t MUX_MAP_NVS_MAGIC = 0x4D583031UL;  // "MX01"

static void applyDefaults() {
    for (int i = 0; i < SIM_COUNT; i++) {
        gChannels[i] = kDefaultChannels[i];
    }
    gSettleMs = MUX_SETTLE_MS;
}

void muxMapInitDefaults() {
    applyDefaults();
}

void muxMapLoad() {
    applyDefaults();
    Preferences prefs;
    prefs.begin("mux_map", true);
    const uint32_t magic = prefs.getUInt("magic", 0);
    const int saved = prefs.getInt("count", 0);
    const bool hasSavedMap =
        saved == SIM_COUNT &&
        (magic == MUX_MAP_NVS_MAGIC || magic == 0);  // magic=0: legacy saves
    if (hasSavedMap) {
        for (int i = 0; i < SIM_COUNT; i++) {
            char key[8];
            snprintf(key, sizeof(key), "c%d", i);
            const int ch = prefs.getInt(key, (int)gChannels[i]);
            if (ch >= 0 && ch < SIM_COUNT) {
                gChannels[i] = (uint8_t)ch;
            }
        }
        char ch1[8];
        char ch16[8];
        snprintf(ch1, sizeof(ch1), "%d", (int)gChannels[0]);
        snprintf(ch16, sizeof(ch16), "%d", (int)gChannels[SIM_COUNT - 1]);
        logMsg2Val("[MUX_MAP] Loaded from NVS slot1->ch", ch1, "slot16->ch", ch16);
    } else {
        logMsg("[MUX_MAP] No saved mapping — using compile-time defaults");
    }
    const unsigned long settle = prefs.getULong("settle", gSettleMs);
    if (settle >= 50 && settle <= 2000) {
        gSettleMs = settle;
    }
    prefs.end();
}

bool muxMapSave() {
    Preferences prefs;
    if (!prefs.begin("mux_map", false)) {
        logMsg("[MUX_MAP] NVS open failed");
        return false;
    }
    prefs.putUInt("magic", MUX_MAP_NVS_MAGIC);
    prefs.putInt("count", SIM_COUNT);
    for (int i = 0; i < SIM_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "c%d", i);
        prefs.putInt(key, (int)gChannels[i]);
    }
    prefs.putULong("settle", gSettleMs);
    prefs.end();
    logMsg("[MUX_MAP] Saved to NVS");
    return true;
}

void muxMapApplyHardware() {
    muxInvalidateSelection();
    selectSIM(getCurrentLogicalSlot());
}

uint8_t muxMapChannelForSlot(int logicalSlot) {
    if (logicalSlot < 0) logicalSlot = 0;
    if (logicalSlot >= SIM_COUNT) logicalSlot = SIM_COUNT - 1;
    return gChannels[logicalSlot];
}

void muxMapSetChannelForSlot(int logicalSlot, uint8_t muxChannel) {
    if (logicalSlot < 0 || logicalSlot >= SIM_COUNT) return;
    if (muxChannel >= SIM_COUNT) muxChannel = SIM_COUNT - 1;
    gChannels[logicalSlot] = muxChannel;
}

unsigned long muxMapSettleMs() {
    return gSettleMs;
}

void muxMapSetSettleMs(unsigned long ms) {
    if (ms < 50) ms = 50;
    if (ms > 2000) ms = 2000;
    gSettleMs = ms;
}

static const char* skipWs(const char* p) {
    while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

bool muxMapImportJson(const char* json) {
    if (!json) return false;

    const char* channelsKey = strstr(json, "\"channels\"");
    if (!channelsKey) return false;

    const char* arr = strchr(channelsKey, '[');
    if (!arr) return false;
    arr++;

    int parsed = 0;
    for (int i = 0; i < SIM_COUNT; i++) {
        arr = skipWs(arr);
        if (*arr == ']') break;
        char* end = nullptr;
        const long v = strtol(arr, &end, 10);
        if (end == arr) return false;
        if (v < 0 || v >= SIM_COUNT) return false;
        gChannels[i] = (uint8_t)v;
        parsed++;
        arr = end;
        if (*arr == ',') arr++;
    }
    if (parsed != SIM_COUNT) return false;

    const char* settleKey = strstr(json, "\"settle_ms\"");
    if (settleKey) {
        const char* colon = strchr(settleKey, ':');
        if (colon) {
            const long ms = strtol(colon + 1, nullptr, 10);
            if (ms >= 50 && ms <= 2000) {
                gSettleMs = (unsigned long)ms;
            }
        }
    }

    if (!muxMapSave()) return false;
    muxMapApplyHardware();
    return true;
}

size_t muxMapExportJson(char* out, size_t outSize) {
    if (!out || outSize < 64) return 0;
    int n = snprintf(out, outSize, "{\"channels\":[");
    if (n < 0 || (size_t)n >= outSize) return 0;
    size_t pos = (size_t)n;
    for (int i = 0; i < SIM_COUNT; i++) {
        n = snprintf(out + pos, outSize - pos, "%s%d", i ? "," : "", (int)gChannels[i]);
        if (n < 0 || (size_t)n >= outSize - pos) return 0;
        pos += (size_t)n;
    }
    n = snprintf(out + pos, outSize - pos, "],\"settle_ms\":%lu}", gSettleMs);
    if (n < 0 || (size_t)n >= outSize - pos) return 0;
    return pos + (size_t)n;
}

#endif
