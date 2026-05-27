// ============================================================================

// USSD *143# — per-SIM manual check + bulk queue with result summary

// ============================================================================



#include "ussd.h"

#include "sim800.h"

#include "mux.h"

#include "logger.h"

#include "utils.h"

#include <Arduino.h>

#include <ctype.h>



extern SimState simStates[SIM_COUNT];



bool isWebRefreshAllInProgress();

static bool ussdSlotHasNumber(int slot) {
    if (slot < 0 || slot >= SIM_COUNT) return false;
    return !charBufIsEmpty(simStates[slot].number);
}

static bool bulkActive = false;

static int bulkNextIdx = 0;

static int bulkTotal = 0;

static int bulkDone = 0;

static int bulkCurrentSlot = -1;

static unsigned long bulkPauseUntil = 0;

static unsigned long bulkStartedMs = 0;



static char bulkResultJson[3072];

static bool bulkResultReady = false;

static bool manualActive = false;

static int manualSlot = -1;

static int manualLastSlot = -1;



static bool copyLine(const char* start, char* out, size_t outSize, size_t maxLen) {

    if (!out || outSize < 1) return false;

    out[0] = '\0';

    if (!start) return false;



    size_t j = 0;

    for (size_t k = 0; start[k] && j < outSize - 1 && k < maxLen; k++) {

        char c = start[k];

        if (c == '\r' || c == '\n') break;

        out[j++] = c;

    }

    out[j] = '\0';

    while (j > 0 && (out[j - 1] == ' ')) {

        out[--j] = '\0';

    }

    return j > 0;

}



static bool extractPhpAmount(const char* s, char* out, size_t outSize) {

    if (!s || !out || outSize < 2) return false;



    for (const char* p = s; *p; p++) {

        if (p[0] == 'P' && isdigit((unsigned char)p[1])) {

            size_t j = 0;

            out[j++] = 'P';

            for (p++; *p && j < outSize - 1; p++) {

                if (isdigit((unsigned char)*p) || *p == '.' || *p == ',') {

                    out[j++] = *p;

                } else {

                    break;

                }

            }

            out[j] = '\0';

            return j > 1;

        }

        if (strncmp(p, "PHP", 3) == 0 && isdigit((unsigned char)p[3])) {

            size_t j = 0;

            for (; *p && j < outSize - 1; p++) {

                if (isdigit((unsigned char)*p) || *p == '.' || *p == ',' ||

                    *p == 'P' || *p == 'H') {

                    out[j++] = *p;

                } else {

                    break;

                }

            }

            out[j] = '\0';

            return j > 3;

        }

    }

    return false;

}



// Turn modem USSD text into short UI string; sets hasBalance when an amount was found.

static void ussdFormatSummary(const char* raw, bool modemOk, char* out, size_t outSize, bool* hasBalance) {

    if (hasBalance) *hasBalance = false;

    if (!out || outSize < 1) return;

    out[0] = '\0';



    if (!raw || !raw[0] || !modemOk) {

        snprintf(out, outSize, "No response");

        return;

    }



    const char* bal = strstr(raw, "BAL:");

    if (!bal) bal = strstr(raw, "Bal:");

    if (!bal) bal = strstr(raw, "bal:");



    if (bal) {

        bal += 4;

        while (*bal == ' ' || *bal == ':') bal++;



        char line[72];

        if (!copyLine(bal, line, sizeof(line), sizeof(line) - 1)) {

            snprintf(out, outSize, "Error");

            return;

        }



        char amt[20];

        if (extractPhpAmount(line, amt, sizeof(amt))) {

            snprintf(out, outSize, "%s", amt);

            if (hasBalance) *hasBalance = true;

            return;

        }

        snprintf(out, outSize, "%s", line);

        if (hasBalance) *hasBalance = true;

        return;

    }



    if (strstr(raw, "1)") || strstr(raw, "2)") || strstr(raw, "1.")) {

        snprintf(out, outSize, "Error");

        return;

    }

    char amt[20];

    if (extractPhpAmount(raw, amt, sizeof(amt))) {

        snprintf(out, outSize, "%s", amt);

        if (hasBalance) *hasBalance = true;

        return;

    }

    snprintf(out, outSize, "Error");

}



