// ============================================================================
// SIM Multiplexer Control Implementation
// ============================================================================

#include "mux.h"
#include "sim800.h"
#include "logger.h"
#include <Arduino.h>

#if !USE_DUAL_UART

// Current channel tracking
static int currentChannel = 0;

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

void selectSIM(int channel) {
    // Clamp to valid range
    if (channel < 0) channel = 0;
    if (channel >= SIM_COUNT) channel = SIM_COUNT - 1;

    // Skip if already on this channel
    if (currentChannel == channel) {
        return;
    }

    // Set S0-S3 based on channel bits
    digitalWrite(MUX_S0, (channel & 0x01) ? HIGH : LOW);
    digitalWrite(MUX_S1, (channel & 0x02) ? HIGH : LOW);
    digitalWrite(MUX_S2, (channel & 0x04) ? HIGH : LOW);
    digitalWrite(MUX_S3, (channel & 0x08) ? HIGH : LOW);

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

    currentChannel = channel;
}

// -----------------------------------------------------------------------------

int getCurrentMuxChannel() {
    return currentChannel;
}

// -----------------------------------------------------------------------------
// Reset Control
// -----------------------------------------------------------------------------

void resetSIM(int channel) {
    if (!isValidChannel(channel)) return;

    logMsgInt("[MUX] Resetting SIM", channel + 1);

    // Select the channel first
    selectSIM(channel);

    // Toggle reset line
    digitalWrite(RESET_PIN, LOW);
    delay(200);
    digitalWrite(RESET_PIN, HIGH);

    // Wait for SIM to boot
    delay(6000);

    logMsgInt("[MUX] SIM reset complete", channel + 1);
}

#endif // !USE_DUAL_UART
