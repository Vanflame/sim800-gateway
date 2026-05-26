#pragma once

// ============================================================================
// Missed call detection → forward as Viber-style SMS to backend
// ============================================================================

#include "config.h"

// Configure modem for missed-call capture (CLIP on, allow ring, hang up on CLIP in software)
void configureModemForMissedCallDetect();

// Block incoming calls without forwarding (CLIP off, GSMBUSY reject)
void configureModemCallBlockOnly();

// Apply missed-call or block mode on every enabled, responsive SIM
void applyMissedCallModemToAllSims();

// Non-blocking UART listen after mux select (per SIM during SMS poll)
void listenForCallUrc(int slot, unsigned long listenMs);

// Main-loop fast rotation: one enabled SIM per tick (no AT, no UART flush)
void pollMissedCallsFast();

// SIM index (0..15) with active Viber OTP session from heartbeat, or -1
int getPriorityMissedCallSlot();

// Parse +CLIP already in UART or AT buffer (call before flush / during CMGL read)
void processCallUrcDuringAtRead(int slot);

// Drain UART for +CLIP before AT flush (sendATCapture calls this)
void drainUartForMissedCall(int slot);

// Parse RING / +CLIP from a UART or AT response buffer
void processCallUrcFromBuffer(int slot, const char* buf);

// After heartbeat loads activeSessions — send queued missed calls for Viber OTP slots
void flushPendingMissedCallsAfterHeartbeat();
