#pragma once

#include "config.h"
#include <stddef.h>

// Saved OTA binary URL (preferences or built from OTA_GITHUB_* in config.h)
extern char otaFirmwareUrl[256];

void otaLoadUrlFromPreferences();
void otaSaveUrlToPreferences(const char* url);

// Build default GitHub release URL into buf (returns false if not configured)
bool otaBuildDefaultUrl(char* buf, size_t bufSize);

// Fetch remote version string (one line, e.g. "1.0.1"); returns false on error
bool otaFetchRemoteVersion(char* version, size_t versionSize);

// Compare remote vs FIRMWARE_VERSION; sets updateAvailable
bool otaCheckForUpdate(bool* updateAvailable, char* remoteVersion, size_t remoteSize);

// True if version string a is semantically newer than b (e.g. 1.0.2 > 1.0.1)
bool otaVersionIsNewer(const char* a, const char* b);

// Download and flash firmware from url; on success device reboots (does not return)
bool otaPerformUpdate(const char* url, char* errorOut, size_t errorOutSize);

extern volatile bool otaInProgress;