static void buildBulkResultJson() {

    int okCount = 0;

    int failCount = 0;



    for (int i = 0; i < SIM_COUNT; i++) {

        if (simStates[i].ussdLastCheckMs < bulkStartedMs) continue;

        if (simStates[i].ussdStatus == 1) okCount++;

        else failCount++;

    }



    int pos = snprintf(bulkResultJson, sizeof(bulkResultJson),

        "{\"success\":true,\"in_progress\":false,\"ok_count\":%d,\"fail_count\":%d,\"results\":[",

        okCount, failCount);



    bool first = true;

    for (int i = 0; i < SIM_COUNT; i++) {

        if (simStates[i].ussdLastCheckMs < bulkStartedMs) continue;



        const bool ok = (simStates[i].ussdStatus == 1);

        char numEsc[64];

        char msgEsc[96];

        jsonEscape(simStates[i].number[0] ? simStates[i].number : "-", numEsc, sizeof(numEsc));

        jsonEscape(simStates[i].ussdLastResult, msgEsc, sizeof(msgEsc));



        if (!first) pos += snprintf(bulkResultJson + pos, sizeof(bulkResultJson) - pos, ",");

        first = false;



        pos += snprintf(bulkResultJson + pos, sizeof(bulkResultJson) - pos,
            "{\"slot\":%d,\"sim\":%d,\"number\":%s,\"ok\":%s,\"message\":%s,\"duration_sec\":%u}",
            i, i + 1, numEsc, ok ? "true" : "false", msgEsc,
            (unsigned)simStates[i].ussdLastDurationSec);



        if (pos >= (int)sizeof(bulkResultJson) - 32) break;

    }



    pos += snprintf(bulkResultJson + pos, sizeof(bulkResultJson) - pos, "]}");

    bulkResultReady = true;

}



bool ussdBulkInProgress() {

    return bulkActive;

}



int ussdBulkCurrentSlot() {

    return bulkCurrentSlot;

}



int ussdBulkDoneCount() {

    return bulkDone;

}



int ussdBulkTotalCount() {

    return bulkTotal;

}



void ussdStartBulk() {

    bulkActive = true;

    bulkNextIdx = 0;

    bulkDone = 0;

    bulkCurrentSlot = -1;

    bulkPauseUntil = 0;

    bulkStartedMs = millis();

    bulkResultReady = false;

    bulkResultJson[0] = '\0';



    bulkTotal = 0;
    for (int i = 0; i < SIM_COUNT; i++) {
        simStates[i].ussdStatus = 0;
        simStates[i].ussdLastDurationSec = 0;
        charBufClear(simStates[i].ussdLastResult, sizeof(simStates[i].ussdLastResult));
        if (ussdSlotHasNumber(i)) bulkTotal++;
    }



    appendMonitorLog("[USSD] Bulk *143# started");

}



bool ussdRunOnSlot(int slot) {
    if (slot < 0 || slot >= SIM_COUNT) return false;
    if (!ussdSlotHasNumber(slot)) return false;

    simStates[slot].ussdStatus = 3;
    simStates[slot].ussdLastDurationSec = 0;
    charBufClear(simStates[slot].ussdLastResult, sizeof(simStates[slot].ussdLastResult));

    const unsigned long ussdStartMs = millis();

    setSimBusy(true);
    // Avoid webserver re-entrancy / extra heap churn while modem session is open.
    // This makes USSD more stable on marginal SIMs.
    setSimYieldToWebServer(false);
    selectSIM(slot);
    setActiveSimSlot(slot);

    char raw[USSD_RAW_SIZE];
    int mode = -1;
    const bool modemOk = runUssdCode(USSD_BALANCE_CODE, raw, sizeof(raw), &mode);



    bool hasBalance = false;

    char summary[USSD_RESULT_SIZE];

    ussdFormatSummary(raw, modemOk, summary, sizeof(summary), &hasBalance);



    charBufSet(simStates[slot].ussdLastResult, sizeof(simStates[slot].ussdLastResult), summary);

    simStates[slot].ussdStatus = hasBalance ? 1 : 2;
    {
        unsigned long elapsed = millis() - ussdStartMs;
        simStates[slot].ussdLastDurationSec =
            (elapsed > 255000UL) ? 255 : (uint8_t)((elapsed + 999UL) / 1000UL);
    }
    simStates[slot].ussdLastCheckMs = millis();
    setSimBusy(false);
    setSimYieldToWebServer(true);

    char logDetail[64];
    snprintf(logDetail, sizeof(logDetail), "%s (%us)", summary,
        (unsigned)simStates[slot].ussdLastDurationSec);
    char simNum[8];
    snprintf(simNum, sizeof(simNum), "%d", slot + 1);
    logMsg2Val("[USSD] SIM", simNum, hasBalance ? "OK" : "FAIL", logDetail);

    return hasBalance;

}



