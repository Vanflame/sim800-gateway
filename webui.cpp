// ============================================================================
// Web UI Server Implementation
// ============================================================================

#include "webui.h"
#include "ota.h"
#include "calls.h"
#include "config.h"
#include "sim800.h"
#include "mux.h"
#include "sms.h"
#include "logger.h"
#include "utils.h"
#include "ussd.h"
#include "maintenance.h"
#include "network_ping.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Web server on port 80
WebServer server(80);

static volatile bool refreshAllInProgress = false;
static char webJsonBuf[5120];

#define WEB_MESSAGES_LOG_PATH "/littlefs/messages.log"
extern bool webLittleFsReady();

bool isWebRefreshAllInProgress() {
    return refreshAllInProgress;
}

// Preferences for storing settings (defined in main .ino)
extern Preferences preferences;

// External persistent storage functions
extern int readSmsFromFile(char* buf, size_t bufSize, int maxMessages);
extern int readErrorsFromFile(char* buf, size_t bufSize, int maxErrors);

// External state from main .ino
extern SimState simStates[SIM_COUNT];
extern char wifiSsid[64];
extern unsigned long wifiUserSetupUntilMs;
extern char wifiPassword[64];
extern char agentBaseUrl[128];
extern char agentDeviceId[64];
extern char agentBearerToken[AGENT_BEARER_TOKEN_SIZE];
extern char agentRefreshToken[AGENT_REFRESH_TOKEN_SIZE];
extern char agentSimNumber[PHONE_BUFFER_SIZE];
extern int agentSimSlot;
extern char agentApiPath[64];
extern bool agentUseAuth;
extern bool deviceRegistered;
extern bool simRegistered;
extern int activeSim;
extern int currentMuxSim;

// Battery info
extern int batteryPercent;
extern int batteryMv;

// NTP status (from main .ino)
extern bool ntpConfigured;

// Heartbeat pause flag (from main .ino)
extern bool heartbeatPaused;

// Forward declarations for handlers
void handleErrorLog();
void handleClearErrorLog();

