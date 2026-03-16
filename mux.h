#pragma once

// ============================================================================
// SIM Multiplexer Control
// Handles CD74HC4067 16-channel multiplexer switching
// ============================================================================

#include "config.h"

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

// Initialize multiplexer pins
// Call once in setup()
void initMux();

// -----------------------------------------------------------------------------
// Channel Selection
// -----------------------------------------------------------------------------

// Select SIM channel (0-15)
// Waits for MUX_SETTLE_MS after switching
// Flushes UART to avoid ghost bytes from previous SIM
void selectSIM(int channel);

// Get currently selected channel
int getCurrentMuxChannel();

// -----------------------------------------------------------------------------
// Reset Control
// -----------------------------------------------------------------------------

// Reset a specific SIM module via reset multiplexer
// Uses RESET_PIN to toggle reset line
void resetSIM(int channel);

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

// Check if channel is valid
inline bool isValidChannel(int ch) {
    return (ch >= 0 && ch < SIM_COUNT);
}
