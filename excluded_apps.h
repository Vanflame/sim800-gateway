#pragma once

#include <Arduino.h>
#include <stddef.h>

#define EXCLUDED_APPS_MAX   24
#define EXCLUDED_APP_NAME_SIZE 32

void excludedAppsLoad();
bool excludedAppsSave();

int excludedAppsCount();
const char* excludedAppsAt(int index);

bool excludedAppsContains(const char* appName);
bool excludedAppsUpsert(const char* appName);
bool excludedAppsRemove(const char* appName);
void excludedAppsClear();

bool excludedAppsImportJson(const char* json);
size_t excludedAppsExportJson(char* out, size_t outSize);

/** Comma-separated for heartbeat/register payloads. */
size_t excludedAppsExportCsv(char* out, size_t outSize);
