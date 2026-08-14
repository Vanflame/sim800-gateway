#pragma once

#include <Arduino.h>
#include <stddef.h>

#define SENDER_MAP_MAX_ENTRIES  32
#define SENDER_MAP_CODE_SIZE    16
#define SENDER_MAP_BRAND_SIZE   32

typedef struct {
    char code[SENDER_MAP_CODE_SIZE];
    char brand[SENDER_MAP_BRAND_SIZE];
} SenderMapEntry;

void senderMapInitDefaults();
void senderMapLoad();
bool senderMapSave();

int senderMapCount();
const SenderMapEntry* senderMapEntry(int index);

/** Returns mapped brand or nullptr if no match. */
const char* senderMapLookup(const char* rawSender);

bool senderMapUpsert(const char* code, const char* brand);
bool senderMapRemove(const char* code);
void senderMapClear();

/** JSON array: {"mappings":[{"code":"*4","brand":"JoyRide"},...]} */
bool senderMapImportJson(const char* json);
size_t senderMapExportJson(char* out, size_t outSize);
