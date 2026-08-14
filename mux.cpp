// ============================================================================
// SIM Multiplexer Control Implementation
// ============================================================================

#include "mux.h"
#include "mux_map.h"
#include "sim800.h"
#include "webui.h"
#include "logger.h"
#include <Arduino.h>

#if !USE_DUAL_UART

// UI slot index -> physical mux channel (NVS-backed; see mux_map.cpp)
static int currentSlot = 0;
static int lastAppliedMuxChannel = -1;

void muxInvalidateSelection() {
    lastAppliedMuxChannel = -1;
}

int logicalSlotToMuxChannel(int logicalSlot) {
    if (logicalSlot < 0) logicalSlot = 0;
    if (logicalSlot >= SIM_COUNT) logicalSlot = SIM_COUNT - 1;
    return (int)muxMapChannelForSlot(logicalSlot);
}

int muxChannelToLogicalSlot(int muxChannel) {
    if (muxChannel < 0) muxChannel = 0;
    if (muxChannel >= SIM_COUNT) muxChannel = SIM_COUNT - 1;
    for (int i = 0; i < SIM_COUNT; i++) {
        if ((int)muxMapChannelForSlot(i) == muxChannel) {
            return i;
        }
    }
    return muxChannel;
}

int muxUiSimNumber(int slot) {
    if (slot < 0) slot = 0;
    if (slot >= SIM_COUNT) slot = SIM_COUNT - 1;
    return slot + 1;
}

void initMux() {
    pinMode(MUX_S0, OUTPUT);
    pinMode(MUX_S1, OUTPUT);
    pinMode(MUX_S2, OUTPUT);
    pinMode(MUX_S3, OUTPUT);

    pinMode(RESET_PIN, OUTPUT);
    digitalWrite(RESET_PIN, HIGH);

    selectSIM(0);

    logMsg("[MUX] Initialized (custom slot -> mux map)");
}

static void cooperativeDelayMux(unsigned long ms) {
    const unsigned long until = millis() + ms;
    while ((long)(until - millis()) > 0) {
        handleWebRequests();
        yield();
        delay(5);
    }
}

void selectSIM(int slot) {
    if (slot < 0) slot = 0;
    if (slot >= SIM_COUNT) slot = SIM_COUNT - 1;

    const int muxChannel = logicalSlotToMuxChannel(slot);

    if (currentSlot == slot && lastAppliedMuxChannel == muxChannel) {
        return;
    }

    digitalWrite(MUX_S0, (muxChannel & 0x01) ? HIGH : LOW);
    digitalWrite(MUX_S1, (muxChannel & 0x02) ? HIGH : LOW);
    digitalWrite(MUX_S2, (muxChannel & 0x04) ? HIGH : LOW);
    digitalWrite(MUX_S3, (muxChannel & 0x08) ? HIGH : LOW);

    cooperativeDelayMux(muxMapSettleMs());

    HardwareSerial& serial = simSerial();
    for (int pass = 0; pass < 3; pass++) {
        int flushed = 0;
        while (serial.available() && flushed < UART_FLUSH_ITER) {
            (void)serial.read();
            flushed++;
        }
        if (flushed > 0) {
            cooperativeDelayMux(UART_FLUSH_WAIT_MS);
        }
    }

    currentSlot = slot;
    lastAppliedMuxChannel = muxChannel;
}

int getCurrentLogicalSlot() {
    return currentSlot;
}

void resetSIM(int slot) {
    if (!isValidChannel(slot)) return;

    logMsgInt("[MUX] Resetting SIM", slot + 1);

    selectSIM(slot);

    digitalWrite(RESET_PIN, LOW);
    cooperativeDelayMux(200);
    digitalWrite(RESET_PIN, HIGH);
    cooperativeDelayMux(6000);

    logMsgInt("[MUX] SIM reset complete", slot + 1);
}

#endif // !USE_DUAL_UART