// HTML page stored in PROGMEM
static const char INDEX_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SIM800 Gateway | OTPocket Agent</title>
  <link rel="icon" href="data:,">
  <style>
    /* OTPocket Design System */
    :root {
      --bg: #06122b;
      --bg-secondary: #0b1a3a;
      --card: #0b1a3a;
      --card-border: rgba(255,255,255,0.05);
      --text: #f8fafc;
      --text-secondary: #94a3b8;
      --muted: #64748b;
      --primary: #3b82f6;
      --primary-hover: #2563eb;
      --success: #10b981;
      --warning: #f59e0b;
      --danger: #ef4444;
      --border: rgba(255,255,255,0.08);
      --input-bg: rgba(255,255,255,0.05);
      --radius: 0.75rem;
      --shadow: 0 4px 6px -1px rgba(0,0,0,0.3), 0 2px 4px -2px rgba(0,0,0,0.2);
      --sidebar-width: 200px;
    }
    
    * { box-sizing: border-box; margin: 0; padding: 0; }
    
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
      background: var(--bg);
      color: var(--text);
      min-height: 100vh;
      line-height: 1.5;
      -webkit-font-smoothing: antialiased;
    }
    
    /* Layout */
    .app-layout {
      display: flex;
      min-height: 100vh;
      position: relative;
    }
    
    /* Sidebar */
    .sidebar {
      width: var(--sidebar-width);
      background: rgba(11, 26, 58, 0.95);
      border-right: 1px solid var(--border);
      display: flex;
      flex-direction: column;
      position: fixed;
      top: 0;
      left: 0;
      bottom: 0;
      z-index: 200;
      transform: translateX(-100%);
      transition: transform 0.3s ease, width 0.3s ease;
    }
    
    .sidebar.open { transform: translateX(0); }
    
    /* Desktop: show sidebar always */
    @media (min-width: 768px) {
      .sidebar { transform: translateX(0); }
      .main-content { margin-left: var(--sidebar-width); transition: margin-left 0.3s ease; }
      .mobile-header { display: none !important; }
      .overlay.show { opacity: 0 !important; pointer-events: none !important; }
    }
    
    .sidebar-header {
      padding: 16px;
      border-bottom: 1px solid var(--border);
      display: flex;
      align-items: center;
      position: relative;
    }
    
    .sidebar-nav {
      flex: 1;
      padding: 6px;
      overflow-y: auto;
    }

    .nav-restart {
      display: none;
      align-items: center;
      gap: 10px;
      margin: 6px 8px 10px;
      padding: 10px 12px;
      border-radius: 8px;
      background: rgba(245, 158, 11, 0.12);
      border: 1px solid rgba(245, 158, 11, 0.35);
      color: #fbbf24;
      font-size: 11px;
    }
    .nav-restart.show { display: flex; }
    .nav-restart.overdue {
      background: rgba(239, 68, 68, 0.12);
      border-color: rgba(239, 68, 68, 0.4);
      color: #f87171;
    }
    .nav-restart svg { width: 18px; height: 18px; flex-shrink: 0; }
    .nav-restart-label { font-weight: 600; line-height: 1.2; }
    .nav-restart-timer {
      font-family: 'SF Mono', Consolas, monospace;
      font-size: 15px;
      font-weight: 700;
      margin-top: 2px;
    }
    .sidebar.collapsed .nav-restart { display: none !important; }

    /* Mobile restart strip — glued directly under the sticky header */
    .mobile-restart-strip {
      display: none;
      align-items: center;
      gap: 8px;
      padding: 7px 14px;
      background: rgba(245, 158, 11, 0.14);
      border-bottom: 1px solid rgba(245, 158, 11, 0.35);
      font-size: 12px;
      font-weight: 600;
      color: #fbbf24;
      position: sticky;
      top: 49px;
      z-index: 49;
    }
    .mobile-restart-strip.show { display: flex; }
    .mobile-restart-strip.overdue { background: rgba(239, 68, 68, 0.14); border-color: rgba(239, 68, 68, 0.4); color: #f87171; }
    .mobile-restart-strip svg { width: 14px; height: 14px; flex-shrink: 0; }
    .mobile-restart-strip-timer { font-family: 'SF Mono', Consolas, monospace; font-weight: 700; margin-left: 4px; }
    @media (min-width: 768px) { .mobile-restart-strip { display: none !important; } }

    /* Sidebar collapse toggle (desktop) */
    .sidebar-collapse-btn {
      display: none;
      background: none;
      border: none;
      color: var(--muted);
      cursor: pointer;
      padding: 4px;
      border-radius: 6px;
      margin-left: auto;
      line-height: 0;
      transition: color 0.2s, background 0.2s;
    }
    .sidebar-collapse-btn:hover { color: var(--text); background: rgba(255,255,255,0.07); }
    .sidebar-collapse-btn svg { width: 18px; height: 18px; }
    @media (min-width: 768px) { .sidebar-collapse-btn { display: flex; align-items: center; } }

    /* Collapsed sidebar */
    @media (min-width: 768px) {
      .sidebar.collapsed { width: 52px; }
      .sidebar.collapsed .logo-text { display: none; }
      .sidebar.collapsed .nav-item { justify-content: center; padding: 9px 0; }
      .sidebar.collapsed .nav-item span,
      .sidebar.collapsed .nav-item-label { display: none; }
      .sidebar.collapsed .nav-badge { display: none !important; }
      .sidebar.collapsed .sidebar-collapse-btn { margin-left: 0; }
      .main-content.sidebar-collapsed { margin-left: 52px; }
    }

    /* Mask URL field */
    .url-field-wrap { position: relative; }
    .url-field-wrap input { padding-right: 72px; font-family: 'SF Mono', Consolas, monospace; font-size: 12px; letter-spacing: 0.04em; }
    .url-field-actions {
      position: absolute;
      right: 6px;
      top: 50%;
      transform: translateY(-50%);
      display: flex;
      gap: 4px;
    }
    .url-field-btn {
      background: rgba(255,255,255,0.07);
      border: none;
      color: var(--muted);
      cursor: pointer;
      padding: 4px 6px;
      border-radius: 4px;
      font-size: 11px;
      font-weight: 600;
      transition: background 0.15s, color 0.15s;
      line-height: 1;
    }
    .url-field-btn:hover { background: rgba(255,255,255,0.13); color: var(--text); }
    
    .nav-item {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 9px 12px;
      border-radius: 8px;
      color: var(--text-secondary);
      text-decoration: none;
      cursor: pointer;
      transition: all 0.2s;
      margin-bottom: 4px;
    }
    
    .nav-item:hover { background: rgba(255,255,255,0.05); color: var(--text); }
    .nav-item.active { background: rgba(59,130,246,0.15); color: var(--primary); }
    
    .nav-item svg { width: 20px; height: 20px; flex-shrink: 0; }
    
    .nav-badge {
      margin-left: auto;
      background: var(--primary);
      color: white;
      font-size: 11px;
      padding: 2px 6px;
      border-radius: 10px;
      font-weight: 600;
    }
    
    .sessions-card {
      margin-top: 16px;
    }

    .sessions-list {
      display: flex;
      flex-direction: column;
      gap: 8px;
    }

    .sessions-list .session-row {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 8px 12px;
      background: var(--bg);
      border-radius: 6px;
    }

    .sessions-list .session-app {
      font-weight: 600;
      color: var(--primary);
      min-width: 80px;
    }

    .sessions-list .session-sim {
      color: var(--muted);
      font-size: 12px;
    }

    .sessions-list .session-msgs {
      color: var(--muted);
      font-size: 12px;
    }

    .sessions-list .session-time {
      font-family: 'SF Mono', Consolas, monospace;
      color: var(--success);
      font-weight: 600;
      margin-left: auto;
    }

    .sessions-list .session-time.warning {
      color: var(--warning);
    }

    .sessions-list .session-time.danger {
      color: var(--danger);
    }
    
    /* Main content */
    .main-content {
      flex: 1;
      margin-left: 0;
      min-height: 100vh;
    }
    
    /* Mobile header */
    .mobile-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 12px 16px;
      background: rgba(11, 26, 58, 0.9);
      border-bottom: 1px solid var(--border);
      position: sticky;
      top: 0;
      z-index: 50;
    }
    
    .menu-btn {
      background: none;
      border: none;
      color: var(--text);
      padding: 8px;
      cursor: pointer;
      border-radius: 8px;
    }
    
    .menu-btn:hover { background: rgba(255,255,255,0.1); }
    
    .overlay {
      position: fixed;
      inset: 0;
      background: rgba(0,0,0,0.5);
      z-index: 50;
      opacity: 0;
      pointer-events: none;
      transition: opacity 0.3s;
    }
    
    .overlay.show { opacity: 1; pointer-events: auto; }
    
    .container {
      padding: 12px 14px;
      padding-bottom: 72px;
      max-width: 1200px;
      margin: 0 auto;
    }
    
    .container.wide { max-width: 100%; }
    
    /* Login container - centered */
    .login-container {
      min-height: calc(100vh - 48px);
      display: flex;
      align-items: center;
      justify-content: center;
    }
    
    /* Logo */
    .logo {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    
    .logo-icon {
      width: 36px;
      height: 36px;
      background: linear-gradient(135deg, var(--primary), #7c3aed);
      border-radius: 10px;
      display: flex;
      align-items: center;
      justify-content: center;
      font-weight: 700;
      font-size: 18px;
      color: white;
    }
    
    .logo-text {
      font-size: 18px;
      font-weight: 600;
      letter-spacing: -0.02em;
    }
    
    .logo-text span { color: var(--primary); }
    
    .header-status {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    
    .status-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--danger);
      animation: pulse 2s infinite;
    }
    
    .status-dot.ok { background: var(--success); }
    .status-dot.warn { background: var(--warning); }
    
    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.5; }
    }
    
    /* Tab content */
    .tab-content { display: none; }
    .tab-content.active { display: block; }
    
    /* Page header */
    .page-header {
      margin-bottom: 10px;
    }
    
    .page-title {
      font-size: 18px;
      font-weight: 600;
      margin-bottom: 2px;
    }
    
    .page-subtitle {
      font-size: 12px;
      color: var(--muted);
    }
    
    /* Cards */
    .card {
      background: var(--card);
      border: 1px solid var(--card-border);
      border-radius: var(--radius);
      padding: 12px 14px;
      margin-bottom: 8px;
      box-shadow: var(--shadow);
    }
    
    .card-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 12px;
    }
    
    .card-title {
      font-size: 14px;
      font-weight: 600;
      color: var(--text);
      display: flex;
      align-items: center;
      gap: 8px;
    }
    
    .card-title svg {
      width: 16px;
      height: 16px;
      color: var(--primary);
    }
    
    .card-subtitle {
      font-size: 12px;
      color: var(--muted);
      margin-top: 2px;
    }
    
    /* Buttons */
    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      padding: 10px 16px;
      font-size: 14px;
      font-weight: 500;
      border-radius: 8px;
      border: none;
      cursor: pointer;
      transition: all 0.15s ease;
      background: var(--primary);
      color: white;
    }
    
    .btn:hover { background: var(--primary-hover); }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .btn.full { width: 100%; }
    .btn.sm { padding: 6px 12px; font-size: 12px; border-radius: 6px; }
    .btn.xs { padding: 4px 8px; font-size: 11px; border-radius: 4px; }
    
    .btn.secondary {
      background: var(--input-bg);
      color: var(--text);
      border: 1px solid var(--border);
    }
    .btn.secondary:hover { background: rgba(255,255,255,0.1); }
    
    .btn.success { background: var(--success); }
    .btn.success:hover { background: #059669; }
    
    .btn.danger { background: var(--danger); }
    .btn.danger:hover { background: #dc2626; }
    
    .btn.warning { background: var(--warning); color: #000; }
    .btn.warning:hover { background: #d97706; }
    
    .btn-group {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    
    /* Form elements */
    .form-group { margin-bottom: 12px; }
    
    label {
      display: block;
      font-size: 12px;
      font-weight: 500;
      color: var(--text-secondary);
      margin-bottom: 6px;
    }
    
    input, select, textarea {
      width: 100%;
      padding: 10px 12px;
      font-size: 14px;
      border-radius: 8px;
      border: 1px solid var(--border);
      background: var(--input-bg);
      color: var(--text);
      transition: border-color 0.15s ease, box-shadow 0.15s ease;
    }
    
    input:focus, select:focus, textarea:focus {
      outline: none;
      border-color: var(--primary);
      box-shadow: 0 0 0 3px rgba(59,130,246,0.15);
    }
    
    input::placeholder { color: var(--muted); }
    
    /* Grid */
    .grid { display: grid; gap: 8px; }
    .grid-2 { grid-template-columns: 1fr 1fr; }
    
    /* Row flex */
    .row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      flex-wrap: wrap;
    }
    
    /* Badges */
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 4px;
      padding: 4px 10px;
      font-size: 11px;
      font-weight: 500;
      border-radius: 20px;
    }
    
    .badge.success { background: rgba(16,185,129,0.15); color: #34d399; }
    .badge.danger { background: rgba(239,68,68,0.15); color: #f87171; }
    .badge.warning { background: rgba(245,158,11,0.15); color: #fbbf24; }
    .badge.primary { background: rgba(59,130,246,0.15); color: #60a5fa; }
    .badge.muted { background: rgba(100,116,139,0.15); color: #94a3b8; }
    
    /* SIM Grid */
    .sim-grid {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 8px;
    }
    
    .sim-card {
      background: var(--input-bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 8px;
      transition: all 0.15s ease;
    }
    
    .sim-card:hover { border-color: var(--primary); }
    .sim-card.disabled { opacity: 0.4; }
    .sim-card.active { border-color: var(--success); }
    
    .sim-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 8px;
    }

    .sim-header .session-badge {
      font-size: 10px;
      padding: 2px 6px;
      margin-left: 8px;
      animation: pulse 2s infinite;
    }

    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.7; }
    }
    
    .sim-slot {
      font-size: 11px;
      font-weight: 600;
      color: var(--text-secondary);
    }
    
    .sim-status {
      width: 6px;
      height: 6px;
      border-radius: 50%;
      background: var(--danger);
    }
    
    .sim-status.ok { background: var(--success); }
    .sim-status.warn { background: var(--warning); }
    
    .sim-number {
      font-family: 'SF Mono', Consolas, monospace;
      font-size: 13px;
      font-weight: 500;
      color: var(--primary);
      margin-bottom: 4px;
    }
    
    .sim-meta {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 10px;
      color: var(--muted);
    }
    
    .sim-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 4px;
      margin-top: 8px;
    }

    .sim-ussd {
      font-size: 10px;
      margin-top: 6px;
      line-height: 1.35;
      word-break: break-word;
    }
    .sim-ussd.ok { color: var(--success); }
    .sim-ussd.err { color: var(--danger); }
    .sim-ussd.pending { color: var(--muted); }
    .sim-card.ussd-checking {
      outline: 2px solid var(--primary);
      outline-offset: -2px;
    }

    .ussd-bulk-panel { margin-bottom: 12px; }
    .ussd-bulk-head { cursor: pointer; display: flex; align-items: center; gap: 10px; flex-wrap: wrap; padding: 12px 16px; }
    .ussd-bulk-head h4 { margin: 0; font-size: 14px; flex: 1; }
    .ussd-bulk-panel.collapsed .ussd-bulk-collapse { display: none; }
    #ussdBulkChevron { font-size: 12px; color: var(--muted); }
    .ussd-bulk-ok { color: var(--success); }
    .ussd-bulk-fail { color: var(--danger); }
    .ussd-bulk-line { font-size: 12px; margin: 4px 0; font-family: 'SF Mono', Consolas, monospace; }

    /* Settings — service rows with status pills */
    .settings-card .card-header { margin-bottom: 4px; }
    .settings-row {
      display: flex;
      flex-wrap: wrap;
      align-items: flex-start;
      justify-content: space-between;
      gap: 10px;
      padding: 10px 0;
      border-bottom: 1px solid var(--card-border);
    }
    .settings-row:last-child { border-bottom: none; padding-bottom: 0; }
    .settings-row-title { font-size: 13px; font-weight: 600; color: var(--text); }
    .settings-row-desc { font-size: 12px; color: var(--muted); margin-top: 4px; line-height: 1.45; max-width: 440px; }
    .settings-row-side {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      justify-content: flex-end;
      gap: 8px;
      flex-shrink: 0;
    }
    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 5px 11px;
      border-radius: 999px;
      font-size: 11px;
      font-weight: 600;
      white-space: nowrap;
    }
    .status-pill.on { background: rgba(34, 197, 94, 0.14); color: var(--success); }
    .status-pill.off { background: rgba(148, 163, 184, 0.12); color: var(--muted); }
    .status-pill.warn { background: rgba(234, 179, 8, 0.14); color: var(--warning); }
    .status-pill .status-dot { width: 7px; height: 7px; animation: none; flex-shrink: 0; }

    /* SIM tab toolbar */
    .sim-toolbar {
      display: flex;
      flex-wrap: wrap;
      align-items: flex-start;
      justify-content: space-between;
      gap: 12px;
      margin-bottom: 14px;
      padding-bottom: 14px;
      border-bottom: 1px solid var(--card-border);
    }
    .sim-stat-pill {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 6px 12px;
      border-radius: 8px;
      background: var(--bg);
      font-size: 12px;
      font-weight: 600;
      color: var(--text);
    }
    .sim-stat-pill .stat-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--success);
      flex-shrink: 0;
    }
    .sim-stat-pill.warn .stat-dot { background: var(--warning); }
    .sim-toolbar-actions { display: flex; flex-wrap: wrap; gap: 14px; align-items: center; }
    .action-group { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; }
    .action-group-label {
      font-size: 10px;
      text-transform: uppercase;
      letter-spacing: 0.06em;
      color: var(--muted);
      font-weight: 600;
      margin-right: 2px;
    }

    /* Session info inside SIM card */
    .sim-session {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-top: 8px;
      padding: 6px 8px;
      background: var(--bg);
      border-radius: 4px;
      font-size: 11px;
    }

    .sim-session .session-app {
      font-weight: 500;
      color: var(--text);
    }

    .sim-session .session-timer {
      font-family: 'SF Mono', Consolas, monospace;
      font-weight: 600;
      color: var(--success);
    }

    .sim-session .session-timer.warning {
      color: var(--warning);
    }

    .sim-session .session-timer.danger {
      color: var(--danger);
    }

    .sim-session .session-msgs {
      color: var(--muted);
      margin-left: auto;
    }

    /* Signal strength */
    .signal {
      display: inline-flex;
      align-items: flex-end;
      gap: 2px;
      height: 12px;
    }
    
    .signal i {
      width: 3px;
      background: var(--muted);
      border-radius: 1px;
    }
    
    .signal i:nth-child(1) { height: 3px; }
    .signal i:nth-child(2) { height: 5px; }
    .signal i:nth-child(3) { height: 8px; }
    .signal i:nth-child(4) { height: 11px; }
    
    .signal.s1 i:nth-child(1) { background: var(--danger); }
    .signal.s2 i:nth-child(-n+2) { background: var(--warning); }
    .signal.s3 i:nth-child(-n+3) { background: var(--success); }
    .signal.s4 i:nth-child(-n+4) { background: var(--success); }
    
    /* Logs */
    .log-panel-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      margin-bottom: 12px;
      flex-wrap: wrap;
    }
    .log-tabs {
      display: flex;
      gap: 6px;
      background: var(--bg);
      padding: 4px;
      border-radius: 8px;
      border: 1px solid var(--card-border);
    }
    .log-tab {
      padding: 8px 16px;
      font-size: 12px;
      font-weight: 600;
      border-radius: 6px;
      cursor: pointer;
      transition: all 0.15s ease;
      color: var(--muted);
      border: none;
      background: transparent;
    }
    .log-tab:hover { color: var(--text); background: rgba(255,255,255,0.05); }
    .log-tab.active {
      background: var(--card);
      color: var(--text);
      box-shadow: 0 1px 3px rgba(0,0,0,0.2);
    }
    .log-tab-badge {
      display: inline-block;
      min-width: 18px;
      padding: 1px 6px;
      margin-left: 6px;
      font-size: 10px;
      font-weight: 700;
      border-radius: 999px;
      background: rgba(255,255,255,0.08);
    }
    .log-tab.active .log-tab-badge { background: rgba(99, 102, 241, 0.25); color: var(--primary); }
    .log-viewport {
      background: #0a0e14;
      border: 1px solid var(--card-border);
      border-radius: 10px;
      max-height: min(52vh, 480px);
      min-height: 280px;
      overflow-y: auto;
      overflow-x: hidden;
    }
    .log-line {
      display: flex;
      align-items: flex-start;
      gap: 10px;
      padding: 7px 14px;
      font-family: 'SF Mono', Consolas, monospace;
      font-size: 11px;
      line-height: 1.45;
      border-bottom: 1px solid rgba(255,255,255,0.04);
    }
    .log-line:last-child { border-bottom: none; }
    .log-line:hover { background: rgba(255,255,255,0.02); }
    .log-tag {
      flex-shrink: 0;
      color: #64748b;
      font-weight: 600;
      min-width: 64px;
    }
    .log-msg { flex: 1; word-break: break-word; color: #cbd5e1; }
    .log-line.ok .log-msg { color: #4ade80; }
    .log-line.err .log-msg { color: #f87171; }
    .log-line.warn .log-msg { color: #fbbf24; }
    .log-line.info .log-msg { color: #94a3b8; }
    .log-empty {
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: 200px;
      color: #64748b;
      font-size: 13px;
    }
    .log-footer {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-top: 10px;
      padding-top: 10px;
      border-top: 1px solid var(--card-border);
    }
    .log-footer-meta { font-size: 12px; color: var(--muted); }
    .fw-lead { margin: 0 0 14px; line-height: 1.5; }
    .fw-notice {
      display: flex;
      align-items: flex-start;
      gap: 10px;
      padding: 10px 12px;
      margin-bottom: 14px;
      border-radius: 8px;
      background: rgba(234, 179, 8, 0.1);
      border: 1px solid rgba(234, 179, 8, 0.35);
      font-size: 12px;
      line-height: 1.45;
      color: var(--text);
    }
    .fw-notice.hide { display: none !important; }
    .fw-hero {
      display: grid;
      grid-template-columns: 1fr auto 1fr;
      gap: 10px;
      align-items: center;
      margin-bottom: 10px;
    }
    .fw-hero-box {
      padding: 14px 12px;
      background: var(--bg);
      border-radius: 10px;
      border: 1px solid var(--card-border);
      text-align: center;
    }
    .fw-hero-box.highlight { border-color: var(--primary); box-shadow: 0 0 0 1px rgba(59, 130, 246, 0.25); }
    .fw-hero-label {
      display: block;
      font-size: 10px;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.06em;
      color: var(--muted);
      margin-bottom: 6px;
    }
    .fw-hero-version {
      display: block;
      font-size: 22px;
      font-weight: 700;
      font-family: 'SF Mono', Consolas, monospace;
      color: var(--text);
      line-height: 1.2;
    }
    .fw-hero-arrow { font-size: 18px; color: var(--muted); text-align: center; }
    .fw-status-line { margin: 0 0 14px; min-height: 1.25em; }
    .fw-actions { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 10px; }
    .fw-meta { margin: 0 0 12px; line-height: 1.5; }
    .fw-advanced {
      border-top: 1px solid var(--card-border);
      padding-top: 10px;
      margin-top: 4px;
    }
    .fw-advanced summary {
      cursor: pointer;
      font-size: 12px;
      font-weight: 600;
      color: var(--muted);
      user-select: none;
      list-style: none;
    }
    .fw-advanced summary::-webkit-details-marker { display: none; }
    .fw-advanced summary::before { content: '▸ '; }
    .fw-advanced[open] summary::before { content: '▾ '; }
    .fw-advanced-body { margin-top: 12px; }
    .fw-advanced-body label { display: block; font-size: 12px; font-weight: 600; margin-bottom: 6px; }
    .fw-advanced-body input { width: 100%; margin-bottom: 8px; font-size: 12px; }
    .fw-advanced-hint { margin: 6px 0 0; font-size: 11px; line-height: 1.45; color: var(--muted); }
    
    /* Modal */
    .modal {
      position: fixed;
      inset: 0;
      background: rgba(0,0,0,0.8);
      display: flex;
      align-items: center;
      justify-content: center;
      z-index: 1000;
      padding: 16px;
    }
    
    .modal-content {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      padding: 20px;
      max-width: 400px;
      width: 100%;
      max-height: 80vh;
      overflow-y: auto;
    }
    
    .modal-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 16px;
    }
    
    .modal-title {
      font-size: 16px;
      font-weight: 600;
    }
    
    .modal-close {
      background: none;
      border: none;
      color: var(--muted);
      cursor: pointer;
      padding: 4px;
    }
    
    .modal-close:hover { color: var(--text); }
    
    /* Toast */
    .toast {
      position: fixed;
      bottom: 24px;
      left: 50%;
      transform: translateX(-50%);
      background: var(--card);
      border: 1px solid var(--primary);
      border-radius: 8px;
      padding: 12px 20px;
      z-index: 9999;
      max-width: 90%;
      text-align: center;
      box-shadow: var(--shadow);
    }
    
    /* Spinner */
    .spinner {
      display: inline-block;
      width: 14px;
      height: 14px;
      border: 2px solid rgba(255,255,255,0.2);
      border-top-color: white;
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
    }
    
    @keyframes spin { to { transform: rotate(360deg); } }
    
    /* Utilities */
    .hide { display: none !important; }
    .muted { color: var(--muted); }
    .text-sm { font-size: 12px; }
    .text-center { text-align: center; }
    .mb-2 { margin-bottom: 8px; }
    .mb-4 { margin-bottom: 16px; }
    .divider {
      height: 1px;
      background: var(--border);
      margin: 12px 0;
    }
    
    /* Scrollbar */
    ::-webkit-scrollbar { width: 6px; height: 6px; }
    ::-webkit-scrollbar-track { background: transparent; }
    ::-webkit-scrollbar-thumb { background: var(--border); border-radius: 3px; }
    ::-webkit-scrollbar-thumb:hover { background: var(--muted); }
    
    /* Network list */
    .net-item {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 12px;
      border: 1px solid var(--border);
      border-radius: 8px;
      background: var(--input-bg);
      margin-bottom: 8px;
      cursor: pointer;
      transition: all 0.15s ease;
    }
    
    .net-item:hover { border-color: var(--primary); background: rgba(59,130,246,0.1); }
    .net-item.selected { border-color: var(--primary); background: rgba(59,130,246,0.15); }
    
    .net-info { display: flex; flex-direction: column; gap: 2px; }
    .net-name { font-weight: 500; }
    .net-signal { font-size: 11px; color: var(--muted); }
    
    /* Messages tab */
    .msg-toolbar {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-bottom: 12px;
      flex-wrap: wrap;
    }
    .msg-search-wrap {
      position: relative;
      flex: 1;
      min-width: 160px;
    }
    .msg-search-wrap svg {
      position: absolute;
      left: 10px;
      top: 50%;
      transform: translateY(-50%);
      width: 15px;
      height: 15px;
      color: var(--muted);
      pointer-events: none;
    }
    .msg-search-wrap input {
      width: 100%;
      padding-left: 32px;
      margin: 0;
    }
    .msg-count { font-size: 12px; color: var(--muted); white-space: nowrap; }

    .sms-list { display: flex; flex-direction: column; gap: 1px; }

    .sms-item {
      display: flex;
      align-items: flex-start;
      gap: 12px;
      padding: 12px 14px;
      background: var(--card);
      border-bottom: 1px solid var(--card-border);
      cursor: default;
      transition: background 0.12s;
    }
    .sms-item:first-child { border-radius: 8px 8px 0 0; }
    .sms-item:last-child { border-bottom: none; border-radius: 0 0 8px 8px; }
    .sms-item:only-child { border-radius: 8px; border-bottom: none; }
    .sms-item:hover { background: rgba(59,130,246,0.06); }

    .sms-avatar {
      width: 36px;
      height: 36px;
      border-radius: 50%;
      background: rgba(59,130,246,0.18);
      color: var(--primary);
      font-weight: 700;
      font-size: 13px;
      display: flex;
      align-items: center;
      justify-content: center;
      flex-shrink: 0;
      margin-top: 1px;
    }
    .sms-main { flex: 1; min-width: 0; }
    .sms-row1 {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      gap: 8px;
      margin-bottom: 2px;
    }
    .sms-sender {
      font-weight: 600;
      font-size: 13px;
      color: var(--text);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .sms-time {
      font-size: 11px;
      color: var(--muted);
      white-space: nowrap;
      flex-shrink: 0;
    }
    .sms-row2 {
      display: flex;
      align-items: center;
      gap: 6px;
      margin-bottom: 4px;
    }
    .sms-slot {
      font-size: 10px;
      color: var(--muted);
      background: rgba(255,255,255,0.05);
      border: 1px solid var(--card-border);
      padding: 1px 6px;
      border-radius: 4px;
      white-space: nowrap;
      flex-shrink: 0;
    }
    .sms-number { font-size: 11px; color: var(--muted); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .sms-body {
      font-size: 13px;
      line-height: 1.5;
      color: var(--text-secondary);
      word-break: break-word;
      white-space: pre-wrap;
    }
    .sms-actions {
      display: flex;
      flex-direction: column;
      gap: 4px;
      flex-shrink: 0;
    }
    .sms-copy-btn {
      background: none;
      border: 1px solid var(--card-border);
      color: var(--muted);
      cursor: pointer;
      padding: 4px 8px;
      border-radius: 6px;
      font-size: 11px;
      font-weight: 600;
      white-space: nowrap;
      transition: all 0.15s;
    }
    .sms-copy-btn:hover { border-color: var(--primary); color: var(--primary); background: rgba(59,130,246,0.08); }
    .sms-highlight { background: rgba(234,179,8,0.25); border-radius: 2px; }
    
    .stat-item.success .stat-value { color: var(--success); }
    .stat-item.warning .stat-value { color: var(--warning); }
    .stat-item.danger .stat-value { color: var(--danger); }
    
    /* Copy button for SIM numbers */
    .copy-btn {
      background: rgba(59,130,246,0.1);
      border: none;
      color: var(--primary);
      cursor: pointer;
      padding: 4px;
      font-size: 0;
      border-radius: 4px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      transition: all 0.2s;
    }
    
    .copy-btn:hover { background: rgba(59,130,246,0.2); }
    .copy-btn svg { width: 14px; height: 14px; }
    .copy-btn.copied { background: rgba(34,197,94,0.2); color: var(--success); }
    
    /* Empty state */
    .empty-state {
      text-align: center;
      padding: 40px 20px;
      color: var(--muted);
    }
    
    .empty-state svg {
      width: 48px;
      height: 48px;
      margin-bottom: 12px;
      opacity: 0.5;
    }
  </style>
</head>
<body>
  <div class="overlay" id="overlay" onclick="toggleSidebar()"></div>
  
  <div class="app-layout">
    <!-- Sidebar -->
    <aside class="sidebar" id="sidebar">
      <div class="sidebar-header">
        <div class="logo">
          <div class="logo-icon">OT</div>
          <div class="logo-text">OTPocket<span>Agent</span></div>
        </div>
        <button class="sidebar-collapse-btn" id="sidebarCollapseBtn" onclick="toggleSidebarCollapse()" title="Collapse sidebar">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M15 18l-6-6 6-6"/></svg>
        </button>
      </div>
      <nav class="sidebar-nav">
        <div class="nav-item active" data-tab="sims" onclick="switchTab('sims')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="4" width="20" height="16" rx="2"/><path d="M6 8h.01M10 8h.01"/></svg>
          <span class="nav-item-label">SIM Slots</span>
          <span class="nav-badge" id="sessionsNavBadge" style="display:none;">0</span>
        </div>
        <div class="nav-item" data-tab="messages" onclick="switchTab('messages')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg>
          <span class="nav-item-label">Messages</span>
          <span class="nav-badge" id="msgBadge">0</span>
        </div>
        <div class="nav-item" data-tab="account" onclick="switchTab('account')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
          <span class="nav-item-label">Account</span>
          <span class="nav-badge" id="accountNavBadge" style="display:none;">!</span>
        </div>
        <div class="nav-item" data-tab="settings" onclick="switchTab('settings')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>
          <span class="nav-item-label">Settings</span>
        </div>
        <div class="nav-item" data-tab="logs" onclick="switchTab('logs')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/></svg>
          <span class="nav-item-label">Logs</span>
        </div>
      </nav>
      <div class="nav-restart" id="navRestartBanner">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/></svg>
        <div>
          <div class="nav-restart-label">Scheduled restart</div>
          <div class="nav-restart-timer" id="navRestartTimer">—</div>
        </div>
      </div>
    </aside>
    
    <!-- Main Content -->
    <main class="main-content" id="mainContent">
      <!-- Mobile Header -->
      <header class="mobile-header">
        <button class="menu-btn" onclick="toggleSidebar()">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="24" height="24"><line x1="3" y1="12" x2="21" y2="12"/><line x1="3" y1="6" x2="21" y2="6"/><line x1="3" y1="18" x2="21" y2="18"/></svg>
        </button>
        <div class="logo">
          <div class="logo-icon" style="width:28px;height:28px;font-size:14px;">OT</div>
          <span style="font-weight:600;">OTPocket Agent</span>
        </div>
        <div class="header-status" style="display:flex;align-items:center;gap:8px;">
          <span class="badge warning hide" id="offlineModeBadge" style="font-size:11px;">Offline</span>
          <div class="status-dot" id="statusDot"></div>
        </div>
      </header>
      <!-- Mobile restart strip (shown below header when restart is pending) -->
      <div class="mobile-restart-strip" id="mobileRestartStrip">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/></svg>
        Scheduled restart in <span class="mobile-restart-strip-timer" id="mobileRestartTimer">—</span>
      </div>
    
    <!-- Legacy login page (hidden — use Account tab) -->
    <div id="loginPage" class="container login-container hide">
      <div style="max-width:400px;width:100%;">
        <!-- Logo -->
        <div style="text-align:center;margin-bottom:24px;">
          <div style="display:flex;justify-content:center;margin-bottom:16px;">
            <div class="logo-icon" style="width:64px;height:64px;font-size:28px;">OT</div>
          </div>
          <h1 style="font-size:24px;font-weight:700;margin-bottom:4px;">Sign In</h1>
          <p class="muted">Access your SIM gateway dashboard</p>
        </div>
        
        <!-- Login Card -->
        <div class="card">
          <div class="card-header" style="border-bottom:1px solid var(--border);padding-bottom:12px;margin-bottom:16px;">
            <div>
              <div class="card-title" style="font-size:16px;">Welcome Back</div>
              <p class="muted text-sm">Sign in to access your gateway</p>
            </div>
          </div>
          
          <div id="loginError" class="hide" style="background:rgba(239,68,68,0.1);border:1px solid rgba(239,68,68,0.2);border-radius:8px;padding:12px;margin-bottom:16px;">
          <p style="font-size:13px;color:var(--danger);" id="loginErrorText"></p>
        </div>
        
        <form onsubmit="return false;">
          <div class="form-group">
            <label for="loginEmail">Email</label>
            <input id="loginEmail" type="email" placeholder="your@email.com" autocomplete="email" />
          </div>
          <div class="form-group">
            <label for="loginPassword">Password</label>
            <input id="loginPassword" type="password" placeholder="Enter your password" autocomplete="current-password" />
          </div>
          <button class="btn full" id="loginBtn" style="margin-top:8px;">Sign In</button>
        </form>

        <div id="loginGraceBanner" class="hide" style="margin-top:12px;padding:12px;border-radius:8px;background:rgba(34,197,94,0.12);border:1px solid rgba(34,197,94,0.25);">
          <p style="font-size:13px;color:var(--success);margin:0 0 6px 0;" id="loginGraceText">Signed in — WiFi details stay visible below.</p>
          <button type="button" class="btn sm secondary" id="loginGraceContinueBtn">Open dashboard now</button>
        </div>
        
        <div class="card" id="loginModemRunCard" style="margin-top:12px;">
          <div class="card-header" style="border-bottom:1px solid var(--border);padding-bottom:10px;margin-bottom:12px;">
            <div class="card-title" style="font-size:14px;">SIM800 gateway</div>
            <span class="badge warning" id="loginModemRunBadge">Idle</span>
          </div>
          <p class="muted text-sm" style="margin-bottom:10px;" id="loginModemRunHint">Connect WiFi first, then run SIM init when modems are powered.</p>
          <button type="button" class="btn full" id="loginModemRunBtn">Run — init SIM slots</button>
        </div>

        <div class="card" style="margin-top:12px;">
          <div class="card-header" style="border-bottom:1px solid var(--border);padding-bottom:10px;margin-bottom:12px;">
            <div class="card-title" style="font-size:14px;">WiFi Setup (AP mode)</div>
            <span class="badge danger" id="loginWifiBadge">Disconnected</span>
          </div>
          <p class="muted text-sm" style="margin-bottom:10px;">
            Not connected yet? Configure WiFi here so the gateway can go online.
          </p>
          <div id="loginWifiConnected" class="hide" style="margin-bottom:10px;">
            <div class="row mb-2"><span class="muted text-sm">Network</span><span class="text-sm" id="loginWifiSsid">-</span></div>
            <div class="row mb-2"><span class="muted text-sm">IP</span><span class="text-sm" id="loginWifiIp">-</span></div>
            <div class="row mb-2"><span class="muted text-sm">Gateway</span><span class="text-sm" id="loginWifiGw">-</span></div>
            <div class="row mb-2"><span class="muted text-sm">DNS</span><span class="text-sm" id="loginWifiDns">-</span></div>
            <div class="row mb-2"><span class="muted text-sm">URL</span><span class="text-sm" id="loginWifiUrl">-</span></div>
            <p id="loginWifiNetWarn" class="hide text-sm" style="color:var(--warning);margin-bottom:8px;">WiFi connected but gateway or DNS missing — login/HTTPS may fail. Disable guest isolation or use 2.4GHz WPA2.</p>
            <button class="btn secondary full mb-2" id="copyLoginStaUrlBtn">Copy STA URL</button>
            <button class="btn secondary full mb-2" id="loginDisconnectBtn">Disconnect WiFi</button>
          </div>
          <button class="btn secondary full mb-2" id="loginScanBtn">Scan Networks</button>
          <div id="loginScanResults" class="grid mb-2"></div>
          <input id="loginManualSsid" placeholder="SSID" style="margin-bottom:8px;" autocomplete="off" />
          <input id="loginManualPw" type="password" placeholder="Password" style="margin-bottom:8px;" />
          <button class="btn full" id="loginConnectBtn">Connect WiFi</button>
        </div>
      </div>
    </div>
  </div>
  
  <!-- Main app (always available; sign-in optional) -->
  <div id="dashboardPage" class="container">
    
    <!-- SIM Slots Tab -->
    <div class="tab-content active" id="tab-sims">
      <div class="page-header">
        <h1 class="page-title">SIM Slots</h1>
        <p class="page-subtitle">Manage your SIM cards</p>
      </div>

      <div class="card ussd-bulk-panel" id="ussdBulkPanel">
        <div class="ussd-bulk-head" id="ussdBulkHead" onclick="toggleUssdBulkPanel()">
          <h4>*143# bulk results</h4>
          <span id="ussdBulkSummary" class="muted text-sm"></span>
          <span id="ussdBulkChevron">▼</span>
        </div>
        <div id="ussdBulkCollapse" class="ussd-bulk-collapse">
          <div id="ussdBulkBody" class="muted text-sm" style="padding:0 16px 14px;">*143# All: slots with a number only (Off OK).</div>
        </div>
      </div>

      <div class="card" id="modemRunCard" style="margin-bottom:12px;">
        <div class="card-header" style="border-bottom:1px solid var(--border);padding-bottom:10px;margin-bottom:12px;">
          <div>
            <div class="card-title" style="font-size:15px;">SIM800 gateway</div>
            <p class="muted text-sm" id="modemRunHint">Idle — init modems and start SMS polling when ready.</p>
          </div>
          <span class="badge warning" id="modemRunBadge">Idle</span>
        </div>
        <button type="button" class="btn full" id="modemRunBtn">Run — init SIM slots &amp; start polling</button>
        <button type="button" class="btn secondary full hide" id="modemStopBtn" style="margin-top:8px;">Stop modem tasks</button>
      </div>

      <div class="card sim-slots-card">
        <div class="sim-toolbar">
          <span class="sim-stat-pill" id="simCountPill">
            <span class="stat-dot" id="simCountDot"></span>
            <span id="simCount">0 active slots</span>
          </span>
          <div class="sim-toolbar-actions">
            <div class="action-group">
              <span class="action-group-label">Slots</span>
              <button class="btn sm secondary" id="checkAllBtn" title="Re-scan signal, registration, and numbers for every slot">Refresh slots</button>
              <button class="btn sm danger" id="disableAllSimsBtn" title="Turn off every SIM slot">Disable all</button>
            </div>
            <div class="action-group">
              <span class="action-group-label">Balance</span>
              <button class="btn sm secondary" id="ussdBulkBtn" title="Dial *143# on each slot that has a number">*143# all</button>
            </div>
          </div>
        </div>
        <div class="sim-grid" id="simGrid"></div>
      </div>

      <!-- Active Sessions Card -->
      <div class="card sessions-card" id="sessionsCard" style="display:none;">
        <div class="card-header">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:16px;height:16px;"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
            Active Sessions
          </div>
          <span class="badge primary" id="sessionsBadge">0</span>
        </div>
        <div class="sessions-list" id="sessionsList"></div>
      </div>
    </div>
    
    <!-- Messages Tab -->
    <div class="tab-content" id="tab-messages">
      <div class="page-header">
        <h1 class="page-title">Messages</h1>
      </div>

      <div class="msg-toolbar">
        <div class="msg-search-wrap">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg>
          <input id="smsSearch" type="search" placeholder="Search messages or senders…" oninput="filterMessages()" autocomplete="off" />
        </div>
        <span class="msg-count" id="smsCount">—</span>
        <button class="btn xs secondary" id="refreshSmsBtn">Refresh</button>
        <button class="btn xs danger" id="clearSmsBtn">Clear all</button>
      </div>

      <div class="sms-list" id="smsList">
        <div class="empty-state">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg>
          <p>No messages yet</p>
        </div>
      </div>
    </div>
    
    <!-- Account Tab (optional sign-in) -->
    <div class="tab-content" id="tab-account">
      <div class="page-header">
        <h1 class="page-title">Account</h1>
        <p class="page-subtitle">Sign in to enable cloud SMS forward and heartbeat</p>
      </div>

      <div class="card" id="accountOfflineCard">
        <div class="card-header">
          <div class="card-title">Offline mode</div>
          <span class="badge warning" id="accountModeBadge">Not signed in</span>
        </div>
        <p class="muted text-sm" style="margin-bottom:12px;">
          The full panel works without login. SIM init, WiFi, ping, and logs run locally.
          SMS forward, heartbeat, and other backend HTTP calls stay disabled until you sign in.
        </p>
      </div>

      <div class="card" id="accountLoginCard">
        <div class="card-header" style="border-bottom:1px solid var(--border);padding-bottom:12px;margin-bottom:16px;">
          <div class="card-title">Sign in</div>
        </div>
        <div id="accountLoginError" class="hide" style="background:rgba(239,68,68,0.1);border:1px solid rgba(239,68,68,0.2);border-radius:8px;padding:12px;margin-bottom:16px;">
          <p style="font-size:13px;color:var(--danger);" id="accountLoginErrorText"></p>
        </div>
        <form onsubmit="return false;">
          <div class="form-group">
            <label for="accountLoginEmail">Email</label>
            <input id="accountLoginEmail" type="email" placeholder="your@email.com" autocomplete="email" />
          </div>
          <div class="form-group">
            <label for="accountLoginPassword">Password</label>
            <input id="accountLoginPassword" type="password" placeholder="Password" autocomplete="current-password" />
          </div>
          <button class="btn full" id="accountLoginBtn" type="button">Sign in</button>
        </form>
      </div>

      <div class="card hide" id="accountSignedInCard">
        <div class="card-header">
          <div class="card-title">Signed in</div>
          <span class="badge success">Online</span>
        </div>
        <div class="row mb-2"><span class="muted text-sm">Device</span><span class="badge primary" id="accountDeviceBadge">-</span></div>
        <div class="row mb-2"><span class="muted text-sm">SIMs registered</span><span class="text-sm" id="accountSimsRegCount">-</span></div>
        <div class="btn-group">
          <button class="btn sm secondary" id="accountRegisterDeviceBtn">Register device</button>
          <button class="btn sm secondary" id="accountRefreshTokenBtn">Refresh token</button>
          <button class="btn sm danger" id="accountLogoutBtn">Sign out</button>
        </div>
      </div>
    </div>

    <!-- Settings Tab -->
    <div class="tab-content" id="tab-settings">
      <div class="page-header">
        <h1 class="page-title">Settings</h1>
      </div>

      <div class="card" id="wifiCard">
        <div class="card-header">
          <div class="card-title">WiFi</div>
          <span class="badge danger" id="wifiBadge">Disconnected</span>
        </div>
        <div id="wifiConnected" class="hide">
          <div class="row mb-2"><span class="muted text-sm">Network</span><span class="text-sm" id="wifiSsid">-</span></div>
          <div class="row mb-2"><span class="muted text-sm">IP</span><span class="text-sm" id="wifiIp">-</span></div>
          <div class="row mb-2"><span class="muted text-sm">Gateway</span><span class="text-sm" id="wifiGw">-</span></div>
          <div class="row mb-2"><span class="muted text-sm">DNS</span><span class="text-sm" id="wifiDns">-</span></div>
          <div class="row mb-2"><span class="muted text-sm">URL</span><span class="text-sm" id="wifiUrl">-</span></div>
          <p id="wifiNetWarn" class="hide text-sm" style="color:var(--warning);margin-bottom:8px;">Connected to router but no internet route (bad gateway/DNS).</p>
          <button class="btn secondary sm" id="copyStaUrlBtn">Copy STA URL</button>
          <button class="btn secondary sm" id="disconnectBtn">Disconnect</button>
        </div>
        <div id="wifiSetup">
          <button class="btn secondary full mb-2" id="scanBtn">Scan</button>
          <div id="scanResults" class="grid mb-2"></div>
          <input id="manualSsid" placeholder="SSID" style="margin-bottom:8px;" autocomplete="off" />
          <input id="manualPw" type="password" placeholder="Password" style="margin-bottom:8px;" />
          <button class="btn full" id="connectBtn">Connect</button>
        </div>
      </div>

      <div class="card" id="networkTestCard">
        <div class="card-header">
          <div class="card-title">Ping</div>
        </div>
        <p class="muted text-sm" style="margin-bottom:10px;">Like <code>ping google.com</code> on a PC — 4 probes to any host or URL.</p>
        <div class="form-group">
          <label for="pingHost">Host, IP, or URL</label>
          <input id="pingHost" placeholder="google.com" autocomplete="off" />
        </div>
        <div class="btn-group mb-2" style="flex-wrap:wrap;gap:6px;">
          <button type="button" class="btn sm secondary ping-preset" data-host="google.com">Google</button>
          <button type="button" class="btn sm secondary ping-preset" data-host="8.8.8.8">8.8.8.8</button>
          <button type="button" class="btn sm secondary ping-preset" data-host="seller.otpocket.app">Backend</button>
        </div>
        <button type="button" class="btn secondary full mb-2" id="pingHostBtn">Ping</button>
        <pre id="pingResult" class="muted text-sm" style="white-space:pre-wrap;margin:0;padding:10px;background:var(--bg-secondary);border-radius:8px;min-height:48px;">Not tested yet.</pre>
      </div>

      <div class="card" id="authCard">
        <div class="card-header">
          <div class="card-title">Cloud sync</div>
          <span class="badge warning" id="authBadge">Offline</span>
        </div>
        <div id="authSignedOutView">
          <p class="muted text-sm" style="margin-bottom:10px;">Not signed in — no SMS forward or heartbeat. Use <strong>Account</strong> in the sidebar to sign in.</p>
          <button type="button" class="btn secondary full" onclick="switchTab('account')">Open Account</button>
        </div>
        <div id="authSignedInView" class="hide">
          <div class="row mb-2"><span class="muted text-sm">Device</span><span class="badge primary" id="deviceBadge">-</span></div>
          <div class="row mb-2"><span class="muted text-sm">SIMs</span><span class="text-sm" id="simsRegCount">-</span></div>
          <div class="btn-group">
            <button class="btn sm secondary" id="registerDeviceBtn">Register</button>
            <button class="btn sm danger" id="logoutBtn">Logout</button>
          </div>
        </div>
      </div>
      
      <div class="card settings-card">
        <div class="card-header">
          <div>
            <div class="card-title">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 4h16c1.1 0 2 .9 2 2v12c0 1.1-.9 2-2 2H4c-1.1 0-2-.9-2-2V6c0-1.1.9-2 2-2z"/><polyline points="22,6 12,13 2,6"/></svg>
              Backend
            </div>
          </div>
        </div>
        
        <div class="form-group">
          <label for="baseUrl">Base URL</label>
          <input id="baseUrl" placeholder="https://api.example.com" />
        </div>
        <div class="form-group">
          <label for="apiPath">API Path</label>
          <input id="apiPath" placeholder="/api/agent/incoming-sms" />
        </div>
        <div class="form-group">
          <label for="deviceId">Device ID</label>
          <input id="deviceId" placeholder="SIM800-XXXXXX (auto-generated if empty)" />
        </div>
        <button class="btn full" id="saveConfigBtn">Save backend settings</button>
      </div>

      <div class="card settings-card">
        <div class="card-header">
          <div>
            <div class="card-title">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v4M12 18v4M4.93 4.93l2.83 2.83M16.24 16.24l2.83 2.83M2 12h4M18 12h4M4.93 19.07l2.83-2.83M16.24 7.76l2.83-2.83"/></svg>
              Gateway services
            </div>
          </div>
        </div>

        <div class="settings-row">
          <div class="settings-row-main">
            <div class="settings-row-title">Heartbeat</div>
          </div>
          <div class="settings-row-side">
            <span class="status-pill on" id="heartbeatStatusPill"><span class="status-dot ok"></span> Active</span>
            <button class="btn sm secondary" id="pauseHeartbeatBtn">Pause heartbeat</button>
          </div>
        </div>

        <div class="settings-row">
          <div class="settings-row-main">
            <div class="settings-row-title">SMS polling</div>
          </div>
          <div class="settings-row-side">
            <span class="status-pill on" id="smsPollingStatusPill"><span class="status-dot ok"></span> Active</span>
            <button class="btn sm secondary" id="pausePollingBtn">Pause SMS polling</button>
          </div>
        </div>

        <div class="settings-row">
          <div class="settings-row-main">
            <div class="settings-row-title">Missed-call forwarding</div>
          </div>
          <div class="settings-row-side">
            <span class="status-pill off" id="missedCallStatusPill"><span class="status-dot"></span> Off</span>
            <button class="btn sm success" id="toggleMissedCallBtn">Enable forwarding</button>
          </div>
        </div>
      </div>

      <div class="card settings-card" id="firmwareCard">
        <div class="card-header">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
            Software update
          </div>
          <span class="status-pill off" id="firmwareStatusPill"><span class="status-dot"></span> —</span>
        </div>

        <div class="fw-notice hide" id="fwRestartNotice">
          <span>⏱</span>
          <span id="fwRestartNoticeText">A restart is scheduled.</span>
        </div>

        <div class="fw-hero">
          <div class="fw-hero-box" id="fwInstalledBox">
            <span class="fw-hero-label">Installed</span>
            <span class="fw-hero-version" id="fwVersion">—</span>
          </div>
          <div class="fw-hero-arrow" aria-hidden="true">→</div>
          <div class="fw-hero-box" id="fwPublishedBox">
            <span class="fw-hero-label">Published</span>
            <span class="fw-hero-version" id="fwRemoteVersion">—</span>
          </div>
        </div>
        <p class="fw-status-line muted text-sm" id="fwStatusLine">Checking version status…</p>

        <div class="fw-actions">
          <button class="btn sm secondary" id="checkFirmwareBtn">Check for updates</button>
          <button class="btn sm warning" id="installFirmwareBtn" disabled>Install update</button>
        </div>
        <p class="muted text-sm hide" id="otaInstallNote" style="line-height:1.4;"></p>
        <p class="fw-meta muted text-xs" id="fwMetaLine"></p>

        <details class="fw-advanced">
          <summary>Custom download URL</summary>
          <div class="fw-advanced-body">
            <label for="otaUrl">Update URL</label>
            <div class="url-field-wrap">
              <input id="otaUrl" type="password" autocomplete="off" spellcheck="false"
                placeholder="https://…/firmware.bin" />
              <div class="url-field-actions">
                <button class="url-field-btn" id="otaUrlRevealBtn" onclick="toggleOtaUrlReveal()" title="Show/hide">👁</button>
                <button class="url-field-btn" id="otaUrlCopyBtn" onclick="copyOtaUrl()" title="Copy">⎘</button>
              </div>
            </div>
            <button class="btn sm secondary full" id="saveOtaUrlBtn" style="margin-top:6px;">Save URL</button>
            <p class="fw-advanced-hint">Only change this if told to by support. Leave empty to use the default.</p>
          </div>
        </details>
      </div>
      
      <div class="card" style="border-color: var(--danger);">
        <div class="card-header">
          <div class="card-title" style="color: var(--danger);">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/><path d="M12 7v5l4 2"/></svg>
            Device Actions
          </div>
        </div>
        <p class="muted" style="margin-bottom: 16px; font-size: 13px;">Restart the device to apply changes or recover from issues.</p>
        <button class="btn danger full" id="resetDeviceBtn">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:16px;height:16px;margin-right:8px;"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/></svg>
          Restart Device
        </button>
      </div>
    </div>
    
    <!-- Logs Tab -->
    <div class="tab-content" id="tab-logs">
      <div class="page-header">
        <h1 class="page-title">Activity log</h1>
        <p class="page-subtitle">Live gateway events and error history</p>
      </div>
      
      <div class="card">
        <div class="log-panel-header">
          <div class="log-tabs">
            <button type="button" class="log-tab active" id="logTabLive" onclick="switchLogTab('live')">Activity<span class="log-tab-badge" id="monitorCount">0</span></button>
            <button type="button" class="log-tab" id="logTabErrors" onclick="switchLogTab('errors')">Errors<span class="log-tab-badge" id="errorLogCount">0</span></button>
          </div>
        </div>
        <div class="log-viewport" id="monitor"><div class="log-empty">Loading…</div></div>
        <div class="log-viewport hide" id="errorLog"><div class="log-empty">No errors recorded</div></div>
        <div class="log-footer">
          <span class="log-footer-meta" id="logFooterMeta">Activity log</span>
          <button class="btn xs secondary" id="clearLogBtn">Clear log</button>
        </div>
      </div>
    </div>
  </div>
    </main>
  </div><!-- app-layout -->
  
  <!-- Toast -->
  <div id="toast" class="toast hide"></div>
  
  <script>
let currentNetwork = null;
let statusInterval;
let statusSlowInterval;
let currentLogTab = 'live';
let currentTab = 'sims';
let smsMessages = [];

function $(id) { return document.getElementById(id); }
function show(el, on) { el.classList.toggle('hide', !on); }
function toast(msg) {
  const t = $('toast');
  if (!t) return;
  t.textContent = msg;
  t.classList.remove('hide');
  setTimeout(() => t.classList.add('hide'), 3000);
}

// Reset device with countdown
let resetCountdown = null;
let resetAbortController = null;

async function resetDevice() {
  const btn = $('resetDeviceBtn');
  const card = btn ? btn.closest('.card') : null;
  
  // If countdown is active, do nothing
  if (resetCountdown) return;
  
  // Pause polling and heartbeat first
  try {
    await fetch('/toggle-polling', { method: 'POST' });
    await fetch('/toggle-heartbeat', { method: 'POST' });
  } catch(e) {}
  
  let seconds = 10;
  
  // Update button to show countdown and abort option
  if (btn) {
    btn.innerHTML = `<span style="color:var(--danger);font-weight:600;">${seconds}</span> Restarting...`;
    btn.onclick = abortReset;
    btn.classList.remove('danger');
    btn.classList.add('warning');
  }
  
  // Add abort button
  if (card) {
    const existingAbort = card.querySelector('.abort-btn');
    if (!existingAbort) {
      const abortBtn = document.createElement('button');
      abortBtn.className = 'btn secondary full abort-btn';
      abortBtn.style.marginTop = '8px';
      abortBtn.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:16px;height:16px;margin-right:8px;"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg> Cancel Restart';
      abortBtn.onclick = abortReset;
      btn.parentNode.appendChild(abortBtn);
    }
  }
  
  // Start countdown
  resetCountdown = setInterval(() => {
    seconds--;
    if (btn) {
      btn.innerHTML = `<span style="color:var(--danger);font-weight:600;">${seconds}</span> Restarting...`;
    }
    
    if (seconds <= 0) {
      clearInterval(resetCountdown);
      resetCountdown = null;
      executeReset();
    }
  }, 1000);
}

async function abortReset() {
  if (resetCountdown) {
    clearInterval(resetCountdown);
    resetCountdown = null;
  }
  
  toast('Restart cancelled');
  
  // Resume polling and heartbeat
  try {
    await fetch('/toggle-polling', { method: 'POST' });
    await fetch('/toggle-heartbeat', { method: 'POST' });
  } catch(e) {}
  
  // Reset button state
  const btn = $('resetDeviceBtn');
  if (btn) {
    btn.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:16px;height:16px;margin-right:8px;"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/></svg> Restart Device';
    btn.onclick = resetDevice;
    btn.classList.remove('warning');
    btn.classList.add('danger');
  }
  
  // Remove abort button
  const card = btn ? btn.closest('.card') : null;
  if (card) {
    const abortBtn = card.querySelector('.abort-btn');
    if (abortBtn) abortBtn.remove();
  }
}

async function executeReset() {
  const btn = $('resetDeviceBtn');
  if (btn) {
    btn.innerHTML = '<span class="spinner"></span> Restarting...';
    btn.disabled = true;
  }
  
  try {
    await post('/reset', {});
    toast('Device is restarting...');
  } catch(e) {
    toast('Reset request sent. Device is restarting...');
  }
}

// Sidebar toggle (mobile)
function toggleSidebar() {
  const sidebar = $('sidebar');
  const overlay = $('overlay');
  if (sidebar) sidebar.classList.toggle('open');
  if (overlay) overlay.classList.toggle('show');
}

// Sidebar collapse (desktop)
function toggleSidebarCollapse() {
  const sidebar = $('sidebar');
  const main = $('mainContent');
  const btn = $('sidebarCollapseBtn');
  if (!sidebar) return;
  const collapsed = sidebar.classList.toggle('collapsed');
  if (main) main.classList.toggle('sidebar-collapsed', collapsed);
  if (btn) {
    btn.title = collapsed ? 'Expand sidebar' : 'Collapse sidebar';
    btn.innerHTML = collapsed
      ? '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 18l6-6-6-6"/></svg>'
      : '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M15 18l-6-6 6-6"/></svg>';
  }
}

// Firmware URL reveal/copy
function toggleOtaUrlReveal() {
  const input = $('otaUrl');
  if (!input) return;
  input.type = input.type === 'password' ? 'text' : 'password';
}

function copyOtaUrl() {
  const input = $('otaUrl');
  if (!input || !input.value) return;
  navigator.clipboard.writeText(input.value).then(() => {
    toast('URL copied');
  }).catch(() => {
    toast('Copy failed');
  });
}

// Tab switching
function switchTab(tab) {
  currentTab = tab;
  // Update nav items
  document.querySelectorAll('.nav-item').forEach(item => {
    item.classList.toggle('active', item.dataset.tab === tab);
  });
  // Update tab content
  document.querySelectorAll('.tab-content').forEach(content => {
    content.classList.toggle('active', content.id === 'tab-' + tab);
  });
  // Close sidebar on mobile
  const sidebar = $('sidebar');
  const overlay = $('overlay');
  if (sidebar) sidebar.classList.remove('open');
  if (overlay) overlay.classList.remove('show');
  
  // Refresh data for specific tabs
  if (tab === 'messages') refreshSmsList();
  if (tab === 'sims') refreshSims();
}

function classifyLogLine(line, forceError) {
  if (forceError) return 'err';
  const s = line.toLowerCase();
  if (/\b(fail|failed|failure|error|timeout|dropped|denied|refused|invalid|cannot|could not|unauthorized|abort)\b/.test(s)) return 'err';
  if (/\b(ok|success|forwarded|complete|connected|enabled|resumed|registered|mounted|ready)\b/.test(s)) return 'ok';
  if (/\b(pause|paused|warn|warning|retry|pending|waiting)\b/.test(s)) return 'warn';
  return 'info';
}

function parseLogLine(line) {
  const m = line.match(/^\[([^\]]+)\]\s*(.*)$/);
  if (m) return { tag: m[1], msg: m[2] };
  return { tag: '', msg: line };
}

function renderLogViewport(el, text, forceError) {
  if (!el) return;
  const lines = (text || '').split('\n').filter(l => l.trim());
  if (lines.length === 0) {
    el.innerHTML = '<div class="log-empty">' + (forceError ? 'No errors recorded' : 'No activity yet') + '</div>';
    return;
  }
  el.innerHTML = lines.map(line => {
    const cls = classifyLogLine(line, forceError);
    const { tag, msg } = parseLogLine(line);
    const tagPart = tag ? '<span class="log-tag">[' + escHtml(tag) + ']</span>' : '';
    const body = escHtml(tag ? msg : line);
    return '<div class="log-line ' + cls + '">' + tagPart + '<span class="log-msg">' + body + '</span></div>';
  }).join('');
}

function updateLogFooterMeta() {
  const meta = $('logFooterMeta');
  if (!meta) return;
  meta.textContent = currentLogTab === 'errors' ? 'Error history (newest at bottom)' : 'Activity log (newest at bottom)';
}

function switchLogTab(tab) {
  currentLogTab = tab;
  const liveTab = $('logTabLive');
  const errTab = $('logTabErrors');
  if (liveTab) liveTab.classList.toggle('active', tab === 'live');
  if (errTab) errTab.classList.toggle('active', tab === 'errors');

  const monitor = $('monitor');
  const errorLog = $('errorLog');

  if (tab === 'live') {
    if (monitor) monitor.classList.remove('hide');
    if (errorLog) errorLog.classList.add('hide');
  } else {
    if (monitor) monitor.classList.add('hide');
    if (errorLog) errorLog.classList.remove('hide');
    refreshErrorLog();
  }
  updateLogFooterMeta();
}

async function get(path, timeoutMs) {
  const ms = timeoutMs || 20000;
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), ms);
  try {
    const r = await fetch(path, {cache:'no-store', signal: ctrl.signal});
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  } finally {
    clearTimeout(timer);
  }
}

async function getText(path) {
  const r = await fetch(path, {cache:'no-store'});
  return r.text();
}

async function post(path, data, timeoutMs) {
  const ms = timeoutMs || 15000;
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), ms);
  try {
    const r = await fetch(path, {method:'POST', body: new URLSearchParams(data), signal: ctrl.signal});
    if (!r.ok) {
      throw new Error('HTTP ' + r.status);
    }
    return r.json();
  } catch (e) {
    console.error('POST error:', path, e);
    throw e;
  } finally {
    clearTimeout(timer);
  }
}

function updateStaNetworkUi(s, isLogin) {
  const gwEl = $(isLogin ? 'loginWifiGw' : 'wifiGw');
  const dnsEl = $(isLogin ? 'loginWifiDns' : 'wifiDns');
  const warnEl = $(isLogin ? 'loginWifiNetWarn' : 'wifiNetWarn');
  if (!s || !s.sta_connected) {
    if (gwEl) gwEl.textContent = '-';
    if (dnsEl) dnsEl.textContent = '-';
    if (warnEl) warnEl.classList.add('hide');
    return;
  }
  if (gwEl) gwEl.textContent = s.sta_gateway || '0.0.0.0';
  if (dnsEl) dnsEl.textContent = s.sta_dns || '0.0.0.0';
  if (warnEl) {
    if (s.sta_internet_ready === false) warnEl.classList.remove('hide');
    else warnEl.classList.add('hide');
  }
}

function signalBars(rssi) {
  const level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  const bar = (on, h) => `<span style="display:inline-block;width:3px;height:${h}px;margin-right:2px;border-radius:2px;background:${on ? 'var(--text)' : 'var(--border)'}"></span>`;
  return `<span style="display:inline-flex;align-items:flex-end;height:12px;">${bar(level >= 1, 4)}${bar(level >= 2, 6)}${bar(level >= 3, 9)}${bar(level >= 4, 12)}</span>`;
}

async function refreshStatus() {
  try {
    const s = await get('/status');
    window.lastStatus = s;
    
    // WiFi status
    const wifiBadge = $('wifiBadge');
    const wifiConnected = $('wifiConnected');
    const wifiSetup = $('wifiSetup');
    
    if (wifiBadge && wifiConnected && wifiSetup) {
      if (s.sta_connected) {
        wifiBadge.className = 'badge success';
        wifiBadge.textContent = 'Connected';
        if ($('wifiSsid')) $('wifiSsid').textContent = s.sta_ssid || 'Unknown';
        if ($('wifiIp')) $('wifiIp').textContent = s.sta_ip;
        if ($('wifiUrl')) $('wifiUrl').textContent = s.sta_ip ? ('http://' + s.sta_ip) : '-';
        updateStaNetworkUi(s, false);
        wifiConnected.classList.remove('hide');
        wifiSetup.classList.add('hide');
      } else {
        wifiBadge.className = 'badge danger';
        wifiBadge.textContent = 'Disconnected';
        wifiConnected.classList.add('hide');
        wifiSetup.classList.remove('hide');
      }
    }

    // Login-page WiFi status (available before sign-in)
    const loginWifiBadge = $('loginWifiBadge');
    const loginWifiConnected = $('loginWifiConnected');
    const loginScanBtn = $('loginScanBtn');
    const loginScanResults = $('loginScanResults');
    const loginManualSsid = $('loginManualSsid');
    const loginManualPw = $('loginManualPw');
    const loginConnectBtn = $('loginConnectBtn');
    if (loginWifiBadge) {
      const keepWifiSetup = !s.sta_connected || Date.now() < loginWifiUiGraceUntil;
      if (s.sta_connected) {
        loginWifiBadge.className = 'badge success';
        loginWifiBadge.textContent = 'Connected';
        if ($('loginWifiSsid')) $('loginWifiSsid').textContent = s.sta_ssid || 'Unknown';
        if ($('loginWifiIp')) $('loginWifiIp').textContent = s.sta_ip || '-';
        if ($('loginWifiUrl')) $('loginWifiUrl').textContent = s.sta_ip ? ('http://' + s.sta_ip) : '-';
        updateStaNetworkUi(s, true);
        if (loginWifiConnected) loginWifiConnected.classList.remove('hide');
        if (!keepWifiSetup) {
          if (loginScanBtn) loginScanBtn.classList.add('hide');
          if (loginScanResults) loginScanResults.classList.add('hide');
          if (loginManualSsid) loginManualSsid.classList.add('hide');
          if (loginManualPw) loginManualPw.classList.add('hide');
          if (loginConnectBtn) loginConnectBtn.classList.add('hide');
        } else {
          if (loginScanBtn) loginScanBtn.classList.remove('hide');
          if (loginManualSsid) loginManualSsid.classList.remove('hide');
          if (loginManualPw) loginManualPw.classList.remove('hide');
          if (loginConnectBtn) loginConnectBtn.classList.remove('hide');
        }
      } else {
        loginWifiBadge.className = 'badge danger';
        loginWifiBadge.textContent = 'Disconnected';
        if (loginWifiConnected) loginWifiConnected.classList.add('hide');
        if (loginScanBtn) loginScanBtn.classList.remove('hide');
        if (loginScanResults) loginScanResults.classList.remove('hide');
        if (loginManualSsid) loginManualSsid.classList.remove('hide');
        if (loginManualPw) loginManualPw.classList.remove('hide');
        if (loginConnectBtn) loginConnectBtn.classList.remove('hide');
      }
    }
    
    // Header status
    const statusDot = $('statusDot');
    if (statusDot) {
      statusDot.className = s.sta_connected ? 'status-dot ok' : 'status-dot warn';
    }
    const offlineBadge = $('offlineModeBadge');
    const signedIn = s.signed_in === true || (s.bearer_token && s.bearer_token.length > 0);
    if (offlineBadge) offlineBadge.classList.toggle('hide', signedIn);
    
    // Config
    if ($('baseUrl')) $('baseUrl').value = s.base_url || '';
    if ($('apiPath')) $('apiPath').value = s.api_path || '/api/agent/incoming-sms';
    if ($('pingHost') && !$('pingHost').dataset.userEdited) {
      $('pingHost').value = 'google.com';
    }
    if ($('deviceId')) $('deviceId').value = s.device_id || '';
    
    updateGatewayServicesUi(s);
    updateScheduledRestartNav(s);
    updateModemRunUi(s);
    
    updateFirmwareCard(s);
    
    // Active sessions card in SIM Slots tab
    const sessionsCard = $('sessionsCard');
    const sessionsBadge = $('sessionsBadge');
    const sessionsNavBadge = $('sessionsNavBadge');
    const sessions = s.activeSessions || [];

    // Update nav badge in sidebar
    if (sessionsNavBadge) {
      if (sessions.length > 0) {
        sessionsNavBadge.style.display = 'inline-block';
        sessionsNavBadge.textContent = sessions.length;
      } else {
        sessionsNavBadge.style.display = 'none';
      }
    }

    if (sessionsCard) {
      if (sessions.length > 0) {
        sessionsCard.style.display = 'block';
        if (sessionsBadge) sessionsBadge.textContent = sessions.length;
        // Store sessions for countdown timer
        window.activeSessionsData = sessions;
        updateSessionsDisplay();
      } else {
        sessionsCard.style.display = 'none';
        window.activeSessionsData = [];
      }
    }

    // Refresh SIM cards — skip full rebuild while USSD is running (avoids stale error flash)
    if (window.ussdManualBusy || window.ussdPendingSlot != null || window.ussdBulkPolling) {
      updateSimCardTimers();
    } else {
      refreshSims();
    }

  } catch(e) {
    console.error('Status error:', e);
  }
}

// Update sessions display with countdown
function updateSessionsDisplay() {
  const sessionsList = $('sessionsList');
  if (!sessionsList) return;

  const sessions = window.activeSessionsData || [];
  if (sessions.length === 0) {
    const sessionsCard = $('sessionsCard');
    if (sessionsCard) sessionsCard.style.display = 'none';
    return;
  }

  // Build HTML for each session
  let html = '';
  sessions.forEach(s => {
    const totalSecs = s.expiresIn || 0;
    const mins = Math.floor(totalSecs / 60);
    const secs = totalSecs % 60;

    // Format time: "10m" or "9:30" if under 10 mins
    let timeStr;
    let timeClass = 'session-time';
    if (mins >= 10) {
      timeStr = mins + 'm';
    } else if (mins > 0) {
      timeStr = mins + ':' + (secs < 10 ? '0' : '') + secs;
      timeClass += ' warning';
    } else {
      timeStr = secs + 's';
      timeClass += ' danger';
    }

    // SIM label
    const simLabel = 'SIM' + (s.slot + 1);

    html += '<div class="session-row">';
    html += '<span class="session-app">' + (s.app || '-') + '</span>';
    html += '<span class="session-sim">' + simLabel + '</span>';
    html += '<span class="session-msgs">' + (s.msgs || 0) + ' SMS</span>';
    html += '<span class="' + timeClass + '">' + timeStr + '</span>';
    html += '</div>';
  });

  sessionsList.innerHTML = html;
}

function formatRestartCountdown(sec) {
  if (sec == null || sec < 0) return '—';
  if (sec === 0) return 'Restarting…';
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (h > 0) return h + 'h ' + m + 'm';
  if (m > 0) return m + 'm ' + s + 's';
  return s + 's';
}

function updateScheduledRestartNav(s) {
  const banner = $('navRestartBanner');
  const timer = $('navRestartTimer');
  const mStrip = $('mobileRestartStrip');
  const mTimer = $('mobileRestartTimer');

  if (!s || !s.scheduled_restart_pending) {
    if (banner) banner.classList.remove('show', 'overdue');
    if (mStrip) mStrip.classList.remove('show', 'overdue');
    window.scheduledRestartInSec = null;
    return;
  }
  const sec = s.scheduled_restart_in_sec;
  window.scheduledRestartInSec = (sec != null && sec >= 0) ? sec : 0;
  const overdue = window.scheduledRestartInSec === 0;
  const label = formatRestartCountdown(window.scheduledRestartInSec);

  if (banner) { banner.classList.add('show'); banner.classList.toggle('overdue', overdue); }
  if (timer) timer.textContent = label;
  if (mStrip) { mStrip.classList.add('show'); mStrip.classList.toggle('overdue', overdue); }
  if (mTimer) mTimer.textContent = label;
}

function tickScheduledRestartCountdown() {
  if (window.scheduledRestartInSec == null) return;
  if (window.scheduledRestartInSec > 0) window.scheduledRestartInSec--;
  const label = formatRestartCountdown(window.scheduledRestartInSec);
  const overdue = window.scheduledRestartInSec === 0;
  const banner = $('navRestartBanner');
  const timer = $('navRestartTimer');
  const mStrip = $('mobileRestartStrip');
  const mTimer = $('mobileRestartTimer');
  if (timer) timer.textContent = label;
  if (banner) banner.classList.toggle('overdue', overdue);
  if (mTimer) mTimer.textContent = label;
  if (mStrip) mStrip.classList.toggle('overdue', overdue);
}

// Countdown timer - update every second
let sessionsCountdownInterval = null;
function startSessionsCountdown() {
  if (sessionsCountdownInterval) clearInterval(sessionsCountdownInterval);
  sessionsCountdownInterval = setInterval(() => {
    tickScheduledRestartCountdown();
    const sessions = window.activeSessionsData || [];
    if (sessions.length > 0) {
      sessions.forEach(sess => {
        if (sess.expiresIn > 0) sess.expiresIn--;
      });
      window.activeSessionsData = sessions.filter(sess => sess.expiresIn > 0);
      updateSessionsDisplay();
      updateSimCardTimers();
    }
  }, 1000);
}

// Update timer elements inside SIM cards
function updateSimCardTimers() {
  const sessions = window.activeSessionsData || [];
  sessions.forEach(sess => {
    const timerEl = document.querySelector(`.session-timer[data-slot="${sess.slot}"]`);
    if (!timerEl) return;

    const totalSecs = sess.expiresIn || 0;
    const mins = Math.floor(totalSecs / 60);
    const secs = totalSecs % 60;

    let timeStr;
    timerEl.classList.remove('warning', 'danger');
    if (mins >= 10) {
      timeStr = mins + 'm';
    } else if (mins > 0) {
      timeStr = mins + ':' + (secs < 10 ? '0' : '') + secs;
      timerEl.classList.add('warning');
    } else {
      timeStr = secs + 's';
      timerEl.classList.add('danger');
    }
    timerEl.textContent = timeStr;
  });
}

let lastMonitorRaw = '';

async function refreshMonitor() {
  try {
    const viewport = $('monitor');
    if (!viewport) return;
    const next = await getText('/monitor');
    if (next === lastMonitorRaw) return;
    lastMonitorRaw = next;
    const shouldStick = (viewport.scrollTop + viewport.clientHeight) >= (viewport.scrollHeight - 24);
    renderLogViewport(viewport, next, false);
    if (shouldStick) viewport.scrollTop = viewport.scrollHeight;
    const countEl = $('monitorCount');
    if (countEl) {
      const n = next.split('\n').filter(l => l.trim()).length;
      countEl.textContent = String(n);
    }
  } catch (e) {}
}

async function refreshErrorLog() {
  try {
    const viewport = $('errorLog');
    if (!viewport) return;
    const next = await getText('/error-log');
    renderLogViewport(viewport, next, true);
    viewport.scrollTop = viewport.scrollHeight;
    const countEl = $('errorLogCount');
    if (countEl) {
      const n = (next || '').split('\n').filter(l => l.trim()).length;
      countEl.textContent = String(n);
    }
  } catch (e) {
    if ($('errorLog')) {
      $('errorLog').innerHTML = '<div class="log-empty log-line err"><span class="log-msg">Could not load error log</span></div>';
    }
  }
}

function wifiUi(isLogin) {
  return {
    btn: $(isLogin ? 'loginScanBtn' : 'scanBtn'),
    list: $(isLogin ? 'loginScanResults' : 'scanResults'),
    ssidInput: $(isLogin ? 'loginManualSsid' : 'manualSsid'),
    pwInput: $(isLogin ? 'loginManualPw' : 'manualPw'),
    connectBtn: $(isLogin ? 'loginConnectBtn' : 'connectBtn'),
  };
}

async function scanNetworksFor(isLogin) {
  const ui = wifiUi(!!isLogin);
  const btn = ui.btn;
  const list = ui.list;
  if (!btn || !list) return;
  
  btn.innerHTML = '<span class="spinner"></span> Scanning...';
  btn.disabled = true;
  
  try {
    const nets = await get('/scan', 25000);
    list.innerHTML = '';
    
    if (nets.length === 0) {
      list.innerHTML = '<div class="muted" style="text-align:center;padding:20px;">No networks found</div>';
    } else {
      nets.sort((a, b) => b.rssi - a.rssi);
      nets.forEach(n => {
        const div = document.createElement('div');
        div.className = 'net-item';
        div.innerHTML = `
          <div class="net-info">
            <div class="net-name">${n.ssid}</div>
            <div class="net-signal">${n.rssi} dBm ${n.secure ? '• Secured' : '• Open'}</div>
          </div>
          <div class="net-icon">${signalBars(n.rssi)}</div>
        `;
        div.onclick = (ev) => selectNetworkFor(n.ssid, n.secure, !!isLogin, ev.currentTarget);
        list.appendChild(div);
      });
    }
  } catch(e) {
    console.error('Scan error:', e);
    list.innerHTML = '<div class="muted" style="text-align:center;padding:20px;color:var(--danger);">Scan failed — stay on gateway AP and try again.<br><small>' + escHtml(e.message || String(e)) + '</small></div>';
    toast('Scan failed — keep AP WiFi connected');
  }
  
  btn.textContent = isLogin ? 'Scan Networks' : 'Scan';
  btn.disabled = false;
}

function selectNetworkFor(ssid, secure, isLogin, clickedEl) {
  currentNetwork = ssid;
  const ui = wifiUi(!!isLogin);
  const ssidInput = ui.ssidInput;
  const pwInput = ui.pwInput;
  if (ssidInput) ssidInput.value = ssid;
  if (pwInput) {
    pwInput.value = '';
    pwInput.focus();
  }
  
  // Highlight selected
  document.querySelectorAll('.net-item').forEach(el => el.classList.remove('selected'));
  if (clickedEl) clickedEl.classList.add('selected');
  
  if (!secure) {
    connectWifiFor(!!isLogin);
  }
}

async function connectWifiFor(isLogin) {
  const ui = wifiUi(!!isLogin);
  const ssidInput = ui.ssidInput;
  const pwInput = ui.pwInput;
  const btn = ui.connectBtn;
  
  const ssid = ssidInput ? ssidInput.value.trim() : '';
  const pw = pwInput ? pwInput.value : '';
  
  if (!ssid) {
    toast('Enter network name');
    return;
  }
  
  if (btn) {
    btn.innerHTML = '<span class="spinner"></span> Connecting...';
    btn.disabled = true;
  }
  
  try {
    const r = await post('/save-wifi', {ssid, password: pw}, 15000);
    if (r.success) {
      toast('Connecting to ' + ssid + '...');
      loginWifiUiGraceUntil = Date.now() + LOGIN_UI_GRACE_MS;
      setTimeout(refreshStatus, 3000);
    } else {
      toast(r.error || 'Connection failed');
    }
  } catch(e) {
    toast('Connection failed');
  }
  
  if (btn) {
    btn.textContent = isLogin ? 'Connect WiFi' : 'Connect';
    btn.disabled = false;
  }
}

async function scanNetworks() { return scanNetworksFor(false); }
async function scanNetworksLogin() { return scanNetworksFor(true); }
function selectNetwork(ssid, secure) { return selectNetworkFor(ssid, secure, false, null); }
async function connectWifi() { return connectWifiFor(false); }
async function connectWifiLogin() { return connectWifiFor(true); }

async function disconnectWifi() {
  await fetch('/disconnect');
  toast('Disconnected');
  setTimeout(refreshStatus, 1000);
}

async function copyStaUrl() {
  const s = window.lastStatus || {};
  const url = s && s.sta_ip ? ('http://' + s.sta_ip) : '';
  if (!url) {
    toast('No STA IP yet');
    return;
  }
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(url);
    } else {
      const ta = document.createElement('textarea');
      ta.value = url;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    }
    toast('Copied: ' + url);
  } catch (e) {
    toast('Copy failed');
  }
}

