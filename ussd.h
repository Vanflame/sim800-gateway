#pragma once

#include "config.h"

// Process background bulk *143# queue (call from main loop)
void ussdTick();

// Run *143# on one logical slot; updates simStates[slot].ussd*
bool ussdRunOnSlot(int slot);

// Queue *143# for every enabled SIM
void ussdStartBulk();

bool ussdBulkInProgress();

// Logical slot currently being checked, or -1
int ussdBulkCurrentSlot();

// How many slots finished in current bulk run
int ussdBulkDoneCount();

// Total slots queued in current bulk run
int ussdBulkTotalCount();

// Build JSON for GET /ussd-bulk-status (progress or final results)
void ussdWriteBulkStatusJson(char* buf, size_t bufSize);

// Manual *143# on one slot (background, non-blocking HTTP)
bool ussdManualInProgress();
bool ussdStartManual(int slot);
void ussdWriteManualStatusJson(char* buf, size_t bufSize);
