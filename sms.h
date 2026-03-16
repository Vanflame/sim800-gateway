#pragma once

// ============================================================================
// SMS Parsing and Queue Management
// Handles SMS reading, parsing, and retry queue
// ============================================================================

#include "config.h"

// -----------------------------------------------------------------------------
// SMS Structure
// -----------------------------------------------------------------------------

// Parsed SMS message
typedef struct {
    int simSlot;                        // 0-15
    int messageIndex;                   // Index in SIM memory
    char sender[PHONE_BUFFER_SIZE];     // Sender phone number
    char message[SMS_MESSAGE_SIZE];     // Message content
    char timestamp[32];                 // Network timestamp
    bool unread;                        // Is unread
} SmsMessage;

// -----------------------------------------------------------------------------
// SMS Queue (Pending for retry)
// -----------------------------------------------------------------------------

// Initialize SMS queue
void initSMSQueue();

// Add SMS to pending queue (for retry when network unavailable)
bool enqueuePendingSms(int simSlot, const char* simNumber, const char* sender, const char* message);

// Get next pending SMS for retry
bool dequeuePendingSms(PendingSms* out);

// Get pending queue count
int getPendingSmsCount();

// Clear pending queue
void clearPendingSms();

// Process pending SMS queue (retry sending to backend)
void processPendingSmsQueue();

// -----------------------------------------------------------------------------
// SMS Reading
// -----------------------------------------------------------------------------

// Check for new unread SMS on current SIM
// Returns count of new messages found
int checkUnreadSMS(int simSlot);

// Read all SMS from current SIM
// Returns count of messages
int readAllSMS(int simSlot, SmsMessage* messages, int maxMessages);

// Parse SMS from AT+CMGL response
int parseSMSList(const char* response, SmsMessage* messages, int maxMessages);

// -----------------------------------------------------------------------------
// SMS Processing
// -----------------------------------------------------------------------------

// Process incoming SMS (parse, log, forward to backend)
void processIncomingSMS(int simSlot, const SmsMessage* msg);

// Forward SMS to backend API
bool forwardSmsToBackend(const SmsMessage* msg);

// Forward SMS to backend with full HTTP implementation
bool forwardSmsToBackendFull(const SmsMessage* msg);

// -----------------------------------------------------------------------------
// SMS Polling
// -----------------------------------------------------------------------------

// Initialize SMS polling state
void initSMSPolling();

// Poll all enabled SIMs for new SMS
// Call this in main loop
void pollSIMsForSMS();

// Pause/resume polling
void pauseSmsPolling(unsigned long durationMs);
void resumeSmsPolling();
bool isSmsPollingPaused();

// Get current poll state
int getCurrentPollSim();
bool isPollingInProgress();

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

// Get total SMS received count
unsigned long getTotalSmsReceived();

// Get total SMS forwarded count
unsigned long getTotalSmsForwarded();

// Get total SMS failed count
unsigned long getTotalSmsFailed();
