#pragma once

#include <Arduino.h>

// Called from loadSettings() — restore defaults after a scheduled maintenance reboot
void maintenanceOnBoot();

// After successful heartbeat HTTP response (defers maintenance HTTPS — do not block inside heartbeat)
void maintenanceOnHeartbeatSuccess();

// Run deferred maintenance poll from main loop (one blocking POST); returns true if ran
bool maintenanceRunDeferredPollIfNeeded();

// After follow-up loaded activeSessions (or heartbeat failed path prune)
void maintenanceOnSessionsUpdated();

// Main loop: 12h firmware check + evaluate scheduled restart
void maintenanceTick(unsigned long nowMs);

// For /status JSON and UI countdown (-1 = none, 0 = due now, >0 = seconds until)
long maintenanceGetScheduledRestartInSec();
bool maintenanceHasScheduledRestart();

// Cached result of last local version check (12h tick or manual Check for updates)
void maintenanceRecordFirmwareCheck(const char* remoteVersion, bool updateAvailable);
void maintenanceGetFirmwareCache(char* remoteOut, size_t remoteSize, bool* updateAvailableOut,
    unsigned long* lastCheckMsOut);
