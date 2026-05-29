#pragma once

#include <Arduino.h>

// Runs ping on a dedicated task. Blocks until done or timeoutMs.
bool gatewayPingRunBlocking(const char* host, uint32_t timeoutMs = 25000);

bool gatewayPingIsRunning();

const char* gatewayPingGetHost();
const char* gatewayPingGetOutput();
const char* gatewayPingGetSummary();
bool gatewayPingLastSuccess();
