#include "status_led.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>

extern volatile bool otaInProgress;

static volatile bool simInitActive = false;
static unsigned long lastBlinkMs = 0;
static bool blinkPhase = false;

static void ledOff() {
    pinMode(STATUS_LED_PIN, OUTPUT);
#if STATUS_LED_ACTIVE_LOW
    digitalWrite(STATUS_LED_PIN, HIGH);
#else
    digitalWrite(STATUS_LED_PIN, LOW);
#endif
}

static void ledOn() {
    pinMode(STATUS_LED_PIN, OUTPUT);
#if STATUS_LED_ACTIVE_LOW
    digitalWrite(STATUS_LED_PIN, LOW);
#else
    digitalWrite(STATUS_LED_PIN, HIGH);
#endif
}

static bool staConnected() {
    return WiFi.status() == WL_CONNECTED;
}

static void tickBlink(unsigned long intervalMs) {
    const unsigned long now = millis();
    if (now - lastBlinkMs >= intervalMs) {
        lastBlinkMs = now;
        blinkPhase = !blinkPhase;
    }
    if (blinkPhase) {
        ledOn();
    } else {
        ledOff();
    }
}

void statusLedSetSimInitActive(bool active) {
    simInitActive = active;
    if (active) {
        lastBlinkMs = millis();
        blinkPhase = false;
    }
}

bool statusLedIsSimInitActive() {
    return simInitActive;
}

void statusLedInit() {
    lastBlinkMs = millis();
    blinkPhase = false;
    statusLedTick();
}

void statusLedSetBootComplete() {
    statusLedTick();
}

void statusLedOnOtaEnded() {
    lastBlinkMs = millis();
    blinkPhase = false;
    statusLedTick();
}

void statusLedTick() {
#if !STATUS_LED_ENABLED
    return;
#endif
    if (otaInProgress) {
        tickBlink(STATUS_LED_OTA_BLINK_MS);
        return;
    }
    if (simInitActive) {
        tickBlink(STATUS_LED_SIM_INIT_BLINK_MS);
        return;
    }
    if (staConnected()) {
        ledOff();
    } else {
        ledOn();
    }
}
