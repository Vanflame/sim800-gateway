#include "sender_map.h"
#include "utils.h"
#include <Preferences.h>
#include <cstring>

static SenderMapEntry gEntries[SENDER_MAP_MAX_ENTRIES];
static int gCount = 0;

static void trimEntry(SenderMapEntry* e) {
    if (!e) return;
    charBufTrim(e->code);
    charBufTrim(e->brand);
}

static int findIndex(const char* code) {
    if (!code || !code[0]) return -1;
    for (int i = 0; i < gCount; i++) {
        if (strcasecmp(gEntries[i].code, code) == 0) {
            return i;
        }
    }
    return -1;
}

void senderMapInitDefaults() {
    gCount = 0;
    const char* defaults[][2] = {
        {"*4", "JoyRide"},
        {"3586", "Shopee"},
        {"66", "foodpanda"},
        {"74271626", "Grab"},
        {"49485", "Netflix"},
        {"7434", "GCash"},
        {"45", "TNT"},
        {"42545", "Smart"},
        {"w656", "Playtime"},
        {"w696", "Playtime"},
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]) && gCount < SENDER_MAP_MAX_ENTRIES; i++) {
        charBufSet(gEntries[gCount].code, sizeof(gEntries[gCount].code), defaults[i][0]);
        charBufSet(gEntries[gCount].brand, sizeof(gEntries[gCount].brand), defaults[i][1]);
        gCount++;
    }
}

void senderMapLoad() {
    gCount = 0;
    Preferences prefs;
    prefs.begin("sender_map", true);
    const int saved = prefs.getInt("count", -1);
    if (saved <= 0) {
        prefs.end();
        senderMapInitDefaults();
        senderMapSave();
        return;
    }
    for (int i = 0; i < saved && i < SENDER_MAP_MAX_ENTRIES; i++) {
        char keyCode[8];
        char keyBrand[8];
        snprintf(keyCode, sizeof(keyCode), "c%d", i);
        snprintf(keyBrand, sizeof(keyBrand), "b%d", i);
        prefs.getString(keyCode, gEntries[gCount].code, sizeof(gEntries[gCount].code));
        prefs.getString(keyBrand, gEntries[gCount].brand, sizeof(gEntries[gCount].brand));
        trimEntry(&gEntries[gCount]);
        if (gEntries[gCount].code[0] && gEntries[gCount].brand[0]) {
            gCount++;
        }
    }
    prefs.end();
    if (gCount == 0) {
        senderMapInitDefaults();
    }
}

bool senderMapSave() {
    Preferences prefs;
    prefs.begin("sender_map", false);
    prefs.putInt("count", gCount);
    for (int i = 0; i < gCount; i++) {
        char keyCode[8];
        char keyBrand[8];
        snprintf(keyCode, sizeof(keyCode), "c%d", i);
        snprintf(keyBrand, sizeof(keyBrand), "b%d", i);
        prefs.putString(keyCode, gEntries[i].code);
        prefs.putString(keyBrand, gEntries[i].brand);
    }
    prefs.end();
    return true;
}

int senderMapCount() {
    return gCount;
}

const SenderMapEntry* senderMapEntry(int index) {
    if (index < 0 || index >= gCount) return nullptr;
    return &gEntries[index];
}

const char* senderMapLookup(const char* rawSender) {
    if (!rawSender || !rawSender[0]) return nullptr;
    char trimmed[SENDER_MAP_CODE_SIZE];
    charBufSet(trimmed, sizeof(trimmed), rawSender);
    charBufTrim(trimmed);
    if (!trimmed[0]) return nullptr;

    int idx = findIndex(trimmed);
    if (idx >= 0) return gEntries[idx].brand;

    for (int i = 0; i < gCount; i++) {
        const char* hay = trimmed;
        const char* needle = gEntries[i].code;
        if (!needle[0]) continue;
        while (*hay) {
            if (strncasecmp(hay, needle, strlen(needle)) == 0) {
                return gEntries[i].brand;
            }
            hay++;
        }
    }
    return nullptr;
}

