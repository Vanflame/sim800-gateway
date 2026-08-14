#include "excluded_apps.h"
#include "utils.h"
#include <Preferences.h>
#include <cstring>

static char gApps[EXCLUDED_APPS_MAX][EXCLUDED_APP_NAME_SIZE];
static int gCount = 0;

static int findIndex(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < gCount; i++) {
        if (strcasecmp(gApps[i], name) == 0) return i;
    }
    return -1;
}

void excludedAppsLoad() {
    gCount = 0;
    Preferences prefs;
    prefs.begin("excluded", true);
    const int saved = prefs.getInt("count", 0);
    for (int i = 0; i < saved && i < EXCLUDED_APPS_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "a%d", i);
        prefs.getString(key, gApps[gCount], EXCLUDED_APP_NAME_SIZE);
        charBufTrim(gApps[gCount]);
        if (gApps[gCount][0]) gCount++;
    }
    prefs.end();
}

bool excludedAppsSave() {
    Preferences prefs;
    prefs.begin("excluded", false);
    prefs.putInt("count", gCount);
    for (int i = 0; i < gCount; i++) {
        char key[8];
        snprintf(key, sizeof(key), "a%d", i);
        prefs.putString(key, gApps[i]);
    }
    prefs.end();
    return true;
}

int excludedAppsCount() {
    return gCount;
}

const char* excludedAppsAt(int index) {
    if (index < 0 || index >= gCount) return nullptr;
    return gApps[index];
}

bool excludedAppsContains(const char* appName) {
    return findIndex(appName) >= 0;
}

bool excludedAppsUpsert(const char* appName) {
    if (!appName || !appName[0]) return false;
    if (findIndex(appName) >= 0) return true;
    if (gCount >= EXCLUDED_APPS_MAX) return false;
    charBufSet(gApps[gCount], EXCLUDED_APP_NAME_SIZE, appName);
    charBufTrim(gApps[gCount]);
    gCount++;
    return excludedAppsSave();
}

bool excludedAppsRemove(const char* appName) {
    const int idx = findIndex(appName);
    if (idx < 0) return false;
    for (int i = idx; i < gCount - 1; i++) {
        charBufSet(gApps[i], EXCLUDED_APP_NAME_SIZE, gApps[i + 1]);
    }
    gCount--;
    return excludedAppsSave();
}

void excludedAppsClear() {
    gCount = 0;
    excludedAppsSave();
}

bool excludedAppsImportJson(const char* json) {
    if (!json) return false;
    gCount = 0;
    const char* p = strstr(json, "\"apps\"");
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;
    while (*p && gCount < EXCLUDED_APPS_MAX) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') break;
        p++;
        const char* end = strchr(p, '"');
        if (!end) break;
        const size_t len = (size_t)(end - p);
        if (len > 0 && len < EXCLUDED_APP_NAME_SIZE) {
            strncpy(gApps[gCount], p, len);
            gApps[gCount][len] = '\0';
            charBufTrim(gApps[gCount]);
            if (gApps[gCount][0]) gCount++;
        }
        p = end + 1;
    }
    return excludedAppsSave();
}

size_t excludedAppsExportJson(char* out, size_t outSize) {
    if (!out || outSize < 12) return 0;
    size_t pos = 0;
    pos += snprintf(out + pos, outSize - pos, "{\"apps\":[");
    for (int i = 0; i < gCount && pos < outSize - 4; i++) {
        char esc[72];
        jsonEscape(gApps[i], esc, sizeof(esc));
        if (i > 0) pos += snprintf(out + pos, outSize - pos, ",");
        pos += snprintf(out + pos, outSize - pos, "%s", esc);
    }
    pos += snprintf(out + pos, outSize - pos, "]}");
    return pos;
}

size_t excludedAppsExportCsv(char* out, size_t outSize) {
    if (!out || outSize < 2) return 0;
    size_t pos = 0;
    for (int i = 0; i < gCount && pos < outSize - 2; i++) {
        if (i > 0 && pos < outSize - 1) out[pos++] = ',';
        const size_t len = strlen(gApps[i]);
        if (pos + len >= outSize - 1) break;
        memcpy(out + pos, gApps[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return pos;
}
