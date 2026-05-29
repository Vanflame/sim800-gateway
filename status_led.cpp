#include "status_led.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>

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

void statusLedInit() {
    if (staConnected()) {
        ledOff();
    } else {
        ledOn();
    }
}

void statusLedSetBootComplete() {
    statusLedTick();
}

void statusLedOnOtaEnded() {
    statusLedTick();
}

void statusLedTick() {
#if !STATUS_LED_ENABLED
    return;
#endif
    if (staConnected()) {
        ledOff();
    } else {
        ledOn();
    }
}