bool senderMapUpsert(const char* code, const char* brand) {
    if (!code || !brand || !code[0] || !brand[0]) return false;
    char trimmedCode[SENDER_MAP_CODE_SIZE];
    char trimmedBrand[SENDER_MAP_BRAND_SIZE];
    charBufSet(trimmedCode, sizeof(trimmedCode), code);
    charBufSet(trimmedBrand, sizeof(trimmedBrand), brand);
    charBufTrim(trimmedCode);
    charBufTrim(trimmedBrand);

    const int idx = findIndex(trimmedCode);
    if (idx >= 0) {
        charBufSet(gEntries[idx].brand, sizeof(gEntries[idx].brand), trimmedBrand);
        return senderMapSave();
    }
    if (gCount >= SENDER_MAP_MAX_ENTRIES) return false;
    charBufSet(gEntries[gCount].code, sizeof(gEntries[gCount].code), trimmedCode);
    charBufSet(gEntries[gCount].brand, sizeof(gEntries[gCount].brand), trimmedBrand);
    gCount++;
    return senderMapSave();
}

bool senderMapRemove(const char* code) {
    const int idx = findIndex(code);
    if (idx < 0) return false;
    for (int i = idx; i < gCount - 1; i++) {
        gEntries[i] = gEntries[i + 1];
    }
    gCount--;
    return senderMapSave();
}

void senderMapClear() {
    gCount = 0;
    senderMapSave();
}

static const char* skipWs(const char* p) {
    while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

bool senderMapImportJson(const char* json) {
    if (!json) return false;
    gCount = 0;

    const char* p = strstr(json, "\"mappings\"");
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;

    while (*p && gCount < SENDER_MAP_MAX_ENTRIES) {
        p = skipWs(p);
        if (*p == ']') break;
        const char* obj = strchr(p, '{');
        if (!obj) break;

        char code[SENDER_MAP_CODE_SIZE];
        char brand[SENDER_MAP_BRAND_SIZE];
        code[0] = brand[0] = '\0';

        const char* codeKey = strstr(obj, "\"code\"");
        if (codeKey) {
            const char* q = strchr(codeKey, '"');
            if (q) {
                q = strchr(q + 1, '"');
                if (q) {
                    q++;
                    const char* q2 = strchr(q, '"');
                    if (q2 && (size_t)(q2 - q) < sizeof(code)) {
                        strncpy(code, q, (size_t)(q2 - q));
                        code[q2 - q] = '\0';
                    }
                }
            }
        }
        const char* brandKey = strstr(obj, "\"brand\"");
        if (brandKey) {
            const char* q = strchr(brandKey, '"');
            if (q) {
                q = strchr(q + 1, '"');
                if (q) {
                    q++;
                    const char* q2 = strchr(q, '"');
                    if (q2 && (size_t)(q2 - q) < sizeof(brand)) {
                        strncpy(brand, q, (size_t)(q2 - q));
                        brand[q2 - q] = '\0';
                    }
                }
            }
        }

        if (code[0] && brand[0]) {
            charBufSet(gEntries[gCount].code, sizeof(gEntries[gCount].code), code);
            charBufSet(gEntries[gCount].brand, sizeof(gEntries[gCount].brand), brand);
            gCount++;
        }

        p = strchr(obj, '}');
        if (!p) break;
        p++;
    }

    if (gCount == 0) return false;
    return senderMapSave();
}

size_t senderMapExportJson(char* out, size_t outSize) {
    if (!out || outSize < 16) return 0;
    size_t pos = 0;
    pos += snprintf(out + pos, outSize - pos, "{\"mappings\":[");
    for (int i = 0; i < gCount && pos < outSize - 4; i++) {
        char escCode[40];
        char escBrand[72];
        jsonEscape(gEntries[i].code, escCode, sizeof(escCode));
        jsonEscape(gEntries[i].brand, escBrand, sizeof(escBrand));
        if (i > 0) pos += snprintf(out + pos, outSize - pos, ",");
        pos += snprintf(out + pos, outSize - pos, "{\"code\":%s,\"brand\":%s}",
                        escCode, escBrand);
    }
    pos += snprintf(out + pos, outSize - pos, "]}");
    return pos;
}
