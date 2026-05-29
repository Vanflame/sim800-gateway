#pragma once

void statusLedInit();
void statusLedSetBootComplete();
void statusLedTick();
void statusLedOnOtaEnded();
/** True while checkAllSIMsOnStartup / Run init SIM slots is running. */
void statusLedSetSimInitActive(bool active);
bool statusLedIsSimInitActive();
