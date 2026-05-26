#pragma once

#include <Arduino.h>

// Called from loadSettings() — restore defaults after a scheduled maintenance reboot
void maintenanceOnBoot();

// After successful heartbeat HTTP response
void maintenanceOnHeartbeatSuccess();

// After follow-up loaded activeSessions (or heartbeat failed path prune)
void maintenanceOnSessionsUpdated();

// Main loop: 12h firmware check + evaluate scheduled restart
void maintenanceTick(unsigned long nowMs);
