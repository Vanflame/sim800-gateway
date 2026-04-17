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

// Select SIM channel (0-15 in mux mode, 0-1 in dual-UART mode)
// In mux mode: waits for MUX_SETTLE_MS after switching, flushes UART
// In dual-UART mode: calls setActiveSimSlot() to switch UART
#if !USE_DUAL_UART
void selectSIM(int channel);
#else
// In dual-UART mode, selectSIM just sets the active UART
inline void selectSIM(int channel) { setActiveSimSlot(channel); }
#endif

// Get currently selected channel
#if !USE_DUAL_UART
int getCurrentMuxChannel();
#else
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
