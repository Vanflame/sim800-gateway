#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "config.h"

#if !USE_DUAL_UART

void muxMapInitDefaults();
void muxMapLoad();
bool muxMapSave();

/** Re-drive mux GPIO after mapping change (without reboot). */
void muxMapApplyHardware();

/** Per logical slot (0..15) -> mux channel 0..15 */
uint8_t muxMapChannelForSlot(int logicalSlot);
void muxMapSetChannelForSlot(int logicalSlot, uint8_t muxChannel);

unsigned long muxMapSettleMs();
void muxMapSetSettleMs(unsigned long ms);

/** JSON: {"channels":[0..15 x16],"settle_ms":350} */
bool muxMapImportJson(const char* json);
size_t muxMapExportJson(char* out, size_t outSize);

#endif
