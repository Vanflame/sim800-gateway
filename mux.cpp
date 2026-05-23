// ============================================================================
// SIM Multiplexer Control Implementation
// ============================================================================

#include "mux.h"
#include "sim800.h"
#include "logger.h"
#include <Arduino.h>

#if !USE_DUAL_UART

// Logical slot -> mux channel (see config.h SLOT_TO_MUX_CHANNEL_INIT)
static const uint8_t SLOT_TO_MUX_CHANNEL[SIM_COUNT] = SLOT_TO_MUX_CHANNEL_INIT;

// Current logical slot tracking
static int currentLogicalSlot = 0;

int logicalSlotToMuxChannel(int logicalSlot) {
    if (logicalSlot < 0) logicalSlot = 0;
    if (logicalSlot >= SIM_COUNT) logicalSlot = SIM_COUNT - 1;
    return (int)SLOT_TO_MUX_CHANNEL[logicalSlot];
}

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

void initMux() {
    // Set MUX control pins as outputs
    pinMode(MUX_S0, OUTPUT);
    pinMode(MUX_S1, OUTPUT);
    pinMode(MUX_S2, OUTPUT);
    pinMode(MUX_S3, OUTPUT);

    // Set reset pin as output (high = not reset)
    pinMode(RESET_PIN, OUTPUT);
    digitalWrite(RESET_PIN, HIGH);

    // Initialize to channel 0
    selectSIM(0);

    logMsg("[MUX] Initialized");
}

// -----------------------------------------------------------------------------
// Channel Selection
// -----------------------------------------------------------------------------

void selectSIM(int logicalSlot) {
    // Clamp to valid range
    if (logicalSlot < 0) logicalSlot = 0;
    if (logicalSlot >= SIM_COUNT) logicalSlot = SIM_COUNT - 1;

    // Skip if already on this logical slot
    if (currentLogicalSlot == logicalSlot) {
        return;
    }

    const int muxChannel = logicalSlotToMuxChannel(logicalSlot);

    // Set S0-S3 from mux channel bits (not logical slot index)
    digitalWrite(MUX_S0, (muxChannel & 0x01) ? HIGH : LOW);
    digitalWrite(MUX_S1, (muxChannel & 0x02) ? HIGH : LOW);
    digitalWrite(MUX_S2, (muxChannel & 0x04) ? HIGH : LOW);
    digitalWrite(MUX_S3, (muxChannel & 0x08) ? HIGH : LOW);

    // Wait for MUX to settle (longer for stability)
    delay(MUX_SETTLE_MS);

    // Thorough UART flush to avoid ghost bytes from previous SIM
    // Multiple passes with delays to ensure all residual data is cleared
    HardwareSerial& serial = simSerial();
    for (int pass = 0; pass < 3; pass++) {
        int flushed = 0;
        while (serial.available() && flushed < UART_FLUSH_ITER) {
            (void)serial.read();
            flushed++;
        }
        if (flushed > 0) {
            delay(UART_FLUSH_WAIT_MS);
        }
    }

    currentLogicalSlot = logicalSlot;
}

// -----------------------------------------------------------------------------

int getCurrentLogicalSlot() {
    return currentLogicalSlot;
}

// -----------------------------------------------------------------------------
// Reset Control
// -----------------------------------------------------------------------------

void resetSIM(int logicalSlot) {
    if (!isValidChannel(logicalSlot)) return;

    logMsgInt("[MUX] Resetting SIM", logicalSlot + 1);

    // Select the logical slot first (maps to correct mux channel)
    selectSIM(logicalSlot);

    // Toggle reset line
    digitalWrite(RESET_PIN, LOW);
    delay(200);
    digitalWrite(RESET_PIN, HIGH);

    // Wait for SIM to boot
    delay(6000);

    logMsgInt("[MUX] SIM reset complete", logicalSlot + 1);
}

#endif // !USE_DUAL_UART
