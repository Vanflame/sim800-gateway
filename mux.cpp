// ============================================================================
// SIM Multiplexer Control Implementation
// ============================================================================

#include "mux.h"
#include "logger.h"
#include <Arduino.h>

// External reference to UART
extern HardwareSerial sim800;

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
    
    // Set S0-S3 based on channel bits
    digitalWrite(MUX_S0, (channel & 0x01) ? HIGH : LOW);
    digitalWrite(MUX_S1, (channel & 0x02) ? HIGH : LOW);
    digitalWrite(MUX_S2, (channel & 0x04) ? HIGH : LOW);
    digitalWrite(MUX_S3, (channel & 0x08) ? HIGH : LOW);
    
    // Wait for MUX to settle
    delay(MUX_SETTLE_MS);
    
    // Flush UART to avoid ghost bytes from previous SIM
    // Cap iterations to prevent blocking
    for (int i = 0; i < UART_FLUSH_ITER && sim800.available(); i++) {
        (void)sim800.read();
        delay(1);
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