function toggleUssdBulkPanel() {
  const p = $('ussdBulkPanel');
  const ch = $('ussdBulkChevron');
  if (!p) return;
  p.classList.toggle('collapsed');
  if (ch) ch.textContent = p.classList.contains('collapsed') ? '▶' : '▼';
}

function ussdSecLabel(sec) {
  if (sec == null || sec <= 0) return '';
  return ' · ' + sec + 's';
}

function escHtml(str) {
  if (str == null) return '';
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function updateUssdBulkButton(s) {
  const bulkBtn = $('ussdBulkBtn');
  const active = s.ussd_bulk_active || window.ussdBulkPolling;
  if (!bulkBtn) return;
  bulkBtn.disabled = !!active;
  bulkBtn.textContent = active ? 'Checking…' : '*143# All';
}

function setUssdBulkRunningUi(st) {
  const summary = $('ussdBulkSummary');
  const body = $('ussdBulkBody');
  const panel = $('ussdBulkPanel');
  if (panel) panel.classList.remove('collapsed');
  const ch = $('ussdBulkChevron');
  if (ch) ch.textContent = '▼';
  const cur = st.current_sim || '?';
  const done = st.done != null ? st.done : 0;
  const total = st.total != null ? st.total : '?';
  if (summary) {
    summary.innerHTML = '<span class="spinner"></span> SIM <strong>' + cur + '</strong> (' + done + '/' + total + ')';
  }
  if (body) body.innerHTML = '';
}

async function refreshSims() {
  const grid = $('simGrid');
  if (!grid) return;
  try {
    const s = await get('/sim-config');
    if (!s || s.success === false) {
      throw new Error((s && s.error) ? s.error : 'sim-config failed');
    }
    if (!s.enabled || !Array.isArray(s.enabled)) {
      throw new Error('Invalid sim-config response');
    }
    grid.innerHTML = '';

    let activeCount = 0;
    const bulkCurrent = (s.ussd_bulk_current != null) ? s.ussd_bulk_current : -1;
    const pendingSlot = window.ussdPendingSlot;

    // Build session map by slot
    const sessionsBySlot = {};
    const sessions = window.activeSessionsData || [];
    sessions.forEach(sess => {
      // Keep the first session for display details
      if (!sessionsBySlot[sess.slot]) {
        sessionsBySlot[sess.slot] = sess;
      }
    });

    for (let i = 0; i < s.enabled.length; i++) {
      const div = document.createElement('div');

      let rssi = null;
      let bars = 0;
      if (s.csq_rssi && s.csq_rssi[i] != null && s.csq_rssi[i] >= 0) {
        rssi = s.csq_rssi[i];
      } else if (s.csq && s.csq[i]) {
        const csqMatch = s.csq[i].match(/\+CSQ:\s*(\d+)/);
        if (csqMatch) rssi = parseInt(csqMatch[1]);
      }
      if (rssi != null) {
        if (rssi === 99) bars = 0;
        else if (rssi >= 20) bars = 4;
        else if (rssi >= 15) bars = 3;
        else if (rssi >= 10) bars = 2;
        else if (rssi >= 5) bars = 1;
      }

      let network = '-';
      if (s.operators && s.operators[i]) {
        network = s.operators[i];
      } else if (s.cops && s.cops[i]) {
        const copsMatch = s.cops[i].match(/"([^"]+)"/);
        if (copsMatch) network = copsMatch[1];
      }

      const num = s.numbers[i] || '-';
      const isResponsive = s.responsive && s.responsive[i];
      const isEnabled = s.enabled[i];
      const ussdSt = (s.ussd_status && s.ussd_status[i]) ? s.ussd_status[i] : 0;
      const ussdMsg = (s.ussd_result && s.ussd_result[i]) ? s.ussd_result[i] : '';
      const ussdSec = (s.ussd_duration_sec && s.ussd_duration_sec[i] != null) ? s.ussd_duration_sec[i] : 0;

      if (isEnabled && isResponsive) activeCount++;

      // Card classes
      const isManualOff = s.user_disabled && s.user_disabled[i];
      let cardClass = 'sim-card';
      if (!isEnabled || isManualOff) cardClass += ' disabled';
      else if (isResponsive && s.registered[i]) cardClass += ' active';

      // Status dot
      let statusClass = 'sim-status';
      if (isEnabled && isResponsive && s.registered[i]) statusClass += ' ok';
      else if (isEnabled && isResponsive) statusClass += ' warn';

      // Signal bars and strength value
      let signalHtml = '';
      let signalText = '-';
      if (bars > 0) {
        signalHtml = `<span class="signal s${bars}"><i></i><i></i><i></i><i></i></span>`;
        // Convert CSQ to approximate dBm: dBm ≈ -113 + 2 * CSQ
        const dbm = -113 + (2 * rssi);
        signalText = `${rssi} (${dbm}dBm)`;
      } else if (rssi === 99) {
        signalText = '?';
      }

      // Battery
      const battery = (s.battery_pct && s.battery_pct[i] != null) ? s.battery_pct[i] : -1;

      // Copy button for number
      const copyBtn = num !== '-' ? `<button class="copy-btn" data-copy="${escHtml(num)}" title="Copy number"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg></button>` : '';

      // Session info for this slot
      const sess = sessionsBySlot[i];
      let sessionHtml = '';
      if (sess) {
        const totalSecs = sess.expiresIn || 0;
        const mins = Math.floor(totalSecs / 60);
        const secs = totalSecs % 60;
        let timeStr, timeClass;
        if (mins >= 10) {
          timeStr = mins + 'm';
          timeClass = 'session-timer';
        } else if (mins > 0) {
          timeStr = mins + ':' + (secs < 10 ? '0' : '') + secs;
          timeClass = 'session-timer warning';
        } else {
          timeStr = secs + 's';
          timeClass = 'session-timer danger';
        }
        sessionHtml = `
          <div class="sim-session">
            <span class="session-app">${sess.app || '-'}</span>
            <span class="${timeClass}" data-slot="${i}">${timeStr}</span>
            <span class="session-msgs">SMS: ${sess.msgs || 0}</span>
          </div>`;
      }

      const isChecking = (ussdSt === 3) || (pendingSlot === i) ||
        ((s.ussd_bulk_active || window.ussdBulkPolling) && bulkCurrent === i);
      let ussdHtml = '';
      if (isChecking) {
        ussdHtml = '<div class="sim-ussd pending">*143# checking…</div>';
      } else if (ussdSt === 1) {
        const bal = escHtml(ussdMsg || '');
        ussdHtml = `<div class="sim-ussd ok">Balance: ${bal || '—'}${ussdSecLabel(ussdSec)}</div>`;
      } else if (ussdSt === 2) {
        const err = escHtml(ussdMsg || 'Error');
        const label = err === 'No response' ? 'No response' : 'Error';
        ussdHtml = `<div class="sim-ussd err">${label}${ussdSecLabel(ussdSec)}</div>`;
      }

      div.className = cardClass + (isChecking ? ' ussd-checking' : '');
      div.innerHTML = `
        <div class="sim-header">
          <span class="sim-slot">SIM ${i + 1}</span>
          <div class="${statusClass}"></div>
        </div>
        <div class="sim-number">${escHtml(num)} ${copyBtn}</div>
        ${isManualOff ? '<div class="sim-manual-off" style="font-size:11px;color:var(--warning);margin-top:4px;">Manual OFF — heartbeat will not re-enable</div>' : ''}
        <div class="sim-meta">
          ${signalHtml}
          <span title="Signal strength">${signalText}</span>
          <span>${escHtml(network)}</span>
          ${battery >= 0 ? `<span>BAT ${battery}%</span>` : ''}
        </div>
        ${sessionHtml}
        ${ussdHtml}
        <div class="sim-actions">
          <button class="btn xs ${isEnabled ? 'danger' : 'success'}" onclick="toggleSim(${i})">${isEnabled ? 'Off' : 'On'}</button>
          ${num !== '-' && !window.ussdManualBusy && !window.ussdBulkPolling ? `<button class="btn xs secondary" onclick="checkUssdSlot(${i})" title="Dial *143#">*143#</button>` : ''}
        </div>
      `;
      grid.appendChild(div);
    }

    // Update active count
    const simCountEl = $('simCount');
    const simCountPill = $('simCountPill');
    if (simCountEl) {
      simCountEl.textContent = activeCount + (activeCount === 1 ? ' active slot' : ' active slots');
    }
    if (simCountPill) {
      simCountPill.classList.toggle('warn', activeCount === 0);
    }
    updateUssdBulkButton(s);
    grid.querySelectorAll('.copy-btn[data-copy]').forEach(btn => {
      btn.onclick = () => copyText(btn, btn.getAttribute('data-copy'));
    });
  } catch(e) {
    console.error('refreshSims failed:', e);
    if (grid) {
      grid.innerHTML = '<div class="muted" style="padding:16px;color:var(--danger);">SIM list failed to load. Retry or check serial log.<br><small>' + escHtml(e.message || String(e)) + '</small></div>';
    }
  }
}

// Copy text helper with checkmark animation
function copyText(btn, text) {
  // Handle both old and new call signatures
  if (typeof btn === 'string') {
    text = btn;
    btn = null;
  }
  
  const doCopy = () => {
    if (navigator.clipboard) {
      navigator.clipboard.writeText(text).then(() => toast('Copied!'));
    } else {
      // Fallback
      const ta = document.createElement('textarea');
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
      toast('Copied!');
    }
  };
  
  // If button provided, animate to checkmark
  if (btn && btn.classList) {
    const originalSvg = btn.innerHTML;
    btn.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="20 6 9 17 4 12"/></svg>';
    btn.classList.add('copied');
    doCopy();
    setTimeout(() => {
      btn.innerHTML = originalSvg;
      btn.classList.remove('copied');
    }, 1500);
  } else {
    doCopy();
  }
}

// SMS List refresh - reads from persistent file
let allMessages = [];

function senderInitials(sender) {
  if (!sender) return '?';
  const clean = sender.replace(/[^a-zA-Z0-9+]/g, '');
  if (!clean) return '?';
  if (/^\+?[0-9]/.test(clean)) return '#';
  return clean.slice(0, 2).toUpperCase();
}

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function highlightText(text, query) {
  if (!query) return escHtml(text);
  const re = new RegExp('(' + query.replace(/[.*+?^${}()|[\]\\]/g,'\\$&') + ')', 'gi');
  return escHtml(text).replace(re, '<mark class="sms-highlight">$1</mark>');
}

function renderMessages(messages, query) {
  const list = $('smsList');
  if (!list) return;
  if (messages.length === 0) {
    list.innerHTML = `<div class="empty-state">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg>
      <p>${query ? 'No messages match "' + escHtml(query) + '"' : 'No messages yet'}</p>
    </div>`;
    return;
  }
  list.innerHTML = messages.map(msg => {
    const initials = senderInitials(msg.sender);
    const senderH = highlightText(msg.sender || 'Unknown', query);
    const bodyH = highlightText(msg.body, query);
    const bodyEsc = escHtml(msg.body).replace(/'/g, '&#39;');
    const senderEsc = escHtml(msg.sender || '').replace(/'/g, '&#39;');
    return `<div class="sms-item">
      <div class="sms-avatar">${initials}</div>
      <div class="sms-main">
        <div class="sms-row1">
          <span class="sms-sender">${senderH}</span>
          <span class="sms-time">${escHtml(msg.time)}</span>
        </div>
        <div class="sms-row2">
          <span class="sms-slot">SIM ${msg.slot}</span>
          <span class="sms-number">${escHtml(msg.number || '')}</span>
        </div>
        <div class="sms-body">${bodyH}</div>
      </div>
      <div class="sms-actions">
        <button class="sms-copy-btn" onclick="copyText('${bodyEsc}')">Copy</button>
        <button class="sms-copy-btn" onclick="copyText('${senderEsc}')">Sender</button>
      </div>
    </div>`;
  }).join('');
}

function filterMessages() {
  const q = ($('smsSearch') ? $('smsSearch').value.trim() : '').toLowerCase();
  const filtered = q
    ? allMessages.filter(m =>
        (m.sender || '').toLowerCase().includes(q) ||
        (m.body || '').toLowerCase().includes(q) ||
        (m.number || '').toLowerCase().includes(q))
    : allMessages;
  const count = $('smsCount');
  if (count) count.textContent = filtered.length + ' of ' + allMessages.length;
  renderMessages(filtered, q);
}

async function refreshSmsList() {
  try {
    const data = await getText('/messages');
    const lines = data.split('\n').filter(l => l.trim());
    allMessages = [];
    for (const line of lines) {
      const parts = line.split('|');
      if (parts.length >= 5) {
        allMessages.push({
          time: parts[0],
          slot: parseInt(parts[1]),
          number: parts[2],
          sender: parts[3],
          body: parts.slice(4).join('|')
        });
      }
    }
    allMessages.reverse();
    const badge = $('msgBadge');
    if (badge) badge.textContent = allMessages.length;
    const count = $('smsCount');
    if (count) count.textContent = allMessages.length ? (allMessages.length + ' messages') : 'No messages';
    filterMessages();
  } catch(e) {
    console.error('Failed to load SMS list', e);
  }
}

async function clearMessages() {
  if (!confirm('Delete all saved messages from this device?')) return;
  try {
    const r = await get('/clear-messages');
    if (!r || r.success === false) {
      toast((r && r.message) || 'Could not clear messages');
      return;
    }
    allMessages = [];
    const badge = $('msgBadge');
    if (badge) badge.textContent = '0';
    const count = $('smsCount');
    if (count) count.textContent = 'No messages';
    renderMessages([], ($('smsSearch') ? $('smsSearch').value.trim() : '').toLowerCase());
    toast('Messages cleared');
  } catch (e) {
    console.error('Failed to clear messages', e);
    toast('Could not clear messages');
  }
}

async function checkAllSims() {
  const btn = $('checkAllBtn');
  if (!btn) return;
  btn.innerHTML = '<span class="spinner"></span>';
  btn.disabled = true;
  try {
    await get('/check-all-sim');
    await refreshSims();
    toast('Slot scan complete');
  } catch(e) {
    toast('Slot refresh failed');
  }
  btn.textContent = 'Refresh slots';
  btn.disabled = false;
}

function setStatusPill(el, state, label) {
  if (!el) return;
  el.className = 'status-pill ' + state;
  const dotClass = state === 'on' ? 'ok' : (state === 'warn' ? 'warn' : '');
  el.innerHTML = '<span class="status-dot' + (dotClass ? ' ' + dotClass : '') + '"></span> ' + label;
}

function updateModemRunUi(s) {
  const running = !!(s && s.modem_running);
  const queued = !!(s && s.modem_start_queued);
  const apply = (badgeId, hintId, runBtnId, stopBtnId) => {
    const badge = $(badgeId);
    const hint = $(hintId);
    const runBtn = $(runBtnId);
    const stopBtn = stopBtnId ? $(stopBtnId) : null;
    if (badge) {
      if (running) {
        badge.className = 'badge success';
        badge.textContent = 'Running';
      } else if (queued) {
        badge.className = 'badge warning';
        badge.textContent = 'Starting…';
      } else {
        badge.className = 'badge warning';
        badge.textContent = 'Idle';
      }
    }
    if (hint) {
      if (running) {
        hint.textContent = 'SMS polling and slot watchdog are active.';
      } else if (queued) {
        hint.textContent = 'Initializing all SIM slots — keep this page open.';
      } else {
        hint.textContent = 'Idle — press Run when SIM800 modules are powered and wired.';
      }
    }
    if (runBtn) {
      runBtn.disabled = running || queued;
      runBtn.textContent = queued ? 'Starting…' : 'Run — init SIM slots & start polling';
    }
    if (stopBtn) {
      if (running) stopBtn.classList.remove('hide');
      else stopBtn.classList.add('hide');
    }
  };
  apply('modemRunBadge', 'modemRunHint', 'modemRunBtn', 'modemStopBtn');
  apply('loginModemRunBadge', 'loginModemRunHint', 'loginModemRunBtn', null);
}

async function startModemGateway() {
  try {
    const r = await post('/modem-start', {}, 8000);
    toast(r.message || r.error || 'Start queued');
    refreshStatus();
  } catch (e) {
    toast('Failed to queue start');
  }
}

async function stopModemGatewayUi() {
  try {
    await post('/modem-stop', {}, 8000);
    toast('Modem stopped');
    refreshStatus();
  } catch (e) {
    toast('Stop failed');
  }
}

function updateGatewayServicesUi(s) {
  if (!s) return;

  const hbPaused = !!s.heartbeat_paused;
  const pollPaused = !!s.sms_polling_paused;
  const mcfOn = !!s.missed_call_forward;
  const watchSim = s.missed_call_watch_sim || 0;

  setStatusPill($('heartbeatStatusPill'), hbPaused ? 'off' : 'on', hbPaused ? 'Paused' : 'Active');
  const hbBtn = $('pauseHeartbeatBtn');
  if (hbBtn) {
    hbBtn.textContent = hbPaused ? 'Resume heartbeat' : 'Pause heartbeat';
    hbBtn.className = hbPaused ? 'btn sm warning' : 'btn sm secondary';
  }

  setStatusPill($('smsPollingStatusPill'), pollPaused ? 'warn' : 'on', pollPaused ? 'Paused' : 'Active');
  const pollBtn = $('pausePollingBtn');
  if (pollBtn) {
    pollBtn.textContent = pollPaused ? 'Resume SMS polling' : 'Pause SMS polling';
    pollBtn.className = pollPaused ? 'btn sm warning' : 'btn sm secondary';
  }

  let mcfState = mcfOn ? 'on' : 'off';
  let mcfLabel = mcfOn ? 'On' : 'Off';
  if (mcfOn && watchSim > 0) {
    mcfState = 'on';
    mcfLabel = 'On · SIM ' + watchSim;
  }
  setStatusPill($('missedCallStatusPill'), mcfState, mcfLabel);

  const mcfBtn = $('toggleMissedCallBtn');
  if (mcfBtn) {
    mcfBtn.textContent = mcfOn ? 'Disable forwarding' : 'Enable forwarding';
    mcfBtn.className = mcfOn ? 'btn sm warning' : 'btn sm success';
  }
}

async function togglePollingPause() {
  try {
    const r = await post('/toggle-polling', {});
    if (r.success) {
      toast(r.message || 'SMS polling updated');
      const st = await get('/status');
      updateGatewayServicesUi(st);
    } else {
      toast(r.error || 'Failed');
    }
  } catch(e) {
    toast('Failed to toggle SMS polling');
  }
}

async function toggleMissedCallForward() {
  try {
    const s = await get('/status');
    const turnOn = !s.missed_call_forward;
    const r = await post('/toggle-missed-call', { enabled: turnOn ? 1 : 0 });
    if (r.success) {
      const st = await get('/status');
      st.missed_call_forward = r.enabled;
      updateGatewayServicesUi(st);
      toast(r.message || (r.enabled ? 'Missed-call forwarding enabled' : 'Missed-call forwarding disabled'));
    } else {
      toast(r.error || 'Failed');
    }
  } catch (e) {
    toast('Failed to toggle missed-call forwarding');
  }
}

function formatFwCheckAgo(secAgo) {
  if (secAgo == null || secAgo < 0) return 'never';
  if (secAgo < 60) return 'just now';
  if (secAgo < 3600) return Math.floor(secAgo / 60) + 'm ago';
  if (secAgo < 86400) return Math.floor(secAgo / 3600) + 'h ago';
  return Math.floor(secAgo / 86400) + 'd ago';
}

function applyFirmwareUiState(opts) {
  const pill = $('firmwareStatusPill');
  const installBtn = $('installFirmwareBtn');
  const statusLine = $('fwStatusLine');
  const installedBox = $('fwInstalledBox');
  const publishedBox = $('fwPublishedBox');
  const otaEnabled = opts.ota_install_enabled !== false;

  if ($('fwVersion') && opts.current) $('fwVersion').textContent = opts.current;
  if ($('fwRemoteVersion')) {
    $('fwRemoteVersion').textContent = opts.remote && opts.remote.length ? opts.remote : '—';
  }
  if (installedBox) installedBox.classList.remove('highlight');
  if (publishedBox) publishedBox.classList.remove('highlight');

  if (!opts.success) {
    setStatusPill(pill, 'off', '—');
    if (statusLine) statusLine.textContent = opts.message || 'Could not check for updates. Check WiFi and try again.';
    if (installBtn) installBtn.disabled = true;
    return;
  }

  if (opts.update_available) {
    setStatusPill(pill, 'warn', 'Update available');
    if (publishedBox) publishedBox.classList.add('highlight');
    if (statusLine) statusLine.textContent = 'Version ' + opts.remote + ' is ready to install. Tap Install update below.';
    if (installBtn) installBtn.disabled = !otaEnabled;
  } else if (opts.local_ahead) {
    setStatusPill(pill, 'on', 'Up to date');
    if (statusLine) statusLine.textContent = 'Your device is running the latest software.';
    if (installBtn) installBtn.disabled = true;
  } else {
    setStatusPill(pill, 'on', 'Up to date');
    if (statusLine) statusLine.textContent = 'Your device is running the latest software.';
    if (installBtn) installBtn.disabled = true;
  }
}

function updateFirmwareOtaUi(s) {
  const installBtn = $('installFirmwareBtn');
  const note = $('otaInstallNote');
  if (!installBtn) return;
  if (s && s.ota_install_enabled === false) {
    installBtn.disabled = true;
    if (note) {
      note.classList.remove('hide');
      note.textContent = 'Wireless updates need to be enabled first. Please contact support to activate this feature on your device.';
    }
  } else if (note) {
    note.classList.add('hide');
  }
}

function updateFirmwareCard(s) {
  if (!s) return;
  if ($('otaUrl') && s.ota_url) $('otaUrl').value = s.ota_url;

  const restartNotice = $('fwRestartNotice');
  const restartText = $('fwRestartNoticeText');
  if (restartNotice && restartText) {
    if (s.scheduled_restart_pending) {
      restartNotice.classList.remove('hide');
      const sec = s.scheduled_restart_in_sec;
      if (sec != null && sec >= 0) {
        restartText.textContent = sec === 0
          ? 'Restart due now — will happen automatically when safe.'
          : 'Device restarts in ' + formatRestartCountdown(sec) + '.';
      } else {
        restartText.textContent = 'A restart has been scheduled.';
      }
    } else {
      restartNotice.classList.add('hide');
    }
  }

  const meta = $('fwMetaLine');
  if (meta) {
    const ago = s.firmware_last_check_sec_ago >= 0
      ? 'Last checked ' + formatFwCheckAgo(s.firmware_last_check_sec_ago)
      : 'Not checked yet';
    meta.textContent = ago;
  }

  updateFirmwareOtaUi(s);

  applyFirmwareUiState({
    success: s.firmware_remote_version != null && s.firmware_remote_version.length > 0,
    current: s.firmware_version || '—',
    remote: s.firmware_remote_version || '',
    update_available: !!s.firmware_update_available,
    local_ahead: !!s.firmware_local_ahead,
    ota_install_enabled: s.ota_install_enabled,
    message: s.firmware_remote_version ? '' : 'Run Check for updates to fetch the published version.'
  });
}

async function checkFirmwareUpdate() {
  const btn = $('checkFirmwareBtn');
  if (btn) {
    btn.innerHTML = '<span class="spinner"></span>';
    btn.disabled = true;
  }
  try {
    const r = await get('/firmware-check');
    const st = await get('/status').catch(() => ({}));
    applyFirmwareUiState({
      success: r.success,
      current: r.current,
      remote: r.remote,
      update_available: r.update_available,
      local_ahead: r.local_ahead,
      ota_install_enabled: st.ota_install_enabled,
      message: r.message
    });
    updateFirmwareOtaUi(st);
    if (r.success && r.update_available) {
      toast('Update available: v' + r.remote);
    } else if (r.success && r.local_ahead) {
      toast('Installed v' + r.current + ' is ahead of published v' + r.remote);
    } else if (r.success) {
      toast('Up to date with published v' + (r.remote || r.current));
    } else {
      toast(r.message || 'Could not reach update server');
    }
    if (st.firmware_version) updateFirmwareCard(st);
  } catch (e) {
    setStatusPill($('firmwareStatusPill'), 'off', 'Check failed');
    if ($('fwStatusLine')) $('fwStatusLine').textContent = 'Firmware check failed. Check WiFi and version URL.';
    toast('Firmware check failed');
  }
  if (btn) {
    btn.textContent = 'Check for updates';
    btn.disabled = false;
  }
}

async function saveOtaUrl() {
  const input = $('otaUrl');
  if (!input) return;
  const url = input.value.trim();
  try {
    const btn = $('saveOtaUrlBtn');
    if (btn) {
      btn.innerHTML = '<span class="spinner"></span> Saving...';
      btn.disabled = true;
    }
    const r = await post('/firmware-config', url ? { url } : {});
    if (r && r.success) {
      if (r.ota_url) input.value = r.ota_url;
      toast('Firmware URL saved');
      const st = await get('/status').catch(() => ({}));
      updateFirmwareCard(st);
    } else {
      toast((r && r.error) || 'Failed to save firmware URL');
    }
    if (btn) {
      btn.textContent = 'Save URL';
      btn.disabled = false;
    }
  } catch (e) {
    toast('Failed to save firmware URL');
  }
}

async function installFirmwareUpdate() {
  if (!confirm('Install the latest software update and restart the gateway?')) return;
  const btn = $('installFirmwareBtn');
  if (btn) {
    btn.innerHTML = '<span class="spinner"></span>';
    btn.disabled = true;
  }
  toast('Downloading firmware… device will restart');
  try {
    const otaInput = $('otaUrl');
    const url = otaInput ? otaInput.value.trim() : '';
    const payload = url ? { url } : {};
    const r = await post('/firmware-update', payload);
    if (r && r.success === false) {
      toast(r.message || 'OTA not available in this build');
    }
  } catch (e) {
    toast('Update request failed');
    if (btn) {
      btn.textContent = 'Install update';
      btn.disabled = false;
    }
  }
}

async function toggleHeartbeatPause() {
  try {
    const r = await post('/toggle-heartbeat', {});
    if (r.success) {
      toast(r.message || 'Heartbeat updated');
      const st = await get('/status');
      st.heartbeat_paused = r.paused;
      updateGatewayServicesUi(st);
    } else {
      toast(r.error || 'Failed');
    }
  } catch(e) {
    toast('Failed to toggle heartbeat');
  }
}

let ussdManualPollTimer = null;

async function checkUssdSlot(idx) {
  if (window.ussdManualBusy) {
    toast('*143# check already running');
    return;
  }
  window.ussdManualBusy = true;
  window.ussdPendingSlot = idx;
  refreshSims();
  try {
    const r = await post('/ussd-check', { slot: idx });
    if (!r.success || !r.started) {
      window.ussdManualBusy = false;
      window.ussdPendingSlot = null;
      toast(r.error || 'Could not start USSD');
      refreshSims();
      return;
    }
    toast('Checking SIM ' + (idx + 1) + '…');
    let polls = 0;
    const maxPolls = 120;
    if (ussdManualPollTimer) clearInterval(ussdManualPollTimer);
    ussdManualPollTimer = setInterval(async () => {
      polls++;
      if (polls > maxPolls) {
        clearInterval(ussdManualPollTimer);
        ussdManualPollTimer = null;
        window.ussdManualBusy = false;
        window.ussdPendingSlot = null;
        toast('USSD check timed out');
        refreshSims();
        return;
      }
      try {
        const st = await get('/ussd-manual-status');
        if (st.in_progress) {
          if (st.slot != null) window.ussdPendingSlot = st.slot;
          refreshSims();
          return;
        }
        clearInterval(ussdManualPollTimer);
        ussdManualPollTimer = null;
        window.ussdManualBusy = false;
        window.ussdPendingSlot = null;
        // st.sim is set to the slot we actually ran on; idx is what was clicked.
        // If the server never ran (manualLastSlot still -1) st.sim will be absent — skip toast.
        if (st.sim == null) {
          // No result available yet (cleared on new start) — silently ignore
          refreshSims();
          return;
        }
        const sec = st.duration_sec != null ? st.duration_sec : 0;
        const simNum = st.sim;
        if (st.ok) {
          toast('SIM ' + simNum + ': ' + (st.message || '—') + ussdSecLabel(sec));
        } else {
          // Only show error toast if this result belongs to the slot we clicked
          if (st.slot === idx) {
            toast('SIM ' + simNum + ': ' + (st.message || 'No balance info') + ussdSecLabel(sec));
          }
        }
        refreshSims();
      } catch (e) {
        console.error('ussd manual poll', e);
      }
    }, 800);
  } catch (e) {
    window.ussdManualBusy = false;
    window.ussdPendingSlot = null;
    toast('Could not start USSD check');
    refreshSims();
  }
}

let ussdBulkPollTimer = null;

function renderUssdBulkResults(data) {
  const panel = $('ussdBulkPanel');
  const body = $('ussdBulkBody');
  const summary = $('ussdBulkSummary');
  if (!panel || !body) return;
  panel.classList.remove('collapsed');
  const ch = $('ussdBulkChevron');
  if (ch) ch.textContent = '▼';

  const okN = data.ok_count != null ? data.ok_count : (data.results ? data.results.filter(x => x.ok).length : 0);
  const failN = data.fail_count != null ? data.fail_count : (data.results ? data.results.filter(x => !x.ok).length : 0);
  if (summary) summary.textContent = okN + ' OK · ' + failN + ' failed';

  if (!data.results || data.results.length === 0) {
    body.innerHTML = '<span class="muted">No results.</span>';
    return;
  }

  const okList = data.results.filter(x => x.ok);
  const failList = data.results.filter(x => !x.ok);

  let html = '';

  if (okList.length) {
    html += '<p class="ussd-bulk-ok" style="margin-top:10px;font-weight:600;">Success</p>';
    okList.forEach(r => {
      html += `<div class="ussd-bulk-line ussd-bulk-ok">SIM ${r.sim} · ${escHtml(r.number)} · Balance ${escHtml(r.message || '—')}${ussdSecLabel(r.duration_sec)}</div>`;
    });
  }
  if (failList.length) {
    html += '<p class="ussd-bulk-fail" style="margin-top:10px;font-weight:600;">Failed</p>';
    failList.forEach(r => {
      const fl = r.message === 'No response' ? 'No response' : 'Error';
      html += `<div class="ussd-bulk-line ussd-bulk-fail">SIM ${r.sim} · ${escHtml(r.number)} · ${fl}${ussdSecLabel(r.duration_sec)}</div>`;
    });
  }
  body.innerHTML = html;
}

async function bulkCheckUssd() {
  if (ussdBulkPollTimer) {
    toast('Bulk *143# already running');
    return;
  }
  try {
    const r = await post('/ussd-bulk', {});
    if (!r.success) {
      toast(r.error || 'Could not start bulk check');
      return;
    }
    window.ussdBulkPolling = true;
    window.ussdBulkTotal = r.total || 0;
    window.ussdBulkDone = 0;
    const panel = $('ussdBulkPanel');
    const body = $('ussdBulkBody');
    const summary = $('ussdBulkSummary');
    if (panel) { panel.classList.remove('collapsed'); }
    const ch = $('ussdBulkChevron');
    if (ch) ch.textContent = '▼';
    if (summary) summary.innerHTML = '<span class="spinner"></span> Starting (' + (r.total || '?') + ' SIMs)…';
    if (body) body.innerHTML = '';
    refreshSims();

    ussdBulkPollTimer = setInterval(async () => {
      try {
        const st = await get('/ussd-bulk-status');
        if (st.in_progress) {
          window.ussdBulkDone = st.done || 0;
          window.ussdBulkTotal = st.total || window.ussdBulkTotal;
          window.ussdBulkCurrentSim = st.current_sim || null;
          setUssdBulkRunningUi(st);
          refreshSims();
          return;
        }
        clearInterval(ussdBulkPollTimer);
        ussdBulkPollTimer = null;
        window.ussdBulkPolling = false;
        window.ussdBulkCurrentSim = null;
        renderUssdBulkResults(st);
        refreshSims();
        toast('Bulk *143# done: ' + (st.ok_count || 0) + ' OK, ' + (st.fail_count || 0) + ' failed');
      } catch (e) {
        clearInterval(ussdBulkPollTimer);
        ussdBulkPollTimer = null;
        window.ussdBulkPolling = false;
        console.error('ussd bulk poll', e);
      }
    }, 1500);
  } catch (e) {
    window.ussdBulkPolling = false;
    toast('Bulk *143# failed to start');
  }
}

async function toggleSim(idx) {
  const s = await get('/sim-config');
  const newEnabled = !s.enabled[idx];
  const r = await post('/sim-enable', { slot: idx, enabled: newEnabled ? 1 : 0 });
  if (r.success) {
    toast('SIM ' + (idx + 1) + ' ' + (newEnabled ? 'enabled' : 'disabled'));
    await refreshSims();
  } else {
    toast('Failed: ' + (r.error || 'Unknown error'));
  }
}

async function disableAllSims() {
  if (!confirm('Disable all SIM slots? They will stop polling and reporting until turned on again.')) return;
  const btn = $('disableAllSimsBtn');
  if (btn) {
    btn.innerHTML = '<span class="spinner"></span>';
    btn.disabled = true;
  }
  try {
    const r = await post('/sim-disable-all', {});
    if (r.success) {
      toast(r.message || 'All SIMs disabled');
      await refreshSims();
    } else {
      toast(r.error || 'Failed');
    }
  } catch (e) {
    toast('Failed to disable all SIMs');
  }
  if (btn) {
    btn.textContent = 'Disable All';
    btn.disabled = false;
  }
}

async function saveConfig() {
  const btn = $('saveConfigBtn');
  const origText = btn.textContent;
  btn.textContent = 'Saving...';
  btn.disabled = true;

  try {
    const r = await post('/agent-config', {
      base_url: $('baseUrl').value,
      api_path: $('apiPath').value,
      device_id: ($('deviceId')?.value || '').trim()
    });
    toast(r.success ? 'Saved successfully' : 'Failed: ' + (r.error || 'Unknown error'));
  } catch (e) {
    toast('Error: ' + e.message);
  } finally {
    btn.textContent = origText;
    btn.disabled = false;
  }
}

async function clearLog() {
  if (currentLogTab === 'live') {
    await fetch('/clear-monitor');
    lastMonitorRaw = '';
    const mon = $('monitor');
    if (mon) renderLogViewport(mon, '', false);
    const mc = $('monitorCount');
    if (mc) mc.textContent = '0';
  } else {
    await fetch('/clear-error-log');
    const el = $('errorLog');
    if (el) renderLogViewport(el, '', true);
    const ec = $('errorLogCount');
    if (ec) ec.textContent = '0';
  }
  toast('Log cleared');
}

// Auth functions
let loginClientCooldownUntil = 0;
let loginClientFailStreak = 0;
const LOGIN_UI_GRACE_MS = 30000;
let dashboardGraceUntil = 0;
let loginWifiUiGraceUntil = 0;

function showLoginGraceBanner(on, secondsLeft) {
  const box = $('loginGraceBanner');
  const text = $('loginGraceText');
  if (!box) return;
  if (on) {
    const sec = secondsLeft != null ? secondsLeft : Math.ceil((dashboardGraceUntil - Date.now()) / 1000);
    if (text) {
      text.textContent = 'Signed in — STA WiFi info below for ' + Math.max(0, sec) + 's (AP page stays open).';
    }
    box.classList.remove('hide');
  } else {
    box.classList.add('hide');
  }
}

function openDashboardNow() {
  dashboardGraceUntil = 0;
  loginWifiUiGraceUntil = 0;
  showLoginGraceBanner(false);
  checkAuthStatus();
}

async function doLogin() {
  const email = ($('accountLoginEmail')?.value || $('loginEmail')?.value || '').trim();
  const password = ($('accountLoginPassword')?.value || $('loginPassword')?.value || '');
  
  if (!email || !password) {
    toast('Enter email and password');
    return;
  }

  const now = Date.now();
  if (now < loginClientCooldownUntil) {
    const msg = 'Too many attempts — wait 1 minute before login again';
    showLoginError(msg);
    return;
  }
  
  const loginBtn = $('accountLoginBtn') || $('loginBtn');
  if (loginBtn) {
    loginBtn.innerHTML = '<span class="spinner"></span> Signing in...';
    loginBtn.disabled = true;
  }
  
  try {
    // Send as JSON, not URLSearchParams
    const res = await fetch('/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email, password })
    });
    const r = await res.json();
    
    if (r.success) {
      loginClientFailStreak = 0;
      loginClientCooldownUntil = 0;
      showLoginError('');
      toast('Signed in successfully');
      setTimeout(() => {
        refreshStatus();
        checkAuthStatus();
      }, 600);
    } else {
      const msg = r.error || 'Login failed';
      showLoginError(msg);
      loginClientFailStreak++;
      if (loginClientFailStreak >= 3) {
        loginClientFailStreak = 0;
        loginClientCooldownUntil = Date.now() + 60000;
      }
    }
  } catch(e) {
    const msg = 'Cannot reach gateway — stay on the device AP or gateway WiFi IP';
    showLoginError(msg);
    console.error('Login error:', e);
  }
  
  if (loginBtn) {
    loginBtn.textContent = 'Sign in';
    const waitMs = Math.max(0, loginClientCooldownUntil - Date.now());
    if (waitMs > 0) {
      loginBtn.disabled = true;
      setTimeout(() => { loginBtn.disabled = false; }, waitMs);
    } else {
      loginBtn.disabled = false;
    }
  }
}

function showLoginError(msg) {
  const boxes = [
    { box: $('accountLoginError'), text: $('accountLoginErrorText') },
    { box: $('loginError'), text: $('loginErrorText') }
  ];
  boxes.forEach(({ box, text }) => {
    if (!box || !text) return;
    if (msg) {
      text.textContent = msg;
      box.classList.remove('hide');
    } else {
      box.classList.add('hide');
    }
  });
}

async function doLogout() {
  const r = await get('/logout');
  toast('Logged out');
  refreshStatus();
  checkAuthStatus();
}

async function doRefreshToken() {
  $('refreshTokenBtn').innerHTML = '<span class="spinner"></span>';
  $('refreshTokenBtn').disabled = true;
  
  const r = await post('/refresh-token', {});
  toast(r.success ? 'Token refreshed' : r.error);
  
  $('refreshTokenBtn').textContent = 'Refresh Token';
  $('refreshTokenBtn').disabled = false;
  refreshStatus();
}

async function doRegisterDevice() {
  $('registerDeviceBtn').innerHTML = '<span class="spinner"></span>';
  $('registerDeviceBtn').disabled = true;
  
  const r = await post('/register-device', {});
  toast(r.success ? 'Device registered' : r.error);
  
  $('registerDeviceBtn').textContent = 'Register Device';
  $('registerDeviceBtn').disabled = false;
  checkAuthStatus();
}

async function registerSimSlot(idx) {
  const btn = $('regSimBtn' + idx);
  if (btn) {
    btn.innerHTML = '<span class="spinner"></span>';
    btn.disabled = true;
  }
  try {
    const r = await post('/register-sim', { slot: idx });
    toast(r.success ? ('SIM ' + idx + ' registered') : (r.error || 'Register failed'));
  } catch(e) {
    toast('Register failed');
  }
  if (btn) {
    btn.textContent = 'Register';
    btn.disabled = false;
  }
  await refreshSims();
  checkAuthStatus();
}

async function checkAuthStatus() {
  try {
    const s = await get('/status');
    const hasToken = s.signed_in === true || (s.bearer_token && s.bearer_token.length > 0);

    $('loginPage')?.classList.add('hide');
    $('dashboardPage')?.classList.remove('hide');

    const offlineBadge = $('offlineModeBadge');
    const accountNavBadge = $('accountNavBadge');
    if (offlineBadge) offlineBadge.classList.toggle('hide', hasToken);
    if (accountNavBadge) accountNavBadge.style.display = hasToken ? 'none' : 'inline-flex';

    $('accountLoginCard')?.classList.toggle('hide', hasToken);
    $('accountSignedInCard')?.classList.toggle('hide', !hasToken);
    $('authSignedOutView')?.classList.toggle('hide', hasToken);
    $('authSignedInView')?.classList.toggle('hide', !hasToken);

    const authBadge = $('authBadge');
    const accountModeBadge = $('accountModeBadge');
    if (hasToken) {
      if (authBadge) { authBadge.className = 'badge success'; authBadge.textContent = 'Online'; }
      if (accountModeBadge) { accountModeBadge.className = 'badge success'; accountModeBadge.textContent = 'Signed in'; }
    } else {
      if (authBadge) { authBadge.className = 'badge warning'; authBadge.textContent = 'Offline'; }
      if (accountModeBadge) { accountModeBadge.className = 'badge warning'; accountModeBadge.textContent = 'Not signed in'; }
    }

    const setDeviceBadge = (id) => {
      const el = $(id);
      if (!el) return;
      el.textContent = s.device_registered ? 'Registered' : 'Not Registered';
      el.className = s.device_registered ? 'badge success' : 'badge warning';
    };
    setDeviceBadge('deviceBadge');
    setDeviceBadge('accountDeviceBadge');

    updateGatewayServicesUi(s);
    updateScheduledRestartNav(s);

    try {
      const simConfig = await get('/sim-config');
      const regCount = simConfig.backend_registered ? simConfig.backend_registered.filter(Boolean).length : 0;
      const regText = regCount + ' / ' + simConfig.enabled.length;
      if ($('simsRegCount')) $('simsRegCount').textContent = regText;
      if ($('accountSimsRegCount')) $('accountSimsRegCount').textContent = regText;
    } catch (e) {}
  } catch (e) {
    console.error('Auth check error:', e);
  }
}

// Event listeners
function addClick(id, fn) { const el = $(id); if (el) el.onclick = fn; }

addClick('scanBtn', scanNetworks);
addClick('connectBtn', connectWifi);
addClick('loginScanBtn', scanNetworksLogin);
addClick('loginConnectBtn', connectWifiLogin);
addClick('disconnectBtn', disconnectWifi);
addClick('loginDisconnectBtn', disconnectWifi);
addClick('copyStaUrlBtn', copyStaUrl);
addClick('copyLoginStaUrlBtn', copyStaUrl);
addClick('checkFirmwareBtn', checkFirmwareUpdate);
addClick('installFirmwareBtn', installFirmwareUpdate);
addClick('saveOtaUrlBtn', saveOtaUrl);
addClick('toggleMissedCallBtn', toggleMissedCallForward);
addClick('pingHostBtn', runNetworkPing);
const pingHostInput = $('pingHost');
if (pingHostInput) {
  pingHostInput.addEventListener('input', () => { pingHostInput.dataset.userEdited = '1'; });
}
document.querySelectorAll('.ping-preset').forEach((btn) => {
  btn.addEventListener('click', () => {
    const h = btn.getAttribute('data-host');
    if (pingHostInput && h) {
      pingHostInput.value = h;
      pingHostInput.dataset.userEdited = '1';
    }
    runNetworkPing();
  });
});
async function runNetworkPing() {
  const host = ($('pingHost')?.value || '').trim();
  const btn = $('pingHostBtn');
  const out = $('pingResult');
  const s = window.lastStatus || {};
  if (!s.sta_connected) {
    if (out) {
      out.textContent = 'WiFi is not connected.\nConnect STA in Settings first.';
      out.style.color = 'var(--danger)';
    }
    toast('WiFi not connected');
    return;
  }
  if (btn) { btn.disabled = true; btn.textContent = 'Pinging…'; }
  if (out) { out.textContent = 'Pinging ' + (host || 'google.com') + '…'; out.style.color = ''; }
  try {
    const r = await post('/network-ping', { host: host || 'google.com' }, 20000);
    if (out) {
      out.textContent = r.output || r.summary || (r.success ? 'OK' : 'Failed');
      out.style.color = r.success ? 'var(--success)' : 'var(--danger)';
    }
    toast(r.summary || (r.success ? 'Ping OK' : 'Ping failed'));
  } catch (e) {
    if (out) {
      out.textContent = 'Ping failed: ' + (e.message || e);
      out.style.color = 'var(--danger)';
    }
    toast('Ping failed');
  }
  if (btn) { btn.disabled = false; btn.textContent = 'Ping'; }
}

addClick('modemRunBtn', startModemGateway);
addClick('modemStopBtn', stopModemGatewayUi);
addClick('loginModemRunBtn', startModemGateway);
addClick('checkAllBtn', checkAllSims);
addClick('ussdBulkBtn', bulkCheckUssd);
addClick('disableAllSimsBtn', disableAllSims);
addClick('pausePollingBtn', togglePollingPause);
addClick('pauseHeartbeatBtn', toggleHeartbeatPause);
addClick('saveConfigBtn', saveConfig);
addClick('clearLogBtn', clearLog);
addClick('clearErrorLogBtn', function() { currentLogTab = 'errors'; clearLog(); });
addClick('resetDeviceBtn', resetDevice);
addClick('logoutBtn', doLogout);
addClick('registerDeviceBtn', doRegisterDevice);
addClick('refreshSmsBtn', refreshSmsList);
addClick('clearSmsBtn', clearMessages);

// Event listeners - Login page
addClick('loginBtn', doLogin);
addClick('accountLoginBtn', doLogin);
addClick('loginGraceContinueBtn', openDashboardNow);
addClick('accountLogoutBtn', doLogout);
addClick('accountRegisterDeviceBtn', doRegisterDevice);
addClick('accountRefreshTokenBtn', doRefreshToken);

function getTabFromHash() {
  const hash = window.location.hash.slice(1);
  return ['sims', 'messages', 'account', 'settings', 'logs'].includes(hash) ? hash : 'sims';
}

function updateHash(tab) {
  history.replaceState(null, '', '#' + tab);
}

// Override switchTab to update hash
const originalSwitchTab = switchTab;
switchTab = function(tab) {
  originalSwitchTab(tab);
  updateHash(tab);
};

// Init
const initialTab = getTabFromHash();
if (initialTab !== 'sims') {
  switchTab(initialTab);
}

refreshStatus();
refreshSims();
checkAuthStatus();
refreshSmsList();
startSessionsCountdown();
updateLogFooterMeta();
statusInterval = setInterval(refreshMonitor, 2000);
statusSlowInterval = setInterval(refreshStatus, 10000);
setTimeout(refreshMonitor, 300);
</script>
</body>
</html>
)=====";

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

// Forward declarations
void handleReset();

void initWebUI() {
    // Register routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/monitor", HTTP_GET, handleMonitor);
    server.on("/messages", HTTP_GET, handleMessages);
    server.on("/clear-messages", HTTP_GET, handleClearMessages);
    server.on("/clear-monitor", HTTP_GET, handleClearMonitor);
    server.on("/error-log", HTTP_GET, handleErrorLog);
    server.on("/clear-error-log", HTTP_GET, handleClearErrorLog);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/save-wifi", HTTP_POST, handleSaveWifi);
    server.on("/disconnect", HTTP_GET, handleDisconnect);
    server.on("/modem-start", HTTP_POST, handleModemStart);
    server.on("/modem-stop", HTTP_POST, handleModemStop);
    server.on("/network-ping", HTTP_POST, handleNetworkPing);
    server.on("/sim-config", HTTP_GET, handleSimConfig);
    server.on("/check-sim", HTTP_GET, handleCheckSim);
    server.on("/check-all-sim", HTTP_GET, handleCheckAllSim);
    server.on("/ussd-check", HTTP_POST, handleUssdCheck);
    server.on("/ussd-manual-status", HTTP_GET, handleUssdManualStatus);
    server.on("/ussd-bulk", HTTP_POST, handleUssdBulk);
    server.on("/ussd-bulk-status", HTTP_GET, handleUssdBulkStatus);
    server.on("/sim-enable", HTTP_POST, handleSimEnable);
    server.on("/sim-disable-all", HTTP_POST, handleSimDisableAll);
    server.on("/calls", HTTP_GET, handleCalls);
    server.on("/clear-calls", HTTP_GET, handleClearCalls);
    server.on("/call", HTTP_POST, handleCall);
    server.on("/hangup", HTTP_POST, handleHangup);
    server.on("/send-sms", HTTP_POST, handleSendSms);
    server.on("/agent-config", HTTP_POST, handleAgentConfig);
    server.on("/login", HTTP_POST, handleLogin);
    server.on("/logout", HTTP_GET, handleLogout);
    server.on("/refresh-token", HTTP_POST, handleRefreshToken);
    server.on("/register-device", HTTP_POST, handleRegisterDevice);
    server.on("/register-sim", HTTP_POST, handleRegisterSim);
    server.on("/heartbeat", HTTP_POST, handleHeartbeatManual);
    server.on("/toggle-polling", HTTP_POST, handleTogglePolling);
    server.on("/toggle-heartbeat", HTTP_POST, handleToggleHeartbeat);
    server.on("/toggle-missed-call", HTTP_POST, handleToggleMissedCall);
    server.on("/firmware-check", HTTP_GET, handleFirmwareCheck);
    server.on("/firmware-update", HTTP_POST, handleFirmwareUpdate);
    server.on("/firmware-config", HTTP_POST, handleFirmwareConfig);
    server.on("/reset", HTTP_POST, handleReset);
    
    // Start server
    server.begin();
    logMsg("[WEB] Server started on port 80");
    appendMonitorLog("[WEB] Server started");
}

void handleWebRequests() {
    server.handleClient();
}

// -----------------------------------------------------------------------------
// Route Handlers - Status
// -----------------------------------------------------------------------------

void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
    char buf[2048];
    buildStatusJson(buf, sizeof(buf));
    server.send(200, "application/json", buf);
}

extern void pruneExpiredActiveSessions();

void buildStatusJson(char* buf, size_t bufSize) {
    pruneExpiredActiveSessions();

    // Get WiFi status
    bool staConnected = WiFi.isConnected();
    IPAddress staIp = WiFi.localIP();
    IPAddress staGw = WiFi.gatewayIP();
    IPAddress staDns = WiFi.dnsIP();
    IPAddress staMask = WiFi.subnetMask();
    const bool staInternetReady = staConnected && wifiStaNetworkLooksValid();
    IPAddress apIp = WiFi.softAPIP();
    
    // Get uptime
    unsigned long uptimeS = millis() / 1000;
    
    // Count pending SMS
    int pendingSms = getPendingSmsCount();
    
    // Check if bearer token is set (don't expose full token)
    const bool signedIn = agentIsSignedIn();
    bool hasBearerToken = signedIn;
    
    // Find lowest battery among responsive SIMs
    int lowestBattery = 100;
    int lowestBatterySim = 0;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (simStates[i].responsive && simStates[i].batteryPercent > 0) {
            if (simStates[i].batteryPercent < lowestBattery) {
                lowestBattery = simStates[i].batteryPercent;
                lowestBatterySim = i + 1;
            }
        }
    }
    if (lowestBatterySim == 0) lowestBattery = batteryPercent;  // Fallback to global
    
    // Get STA SSID
    String staSsid = WiFi.SSID();
    
    // Build activeSessions array with countdown
    char sessionsBuf[768];
    sessionsBuf[0] = '\0';
    if (activeSessionCount > 0) {
        // Get current time from NTP (seconds since 1970)
        time_t nowSecs = time(nullptr);
        unsigned long nowMs = nowSecs * 1000L;

        strcpy(sessionsBuf, ",\"activeSessions\":[");
        for (int i = 0; i < activeSessionCount && i < 8; i++) {
            if (i > 0) strcat(sessionsBuf, ",");
            char sessObj[160];

            // Calculate remaining seconds
            long expiresIn = 0;
            if (activeSessions[i].expiresAtMs > nowMs) {
                expiresIn = (activeSessions[i].expiresAtMs - nowMs) / 1000L;
            }

            snprintf(sessObj, sizeof(sessObj),
                "{\"app\":\"%s\",\"slot\":%d,\"msgs\":%d,\"expiresIn\":%ld}",
                activeSessions[i].appName[0] ? activeSessions[i].appName : "-",
                activeSessions[i].slot,
                activeSessions[i].messageCount,
                expiresIn
            );
            strcat(sessionsBuf, sessObj);
        }
        strcat(sessionsBuf, "]");
    }

    const bool restartPending = maintenanceHasScheduledRestart();
    const long restartInSec = maintenanceGetScheduledRestartInSec();

    char fwRemote[32];
    bool fwUpdateAvail = false;
    unsigned long fwLastCheckMs = 0;
    maintenanceGetFirmwareCache(fwRemote, sizeof(fwRemote), &fwUpdateAvail, &fwLastCheckMs);
    const bool fwLocalAhead =
        fwRemote[0] && otaVersionIsNewer(FIRMWARE_VERSION, fwRemote);
    long fwLastCheckSecAgo = -1;
    if (fwLastCheckMs > 0) {
        fwLastCheckSecAgo = (long)((millis() - fwLastCheckMs) / 1000UL);
    }
    
    snprintf(buf, bufSize,
        "{"
        "\"sta_connected\":%s,"
        "\"sta_internet_ready\":%s,"
        "\"sta_ip\":\"%s\","
        "\"sta_gateway\":\"%s\","
        "\"sta_dns\":\"%s\","
        "\"sta_subnet\":\"%s\","
        "\"sta_ssid\":\"%s\","
        "\"ap_ip\":\"%s\","
        "\"uptime_s\":%lu,"
        "\"battery_percent\":%d,"
        "\"battery_mv\":%d,"
        "\"lowest_battery\":%d,"
        "\"lowest_battery_sim\":%d,"
        "\"device_registered\":%s,"
        "\"sim_registered\":%s,"
        "\"pending_sms\":%d,"
        "\"base_url\":\"%s\","
        "\"api_path\":\"%s\","
        "\"device_id\":\"%s\","
        "\"bearer_token\":\"%s\","
        "\"signed_in\":%s,"
        "\"offline_mode\":%s,"
        "\"modem_running\":%s,"
        "\"modem_start_queued\":%s,"
        "\"heartbeat_paused\":%s,"
        "\"sms_polling_paused\":%s,"
        "\"missed_call_forward\":%s,"
        "\"missed_call_watch_sim\":%d,"
        "\"firmware_version\":\"%s\","
        "\"firmware_remote_version\":\"%s\","
        "\"firmware_update_available\":%s,"
        "\"firmware_local_ahead\":%s,"
        "\"firmware_last_check_sec_ago\":%ld,"
        "\"firmware_check_interval_h\":12,"
        "\"firmware_auto_install\":false,"
        "\"ota_url\":\"%s\","
#if OTA_ENABLED && OTA_WEB_INSTALL_ENABLED
        "\"ota_install_enabled\":true,"
#else
        "\"ota_install_enabled\":false,"
#endif
        "\"scheduled_restart_pending\":%s,"
        "\"scheduled_restart_in_sec\":%ld"
        "%s"
        "}",
        staConnected ? "true" : "false",
        staInternetReady ? "true" : "false",
        staConnected ? staIp.toString().c_str() : "",
        staConnected ? staGw.toString().c_str() : "",
        staConnected ? staDns.toString().c_str() : "",
        staConnected ? staMask.toString().c_str() : "",
        staConnected ? staSsid.c_str() : "",
        apIp.toString().c_str(),
        uptimeS,
        batteryPercent,
        batteryMv,
        lowestBattery,
        lowestBatterySim,
        deviceRegistered ? "true" : "false",
        simRegistered ? "true" : "false",
        pendingSms,
        agentBaseUrl,
        agentApiPath,
        agentDeviceId,
        hasBearerToken ? "(set)" : "",
        signedIn ? "true" : "false",
        signedIn ? "false" : "true",
        isModemGatewayRunning() ? "true" : "false",
        isModemGatewayStartQueued() ? "true" : "false",
        heartbeatPaused ? "true" : "false",
        smsPollingPaused ? "true" : "false",
        missedCallForwardEnabled ? "true" : "false",
        getPriorityMissedCallSlot() + 1,
        FIRMWARE_VERSION,
        fwRemote,
        fwUpdateAvail ? "true" : "false",
        fwLocalAhead ? "true" : "false",
        fwLastCheckSecAgo,
        otaFirmwareUrl,
        restartPending ? "true" : "false",
        restartInSec,
        sessionsBuf
    );
}

void handleMonitor() {
    static char buf[2048];  // Reduced from 4096
    getMonitorLogText(buf, sizeof(buf));
    server.send(200, "text/plain", buf);
}

void handleMessages() {
    if (!webLittleFsReady()) {
        server.send(200, "text/plain", "");
        return;
    }

    FILE* file = fopen(WEB_MESSAGES_LOG_PATH, "r");
    if (!file) {
        server.send(200, "text/plain", "");
        return;
    }

    char line[280];
    int totalLines = 0;
    while (fgets(line, sizeof(line), file)) {
        if (line[0] != '\0') {
            totalLines++;
        }
    }

    const int maxMessages = 50;
    const int skip = (totalLines > maxMessages) ? (totalLines - maxMessages) : 0;

    rewind(file);
    for (int i = 0; i < skip; i++) {
        if (!fgets(line, sizeof(line), file)) {
            fclose(file);
            server.send(200, "text/plain", "");
            return;
        }
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain", "");
    while (fgets(line, sizeof(line), file)) {
        server.sendContent(line);
    }
    server.sendContent("");
    fclose(file);
}

void handleClearMessages() {
    extern void clearMessagesLog();
    clearMessagesLog();
    sendJsonSuccess("Messages cleared");
}

void handleClearMonitor() {
    clearMonitorLog();
    server.send(200, "application/json", "{\"success\":true}");
}

void handleErrorLog() {
    static char buf[2048];  // Reduced from 4096
    readErrorsFromFile(buf, sizeof(buf), 30);  // Last 30 errors
    server.send(200, "text/plain", buf);
}

void handleClearErrorLog() {
    clearErrorLog();
    server.send(200, "application/json", "{\"success\":true}");
}

// -----------------------------------------------------------------------------
// Route Handlers - WiFi
// -----------------------------------------------------------------------------

void handleScan() {
    wifiPrepareForScan();
    WiFi.scanDelete();
    delay(80);

    appendMonitorLog("[WIFI] Scan start");
    const int n = WiFi.scanNetworks(false);
    if (n == WIFI_SCAN_FAILED) {
        logMsg("[WIFI] Scan failed");
        appendMonitorLog("[WIFI] Scan failed");
        server.send(200, "application/json", "[]");
        return;
    }
    {
        char line[32];
        snprintf(line, sizeof(line), "[WIFI] Scan %d nets", n);
        appendMonitorLog(line);
    }
    
    static char buf[2048];  // Static to avoid stack overflow
    size_t pos = 0;
    
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[");
    
    for (int i = 0; i < n && pos < sizeof(buf) - 100; i++) {
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        
        char ssid[64];
        charBufSet(ssid, sizeof(ssid), WiFi.SSID(i).c_str());
        
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
            ssid,
            WiFi.RSSI(i),
            (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false"
        );
    }
    
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]");
    
    server.send(200, "application/json", buf);
}

void handleSaveWifi() {
    if (!server.hasArg("ssid")) {
        sendJsonError("Missing SSID");
        return;
    }
    
    charBufSet(wifiSsid, sizeof(wifiSsid), server.arg("ssid").c_str());
    charBufSet(wifiPassword, sizeof(wifiPassword), server.arg("password").c_str());
    
    // Save to preferences
    preferences.begin("wifi", false);
    preferences.putString("ssid", wifiSsid);
    preferences.putString("pw", wifiPassword);
    preferences.end();
    
    armWifiUserSetupMs(45000);
    WiFi.setAutoReconnect(false);
    ensureGatewaySoftAp();

    logMsg2Val("[WIFI] Saved", wifiSsid, "connecting", "...");
    appendMonitorLogVal("[WIFI] Connect ", wifiSsid);
    sendJsonSuccess("WiFi saved, connecting...");

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true, false);
        delay(150);
    }
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.begin(wifiSsid, wifiPassword);
}

void handleDisconnect() {
    armWifiUserSetupMs(WIFI_USER_SETUP_ARM_MS);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true, false);
    delay(100);
    ensureGatewaySoftAp();
    logMsg("[WIFI] Disconnected");
    appendMonitorLog("[WIFI] Disconnected");
    sendJsonSuccess("Disconnected");
}

void handleModemStart() {
    if (isModemGatewayRunning()) {
        sendJsonSuccess("Gateway already running");
        return;
    }
    if (isModemGatewayStartQueued()) {
        sendJsonSuccess("Start already queued");
        return;
    }
    requestModemGatewayStart();
    sendJsonSuccess("SIM init queued — watch Logs (may take 1–2 min)");
}

void handleModemStop() {
    stopModemGateway();
    sendJsonSuccess("Modem tasks stopped");
}

static void extractPingHostname(const char* in, char* hostOut, size_t hostOutSize) {
    if (!hostOut || hostOutSize < 2) return;
    hostOut[0] = '\0';
    if (!in || !in[0]) return;

    const char* p = in;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && i < hostOutSize - 1) {
        hostOut[i] = p[i];
        i++;
    }
    hostOut[i] = '\0';
}

static volatile bool networkPingBusy = false;

void handleNetworkPing() {
    if (networkPingBusy) {
        sendJsonError("Ping already in progress", 409);
        return;
    }
    if (httpsBusy) {
        sendJsonError("HTTPS busy — wait for heartbeat/login to finish");
        return;
    }

    char host[96];
    if (server.hasArg("host")) {
        extractPingHostname(server.arg("host").c_str(), host, sizeof(host));
    } else {
        host[0] = '\0';
    }
    if (charBufIsEmpty(host)) {
        charBufSet(host, sizeof(host), "google.com");
    }

    if (!WiFi.isConnected()) {
        server.send(200, "application/json",
            "{\"success\":false,\"host\":\"\",\"summary\":\"WiFi not connected\","
            "\"output\":\"WiFi is not connected.\\nConnect STA in Settings first.\"}");
        return;
    }

    wifiFixStaNetworkIfNeeded();

    networkPingBusy = true;
    const bool ok = gatewayPingRunBlocking(host, 25000);
    networkPingBusy = false;

    const char* summary = gatewayPingGetSummary();
    const char* output = gatewayPingGetOutput();
    if (!summary[0]) {
        summary = ok ? "OK" : "Ping failed or timed out";
    }

    // Escape into the tail of webJsonBuf; build JSON at the front (no overlap).
    constexpr size_t kPingJsonMax = 3584;
    constexpr size_t kPingEscOutputMax = 1024;
    constexpr size_t kPingEscSummaryMax = 256;
    constexpr size_t kPingEscHostMax = 128;
    static_assert(kPingJsonMax + kPingEscOutputMax + kPingEscSummaryMax + kPingEscHostMax <= sizeof(webJsonBuf),
        "webJsonBuf too small for ping JSON");

    char* escHost = webJsonBuf + kPingJsonMax + kPingEscOutputMax + kPingEscSummaryMax;
    char* escSummary = webJsonBuf + kPingJsonMax + kPingEscOutputMax;
    char* escOutput = webJsonBuf + kPingJsonMax;
    char* jsonOut = webJsonBuf;

    jsonEscapeNoQuotes(host, escHost, kPingEscHostMax);
    jsonEscapeNoQuotes(summary, escSummary, kPingEscSummaryMax);
    jsonEscapeNoQuotes(output, escOutput, kPingEscOutputMax);

    snprintf(jsonOut, kPingJsonMax,
        "{\"success\":%s,\"host\":\"%s\",\"summary\":\"%s\",\"output\":\"%s\","
        "\"sta_ip\":\"%s\",\"sta_gateway\":\"%s\",\"sta_dns\":\"%s\"}",
        ok ? "true" : "false",
        escHost,
        escSummary,
        escOutput,
        WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        WiFi.dnsIP().toString().c_str());
    server.send(200, "application/json", jsonOut);
}

// -----------------------------------------------------------------------------
// Route Handlers - SIM Management
// -----------------------------------------------------------------------------

static bool jsonAppendFmt(char* buf, size_t bufSize, size_t* pos, const char* fmt, ...) {
    if (!buf || !pos || *pos >= bufSize) return false;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, bufSize - *pos, fmt, args);
    va_end(args);
    if (n < 0) return false;
    if ((size_t)n >= bufSize - *pos) {
        *pos = bufSize - 1;
        buf[*pos] = '\0';
        return false;
    }
    *pos += (size_t)n;
    return true;
}

static void extractOperatorShort(const char* cops, char* out, size_t outSize) {
    if (!out || outSize < 1) return;
    out[0] = '\0';
    if (!cops || !cops[0]) return;

    const char* q1 = strchr(cops, '"');
    if (q1) {
        q1++;
        const char* q2 = strchr(q1, '"');
        if (q2 && q2 > q1) {
            size_t len = (size_t)(q2 - q1);
            if (len >= outSize) len = outSize - 1;
            strncpy(out, q1, len);
            out[len] = '\0';
            return;
        }
    }

    size_t j = 0;
    for (size_t k = 0; cops[k] && j < outSize - 1; k++) {
        char c = cops[k];
        if (c == '\r' || c == '\n') break;
        if ((unsigned char)c >= 0x20) out[j++] = c;
    }
    out[j] = '\0';
}

void handleSimConfig() {
    buildSimConfigJson(webJsonBuf, sizeof(webJsonBuf));
    server.send(200, "application/json", webJsonBuf);
}

void buildSimConfigJson(char* buf, size_t bufSize) {
    if (!buf || bufSize < 64) return;
    size_t pos = 0;
    buf[0] = '\0';

    const int muxSlot =
#if !USE_DUAL_UART
        logicalSlotToMuxChannel(getCurrentLogicalSlot());
#else
        getCurrentLogicalSlot();
#endif

    if (!jsonAppendFmt(buf, bufSize, &pos,
            "{\"success\":true,\"active_slot\":%d,\"selected_mux_slot\":%d,\"enabled\":[",
            getCurrentLogicalSlot(), muxSlot)) {
        snprintf(buf, bufSize, "{\"success\":false,\"error\":\"JSON buffer full\"}");
        return;
    }

    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%s",
                i > 0 ? "," : "", simStates[i].enabled ? "true" : "false")) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"user_disabled\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%s",
                i > 0 ? "," : "", simStates[i].userDisabled ? "true" : "false")) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"responsive\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%s",
                i > 0 ? "," : "", simStates[i].responsive ? "true" : "false")) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"registered\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%s",
                i > 0 ? "," : "", simStates[i].registered ? "true" : "false")) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"backend_registered\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%s",
                i > 0 ? "," : "", simStates[i].backendRegistered ? "true" : "false")) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"numbers\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        char escaped[48];
        jsonEscape(simStates[i].number, escaped, sizeof(escaped));
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%s", i > 0 ? "," : "", escaped)) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"operators\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        char op[24];
        extractOperatorShort(simStates[i].cops, op, sizeof(op));
        char escaped[48];
        jsonEscape(op, escaped, sizeof(escaped));
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%s", i > 0 ? "," : "", escaped)) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"csq_rssi\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        int rssi = simStates[i].signalStrength;
        if (rssi < 0) rssi = extractSignalQuality(simStates[i].csq);
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%d", i > 0 ? "," : "", rssi)) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"battery_pct\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%d", i > 0 ? "," : "", simStates[i].batteryPercent)) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"battery_mv\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%d", i > 0 ? "," : "", simStates[i].batteryMv)) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"ussd_status\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%d", i > 0 ? "," : "", (int)simStates[i].ussdStatus)) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"ussd_result\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        char escaped[64];
        jsonEscape(simStates[i].ussdLastResult, escaped, sizeof(escaped));
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%s", i > 0 ? "," : "", escaped)) {
            goto json_fail;
        }
    }

    if (!jsonAppendFmt(buf, bufSize, &pos, "],\"ussd_duration_sec\":[")) goto json_fail;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (!jsonAppendFmt(buf, bufSize, &pos, "%s%u", i > 0 ? "," : "",
                (unsigned)simStates[i].ussdLastDurationSec)) {
            goto json_fail;
        }
    }

    jsonAppendFmt(buf, bufSize, &pos, "],\"ussd_bulk_active\":%s,\"ussd_bulk_current\":%d}",
        ussdBulkInProgress() ? "true" : "false",
        ussdBulkInProgress() ? ussdBulkCurrentSlot() : -1);
    return;

json_fail:
    snprintf(buf, bufSize, "{\"success\":false,\"error\":\"JSON buffer full\"}");
}

void handleCheckSim() {
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : 0;
    int simIdx = slot;
    
    if (simIdx < 0 || simIdx >= SIM_COUNT) {
        sendJsonError("Invalid SIM slot");
        return;
    }
    
    if (!simStates[simIdx].enabled) {
        sendJsonError("SIM slot disabled");
        return;
    }
    
    setSimBusy(true);
    selectSIM(simIdx);
    
    sendATCapture("AT", 400);
    sendATCapture("AT+CMGF=1", 600);
    
    char creg[64], cops[64], csq[32];
    sendATCapture("AT+CREG?", 900);
    charBufSet(creg, sizeof(creg), getSimBuffer());
    sendATCapture("AT+COPS?", 900);
    charBufSet(cops, sizeof(cops), getSimBuffer());
    sendATCapture("AT+CSQ", 900);
    charBufSet(csq, sizeof(csq), getSimBuffer());
    
    // Update state
    charBufSet(simStates[simIdx].creg, sizeof(simStates[simIdx].creg), creg);
    charBufSet(simStates[simIdx].cops, sizeof(simStates[simIdx].cops), cops);
    charBufSet(simStates[simIdx].csq, sizeof(simStates[simIdx].csq), csq);
    
    setSimBusy(false);
    
    char buf[512];
    char cregEsc[96], copsEsc[96], csqEsc[48];
    jsonEscape(creg, cregEsc, sizeof(cregEsc));
    jsonEscape(cops, copsEsc, sizeof(copsEsc));
    jsonEscape(csq, csqEsc, sizeof(csqEsc));
    
    snprintf(buf, sizeof(buf),
        "{\"success\":true,\"slot\":%d,\"creg\":%s,\"cops\":%s,\"csq\":%s}",
        simIdx, cregEsc, copsEsc, csqEsc
    );
    
    server.send(200, "application/json", buf);
}

void handleUssdCheck() {
    if (ussdBulkInProgress()) {
        sendJsonError("Bulk *143# in progress");
        return;
    }
    if (ussdManualInProgress()) {
        sendJsonError("USSD check already running");
        return;
    }

    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : 0;
    if (slot < 0 || slot >= SIM_COUNT) {
        sendJsonError("Invalid SIM slot");
        return;
    }

    if (charBufIsEmpty(simStates[slot].number)) {
        sendJsonError("No SIM number on this slot");
        return;
    }

    if (!ussdStartManual(slot)) {
        sendJsonError("Could not start USSD check");
        return;
    }

    char buf[96];
    snprintf(buf, sizeof(buf),
        "{\"success\":true,\"started\":true,\"slot\":%d,\"sim\":%d}",
        slot, slot + 1);
    server.send(200, "application/json", buf);
}

void handleUssdManualStatus() {
    char buf[512];
    ussdWriteManualStatusJson(buf, sizeof(buf));
    server.send(200, "application/json", buf);
}

void handleUssdBulk() {
    if (refreshAllInProgress) {
        sendJsonError("Refresh in progress");
        return;
    }
    if (ussdManualInProgress()) {
        sendJsonError("Manual USSD check in progress");
        return;
    }
    if (ussdBulkInProgress()) {
        sendJsonError("Bulk *143# already running");
        return;
    }

    ussdStartBulk();

    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"success\":true,\"message\":\"Started\",\"total\":%d}", ussdBulkTotalCount());
    server.send(200, "application/json", buf);
}

void handleUssdBulkStatus() {
    char buf[4096];
    ussdWriteBulkStatusJson(buf, sizeof(buf));
    server.send(200, "application/json", buf);
}

void handleCheckAllSim() {
    if (refreshAllInProgress) {
        sendJsonError("Refresh already in progress");
        return;
    }

    refreshAllInProgress = true;
    logMsg("[SIM] Starting refresh all...");
    appendMonitorLog("[SIM] Refresh all start");
    setSimBusy(true);
    pauseSmsPolling(30000);  // Pause SMS polling during refresh
    setSimYieldToWebServer(false);
    
    // Use static buffer to avoid stack overflow
    static char buf[5120];  // Need enough for 16 SIMs (~200 bytes each)
    size_t pos = 0;
    
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"success\":true,\"sims\":[");
    
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"slot\":%d,\"enabled\":%s",
            i,
            simStates[i].enabled ? "true" : "false"
        );
        
        if (simStates[i].enabled) {
            logMsgInt("[SIM] Checking slot", i);
            selectSIM(i);
            
            sendATCapture("AT", 500);
            const char* atResp = getSimBuffer();
            logMsg2Val("[SIM] AT response", atResp, "", "");
            
            // Check if SIM is responsive
            bool isResponsive = (strstr(atResp, "OK") != NULL);
            simStates[i].responsive = isResponsive;
            
            if (!isResponsive) {
                logMsg("[SIM] Not responsive, skipping");
                pos += snprintf(buf + pos, sizeof(buf) - pos, ",\"responsive\":false}");
                continue;
            }
            
            sendATCapture("AT+CSQ", 3000);  // Increased timeout for CSQ
            charBufSet(simStates[i].csq, sizeof(simStates[i].csq), getSimBuffer());
            logMsg2Val("[SIM] CSQ", simStates[i].csq, "", "");
            
            sendATCapture("AT+CREG?", 1500);
            charBufSet(simStates[i].creg, sizeof(simStates[i].creg), getSimBuffer());
            logMsg2Val("[SIM] CREG", simStates[i].creg, "", "");
            
            // Check if registered on network (CREG: 0,1 or 0,5 means registered)
            bool isRegistered = (strstr(simStates[i].creg, "0,1") != NULL || 
                                 strstr(simStates[i].creg, "0,5") != NULL);
            simStates[i].registered = isRegistered;
            
            sendATCapture("AT+COPS?", 1500);
            charBufSet(simStates[i].cops, sizeof(simStates[i].cops), getSimBuffer());
            logMsg2Val("[SIM] COPS", simStates[i].cops, "", "");
            
            // Get phone number
            sendATCapture("AT+CNUM", 1500);
            char numBuf[64];
            charBufSet(numBuf, sizeof(numBuf), getSimBuffer());
            // Parse +CNUM: ,"number",...
            const char* numStart = strstr(numBuf, ",\"");
            if (numStart) {
                numStart += 2;
                const char* numEnd = strchr(numStart, '"');
                if (numEnd) {
                    int len = numEnd - numStart;
                    if (len > 0 && len < (int)sizeof(simStates[i].number)) {
                        strncpy(simStates[i].number, numStart, len);
                        simStates[i].number[len] = '\0';
                    }
                }
            }
            logMsg2Val("[SIM] Number", simStates[i].number, "", "");
            
            // Get battery info
            getBatteryInfo(&simStates[i].batteryPercent, &simStates[i].batteryMv);
            logMsgInt("[SIM] Battery %:", simStates[i].batteryPercent);
            
            char csqEsc[48], cregEsc[96], copsEsc[96];
            jsonEscape(simStates[i].csq, csqEsc, sizeof(csqEsc));
            jsonEscape(simStates[i].creg, cregEsc, sizeof(cregEsc));
            jsonEscape(simStates[i].cops, copsEsc, sizeof(copsEsc));
            
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                ",\"csq\":%s,\"creg\":%s,\"cops\":%s,\"registered\":%s,\"battery_pct\":%d,\"battery_mv\":%d",
                csqEsc, cregEsc, copsEsc,
                isRegistered ? "true" : "false",
                simStates[i].batteryPercent,
                simStates[i].batteryMv
            );
        }
        
        pos += snprintf(buf + pos, sizeof(buf) - pos, "}");
    }
    
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    
    // Check for buffer overflow
    if (pos >= sizeof(buf) - 1) {
        logMsg("[SIM] WARNING: CheckAllSim buffer overflow!");
        appendMonitorLog("[SIM] Buffer overflow!");
    }
    
    setSimYieldToWebServer(true);
    setSimBusy(false);
    resumeSmsPolling();  // Resume SMS polling
    refreshAllInProgress = false;
    logMsg("[SIM] Refresh all complete");
    appendMonitorLog("[SIM] Refresh all done");
    server.send(200, "application/json", buf);
}

extern void setSimUserDisabled(int simIdx, bool disabled);

static void disableSimSlot(int simIdx) {
    setSimUserDisabled(simIdx, true);
    simStates[simIdx].responsive = false;
    simStates[simIdx].registered = false;
    simStates[simIdx].backendRegistered = false;
    simStates[simIdx].basicInitDone = false;
    charBufClear(simStates[simIdx].creg, sizeof(simStates[simIdx].creg));
    charBufClear(simStates[simIdx].cops, sizeof(simStates[simIdx].cops));
    charBufClear(simStates[simIdx].csq, sizeof(simStates[simIdx].csq));
    simStates[simIdx].batteryPercent = 0;
    simStates[simIdx].batteryMv = 0;
}

void handleSimDisableAll() {
    if (isSimBusy()) {
        sendJsonError("SIM busy");
        return;
    }
    for (int i = 0; i < SIM_COUNT; i++) {
        disableSimSlot(i);
    }
    logMsg2Val("[SIM]", "all", "disabled", "");
    appendMonitorLog("[SIM] All slots disabled");
    char msg[64];
    snprintf(msg, sizeof(msg), "All %d SIM slots disabled", SIM_COUNT);
    sendJsonSuccess(msg);
}

void handleSimEnable() {
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : 0;
    int simIdx = slot;
    bool enabled = server.arg("enabled") == "1" || server.arg("enabled") == "true";
    
    if (simIdx < 0 || simIdx >= SIM_COUNT) {
        sendJsonError("Invalid SIM slot");
        return;
    }
    
    if (!enabled) {
        setSimUserDisabled(simIdx, true);
        simStates[simIdx].enabled = false;
        appendMonitorLogVal("[SIM] Manual OFF ", String(slot).c_str());
    } else {
        setSimUserDisabled(simIdx, false);
        // Only enable if SIM responds and can be initialized
        if (isSimBusy()) {
            sendJsonError("SIM busy");
            return;
        }

        pauseSmsPolling(30000);
        setSimYieldToWebServer(false);
        setSimBusy(true);
        selectSIM(simIdx);
        delay(500);  // Wait for mux to settle

        sendATCapture("AT", 1000);  // Longer timeout
        bool responsive = (strstr(getSimBuffer(), "OK") != NULL);
        if (!responsive) {
            // Try once more
            delay(500);
            sendATCapture("AT", 1000);
            responsive = (strstr(getSimBuffer(), "OK") != NULL);
        }
        if (!responsive) {
            setSimBusy(false);
            setSimYieldToWebServer(true);
            resumeSmsPolling();
            simStates[simIdx].enabled = false;
            simStates[simIdx].responsive = false;
            sendJsonError("SIM not responding");
            return;
        }

        // Basic init (SMS mode, call block, etc)
        bool ok = initModemForSMS();
        simStates[simIdx].enabled = ok;
        simStates[simIdx].responsive = true;
        simStates[simIdx].basicInitDone = ok;

        // Refresh SIM info for UI
        sendATCapture("AT+CSQ", 1500);
        charBufSet(simStates[simIdx].csq, sizeof(simStates[simIdx].csq), getSimBuffer());

        sendATCapture("AT+CREG?", 1500);
        charBufSet(simStates[simIdx].creg, sizeof(simStates[simIdx].creg), getSimBuffer());
        simStates[simIdx].registered = (strstr(simStates[simIdx].creg, "0,1") != NULL || strstr(simStates[simIdx].creg, "0,5") != NULL);

        sendATCapture("AT+COPS?", 1500);
        charBufSet(simStates[simIdx].cops, sizeof(simStates[simIdx].cops), getSimBuffer());

        sendATCapture("AT+CNUM", 1500);
        char numBuf[64];
        charBufSet(numBuf, sizeof(numBuf), getSimBuffer());
        const char* numStart = strstr(numBuf, ",\"");
        if (numStart) {
            numStart += 2;
            const char* numEnd = strchr(numStart, '"');
            if (numEnd) {
                int len = (int)(numEnd - numStart);
                if (len > 0 && len < (int)sizeof(simStates[simIdx].number)) {
                    strncpy(simStates[simIdx].number, numStart, (size_t)len);
                    simStates[simIdx].number[len] = '\0';
                }
            }
        }

        int pct = 0, mv = 0;
        getBatteryInfo(&pct, &mv);
        simStates[simIdx].batteryPercent = pct;
        simStates[simIdx].batteryMv = mv;

        setSimBusy(false);
        setSimYieldToWebServer(true);
        resumeSmsPolling();

        if (!ok) {
            simStates[simIdx].enabled = false;
            sendJsonError("SIM init failed");
            return;
        }
    }
    
    logMsg2Val("[SIM]", String(slot).c_str(), enabled ? "enabled" : "disabled", "");
    appendMonitorLogVal(enabled ? "[SIM] Enabled " : "[SIM] Disabled ", String(slot).c_str());
    sendJsonSuccess();
}

// -----------------------------------------------------------------------------
// Route Handlers - Calls
// -----------------------------------------------------------------------------

void handleCalls() {
    static char buf[2048];  // Static to avoid stack overflow
    buildCallLogJson(buf, sizeof(buf));
    server.send(200, "application/json", buf);
}

void buildCallLogJson(char* buf, size_t bufSize) {
    extern CallLogItem callLog[MAX_CALL_LOG];
    extern int callLogCount;
    
    size_t pos = 0;
    pos += snprintf(buf + pos, bufSize - pos, "{\"success\":true,\"data\":[");
    
    for (int i = 0; i < callLogCount && pos < bufSize - 200; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        
        char callerEsc[64], networkEsc[64], tsEsc[64];
        jsonEscape(callLog[i].caller, callerEsc, sizeof(callerEsc));
        jsonEscape(callLog[i].network, networkEsc, sizeof(networkEsc));
        jsonEscape(callLog[i].ts, tsEsc, sizeof(tsEsc));
        
        pos += snprintf(buf + pos, bufSize - pos,
            "{\"caller\":%s,\"network\":%s,\"ts\":%s,\"duration_ms\":%lu}",
            callerEsc, networkEsc, tsEsc, callLog[i].durationMs
        );
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "]}");
}

void handleClearCalls() {
    extern int callLogCount;
    callLogCount = 0;
    sendJsonSuccess();
}

void handleCall() {
    char number[32];
    charBufSet(number, sizeof(number), server.arg("number").c_str());
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : activeSim;
    int simIdx = slot;
    
    // Normalize number
    char normalized[32];
    normalizePhNumber(number, normalized, sizeof(normalized));
    
    if (simIdx < 0 || simIdx >= SIM_COUNT || !simStates[simIdx].enabled) {
        sendJsonError("Invalid or disabled SIM slot");
        return;
    }
    
    setSimBusy(true);
    selectSIM(simIdx);
    
    bool ok = dialNumber(normalized);
    
    setSimBusy(false);
    
    if (ok) {
        logMsg2Val("[CALL] Dialing", normalized, "slot", String(slot).c_str());
        sendJsonSuccess("Dialing...");
    } else {
        sendJsonError("Dial failed");
    }
}

void handleHangup() {
    hangupCall();
    logMsg("[CALL] Hangup");
    sendJsonSuccess();
}

// -----------------------------------------------------------------------------
// Route Handlers - SMS
// -----------------------------------------------------------------------------

void handleSendSms() {
    char number[32], message[SMS_MESSAGE_SIZE];
    charBufSet(number, sizeof(number), server.arg("number").c_str());
    charBufSet(message, sizeof(message), server.arg("message").c_str());
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : activeSim;
    int simIdx = slot;
    
    // Normalize number
    char normalized[32];
    normalizePhNumber(number, normalized, sizeof(normalized));
    
    if (simIdx < 0 || simIdx >= SIM_COUNT || !simStates[simIdx].enabled) {
        sendJsonError("Invalid or disabled SIM slot");
        return;
    }
    
    setSimBusy(true);
    selectSIM(simIdx);
    
    bool ok = sendSMS(normalized, message);
    
    setSimBusy(false);
    
    if (ok) {
        sendJsonSuccess("SMS sent");
    } else {
        sendJsonError("SMS failed");
    }
}

// -----------------------------------------------------------------------------
// Route Handlers - Agent Config
// -----------------------------------------------------------------------------

void handleAgentConfig() {
    if (server.hasArg("base_url")) {
        charBufSet(agentBaseUrl, sizeof(agentBaseUrl), server.arg("base_url").c_str());
    }
    if (server.hasArg("api_path")) {
        charBufSet(agentApiPath, sizeof(agentApiPath), server.arg("api_path").c_str());
    }
    if (server.hasArg("device_id")) {
        String devId = server.arg("device_id");
        devId.trim();
        if (devId.length() > 0) {
            charBufSet(agentDeviceId, sizeof(agentDeviceId), devId.c_str());
        } else if (charBufIsEmpty(agentDeviceId)) {
            snprintf(agentDeviceId, sizeof(agentDeviceId), "SIM800-%06X", (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));
        }
    }
    if (server.hasArg("bearer_token")) {
        charBufSet(agentBearerToken, sizeof(agentBearerToken), server.arg("bearer_token").c_str());
    }
    
    // Save to preferences
    preferences.begin("agent", false);
    preferences.putString("base", agentBaseUrl);
    preferences.putString("path", agentApiPath);
    preferences.putString("dev", agentDeviceId);
    if (server.hasArg("bearer_token")) {
        preferences.putString("tok", agentBearerToken);
    }
    preferences.end();
    
    logMsg("[CONFIG] Agent config saved");
    sendJsonSuccess();
}

// -----------------------------------------------------------------------------
// Route Handlers - Login/Auth
// -----------------------------------------------------------------------------

// Shared auth HTTP buffer (login + refresh) — one copy to save DRAM.
static char gAuthRespBuf[AGENT_AUTH_RESP_SIZE];

static unsigned long loginCooldownUntilMs = 0;
static unsigned long lastLoginAttemptMs = 0;
static uint8_t loginFailStreak = 0;

static void sendAuthLoginJsonError(const char* msg) {
    httpsBusy = false;
    char buf[280];
    char esc[200];
    jsonEscapeNoQuotes(msg ? msg : "Login failed", esc, sizeof(esc));
    snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}", esc);
    server.send(200, "application/json", buf);
}

static const char* authHttpPostErrorMessage(int code) {
    if (WiFi.status() != WL_CONNECTED) {
        return "No WiFi — connect gateway to your router first";
    }
    if (code == HTTPC_ERROR_CONNECTION_REFUSED) {
        return "Cannot reach server (connection refused)";
    }
    if (code == HTTPC_ERROR_CONNECTION_LOST) {
        return "Connection lost — check WiFi or internet";
    }
    if (code == HTTPC_ERROR_SEND_HEADER_FAILED || code == HTTPC_ERROR_SEND_PAYLOAD_FAILED) {
        return "Network error while sending login";
    }
    if (code == HTTPC_ERROR_NOT_CONNECTED) {
        return "Not connected to network";
    }
    if (code == HTTPC_ERROR_READ_TIMEOUT) {
        return "Server timeout — check internet connection";
    }
    if (code < 0) {
        return "No internet or server unreachable";
    }
    return nullptr;
}

/** Extract a JSON string field value (supports optional space after colon). */
static bool extractAuthJsonString(const char* json, const char* fieldName, char* out, size_t outSize, int* outLen) {
    if (!json || !fieldName || !out || outSize < 2) return false;
    out[0] = '\0';
    if (outLen) *outLen = 0;

    char keyPat[40];
    snprintf(keyPat, sizeof(keyPat), "\"%s\"", fieldName);
    const char* keyPos = strstr(json, keyPat);
    if (!keyPos) return false;

    const char* colon = strchr(keyPos, ':');
    if (!colon) return false;
    const char* p = colon + 1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;

    const char* end = nullptr;
    const char* nextField = strstr(p, "\",\"");
    if (nextField) {
        end = nextField;
    } else {
        end = strchr(p, '"');
    }
    if (!end || end <= p) return false;

    const int len = (int)(end - p);
    if (outLen) *outLen = len;
    if (len <= 0 || (size_t)len >= outSize) {
        return false;
    }
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
    return true;
}

static int readHttpBodyToBuffer(HTTPClient& http, char* buf, size_t bufSize) {
    if (!buf || bufSize < 2) return 0;
    buf[0] = '\0';
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) return 0;

    const int contentLen = http.getSize();
    size_t total = 0;
    unsigned long t0 = millis();
    unsigned long lastDataMs = t0;
    const unsigned long maxWaitMs = 8000UL;
    const unsigned long idleDoneMs = 350UL;

    while (millis() - t0 < maxWaitMs && total < bufSize - 1) {
        if (contentLen > 0 && (int)total >= contentLen) {
            break;
        }
        if (stream->available()) {
            const int n = stream->read((uint8_t*)(buf + total), (int)(bufSize - 1 - total));
            if (n > 0) {
                total += (size_t)n;
                lastDataMs = millis();
            }
        } else if (!http.connected()) {
            break;
        } else if (total > 0 && (millis() - lastDataMs) >= idleDoneMs) {
            break;
        } else {
            delay(1);
        }
    }
    buf[total] = '\0';
    return (int)total;
}

/** Resume SMS poll pause when login handler exits (any return path). */
struct LoginSmsPollGuard {
    bool active = false;
    ~LoginSmsPollGuard() {
        if (active) {
            resumeSmsPolling();
        }
    }
};

/** Restore WiFi power-save after login HTTPS (any return path). */
struct LoginWifiGuard {
    wifi_ps_type_t savedPs = WIFI_PS_MIN_MODEM;
    bool active = false;
    void enable() {
        if (!active) {
            savedPs = WiFi.getSleep();
            WiFi.setSleep(WIFI_PS_NONE);
            active = true;
        }
    }
    ~LoginWifiGuard() {
        if (active) {
            WiFi.setSleep(savedPs);
        }
    }
};

void handleLogin() {
    // Login to backend to get JWT tokens
    if (charBufIsEmpty(agentBaseUrl)) {
        sendJsonError("No base URL configured");
        return;
    }
    
    // Parse JSON body
    String body = server.arg("plain");
    if (body.length() == 0) {
        body = server.arg("body");
    }
    
    // Extract email and password from JSON
    char email[128] = "";
    char password[128] = "";
    
    // Simple JSON extraction
    const char* emailKey = "\"email\":\"";
    const char* pwKey = "\"password\":\"";
    
    const char* emailPos = strstr(body.c_str(), emailKey);
    if (emailPos) {
        emailPos += strlen(emailKey);
        const char* endQuote = strchr(emailPos, '"');
        if (endQuote) {
            int len = endQuote - emailPos;
            if (len < (int)sizeof(email)) {
                strncpy(email, emailPos, len);
                email[len] = '\0';
            }
        }
    }
    
    const char* pwPos = strstr(body.c_str(), pwKey);
    if (pwPos) {
        pwPos += strlen(pwKey);
        const char* endQuote = strchr(pwPos, '"');
        if (endQuote) {
            int len = endQuote - pwPos;
            if (len < (int)sizeof(password)) {
                strncpy(password, pwPos, len);
                password[len] = '\0';
            }
        }
    }
    
    if (charBufIsEmpty(email) || charBufIsEmpty(password)) {
        sendJsonError("Email and password required");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        logMsg("[AUTH] Login blocked: no WiFi");
        appendMonitorLog("[AUTH] No WiFi");
        sendAuthLoginJsonError("No WiFi — connect gateway to your router first");
        return;
    }

    const unsigned long loginNowMs = millis();
    if (loginNowMs < loginCooldownUntilMs) {
        sendAuthLoginJsonError("Too many login attempts — wait 1 minute");
        return;
    }
    if (loginNowMs - lastLoginAttemptMs < AUTH_LOGIN_MIN_INTERVAL_MS) {
        sendAuthLoginJsonError("Please wait 10 seconds between login attempts");
        return;
    }
    lastLoginAttemptMs = loginNowMs;

    // Pause SIM UART polling so TLS + auth get CPU/heap (same as token refresh).
    LoginSmsPollGuard smsGuard;
    LoginWifiGuard wifiGuard;
    pauseSmsPolling(20000);
    smsGuard.active = true;
    delay(80);
    
    // Build URL - use /api/agent/auth (not /api/agent/auth/login)
    char url[256];
    snprintf(url, sizeof(url), "%s/api/agent/auth", agentBaseUrl);
    
    // Build payload
    char payload[256];
    snprintf(payload, sizeof(payload), "{\"email\":\"%s\",\"password\":\"%s\"}", email, password);
    
    // Make request
    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    bool isHttps = (strncmp(url, "https://", 8) == 0);
    
    // Wait briefly for other HTTPS operations to finish.
    // Background HTTPS (heartbeat / maintenance / OTA) runs even when the user is not logged in,
    // and it shares this global `httpsBusy` flag. Without a short wait, login can fail repeatedly.
    if (httpsBusy) {
        unsigned long t0 = millis();
        while (httpsBusy && (millis() - t0) < 4000) {
            delay(10);
        }
    }
    if (httpsBusy) {
        sendJsonError("HTTPS busy, try again");
        return;
    }
    httpsBusy = true;
    wifiGuard.enable();

    logMsg2Val("[AUTH] URL", url, "", "");
    const unsigned long authT0 = millis();
    
    auto beginAuthHttp = [&]() -> bool {
        if (isHttps) {
            clientSecure.setInsecure();
            clientSecure.setTimeout(12000);
            return http.begin(clientSecure, url);
        }
        return http.begin(client, url);
    };

    if (!beginAuthHttp()) {
        logMsg("[AUTH] HTTP begin failed");
        appendMonitorLog("[AUTH] HTTP begin failed");
        sendAuthLoginJsonError(isHttps ? "Cannot connect to server (TLS)" : "Cannot connect to server");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(12000);
    
    int code = http.POST(payload);

    const unsigned long postMs = millis() - authT0;
    int respLen = 0;

    if (code > 0) {
        respLen = readHttpBodyToBuffer(http, gAuthRespBuf, sizeof(gAuthRespBuf));
        {
            char timing[64];
            snprintf(timing, sizeof(timing), "[AUTH] POST %lums body %dB read %lums",
                     (unsigned long)postMs, respLen, (unsigned long)(millis() - authT0 - postMs));
            logMsg(timing);
            appendMonitorLog(timing);
        }
    } else {
        logMsgInt("[AUTH] POST failed, code", code);
        appendMonitorLogInt("[AUTH] POST failed", code);
        http.end();
        clientSecure.stop();
        client.stop();
        loginFailStreak++;
        if (loginFailStreak >= AUTH_LOGIN_MAX_FAILS) {
            loginFailStreak = 0;
            loginCooldownUntilMs = millis() + AUTH_LOGIN_COOLDOWN_MS;
        }
        const char* netErr = authHttpPostErrorMessage(code);
        sendAuthLoginJsonError(netErr ? netErr : "Login request failed");
        return;
    }
    http.end();
    clientSecure.stop();
    client.stop();
    
    // Log response size (body may be too long to print fully)
    logMsgInt("[AUTH] HTTP code", code);
    {
        char sizeLine[48];
        snprintf(sizeLine, sizeof(sizeLine), "[AUTH] Body bytes=%d", respLen);
        appendMonitorLog(sizeLine);
    }
    
    // Check if response indicates success
    // Response format: {"success":true,"data":{"access_token":"...","refresh_token":"..."}}
    // Or error: {"success":false,"error":"..."}
    
    if (respLen <= 0) {
        logMsg("[AUTH] Empty response");
        appendMonitorLog("[AUTH] Empty response");
        if (WiFi.status() != WL_CONNECTED) {
            sendAuthLoginJsonError("No WiFi — connection dropped during login");
        } else {
            sendAuthLoginJsonError("Server returned empty response — check internet");
        }
        return;
    }
    
    // Check for success flag (handle both "success":true and "success": true)
    bool success = (strstr(gAuthRespBuf, "\"success\":true") != NULL ||
                    strstr(gAuthRespBuf, "\"success\": true") != NULL);
    
    if (!success) {
        // Extract error message
        const char* errKey = "\"error\":\"";
        const char* errPos = strstr(gAuthRespBuf, errKey);
        char errorMsg[128] = "Login failed";
        
        if (errPos) {
            errPos += strlen(errKey);
            const char* endQuote = strchr(errPos, '"');
            if (endQuote) {
                int len = endQuote - errPos;
                if (len < (int)sizeof(errorMsg)) {
                    strncpy(errorMsg, errPos, len);
                    errorMsg[len] = '\0';
                }
            }
        }
        
        logMsg2Val("[AUTH] Backend error", errorMsg, "", "");
        appendMonitorLogVal("[AUTH] Error", errorMsg);
        httpsBusy = false;
        char buf[256];
        char esc[200];
        jsonEscapeNoQuotes(errorMsg, esc, sizeof(esc));
        snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}", esc);
        server.send(200, "application/json", buf);
        return;
    }
    
    // Extract tokens directly into agent buffers (no extra static copies).
    agentBearerToken[0] = '\0';
    agentRefreshToken[0] = '\0';

    int atLen = 0;
    int rtLen = 0;
    const bool gotAccess = extractAuthJsonString(
        gAuthRespBuf, "access_token", agentBearerToken, sizeof(agentBearerToken), &atLen);
    extractAuthJsonString(
        gAuthRespBuf, "refresh_token", agentRefreshToken, sizeof(agentRefreshToken), &rtLen);

    if (!gotAccess && atLen > 0) {
        char line[64];
        snprintf(line, sizeof(line), "[AUTH] access_token too long (%d)", atLen);
        logMsg(line);
        appendMonitorLog(line);
        sendAuthLoginJsonError("Login token too large for device storage");
        return;
    }
    
    if (!charBufIsEmpty(agentBearerToken)) {
        loginFailStreak = 0;
        loginCooldownUntilMs = 0;
        preferences.begin("agent", false);
        preferences.putString("tok", agentBearerToken);
        preferences.putString("rtok", agentRefreshToken);
        preferences.end();
        
        logMsg("[AUTH] Login successful, tokens saved");
        appendMonitorLog("[AUTH] Login OK");
        deferCloudBackend(HEARTBEAT_POST_LOGIN_DEFER_MS);
        resetAgentInventoryHeartbeat();
        logMsg("[CLOUD] Backend HTTPS paused 90s after login");
        appendMonitorLog("[CLOUD] HTTPS paused 90s after login");

        wifiGuard.active = false;
        WiFi.setSleep(WIFI_PS_NONE);
        ensureGatewaySoftAp();

        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"success\":true,\"message\":\"Logged in\",\"has_refresh\":%s}",
            charBufIsEmpty(agentRefreshToken) ? "false" : "true"
        );
        httpsBusy = false;
        server.send(200, "application/json", buf);
        for (int i = 0; i < 12; i++) {
            handleWebRequests();
            delay(1);
        }
        return;
    }
    
    // Login failed - no token found
    if (strstr(gAuthRespBuf, "access_token") == nullptr) {
        logMsg("[AUTH] No access_token field in response");
        appendMonitorLog("[AUTH] No access_token field");
    } else {
        logMsg("[AUTH] access_token present but not parsed");
        appendMonitorLog("[AUTH] Token parse failed");
    }
    
    sendAuthLoginJsonError("Login succeeded but no access token in response");
}

void handleLogout() {
    resetAgentInventoryHeartbeat();
    // Clear tokens
    agentBearerToken[0] = '\0';
    agentRefreshToken[0] = '\0';
    
    preferences.begin("agent", false);
    preferences.remove("tok");
    preferences.remove("rtok");
    preferences.end();
    
    deviceRegistered = false;
    simRegistered = false;
    
    logMsg("[AUTH] Logged out");
    appendMonitorLog("[AUTH] Logged out");
    sendJsonSuccess("Logged out");
}

bool refreshAgentToken() {
    if (charBufIsEmpty(agentRefreshToken)) {
        logMsg("[AUTH] Refresh failed: no refresh token");
        appendMonitorLog("[AUTH] Refresh failed: no refresh token");
        return false;
    }

    // Diagnostics: refresh token length/prefix (do not log full token)
    {
        int rtLen = (int)strlen(agentRefreshToken);
        char prefix[12];
        prefix[0] = '\0';
        if (rtLen > 0) {
            int n = rtLen < 8 ? rtLen : 8;
            strncpy(prefix, agentRefreshToken, (size_t)n);
            prefix[n] = '\0';
        }
        char diag[96];
        snprintf(diag, sizeof(diag), "[AUTH] RT len=%d pref=%s", rtLen, prefix);
        appendMonitorLog(diag);
    }
    
    if (charBufIsEmpty(agentBaseUrl)) {
        logMsg("[AUTH] Refresh failed: no base URL");
        appendMonitorLog("[AUTH] Refresh failed: no base URL");
        return false;
    }
    
    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED) {
        logMsgInt("[AUTH] Refresh failed: WiFi not connected, status", WiFi.status());
        appendMonitorLog("[AUTH] Refresh failed: WiFi not connected");
        return false;
    }
    
    // Ensure NTP is configured - required for HTTPS/TLS certificate validation
    if (!ntpConfigured) {
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        ntpConfigured = true;
        // Give NTP time to sync (TLS handshake fails if time is invalid)
        unsigned long start = millis();
        while (millis() - start < 6000) {
            time_t now = time(nullptr);
            if (now > 1700000000) break; // ~2023-11
            delay(250);
        }
        logMsg("[AUTH] NTP configured for TLS");
        appendMonitorLog("[AUTH] NTP configured");
    }
    
    logMsg("[AUTH] Token refresh starting");
    appendMonitorLog("[AUTH] Token refresh starting");

    // Pause SMS polling to free heap for SSL
    pauseSmsPolling(15000);
    delay(100); // Let any in-progress polling finish
    
    // Check heap before SSL (needs ~20KB free)
    size_t freeHeap = ESP.getFreeHeap();
    char heapBuf[48];
    snprintf(heapBuf, sizeof(heapBuf), "[AUTH] Heap %u bytes", (unsigned)freeHeap);
    appendMonitorLog(heapBuf);
    
    if (freeHeap < 18000) {
        appendMonitorLog("[AUTH] Low heap - SSL may fail");
    }
    
    static char url[256];
    snprintf(url, sizeof(url), "%s/api/agent/auth/refresh", agentBaseUrl);

    // DNS diagnostics (helps distinguish DNS vs TLS vs timeout)
    {
        IPAddress ip;
        if (WiFi.hostByName("www.otpocket.app", ip)) {
            char ipBuf[48];
            snprintf(ipBuf, sizeof(ipBuf), "[AUTH] DNS %u.%u.%u.%u",
                     (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
            appendMonitorLog(ipBuf);
        } else {
            appendMonitorLog("[AUTH] DNS fail www.otpocket.app");
        }
    }

    static char payload[700];
    char rtEsc[600];
    jsonEscape(agentRefreshToken, rtEsc, sizeof(rtEsc));
    snprintf(payload, sizeof(payload), "{\"refresh_token\":%s}", rtEsc);
    
    // Simple HTTPS request - same pattern as handleLogin
    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    bool isHttps = (strncmp(url, "https://", 8) == 0);
    
    // Note: Caller (heartbeat, SMS forward, etc.) should set httpsBusy before calling
    // We don't check/set it here to allow nested calls from 401 handlers
    
    if (isHttps) {
        clientSecure.setInsecure();
        clientSecure.setTimeout(15000);
        if (!http.begin(clientSecure, url)) {
            logMsg("[AUTH] HTTPS begin failed");
            appendMonitorLog("[AUTH] HTTPS begin failed");
            resumeSmsPolling();
            return false;
        }
    } else {
        if (!http.begin(client, url)) {
            logMsg("[AUTH] HTTP begin failed");
            appendMonitorLog("[AUTH] HTTP begin failed");
            resumeSmsPolling();
            return false;
        }
    }
    
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);
    
    int code = http.POST(payload);
    int respLen = 0;
    if (code > 0) {
        respLen = readHttpBodyToBuffer(http, gAuthRespBuf, sizeof(gAuthRespBuf));
    }
    http.end();
    clientSecure.stop();
    client.stop();
    
    if (code < 0) {
        String err = http.errorToString(code);
        logMsgInt("[AUTH] Refresh failed, code", code);
        if (err.length() > 0) {
            appendMonitorLogVal("[AUTH] HTTP err", err.c_str());
        }
        appendMonitorLogInt("[AUTH] Refresh conn err", code);
        resumeSmsPolling();
        return false;
    }
    
    if (!(code >= 200 && code < 300) || respLen <= 0) {
        logMsgInt("[AUTH] Token refresh failed, HTTP", code);
        appendMonitorLogInt("[AUTH] Refresh HTTP", code);
        resumeSmsPolling();
        return false;
    }
    
    agentBearerToken[0] = '\0';

    int atLen = 0;
    extractAuthJsonString(
        gAuthRespBuf, "access_token", agentBearerToken, sizeof(agentBearerToken), &atLen);

    char newRefresh[AGENT_REFRESH_TOKEN_SIZE];
    newRefresh[0] = '\0';
    extractAuthJsonString(gAuthRespBuf, "refresh_token", newRefresh, sizeof(newRefresh), nullptr);
    
    if (charBufIsEmpty(agentBearerToken)) {
        if (atLen > 0) {
            appendMonitorLogInt("[AUTH] Refresh token too long", atLen);
        } else {
            appendMonitorLog("[AUTH] Refresh failed: no access_token");
        }
        resumeSmsPolling();
        return false;
    }
    
    if (!charBufIsEmpty(newRefresh)) {
        charBufSet(agentRefreshToken, sizeof(agentRefreshToken), newRefresh);
    }
    
    preferences.begin("agent", false);
    preferences.putString("tok", agentBearerToken);
    preferences.putString("rtok", agentRefreshToken);
    preferences.end();
    
    resumeSmsPolling();
    
    logMsg("[AUTH] Token refreshed");
    appendMonitorLog("[AUTH] Token refreshed");
    return true;
}

void handleRefreshToken() {
    // Refresh the access token using refresh token
    if (charBufIsEmpty(agentRefreshToken)) {
        sendJsonError("No refresh token");
        return;
    }
    
    if (charBufIsEmpty(agentBaseUrl)) {
        sendJsonError("No base URL configured");
        return;
    }
    
    if (refreshAgentToken()) {
        sendJsonSuccess("Token refreshed");
    } else {
        sendJsonError("Token refresh failed");
    }
}

bool registerDeviceWithBackend() {
    if (charBufIsEmpty(agentBaseUrl) || charBufIsEmpty(agentBearerToken)) {
        return false;
    }
    if (httpsBusy) {
        return false;
    }

    if (charBufIsEmpty(agentDeviceId)) {
        snprintf(agentDeviceId, sizeof(agentDeviceId), "SIM800-%06X", (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));
        preferences.begin("agent", false);
        preferences.putString("dev", agentDeviceId);
        preferences.end();
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/api/agent/devices", agentBaseUrl);

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"name\":\"%s\"}",
        agentDeviceId, agentDeviceId);

    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    const bool isHttps = (strncmp(url, "https://", 8) == 0);

    httpsBusy = true;

    bool begun = false;
    if (isHttps) {
        clientSecure.setInsecure();
        clientSecure.setTimeout(12000);
        begun = http.begin(clientSecure, url);
    } else {
        begun = http.begin(client, url);
    }
    if (!begun) {
        httpsBusy = false;
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
    http.setTimeout(12000);

    int code = http.POST(payload);
    http.end();

    if (code == 401 && refreshAgentToken()) {
        clientSecure.stop();
        client.stop();
        delay(100);
        if (isHttps) {
            clientSecure.setInsecure();
            http.begin(clientSecure, url);
        } else {
            http.begin(client, url);
        }
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
        http.setTimeout(12000);
        code = http.POST(payload);
        http.end();
    }

    if (isHttps) {
        clientSecure.stop();
    } else {
        client.stop();
    }
    delay(50);
    httpsBusy = false;

    if (code >= 200 && code < 300) {
        deviceRegistered = true;
        logMsg2Val("[REGISTER] Device OK", agentDeviceId, "", "");
        appendMonitorLogVal("[REGISTER] Device OK ", agentDeviceId);
        return true;
    }

    logMsgInt("[REGISTER] Device failed, code", code);
    appendMonitorLogInt("[REGISTER] Device failed", code);
    return false;
}

void handleRegisterDevice() {
    if (charBufIsEmpty(agentBaseUrl)) {
        sendJsonError("No base URL configured");
        return;
    }
    if (charBufIsEmpty(agentBearerToken)) {
        sendJsonError("Not logged in");
        return;
    }
    if (registerDeviceWithBackend()) {
        sendJsonSuccess("Device registered");
    } else {
        sendJsonError("Registration failed");
    }
}

void handleRegisterSim() {
    // Register SIM with backend
    if (!deviceRegistered) {
        sendJsonError("Device not registered");
        return;
    }
    
    if (charBufIsEmpty(agentBearerToken)) {
        sendJsonError("Not logged in");
        return;
    }
    
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : (activeSim + 1);
    int simIdx = slot - 1;
    
    if (simIdx < 0 || simIdx >= SIM_COUNT) {
        sendJsonError("Invalid SIM slot");
        return;
    }
    
    if (charBufIsEmpty(simStates[simIdx].number)) {
        sendJsonError("SIM number not detected");
        return;
    }
    
    char url[256];
    snprintf(url, sizeof(url), "%s/api/agent/sims/register", agentBaseUrl);
    
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"number\":\"%s\",\"device_id\":\"%s\",\"slot\":%d,\"carrier\":null,\"max_active_sims\":16}",
        simStates[simIdx].number, agentDeviceId, simIdx
    );
    
    HTTPClient http;
    WiFiClientSecure clientSecure;
    WiFiClient client;
    bool isHttps = (strncmp(url, "https://", 8) == 0);
    
    // Wait for other HTTPS operations
    if (httpsBusy) {
        sendJsonError("HTTPS busy, try again");
        return;
    }
    httpsBusy = true;
    
    if (isHttps) {
        clientSecure.setInsecure();
        http.begin(clientSecure, url);
    } else {
        http.begin(client, url);
    }
    
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
    http.setTimeout(10000);
    
    int code = http.POST(payload);
    String resp = "";
    if (code > 0) {
        resp = http.getString();
    }
    http.end();

    if (code == 401) {
        // Stop previous connection before refresh
        clientSecure.stop();
        client.stop();
        delay(100);  // Let TCP fully close
        
        if (refreshAgentToken()) {
            if (isHttps) {
                clientSecure.setInsecure();
                http.begin(clientSecure, url);
            } else {
                http.begin(client, url);
            }
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
            http.setTimeout(10000);
            code = http.POST(payload);
            if (code > 0) {
                resp = http.getString();
            }
            http.end();
        }
    }
    
    // Handle 409 conflict with force_bind
    if (code == 409) {
        snprintf(payload, sizeof(payload),
            "{\"number\":\"%s\",\"device_id\":\"%s\",\"slot\":%d,\"carrier\":null,\"max_active_sims\":16,\"force_bind\":true}",
            simStates[simIdx].number, agentDeviceId, simIdx
        );
        
        if (isHttps) {
            http.begin(clientSecure, url);
        } else {
            http.begin(client, url);
        }
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
        code = http.POST(payload);
        http.end();
    }
    
    if (code >= 200 && code < 300) {
        simStates[simIdx].backendRegistered = true;
        simRegistered = true;
        logMsg2Val("[REGISTER] SIM registered", simStates[simIdx].number, "slot", String(slot).c_str());
        appendMonitorLogVal("[REGISTER] SIM ", simStates[simIdx].number);
        sendJsonSuccess("SIM registered");
    } else {
        logMsgInt("[REGISTER] SIM registration failed, code", code);
        appendMonitorLogInt("[REGISTER] SIM failed", code);
        sendJsonError("SIM registration failed");
    }
    clientSecure.stop();
    client.stop();
    delay(50);
    httpsBusy = false;
}

// -----------------------------------------------------------------------------
// Manual Heartbeat (for debugging)
// -----------------------------------------------------------------------------

void handleHeartbeatManual() {
    // Manual heartbeat trigger for debugging
    if (httpsBusy) {
        sendJsonError("HTTPS busy, try again later");
        return;
    }
    
    if (charBufIsEmpty(agentBaseUrl)) {
        sendJsonError("No backend URL configured");
        return;
    }
    
    if (charBufIsEmpty(agentBearerToken)) {
        sendJsonError("Not logged in");
        return;
    }
    
    logMsg("[HEARTBEAT] Manual trigger");
    appendMonitorLog("[HEARTBEAT] Manual");
    
    // Call performHeartbeat directly
    performHeartbeat();
    
    sendJsonSuccess("Heartbeat sent - check monitor for result");
}

void handleTogglePolling() {
    // Toggle SMS polling pause state
    extern bool smsPollingPaused;
    extern unsigned long smsPollingPauseUntil;
    
    if (smsPollingPaused) {
        // Resume polling
        smsPollingPaused = false;
        smsPollingPauseUntil = 0;
        logMsg("[SMS] Polling resumed");
        appendMonitorLog("[SMS] Polling resumed");
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"success\":true,\"paused\":false,\"message\":\"Polling resumed\"}");
        server.send(200, "application/json", buf);
    } else {
        // Pause polling for 5 minutes
        smsPollingPaused = true;
        smsPollingPauseUntil = millis() + 300000;  // 5 minutes
        logMsg("[SMS] Polling paused for 5 min");
        appendMonitorLog("[SMS] Polling paused");
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"success\":true,\"paused\":true,\"message\":\"Polling paused 5 min\"}");
        server.send(200, "application/json", buf);
    }
}

void handleToggleHeartbeat() {
    if (heartbeatPaused) {
        heartbeatPaused = false;
        logMsg("[HEARTBEAT] Resumed");
        appendMonitorLog("[HEARTBEAT] Resumed");
    } else {
        heartbeatPaused = true;
        logMsg("[HEARTBEAT] Paused");
        appendMonitorLog("[HEARTBEAT] Paused");
    }

    // Persist setting
    preferences.begin("agent", false);
    preferences.putBool("hb_pause", heartbeatPaused);
    preferences.end();

    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"success\":true,\"paused\":%s,\"message\":\"Heartbeat %s\"}",
        heartbeatPaused ? "true" : "false",
        heartbeatPaused ? "paused" : "resumed"
    );
    server.send(200, "application/json", buf);
}

void handleToggleMissedCall() {
    if (server.hasArg("enabled")) {
        const String v = server.arg("enabled");
        missedCallForwardEnabled = (v == "1" || v == "true");
    } else {
        missedCallForwardEnabled = !missedCallForwardEnabled;
    }

    preferences.begin("agent", false);
    preferences.putBool("mcall", missedCallForwardEnabled);
    preferences.end();

    applyMissedCallModemToAllSims();

    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"success\":true,\"enabled\":%s,\"message\":\"Missed call forward %s\"}",
        missedCallForwardEnabled ? "true" : "false",
        missedCallForwardEnabled ? "enabled" : "disabled"
    );
    server.send(200, "application/json", buf);
}

void handleFirmwareCheck() {
    bool updateAvailable = false;
    char remote[32];
    remote[0] = '\0';

    const bool ok = otaCheckForUpdate(&updateAvailable, remote, sizeof(remote));
    if (ok) {
        maintenanceRecordFirmwareCheck(remote, updateAvailable);
    }
    const bool localAhead = ok && remote[0] && otaVersionIsNewer(FIRMWARE_VERSION, remote);
    char buf[256];
    if (!ok) {
        snprintf(buf, sizeof(buf),
            "{\"success\":false,\"current\":\"%s\",\"message\":\"Could not fetch remote version\"}",
            FIRMWARE_VERSION);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"success\":true,\"current\":\"%s\",\"remote\":\"%s\","
            "\"update_available\":%s,\"local_ahead\":%s}",
            FIRMWARE_VERSION,
            remote,
            updateAvailable ? "true" : "false",
            localAhead ? "true" : "false");
    }
    server.send(200, "application/json", buf);
}

void handleFirmwareConfig() {
    if (server.hasArg("url")) {
        otaSaveUrlToPreferences(server.arg("url").c_str());
    }
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"success\":true,\"ota_url\":\"%s\"}",
        otaFirmwareUrl);
    server.send(200, "application/json", buf);
}

void handleFirmwareUpdate() {
#if !OTA_ENABLED
    server.send(200, "application/json",
        "{\"success\":false,\"message\":\"OTA install is disabled on this firmware build. "
        "In Arduino IDE set Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA), "
        "compile, and upload via USB once. After that, Install update works from this page.\"}");
    return;
#endif
#if !OTA_WEB_INSTALL_ENABLED
    logMsg("[OTA] Web install blocked (OTA_WEB_INSTALL_ENABLED=0)");
    server.send(200, "application/json",
        "{\"success\":false,\"message\":\"Wireless firmware install is disabled on this device. "
        "Flash via USB in Arduino IDE, or set OTA_WEB_INSTALL_ENABLED=1 and rebuild.\"}");
    return;
#endif
    if (!WiFi.isConnected()) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"WiFi required for OTA\"}");
        return;
    }

    const char* url = server.hasArg("url") ? server.arg("url").c_str() : nullptr;
    if (url && url[0]) {
        otaSaveUrlToPreferences(url);
    }

    logMsg("[OTA] Web install requested");
    appendMonitorLog("[OTA] Web install requested");

    server.send(200, "application/json",
        "{\"success\":true,\"message\":\"Downloading firmware. Device will restart when complete.\"}");

    delay(300);
    server.handleClient();

    char err[96];
    if (!otaPerformUpdate(url, err, sizeof(err))) {
        appendMonitorLogVal("[OTA] Update failed", err);
    }
}

void handleReset() {
    // Debounce: Don't allow reset within first 30 seconds of boot
    // This prevents browser cache replay from causing immediate reset
    unsigned long uptimeMs = millis();
    if (uptimeMs < 30000) {
        logMsgInt("[WEB] Reset ignored - uptime too short:", (int)(uptimeMs / 1000));
        server.send(429, "application/json", "{\"success\":false,\"message\":\"Reset not allowed within 30 seconds of boot\"}");
        return;
    }
    
    logMsg("[WEB] Reset requested");
    appendMonitorLog("[WEB] Device restart requested");
    
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Restarting device...\"}");
    
    // Delay to allow response to be sent
    delay(500);
    
    // Restart ESP32
    ESP.restart();
}

// -----------------------------------------------------------------------------
// JSON Response Helpers
// -----------------------------------------------------------------------------

void sendJsonSuccess(const char* message) {
    char buf[128];
    if (message) {
        snprintf(buf, sizeof(buf), "{\"success\":true,\"message\":\"%s\"}", message);
    } else {
        strcpy(buf, "{\"success\":true}");
    }
    server.send(200, "application/json", buf);
}

void sendJsonError(const char* error, int code) {
    char buf[256];
    char escaped[128];
    jsonEscapeNoQuotes(error, escaped, sizeof(escaped));
    snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}", escaped);
    // Also log to web monitor for visibility
    char logLine[192];
    snprintf(logLine, sizeof(logLine), "[ERR] (%d) %s", code, error ? error : "");
    appendMonitorLog(logLine);
    server.send(code, "application/json", buf);
}

void sendJson(const char* json) {
    server.send(200, "application/json", json);
}