bool ussdManualInProgress() {
    return manualActive;
}

bool ussdStartManual(int slot) {
    if (slot < 0 || slot >= SIM_COUNT) return false;
    if (!ussdSlotHasNumber(slot)) return false;
    if (bulkActive || manualActive) return false;

    // Clear last result so a stale result from a previous run isn't returned
    manualLastSlot = -1;
    manualActive = true;
    manualSlot = slot;
    simStates[slot].ussdStatus = 3;
    simStates[slot].ussdLastDurationSec = 0;
    charBufClear(simStates[slot].ussdLastResult, sizeof(simStates[slot].ussdLastResult));
    return true;
}

void ussdWriteManualStatusJson(char* buf, size_t bufSize) {
    if (!buf || bufSize < 32) return;

    if (manualActive) {
        snprintf(buf, bufSize,
            "{\"success\":true,\"in_progress\":true,\"slot\":%d,\"sim\":%d}",
            manualSlot, manualSlot + 1);
        return;
    }

    if (manualLastSlot < 0 || manualLastSlot >= SIM_COUNT) {
        snprintf(buf, bufSize, "{\"success\":true,\"in_progress\":false}");
        return;
    }

    const int s = manualLastSlot;
    const bool ok = simStates[s].ussdStatus == 1;
    char numEsc[96];
    char msgEsc[USSD_RESULT_SIZE * 2];
    jsonEscape(simStates[s].number[0] ? simStates[s].number : "-", numEsc, sizeof(numEsc));
    jsonEscape(simStates[s].ussdLastResult, msgEsc, sizeof(msgEsc));
    snprintf(buf, bufSize,
        "{\"success\":true,\"in_progress\":false,\"slot\":%d,\"sim\":%d,\"number\":%s,"
        "\"ok\":%s,\"message\":%s,\"duration_sec\":%u}",
        s, s + 1, numEsc, ok ? "true" : "false", msgEsc,
        (unsigned)simStates[s].ussdLastDurationSec);
}

void ussdWriteBulkStatusJson(char* buf, size_t bufSize) {

    if (!buf || bufSize < 32) return;



    if (bulkActive) {

        snprintf(buf, bufSize,

            "{\"success\":true,\"in_progress\":true,\"current_slot\":%d,\"current_sim\":%d,"

            "\"done\":%d,\"total\":%d}",

            bulkCurrentSlot,

            bulkCurrentSlot >= 0 ? bulkCurrentSlot + 1 : 0,

            bulkDone,

            bulkTotal);

        return;

    }



    if (bulkResultReady && bulkResultJson[0]) {

        snprintf(buf, bufSize, "%s", bulkResultJson);

        return;

    }



    snprintf(buf, bufSize,

        "{\"success\":true,\"in_progress\":false,\"ok_count\":0,\"fail_count\":0,\"results\":[]}");

}



void ussdTick() {

    if (isSimBusy()) return;

    if (isWebRefreshAllInProgress()) return;

    if (millis() < bulkPauseUntil) return;

    if (manualActive) {
        const int s = manualSlot;
        manualActive = false;
        manualSlot = -1;
        ussdRunOnSlot(s);
        manualLastSlot = s;
        return;
    }

    if (!bulkActive) return;



    while (bulkNextIdx < SIM_COUNT) {

        const int i = bulkNextIdx++;
        if (!ussdSlotHasNumber(i)) {
            continue;
        }
        bulkCurrentSlot = i;
        ussdRunOnSlot(i);

        bulkDone++;

        bulkPauseUntil = millis() + USSD_BULK_GAP_MS;

        return;

    }



    bulkActive = false;

    bulkCurrentSlot = -1;

    buildBulkResultJson();

    appendMonitorLog("[USSD] Bulk *143# complete");

}


