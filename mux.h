#pragma once

// ============================================================================
// SIM Multiplexer Control
// Handles CD74HC4067 16-channel multiplexer switching
// ============================================================================

#include "config.h"

#if USE_DUAL_UART
#include "sim800.h"  // For setActiveSimSlot, getActiveSimSlot
#endif

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

// Initialize multiplexer pins (mux mode only)
// Call once in setup()
#if !USE_DUAL_UART
void initMux();
#else
// In dual-UART mode, no mux initialization needed
inline void initMux() {}
#endif

// -----------------------------------------------------------------------------
// Channel Selection
// -----------------------------------------------------------------------------

// Map logical slot (0-15) to mux channel (0-15) per SLOT_TO_MUX_CHANNEL wiring
#if !USE_DUAL_UART
int logicalSlotToMuxChannel(int logicalSlot);
#endif

// Select SIM by logical slot (0-15 in mux mode, 0-1 in dual-UART mode)
// In mux mode: maps slot -> mux channel, waits MUX_SETTLE_MS, flushes UART
// In dual-UART mode: calls setActiveSimSlot() to switch UART and flushes buffer
#if !USE_DUAL_UART
void selectSIM(int logicalSlot);
#else
// In dual-UART mode, selectSIM sets the active UART and flushes residual data
inline void selectSIM(int channel) {
    setActiveSimSlot(channel);
    // Flush the newly selected UART to clear any residual data
    HardwareSerial& serial = simSerial();
    delay(10);  // Small settling time
    for (int pass = 0; pass < 3; pass++) {
        int flushed = 0;
        unsigned long flushStart = millis();
        while (serial.available() && flushed < 100 && (millis() - flushStart < 50)) {
            (void)serial.read();
            flushed++;
        }
        if (flushed > 0) {
            delay(5);
        }
    }
}
#endif

// Get currently selected logical slot (not raw mux channel)
#if !USE_DUAL_UART
int getCurrentLogicalSlot();
inline int getCurrentMuxChannel() { return getCurrentLogicalSlot(); }
#else
inline int getCurrentLogicalSlot() { return getActiveSimSlot(); }
inline int getCurrentMuxChannel() { return getActiveSimSlot(); }
#endif

// -----------------------------------------------------------------------------
// Reset Control
// -----------------------------------------------------------------------------

// Reset a specific SIM module via reset multiplexer
// Uses RESET_PIN to toggle reset line (mux mode only)
#if !USE_DUAL_UART
void resetSIM(int channel);
#else
// In dual-UART mode, reset is not supported via software
inline void resetSIM(int channel) {}
#endif

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

// Check if channel is valid
inline bool isValidChannel(int ch) {
    return (ch >= 0 && ch < SIM_COUNT);
}
