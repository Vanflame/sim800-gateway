// ============================================================================
// Web UI Server Implementation
// ============================================================================

#include "webui.h"
#include "ota.h"
#include "mux.h"
#include "config.h"
#include "sim800.h"
#include "mux.h"
#include "sms.h"
#include "logger.h"
#include "utils.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>

// Web server on port 80
WebServer server(80);

static volatile bool refreshAllInProgress = false;

// Preferences for storing settings (defined in main .ino)
extern Preferences preferences;

// External persistent storage functions
extern int readSmsFromFile(char* buf, size_t bufSize, int maxMessages);
extern int readErrorsFromFile(char* buf, size_t bufSize, int maxErrors);

// External state from main .ino
extern SimState simStates[SIM_COUNT];
extern char wifiSsid[64];
extern char wifiPassword[64];
extern char agentBaseUrl[128];
extern char agentDeviceId[64];
extern char agentBearerToken[1024];  // JWT tokens can be 700+ chars
extern char agentRefreshToken[512];
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
void handleScanNetworks();
void handleSelectNetwork();
void handleErrorLog();
void handleClearErrorLog();

// HTML page stored in PROGMEM
static const char INDEX_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SIM800 Gateway | OTPocket Agent</title>
  <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><rect fill='%233b82f6' x='10' y='10' width='80' height='80' rx='15'/><text x='50' y='68' font-size='50' font-weight='bold' fill='white' text-anchor='middle'>OT</text></svg>">
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
      --sidebar-width: 240px;
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
    
    /* Background with gradient and glow */
    .bg-wrapper {
      position: fixed;
      inset: 0;
      pointer-events: none;
      z-index: 0;
      background: linear-gradient(180deg, #06122b 0%, #0b1a3a 45%, #040b1f 100%);
    }
    
    .bg-wrapper::before {
      content: '';
      position: absolute;
      inset: 0;
      background: radial-gradient(1100px 700px at 18% 18%, rgba(59,130,246,0.15), transparent 60%),
                  radial-gradient(1000px 680px at 82% 24%, rgba(124,58,237,0.12), transparent 62%),
                  radial-gradient(1200px 820px at 50% 100%, rgba(59,130,246,0.08), transparent 62%);
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
    
    /* Desktop: show sidebar always, allow collapse */
    @media (min-width: 768px) {
      .sidebar { transform: translateX(0); }
      .sidebar.collapsed { 
        transform: translateX(0);
        width: 60px;
      }
      .sidebar.collapsed .logo-text { display: none !important; }
      .sidebar.collapsed .sidebar-header { 
        padding: 12px; 
        justify-content: center; 
      }
      .sidebar.collapsed .sidebar-header .logo { 
        justify-content: center; 
        margin: 0;
      }
      .sidebar.collapsed .sidebar-toggle { 
        position: absolute;
        right: -16px;
        top: 12px;
        margin-left: 0;
      }
      .sidebar.collapsed .nav-item {
        justify-content: center;
        padding: 12px 8px;
        gap: 0;
        position: relative;
      }
      .sidebar.collapsed .nav-item svg {
        margin: 0;
        flex-shrink: 0;
      }
      /* Show badge as small dot on icon when collapsed */
      .sidebar.collapsed .nav-item .nav-badge {
        position: absolute;
        top: 8px;
        right: 12px;
        min-width: 6px;
        height: 6px;
        padding: 0;
        font-size: 0;
        border-radius: 50%;
        background: var(--success);
      }
      .main-content { margin-left: var(--sidebar-width); }
      .main-content.expanded { margin-left: 60px; }
      .mobile-header { display: none !important; }
      .overlay.show { opacity: 0 !important; pointer-events: none !important; }
      .sidebar-toggle { display: flex !important; }
    }
    
    /* Hide text in nav items when collapsed (desktop only) */
    @media (min-width: 768px) {
      .sidebar.collapsed .nav-item {
        font-size: 0;
        overflow: hidden;
      }
    }
    
    /* Sidebar toggle button (desktop only) */
    .sidebar-toggle {
      display: none;
      width: 32px;
      height: 32px;
      background: rgba(255,255,255,0.05);
      border: 1px solid var(--border);
      border-radius: 6px;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      color: var(--text-secondary);
      transition: transform 0.3s ease, color 0.2s, background 0.2s;
      margin-left: auto;
      flex-shrink: 0;
    }
    
    .sidebar-toggle:hover { 
      background: rgba(255,255,255,0.1); 
      color: var(--text); 
    }
    .sidebar-toggle svg { width: 16px; height: 16px; }
    
    .sidebar-header {
      padding: 16px;
      border-bottom: 1px solid var(--border);
      display: flex;
      align-items: center;
      position: relative;
    }
    
    .sidebar-nav {
      flex: 1;
      padding: 8px;
      overflow-y: auto;
    }
    
    .nav-item {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 12px 16px;
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
      padding: 16px;
      padding-bottom: 100px;
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
      margin-bottom: 16px;
    }
    
    .page-title {
      font-size: 20px;
      font-weight: 600;
      margin-bottom: 4px;
    }
    
    .page-subtitle {
      font-size: 13px;
      color: var(--muted);
    }
    
    /* Cards */
    .card {
      background: var(--card);
      border: 1px solid var(--card-border);
      border-radius: var(--radius);
      padding: 16px;
      margin-bottom: 12px;
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
      padding: 10px;
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
      gap: 4px;
      margin-top: 8px;
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
    
    /* Log section */
    .log-tabs {
      display: flex;
      gap: 4px;
      margin-bottom: 12px;
    }
    
    .log-tab {
      flex: 1;
      padding: 8px;
      font-size: 12px;
      font-weight: 500;
      text-align: center;
      background: var(--input-bg);
      border: 1px solid var(--border);
      border-radius: 6px;
      cursor: pointer;
      transition: all 0.15s ease;
      color: var(--text-secondary);
    }
    
    .log-tab:hover { background: rgba(255,255,255,0.1); }
    .log-tab.active {
      background: var(--primary);
      border-color: var(--primary);
      color: white;
    }
    
    .log-content {
      background: rgba(0,0,0,0.3);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 12px;
      max-height: 250px;
      overflow-y: auto;
      font-family: 'SF Mono', Consolas, monospace;
      font-size: 11px;
      line-height: 1.6;
      white-space: pre-wrap;
      word-break: break-all;
    }
    
    .log-footer {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-top: 8px;
      font-size: 11px;
      color: var(--muted);
    }
    
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
    
    /* SMS List */
    .sms-list { display: flex; flex-direction: column; gap: 8px; }
    
    .sms-item {
      background: var(--input-bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 12px;
      transition: all 0.15s ease;
    }
    
    .sms-item:hover { border-color: var(--primary); }
    
    .sms-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 8px;
    }
    
    .sms-sender {
      font-weight: 600;
      font-size: 13px;
      color: var(--primary);
    }
    
    .sms-slot {
      font-size: 10px;
      color: var(--muted);
      background: rgba(255,255,255,0.05);
      padding: 2px 6px;
      border-radius: 4px;
    }
    
    .sms-time {
      font-size: 10px;
      color: var(--muted);
      margin-bottom: 8px;
    }
    
    .sms-body {
      font-size: 13px;
      line-height: 1.5;
      color: var(--text);
      white-space: pre-wrap;
      word-break: break-word;
    }
    
    .sms-actions {
      display: flex;
      gap: 8px;
      margin-top: 10px;
      padding-top: 10px;
      border-top: 1px solid var(--border);
    }
    
    .sms-copy-btn {
      font-size: 11px;
      color: var(--primary);
      background: none;
      border: none;
      cursor: pointer;
      display: flex;
      align-items: center;
      gap: 4px;
    }
    
    .sms-copy-btn:hover { text-decoration: underline; }
    
    /* Stats Grid */
    .stats-grid {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: 8px;
    }
    
    .stat-item {
      text-align: center;
      padding: 12px 8px;
      background: var(--input-bg);
      border-radius: 8px;
    }
    
    .stat-value {
      font-size: 24px;
      font-weight: 700;
      color: var(--text);
      line-height: 1;
    }
    
    .stat-label {
      font-size: 10px;
      color: var(--muted);
      margin-top: 4px;
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }
    
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
  <div class="bg-wrapper"></div>
  <div class="overlay" id="overlay" onclick="toggleSidebar()"></div>
  
  <div class="app-layout">
    <!-- Sidebar -->
    <aside class="sidebar" id="sidebar">
      <div class="sidebar-header">
        <div class="logo">
          <div class="logo-icon">OT</div>
          <div class="logo-text">OTPocket<span>Agent</span></div>
        </div>
        <button class="sidebar-toggle" id="sidebarToggle" onclick="toggleSidebarDesktop()">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
        </button>
      </div>
      <nav class="sidebar-nav">
        <div class="nav-item active" data-tab="dashboard" onclick="switchTab('dashboard')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/></svg>
          Dashboard
        </div>
        <div class="nav-item" data-tab="sims" onclick="switchTab('sims')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="4" width="20" height="16" rx="2"/><path d="M6 8h.01M10 8h.01"/></svg>
          SIM Slots
          <span class="nav-badge" id="sessionsNavBadge" style="display:none;">0</span>
        </div>
        <div class="nav-item" data-tab="messages" onclick="switchTab('messages')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg>
          Messages
          <span class="nav-badge" id="msgBadge">0</span>
        </div>
        <div class="nav-item" data-tab="settings" onclick="switchTab('settings')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>
          Settings
        </div>
        <div class="nav-item" data-tab="logs" onclick="switchTab('logs')">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/></svg>
          Logs
        </div>
      </nav>
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
      <div class="header-status">
        <div class="status-dot" id="statusDot"></div>
      </div>
    </header>
    
    <!-- Login Page (shown when not logged in) -->
    <div id="loginPage" class="container login-container">
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
        
        <div style="margin-top:24px;text-align:center;">
          <p class="muted text-sm">Don't have an account? <a href="https://otpocket.app/register" style="color:var(--primary);text-decoration:none;">Sign up</a></p>
        </div>
      </div>
      
      <!-- WiFi Quick Setup (shown on login page) -->
      <div class="card" style="margin-top:16px;">
        <div class="card-header">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:16px;height:16px;"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/></svg>
            WiFi Setup
          </div>
          <span class="badge danger" id="loginWifiBadge">Disconnected</span>
        </div>
        <div id="loginWifiConnected" class="hide">
          <div class="row mb-2">
            <span class="muted text-sm">Connected to</span>
            <span class="text-sm" id="loginWifiSsid">-</span>
          </div>
        </div>
        <div id="loginWifiSetup">
          <button class="btn secondary full mb-2" id="loginScanBtn">Scan Networks</button>
          <div id="loginScanResults" class="grid mb-2"></div>
          <div class="divider"></div>
          <form onsubmit="return false;">
            <input id="loginManualSsid" placeholder="Network name (SSID)" style="margin-bottom:8px;" autocomplete="off" />
            <input id="loginManualPw" type="password" placeholder="Password" style="margin-bottom:8px;" autocomplete="new-password" />
            <button class="btn full" id="loginConnectBtn">Connect</button>
          </form>
        </div>
      </div>
    </div>
  </div>
  
  <!-- Dashboard Page (shown when logged in) -->
  <div id="dashboardPage" class="container hide">
    
    <!-- Dashboard Tab -->
    <div class="tab-content active" id="tab-dashboard">
      <div class="page-header">
        <h1 class="page-title">Dashboard</h1>
        <p class="page-subtitle">Overview of your SIM gateway</p>
      </div>
      
      <!-- Stats -->
      <div class="card">
        <div class="stats-grid">
          <div class="stat-item success">
            <div class="stat-value" id="statReceived">0</div>
            <div class="stat-label">Received</div>
          </div>
          <div class="stat-item success">
            <div class="stat-value" id="statForwarded">0</div>
            <div class="stat-label">Forwarded</div>
          </div>
          <div class="stat-item danger">
            <div class="stat-value" id="statFailed">0</div>
            <div class="stat-label">Failed</div>
          </div>
          <div class="stat-item warning">
            <div class="stat-value" id="statPending">0</div>
            <div class="stat-label">Pending</div>
          </div>
        </div>
      </div>
      
      <!-- WiFi Card -->
      <div class="card" id="wifiCard">
        <div class="card-header">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/></svg>
            WiFi
          </div>
          <span class="badge danger" id="wifiBadge">Disconnected</span>
        </div>
        
        <div id="wifiConnected" class="hide">
          <div class="row mb-2">
            <span class="muted text-sm">Network</span>
            <span class="text-sm" id="wifiSsid">-</span>
          </div>
          <div class="row mb-4">
            <span class="muted text-sm">IP Address</span>
            <span class="text-sm" id="wifiIp">-</span>
          </div>
          <button class="btn secondary sm" id="disconnectBtn">Disconnect</button>
        </div>
        
        <div id="wifiSetup">
          <button class="btn secondary full mb-2" id="scanBtn">Scan Networks</button>
          <div id="scanResults" class="grid mb-4"></div>
          <div class="divider"></div>
          <form onsubmit="return false;">
            <input id="manualSsid" placeholder="Network name (SSID)" style="margin-bottom:8px;" autocomplete="off" />
            <input id="manualPw" type="password" placeholder="Password" style="margin-bottom:8px;" autocomplete="new-password" />
            <button class="btn full" id="connectBtn">Connect</button>
          </form>
        </div>
      </div>
      
      <!-- Auth Card -->
      <div class="card" id="authCard">
        <div class="card-header">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
            Authentication
          </div>
          <span class="badge success" id="authBadge">Logged In</span>
        </div>
        
        <div class="row mb-2">
          <span class="muted text-sm">Device</span>
          <span class="badge primary" id="deviceBadge">-</span>
        </div>
        <div class="row mb-4">
          <span class="muted text-sm">SIMs Registered</span>
          <span class="text-sm" id="simsRegCount">-</span>
        </div>
        <div class="btn-group">
          <button class="btn sm secondary" id="registerDeviceBtn">Register Device</button>
          <button class="btn sm secondary" id="registerAllSimsBtn">Register All SIMs</button>
          <button class="btn sm danger" id="logoutBtn">Logout</button>
        </div>
      </div>
    </div>
    
    <!-- SIM Slots Tab -->
    <div class="tab-content" id="tab-sims">
      <div class="page-header">
        <h1 class="page-title">SIM Slots</h1>
        <p class="page-subtitle">Manage your SIM cards</p>
      </div>

      <div class="card">
        <div class="card-header">
          <span class="text-sm muted" id="simCount">0 active</span>
          <div class="btn-group">
            <button class="btn xs secondary" id="checkAllBtn">Check All</button>
            <button class="btn xs secondary" id="pausePollingBtn">Pause Polling</button>
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
        <p class="page-subtitle">View received SMS messages</p>
      </div>
      
      <div class="card">
        <div class="card-header">
          <span class="text-sm muted" id="smsCount">0 messages</span>
          <button class="btn xs secondary" id="refreshSmsBtn">Refresh</button>
        </div>
        <div class="sms-list" id="smsList">
          <div class="empty-state">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg>
            <p>No messages yet</p>
            <p class="text-sm">Received SMS will appear here</p>
          </div>
        </div>
      </div>
    </div>
    
    <!-- Settings Tab -->
    <div class="tab-content" id="tab-settings">
      <div class="page-header">
        <h1 class="page-title">Settings</h1>
        <p class="page-subtitle">Configure your gateway</p>
      </div>
      
      <div class="card">
        <div class="card-header">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 4h16c1.1 0 2 .9 2 2v12c0 1.1-.9 2-2 2H4c-1.1 0-2-.9-2-2V6c0-1.1.9-2 2-2z"/><polyline points="22,6 12,13 2,6"/></svg>
            Backend Configuration
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
          <p class="muted" style="font-size:12px;margin-top:4px;">Used for heartbeat and backend registration. Save after editing.</p>
        </div>
        <button class="btn full" id="saveConfigBtn">Save Configuration</button>
      </div>
      
      <div class="card">
        <div class="card-header">
          <div class="card-title">Heartbeat</div>
          <button class="btn xs secondary" id="pauseHeartbeatBtn">Pause HB</button>
        </div>
      </div>
      
      <div class="card">
        <div class="card-header">
          <div class="card-title">Firmware (OTA)</div>
        </div>
        <p class="muted" style="margin-bottom: 12px; font-size: 13px;">
          Installed: <strong id="fwVersion">-</strong>
          <span id="fwRemoteWrap" class="hide"> · Latest: <strong id="fwRemoteVersion">-</strong></span>
        </p>
        <div class="form-group">
          <label for="otaUrl">Firmware URL (.bin)</label>
          <input id="otaUrl" placeholder="https://github.com/user/repo/releases/latest/download/firmware.bin" />
        </div>
        <div style="display:flex;gap:8px;flex-wrap:wrap;">
          <button class="btn secondary" id="saveOtaUrlBtn">Save URL</button>
          <button class="btn secondary" id="checkFwBtn">Check Update</button>
          <button class="btn" id="updateFwBtn">Install Update</button>
        </div>
        <p class="muted" id="fwStatus" style="margin-top:12px;font-size:12px;"></p>
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
        <h1 class="page-title">Logs</h1>
        <p class="page-subtitle">System activity and errors</p>
      </div>
      
      <div class="card">
        <div class="log-tabs">
          <div class="log-tab active" onclick="switchLogTab('live')">Live</div>
          <div class="log-tab" onclick="switchLogTab('errors')">Errors</div>
        </div>
        <div class="log-content" id="monitor">Loading...</div>
        <div class="log-content hide" id="errorLog">No errors logged</div>
        <div class="log-footer">
          <span id="monitorCount">0 entries</span>
          <button class="btn xs secondary" id="clearLogBtn">Clear</button>
        </div>
      </div>
    </div>
  </div>
    </main>
  </div><!-- app-layout -->
  
  <!-- Network Selection Modal -->
  <div id="networkModal" class="modal hide">
    <div class="modal-content">
      <div class="modal-header">
        <h3 class="modal-title">Network Selection</h3>
        <button class="modal-close" onclick="closeNetworkModal()">×</button>
      </div>
      <div id="networkList">
        <div class="muted text-center">Loading...</div>
      </div>
    </div>
  </div>
  
  <!-- Toast -->
  <div id="toast" class="toast hide"></div>
  
  <script>
let currentNetwork = null;
let statusInterval;
let statusSlowInterval;
let currentLogTab = 'live';
let currentTab = 'dashboard';
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

// Sidebar toggle (desktop)
function toggleSidebarDesktop() {
  const sidebar = $('sidebar');
  const mainContent = $('mainContent');
  if (!sidebar) return;
  
  sidebar.classList.toggle('collapsed');
  if (mainContent) mainContent.classList.toggle('expanded');
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

function switchLogTab(tab) {
  currentLogTab = tab;
  const tabs = document.querySelectorAll('.log-tab');
  tabs.forEach((t, i) => t.classList.toggle('active', (i === 0 && tab === 'live') || (i === 1 && tab === 'errors')));
  
  const monitor = $('monitor');
  const errorLog = $('errorLog');
  
  if (tab === 'live') {
    monitor.classList.remove('hide');
    errorLog.classList.add('hide');
  } else {
    monitor.classList.add('hide');
    errorLog.classList.remove('hide');
    refreshErrorLog();
  }
}

async function get(path) {
  const r = await fetch(path, {cache:'no-store'});
  return r.json();
}

async function getText(path) {
  const r = await fetch(path, {cache:'no-store'});
  return r.text();
}

async function post(path, data) {
  try {
    const r = await fetch(path, {method:'POST', body: new URLSearchParams(data)});
    if (!r.ok) {
      throw new Error('HTTP ' + r.status);
    }
    return r.json();
  } catch (e) {
    console.error('POST error:', path, e);
    throw e;
  }
}

function signalBars(rssi) {
  if (rssi >= -50) return '▂▄▆█';
  if (rssi >= -60) return '▂▄▆ ';
  if (rssi >= -70) return '▂▄  ';
  if (rssi >= -80) return '▂   ';
  return '    ';
}

async function refreshStatus() {
  try {
    const s = await get('/status');
    
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
        wifiConnected.classList.remove('hide');
        wifiSetup.classList.add('hide');
      } else {
        wifiBadge.className = 'badge danger';
        wifiBadge.textContent = 'Disconnected';
        wifiConnected.classList.add('hide');
        wifiSetup.classList.remove('hide');
      }
    }
    
    // Header status
    const statusDot = $('statusDot');
    if (statusDot) {
      statusDot.className = s.sta_connected ? 'status-dot ok' : 'status-dot warn';
    }
    
    // Config
    if ($('baseUrl')) $('baseUrl').value = s.base_url || '';
    if ($('apiPath')) $('apiPath').value = s.api_path || '/api/agent/incoming-sms';
    if ($('deviceId')) $('deviceId').value = s.device_id || '';
    
    // Stats
    if ($('statReceived')) $('statReceived').textContent = s.total_received || 0;
    if ($('statForwarded')) $('statForwarded').textContent = s.total_forwarded || 0;
    if ($('statFailed')) $('statFailed').textContent = s.total_failed || 0;
    if ($('statPending')) $('statPending').textContent = s.pending_sms || 0;
    
    // Heartbeat pause button
    const hbBtn = $('pauseHeartbeatBtn');
    if (hbBtn) {
      hbBtn.textContent = s.heartbeat_paused ? 'Resume HB' : 'Pause HB';
      hbBtn.className = s.heartbeat_paused ? 'btn xs warning' : 'btn xs secondary';
    }
    
    if ($('fwVersion')) $('fwVersion').textContent = s.firmware_version || '-';
    if ($('otaUrl') && s.ota_url && !$('otaUrl').value) $('otaUrl').value = s.ota_url;
    
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

    // Refresh SIM cards to show session timers
    refreshSims();

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

// Countdown timer - update every second
let sessionsCountdownInterval = null;
function startSessionsCountdown() {
  if (sessionsCountdownInterval) clearInterval(sessionsCountdownInterval);
  sessionsCountdownInterval = setInterval(() => {
    const sessions = window.activeSessionsData || [];
    if (sessions.length === 0) return;

    // Decrement each session's expiresIn
    sessions.forEach(s => {
      if (s.expiresIn > 0) s.expiresIn--;
    });

    // Remove expired sessions
    window.activeSessionsData = sessions.filter(s => s.expiresIn > 0);

    updateSessionsDisplay();
    updateSimCardTimers();
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

async function refreshMonitor() {
  try {
    const pre = $('monitor');
    if (!pre) return;
    const prev = pre.textContent;
    const next = await getText('/monitor');
    if (next !== prev) {
      const shouldStick = (pre.scrollTop + pre.clientHeight) >= (pre.scrollHeight - 10);
      pre.textContent = next;
      if (shouldStick) pre.scrollTop = pre.scrollHeight;
    }
    // Update log count
    const countEl = $('monitorCount');
    if (countEl) {
      const lines = next.split('\n').filter(l => l.trim()).length;
      countEl.textContent = lines + ' entries';
    }
  } catch(e) {}
}

async function refreshErrorLog() {
  try {
    const pre = $('errorLog');
    if (!pre) return;
    const next = await getText('/error-log');
    pre.textContent = next || 'No errors logged';
    pre.scrollTop = pre.scrollHeight;
    // Update count
    const countEl = $('errorLogCount');
    if (countEl) {
      const lines = next.split('\n').filter(l => l.trim()).length;
      countEl.textContent = lines + ' errors';
    }
  } catch(e) {
    if ($('errorLog')) $('errorLog').textContent = 'Error loading log';
  }
}

async function scanNetworks() {
  const btn = $('scanBtn');
  const list = $('scanResults');
  if (!btn || !list) return;
  
  btn.innerHTML = '<span class="spinner"></span> Scanning...';
  btn.disabled = true;
  
  try {
    const nets = await get('/scan');
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
        div.onclick = () => selectNetwork(n.ssid, n.secure);
        list.appendChild(div);
      });
    }
  } catch(e) {
    toast('Scan failed');
  }
  
  btn.textContent = 'Scan Networks';
  btn.disabled = false;
}

function selectNetwork(ssid, secure) {
  currentNetwork = ssid;
  const ssidInput = $('manualSsid');
  const pwInput = $('manualPw');
  if (ssidInput) ssidInput.value = ssid;
  if (pwInput) {
    pwInput.value = '';
    pwInput.focus();
  }
  
  // Highlight selected
  document.querySelectorAll('.net-item').forEach(el => el.classList.remove('selected'));
  if (event && event.currentTarget) event.currentTarget.classList.add('selected');
  
  if (!secure) {
    connectWifi();
  }
}

async function connectWifi() {
  const ssidInput = $('manualSsid');
  const pwInput = $('manualPw');
  const btn = $('connectBtn');
  
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
    const r = await post('/save-wifi', {ssid, password: pw});
    if (r.success) {
      toast('Connecting to ' + ssid + '...');
      setTimeout(refreshStatus, 3000);
    } else {
      toast(r.error || 'Connection failed');
    }
  } catch(e) {
    toast('Connection failed');
  }
  
  if (btn) {
    btn.textContent = 'Connect';
    btn.disabled = false;
  }
}

async function disconnectWifi() {
  await fetch('/disconnect');
  toast('Disconnected');
  setTimeout(refreshStatus, 1000);
}

async function refreshSims() {
  try {
    const s = await get('/sim-config');
    const grid = $('simGrid');
    if (!grid) return;
    grid.innerHTML = '';

    let activeCount = 0;

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

      // Extract signal strength from CSQ
      let rssi = null;
      let bars = 0;
      if (s.csq && s.csq[i]) {
        const csqMatch = s.csq[i].match(/\+CSQ:\s*(\d+)/);
        if (csqMatch) {
          rssi = parseInt(csqMatch[1]);
          if (rssi === 99) bars = 0;
          else if (rssi >= 20) bars = 4;
          else if (rssi >= 15) bars = 3;
          else if (rssi >= 10) bars = 2;
          else if (rssi >= 5) bars = 1;
        }
      }

      // Extract network from COPS
      let network = '-';
      if (s.cops && s.cops[i]) {
        const copsMatch = s.cops[i].match(/"([^"]+)"/);
        if (copsMatch) network = copsMatch[1];
      }

      const num = s.numbers[i] || '-';
      const isResponsive = s.responsive && s.responsive[i];
      const isBackendReg = s.backend_registered && s.backend_registered[i];
      const isEnabled = s.enabled[i];

      if (isEnabled && isResponsive) activeCount++;

      // Card classes
      let cardClass = 'sim-card';
      if (!isEnabled) cardClass += ' disabled';
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
      const copyBtn = num !== '-' ? `<button class="copy-btn" onclick="copyText(this, '${num}')" title="Copy number"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg></button>` : '';

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

      div.className = cardClass;
      div.innerHTML = `
        <div class="sim-header">
          <span class="sim-slot">SIM ${i + 1}</span>
          <div class="${statusClass}"></div>
        </div>
        <div class="sim-number">${num} ${copyBtn}</div>
        <div class="sim-meta">
          ${signalHtml}
          <span title="Signal strength">${signalText}</span>
          <span>${network}</span>
          ${battery >= 0 ? `<span>BAT ${battery}%</span>` : ''}
        </div>
        ${sessionHtml}
        <div class="sim-actions">
          <button class="btn xs ${isEnabled ? 'danger' : 'success'}" onclick="toggleSim(${i})">${isEnabled ? 'Off' : 'On'}</button>
          ${isBackendReg ? '<span class="badge primary">Reg</span>' : (isResponsive && isEnabled && num !== '-') ? `<button class="btn xs secondary" id="regSimBtn${i}" onclick="registerSimSlot(${i})">Reg</button>` : ''}
          ${isResponsive ? `<button class="btn xs secondary" onclick="showNetworkModal(${i})">Net</button>` : ''}
        </div>
      `;
      grid.appendChild(div);
    }

    // Update active count
    $('simCount').textContent = activeCount + ' active';
  } catch(e) {}
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
async function refreshSmsList() {
  try {
    const data = await getText('/messages');
    const list = $('smsList');
    const lines = data.split('\n').filter(l => l.trim());
    
    // Parse messages (format: timestamp|sim|number|sender|message)
    const messages = [];
    for (const line of lines) {
      const parts = line.split('|');
      if (parts.length >= 5) {
        messages.push({
          time: parts[0],
          slot: parseInt(parts[1]),
          number: parts[2],
          sender: parts[3],
          body: parts.slice(4).join('|')  // Message may contain |
        });
      }
    }
    
    // Update badge
    $('msgBadge').textContent = messages.length;
    $('smsCount').textContent = messages.length + ' messages';
    
    if (messages.length === 0) {
      list.innerHTML = `
        <div class="empty-state">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg>
          <p>No messages yet</p>
          <p class="text-sm">Received SMS will appear here</p>
        </div>
      `;
      return;
    }
    
    list.innerHTML = '';
    messages.reverse().forEach((msg, idx) => {
      const div = document.createElement('div');
      div.className = 'sms-item';
      div.innerHTML = `
        <div class="sms-header">
          <span class="sms-sender">${msg.sender || 'Unknown'}</span>
          <span class="sms-slot">SIM ${msg.slot} • ${msg.number || '-'}</span>
        </div>
        <div class="sms-time">${msg.time}</div>
        <div class="sms-body">${msg.body}</div>
        <div class="sms-actions">
          <button class="sms-copy-btn" onclick="copyText(\`${msg.body.replace(/`/g, '\\`')}\`)">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>
            Copy Message
          </button>
          <button class="sms-copy-btn" onclick="copyText('${msg.sender}')">
            Copy Sender
          </button>
        </div>
      `;
      list.appendChild(div);
    });
  } catch(e) {
    console.error('Failed to load SMS list', e);
  }
}

async function checkAllSims() {
  $('checkAllBtn').innerHTML = '<span class="spinner"></span>';
  try {
    const r = await get('/check-all-sim');
    await refreshSims();
  } catch(e) {
    toast('Refresh failed');
  }
  $('checkAllBtn').textContent = 'Refresh';
}

async function togglePollingPause() {
  try {
    const r = await post('/toggle-polling', {});
    if (r.success) {
      toast(r.message || 'Toggled');
      const btn = $('pausePollingBtn');
      if (r.paused) {
        btn.textContent = 'Resume SMS';
        btn.className = 'btn xs warning';
      } else {
        btn.textContent = 'Pause SMS';
        btn.className = 'btn xs secondary';
      }
    } else {
      toast(r.error || 'Failed');
    }
  } catch(e) {
    toast('Failed to toggle');
  }
}

async function toggleHeartbeatPause() {
  try {
    const r = await post('/toggle-heartbeat', {});
    if (r.success) {
      toast(r.message || 'Toggled');
      const btn = $('pauseHeartbeatBtn');
      if (r.paused) {
        btn.textContent = 'Resume HB';
        btn.className = 'btn xs warning';
      } else {
        btn.textContent = 'Pause HB';
        btn.className = 'btn xs secondary';
      }
    } else {
      toast(r.error || 'Failed');
    }
  } catch(e) {
    toast('Failed to toggle');
  }
}

async function toggleSim(idx) {
  const s = await get('/sim-config');
  const newEnabled = !s.enabled[idx];
  const r = await post('/sim-enable', { slot: idx, enabled: newEnabled ? 1 : 0 });
  if (r.success) {
    toast('SIM ' + idx + ' ' + (newEnabled ? 'enabled' : 'disabled'));
    await refreshSims();
  } else {
    toast('Failed: ' + (r.error || 'Unknown error'));
  }
}

// Network selection modal
let currentNetSimIdx = -1;

function showNetworkModal(idx) {
  currentNetSimIdx = idx;
  const modal = $('networkModal');
  modal.classList.remove('hide');
  scanCellNetworks(); // Auto-scan when opening
}

function closeNetworkModal() {
  $('networkModal').classList.add('hide');
  currentNetSimIdx = -1;
}

async function scanCellNetworks() {
  if (currentNetSimIdx < 0) return;
  
  const list = $('networkList');
  list.innerHTML = '<div class="muted text-center"><span class="spinner"></span><br>Scanning... (10-20s)</div>';
  
  try {
    const r = await post('/scan-networks', { slot: currentNetSimIdx });
    
    if (!r.success) {
      list.innerHTML = '<div class="muted" style="color:var(--danger)">Error: ' + (r.error || 'Scan failed') + '</div>';
      return;
    }
    
    if (!r.networks || r.networks.length === 0) {
      list.innerHTML = '<div class="muted text-center">No networks found</div>';
      return;
    }
    
    list.innerHTML = '';
    r.networks.forEach(n => {
      const div = document.createElement('div');
      div.className = 'net-item' + (n.current ? ' selected' : '');
      div.innerHTML = `
        <div class="net-info">
          <span class="net-name">${n.name}</span>
          <span class="net-signal">${n.current ? 'Current' : (n.status === 1 ? 'Available' : (n.status === 3 ? 'Forbidden' : 'Unknown'))}</span>
        </div>
        <span class="badge ${n.current ? 'success' : 'muted'}">${n.current ? 'Current' : 'Select'}</span>
      `;
      if (!n.current) {
        div.onclick = () => selectNetwork(n.name);
      }
      list.appendChild(div);
    });
  } catch(e) {
    list.innerHTML = '<div class="muted" style="color:var(--danger)">Connection error</div>';
  }
}

async function selectNetwork(networkName) {
  if (currentNetSimIdx < 0) return;
  
  const list = $('networkList');
  list.innerHTML = '<div class="muted text-center"><span class="spinner"></span><br>Selecting ' + networkName + '...</div>';
  
  try {
    const r = await post('/select-network', { slot: currentNetSimIdx, network: networkName });
    
    if (r.success) {
      toast('Network selected: ' + networkName);
      closeNetworkModal();
      await refreshSims();
    } else {
      list.innerHTML = '<div class="muted" style="color:var(--danger)">Error: ' + (r.error || 'Selection failed') + '</div>';
    }
  } catch(e) {
    list.innerHTML = '<div class="muted" style="color:var(--danger)">Connection error</div>';
  }
}

async function setAutoNetwork() {
  if (currentNetSimIdx < 0) return;
  
  const list = $('networkList');
  list.innerHTML = '<div class="muted text-center"><span class="spinner"></span><br>Setting auto mode...</div>';
  
  try {
    const r = await post('/select-network', { slot: currentNetSimIdx, network: '' });
    
    if (r.success) {
      toast('Auto network mode enabled');
      closeNetworkModal();
      await refreshSims();
    } else {
      toast('Setting auto mode...');
      closeNetworkModal();
    }
  } catch(e) {
    list.innerHTML = '<div class="muted" style="color:var(--danger)">Connection error</div>';
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

async function testPush() {
  const r = await get('/test-push');
  toast(r.success ? 'Push OK' : r.error);
}

async function clearLog() {
  if (currentLogTab === 'live') {
    await fetch('/clear-monitor');
    $('monitor').textContent = '';
    $('monitorCount').textContent = '0 entries';
  } else {
    await fetch('/clear-error-log');
    $('errorLog').textContent = 'No errors logged';
    $('errorLogCount').textContent = '0 errors';
  }
  toast('Log cleared');
}

// Auth functions
async function doLogin() {
  const email = $('loginEmail').value.trim();
  const password = $('loginPassword').value;
  
  if (!email || !password) {
    toast('Enter email and password');
    return;
  }
  
  $('loginBtn').innerHTML = '<span class="spinner"></span> Logging in...';
  $('loginBtn').disabled = true;
  
  try {
    // Send as JSON, not URLSearchParams
    const res = await fetch('/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email, password })
    });
    const r = await res.json();
    
    if (r.success) {
      toast('Logged in successfully');
      refreshStatus();
      checkAuthStatus();
    } else {
      toast(r.error || 'Login failed');
    }
  } catch(e) {
    toast('Login failed');
    console.error('Login error:', e);
  }
  
  $('loginBtn').textContent = 'Login';
  $('loginBtn').disabled = false;
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

async function doRegisterSim() {
  // Kept for backward compatibility (old button removed)
  await doRegisterAllSims();
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

async function doRegisterAllSims() {
  $('registerAllSimsBtn').innerHTML = '<span class="spinner"></span>';
  $('registerAllSimsBtn').disabled = true;
  try {
    const s = await get('/sim-config');
    for (let i = 0; i < s.enabled.length; i++) {
      if (!s.enabled[i]) continue;
      if (!s.responsive || !s.responsive[i]) continue;
      if (!s.numbers || !s.numbers[i] || s.numbers[i] === '-') continue;
      if (s.backend_registered && s.backend_registered[i]) continue;
      await registerSimSlot(i);
      await new Promise(r => setTimeout(r, 500));
    }
  } catch(e) {
    toast('Register all failed');
  }
  $('registerAllSimsBtn').textContent = 'Register All SIMs';
  $('registerAllSimsBtn').disabled = false;
}

async function checkAuthStatus() {
  try {
    const s = await get('/status');
    const hasToken = s.bearer_token && s.bearer_token.length > 0;

    // Switch between login page and dashboard
    const loginPage = $('loginPage');
    const dashboardPage = $('dashboardPage');
    
    if (hasToken) {
      if (loginPage) loginPage.classList.add('hide');
      if (dashboardPage) dashboardPage.classList.remove('hide');
      
      // Update auth badge
      const authBadge = $('authBadge');
      if (authBadge) {
        authBadge.className = 'badge success';
        authBadge.textContent = 'Logged In';
      }
      if ($('deviceBadge')) {
        $('deviceBadge').textContent = s.device_registered ? 'Registered' : 'Not Registered';
        $('deviceBadge').className = s.device_registered ? 'badge success' : 'badge warning';
      }
      
      // Heartbeat pause button
      const hbBtn = $('pauseHeartbeatBtn');
      if (hbBtn) {
        hbBtn.textContent = s.heartbeat_paused ? 'Resume HB' : 'Pause HB';
        hbBtn.className = s.heartbeat_paused ? 'btn xs warning' : 'btn xs secondary';
      }
      
      // Count registered SIMs
      try {
        const simConfig = await get('/sim-config');
        const regCount = simConfig.backend_registered ? simConfig.backend_registered.filter(Boolean).length : 0;
        if ($('simsRegCount')) $('simsRegCount').textContent = regCount + ' / ' + simConfig.enabled.length;
      } catch(e) {}
    } else {
      if (loginPage) loginPage.classList.remove('hide');
      if (dashboardPage) dashboardPage.classList.add('hide');
      
      // Update login page WiFi status
      updateLoginWifiStatus(s);
    }
  } catch(e) {
    console.error('Auth check error:', e);
  }
}

// Login page WiFi status
function updateLoginWifiStatus(s) {
  const badge = $('loginWifiBadge');
  const connected = $('loginWifiConnected');
  const setup = $('loginWifiSetup');
  
  if (!badge || !connected || !setup) return;
  
  if (s.sta_connected) {
    badge.className = 'badge success';
    badge.textContent = 'Connected';
    if ($('loginWifiSsid')) $('loginWifiSsid').textContent = s.sta_ssid || s.sta_ip;
    connected.classList.remove('hide');
    setup.classList.add('hide');
  } else {
    badge.className = 'badge danger';
    badge.textContent = 'Disconnected';
    connected.classList.add('hide');
    setup.classList.remove('hide');
  }
}

// Login page WiFi scan
async function loginScanNetworks() {
  const btn = $('loginScanBtn');
  const list = $('loginScanResults');
  if (!btn || !list) return;
  
  btn.innerHTML = '<span class="spinner"></span> Scanning...';
  btn.disabled = true;
  
  try {
    const nets = await get('/scan');
    list.innerHTML = '';
    
    if (nets.length === 0) {
      list.innerHTML = '<div class="muted text-center" style="padding:20px;">No networks found</div>';
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
        div.onclick = () => {
          const ssidInput = $('loginManualSsid');
          const pwInput = $('loginManualPw');
          if (ssidInput) ssidInput.value = n.ssid;
          if (pwInput) pwInput.focus();
        };
        list.appendChild(div);
      });
    }
  } catch(e) {
    toast('Scan failed');
  }
  
  if (btn) {
    btn.textContent = 'Scan Networks';
    btn.disabled = false;
  }
}

// Login page WiFi connect
async function loginConnectWifi() {
  const ssidInput = $('loginManualSsid');
  const pwInput = $('loginManualPw');
  const btn = $('loginConnectBtn');
  
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
    const r = await post('/save-wifi', { ssid: ssid, password: pw });
    if (r.success) {
      toast('Connected! Refreshing...');
      setTimeout(() => location.reload(), 2000);
    } else {
      toast(r.error || 'Connection failed');
    }
  } catch(e) {
    toast('Connection failed');
  }
  
  if (btn) {
    btn.textContent = 'Connect';
    btn.disabled = false;
  }
}

async function saveOtaUrl() {
  const url = ($('otaUrl')?.value || '').trim();
  const st = $('fwStatus');
  const btn = $('saveOtaUrlBtn');
  if (btn) btn.disabled = true;
  try {
    const r = await post('/firmware-config', { url });
    if (st) st.textContent = r.success ? 'Firmware URL saved.' : (r.message || 'Save failed');
  } catch (e) {
    if (st) st.textContent = 'Save failed: ' + e.message;
  }
  if (btn) btn.disabled = false;
}

async function checkFirmware() {
  const st = $('fwStatus');
  const btn = $('checkFwBtn');
  if (btn) btn.disabled = true;
  if (st) st.textContent = 'Checking for updates...';
  try {
    const r = await get('/firmware-check');
    if ($('fwRemoteVersion')) $('fwRemoteVersion').textContent = r.remote || '-';
    if ($('fwRemoteWrap')) $('fwRemoteWrap').classList.toggle('hide', !r.remote);
    if (r.success) {
      if (r.update_available) {
        if (st) st.textContent = 'Update available: v' + r.remote + ' (installed v' + r.current + ')';
      } else if (r.remote) {
        if (st) st.textContent = 'Up to date (v' + r.current + ')';
      } else {
        if (st) st.textContent = 'Could not read remote version file.';
      }
    } else {
      if (st) st.textContent = r.message || 'Check failed';
    }
  } catch (e) {
    if (st) st.textContent = 'Check failed: ' + e.message;
  }
  if (btn) btn.disabled = false;
}

async function installFirmware() {
  if (!confirm('Download and install firmware? SMS polling will stop and the device will restart.')) return;
  const st = $('fwStatus');
  const btn = $('updateFwBtn');
  const url = ($('otaUrl')?.value || '').trim();
  if (btn) btn.disabled = true;
  if (st) st.textContent = 'Starting OTA download...';
  try {
    const body = url ? { url } : {};
    const r = await post('/firmware-update', body);
    if (st) st.textContent = r.message || 'OTA started';
  } catch (e) {
    if (st) st.textContent = 'OTA failed: ' + e.message;
    if (btn) btn.disabled = false;
  }
}

// Event listeners - Dashboard (with null checks)
function addClick(id, fn) { const el = $(id); if (el) el.onclick = fn; }

addClick('scanBtn', scanNetworks);
addClick('connectBtn', connectWifi);
addClick('disconnectBtn', disconnectWifi);
addClick('checkAllBtn', checkAllSims);
addClick('pausePollingBtn', togglePollingPause);
addClick('pauseHeartbeatBtn', toggleHeartbeatPause);
addClick('saveConfigBtn', saveConfig);
addClick('saveOtaUrlBtn', saveOtaUrl);
addClick('checkFwBtn', checkFirmware);
addClick('updateFwBtn', installFirmware);
addClick('clearLogBtn', clearLog);
addClick('clearErrorLogBtn', function() { currentLogTab = 'errors'; clearLog(); });
addClick('resetDeviceBtn', resetDevice);
addClick('logoutBtn', doLogout);
addClick('registerDeviceBtn', doRegisterDevice);
addClick('registerAllSimsBtn', doRegisterAllSims);
addClick('refreshSmsBtn', refreshSmsList);

// Event listeners - Login page
addClick('loginBtn', doLogin);
addClick('loginScanBtn', loginScanNetworks);
addClick('loginConnectBtn', loginConnectWifi);

// Tab persistence with URL hash
function getTabFromHash() {
  const hash = window.location.hash.slice(1);
  return ['dashboard', 'sims', 'messages', 'settings', 'logs'].includes(hash) ? hash : 'dashboard';
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
if (initialTab !== 'dashboard') {
  switchTab(initialTab);
}

refreshStatus();
refreshSims();
checkAuthStatus();
refreshSmsList();
startSessionsCountdown();
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
    server.on("/clear-monitor", HTTP_GET, handleClearMonitor);
    server.on("/error-log", HTTP_GET, handleErrorLog);
    server.on("/clear-error-log", HTTP_GET, handleClearErrorLog);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/save-wifi", HTTP_POST, handleSaveWifi);
    server.on("/disconnect", HTTP_GET, handleDisconnect);
    server.on("/sim-config", HTTP_GET, handleSimConfig);
    server.on("/check-sim", HTTP_GET, handleCheckSim);
    server.on("/check-all-sim", HTTP_GET, handleCheckAllSim);
    server.on("/sim-enable", HTTP_POST, handleSimEnable);
    server.on("/calls", HTTP_GET, handleCalls);
    server.on("/clear-calls", HTTP_GET, handleClearCalls);
    server.on("/call", HTTP_POST, handleCall);
    server.on("/hangup", HTTP_POST, handleHangup);
    server.on("/send-sms", HTTP_POST, handleSendSms);
    server.on("/agent-config", HTTP_POST, handleAgentConfig);
    server.on("/test-push", HTTP_GET, handleTestPush);
    server.on("/battery", HTTP_GET, handleBattery);
    server.on("/login", HTTP_POST, handleLogin);
    server.on("/logout", HTTP_GET, handleLogout);
    server.on("/refresh-token", HTTP_POST, handleRefreshToken);
    server.on("/register-device", HTTP_POST, handleRegisterDevice);
    server.on("/register-sim", HTTP_POST, handleRegisterSim);
    server.on("/scan-networks", HTTP_POST, handleScanNetworks);
    server.on("/select-network", HTTP_POST, handleSelectNetwork);
    server.on("/heartbeat", HTTP_POST, handleHeartbeatManual);
    server.on("/toggle-polling", HTTP_POST, handleTogglePolling);
    server.on("/toggle-heartbeat", HTTP_POST, handleToggleHeartbeat);
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
    char buf[1536];
    buildStatusJson(buf, sizeof(buf));
    server.send(200, "application/json", buf);
}

void buildStatusJson(char* buf, size_t bufSize) {
    // Get WiFi status
    bool staConnected = WiFi.isConnected();
    IPAddress staIp = WiFi.localIP();
    IPAddress apIp = WiFi.softAPIP();
    
    // Get uptime
    unsigned long uptimeS = millis() / 1000;
    
    // Count pending SMS
    int pendingSms = getPendingSmsCount();
    
    // Check if bearer token is set (don't expose full token)
    bool hasBearerToken = !charBufIsEmpty(agentBearerToken);
    
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
    
    // Get statistics
    unsigned long totalReceived = getTotalSmsReceived();
    unsigned long totalForwarded = getTotalSmsForwarded();
    unsigned long totalFailed = getTotalSmsFailed();
    
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
    
    snprintf(buf, bufSize,
        "{"
        "\"sta_connected\":%s,"
        "\"sta_ip\":\"%s\","
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
        "\"total_received\":%lu,"
        "\"total_forwarded\":%lu,"
        "\"total_failed\":%lu,"
        "\"base_url\":\"%s\","
        "\"api_path\":\"%s\","
        "\"device_id\":\"%s\","
        "\"bearer_token\":\"%s\","
        "\"heartbeat_paused\":%s,"
        "\"firmware_version\":\"%s\","
        "\"ota_url\":\"%s\""
        "%s"
        "}",
        staConnected ? "true" : "false",
        staConnected ? staIp.toString().c_str() : "",
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
        totalReceived,
        totalForwarded,
        totalFailed,
        agentBaseUrl,
        agentApiPath,
        agentDeviceId,
        hasBearerToken ? "(set)" : "",
        heartbeatPaused ? "true" : "false",
        FIRMWARE_VERSION,
        otaFirmwareUrl,
        sessionsBuf
    );
}

void handleMonitor() {
    static char buf[2048];  // Reduced from 4096
    getMonitorLogText(buf, sizeof(buf));
    server.send(200, "text/plain", buf);
}

void handleMessages() {
    static char buf[4096];  // Increased to fit more messages
    int count = readSmsFromFile(buf, sizeof(buf), 50);  // Last 50 messages
    server.send(200, "text/plain", buf);
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
    int n = WiFi.scanNetworks();
    
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
    
    // Connect
    WiFi.begin(wifiSsid, wifiPassword);
    
    logMsg2Val("[WIFI] Saved", wifiSsid, "connecting", "...");
    appendMonitorLogVal("[WIFI] Connect ", wifiSsid);
    sendJsonSuccess("WiFi saved, connecting...");
}

void handleDisconnect() {
    WiFi.disconnect(true);
    logMsg("[WIFI] Disconnected");
    appendMonitorLog("[WIFI] Disconnected");
    sendJsonSuccess("Disconnected");
}

// -----------------------------------------------------------------------------
// Route Handlers - SIM Management
// -----------------------------------------------------------------------------

void handleSimConfig() {
    static char buf[3072];  // Need enough for 16 SIMs config
    buildSimConfigJson(buf, sizeof(buf));
    server.send(200, "application/json", buf);
}

void buildSimConfigJson(char* buf, size_t bufSize) {
    size_t pos = 0;
    
    pos += snprintf(buf + pos, bufSize - pos,
        "{\"success\":true,"
        "\"active_slot\":%d,"
        "\"selected_mux_slot\":%d,"
        "\"enabled\":[",
        getCurrentLogicalSlot(),
#if !USE_DUAL_UART
        logicalSlotToMuxChannel(getCurrentLogicalSlot())
#else
        getCurrentLogicalSlot()
#endif
    );
    
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        pos += snprintf(buf + pos, bufSize - pos, "%s", simStates[i].enabled ? "true" : "false");
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "],\"responsive\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        pos += snprintf(buf + pos, bufSize - pos, "%s", simStates[i].responsive ? "true" : "false");
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "],\"registered\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        pos += snprintf(buf + pos, bufSize - pos, "%s", simStates[i].registered ? "true" : "false");
    }

    pos += snprintf(buf + pos, bufSize - pos, "],\"backend_registered\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        pos += snprintf(buf + pos, bufSize - pos, "%s", simStates[i].backendRegistered ? "true" : "false");
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "],\"numbers\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        char escaped[64];
        jsonEscape(simStates[i].number, escaped, sizeof(escaped));
        pos += snprintf(buf + pos, bufSize - pos, "%s", escaped);
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "],\"creg\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        char escaped[64];
        jsonEscape(simStates[i].creg, escaped, sizeof(escaped));
        pos += snprintf(buf + pos, bufSize - pos, "%s", escaped);
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "],\"cops\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        char escaped[80];
        jsonEscape(simStates[i].cops, escaped, sizeof(escaped));
        pos += snprintf(buf + pos, bufSize - pos, "%s", escaped);
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "],\"csq\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        char escaped[32];
        jsonEscape(simStates[i].csq, escaped, sizeof(escaped));
        pos += snprintf(buf + pos, bufSize - pos, "%s", escaped);
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "],\"battery_pct\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        pos += snprintf(buf + pos, bufSize - pos, "%d", simStates[i].batteryPercent);
    }

    pos += snprintf(buf + pos, bufSize - pos, "],\"battery_mv\":[");
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufSize - pos, ",");
        pos += snprintf(buf + pos, bufSize - pos, "%d", simStates[i].batteryMv);
    }
    
    pos += snprintf(buf + pos, bufSize - pos, "]}");
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

void handleSimEnable() {
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : 0;
    int simIdx = slot;
    bool enabled = server.arg("enabled") == "1" || server.arg("enabled") == "true";
    
    if (simIdx < 0 || simIdx >= SIM_COUNT) {
        sendJsonError("Invalid SIM slot");
        return;
    }
    
    if (!enabled) {
        simStates[simIdx].enabled = false;
        // Clear state
        simStates[simIdx].responsive = false;
        simStates[simIdx].registered = false;
        simStates[simIdx].backendRegistered = false;
        simStates[simIdx].basicInitDone = false;
        charBufClear(simStates[simIdx].number, sizeof(simStates[simIdx].number));
        charBufClear(simStates[simIdx].creg, sizeof(simStates[simIdx].creg));
        charBufClear(simStates[simIdx].cops, sizeof(simStates[simIdx].cops));
        charBufClear(simStates[simIdx].csq, sizeof(simStates[simIdx].csq));
        simStates[simIdx].batteryPercent = 0;
        simStates[simIdx].batteryMv = 0;
    } else {
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
        const String devId = server.arg("device_id");
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

void handleTestPush() {
    // Test HTTP push to backend
    if (charBufIsEmpty(agentBaseUrl)) {
        sendJsonError("No base URL configured");
        return;
    }
    
    HTTPClient http;
    char url[256];
    snprintf(url, sizeof(url), "%s%s", agentBaseUrl, agentApiPath);
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    if (!charBufIsEmpty(agentBearerToken)) {
        http.addHeader("Authorization", String("Bearer ") + agentBearerToken);
    }
    
    char body[256];
    snprintf(body, sizeof(body), "{\"test\":true,\"device_id\":\"%s\"}", agentDeviceId);
    
    int code = http.POST(body);
    http.end();
    
    if (code > 0 && code < 400) {
        logMsgInt("[TEST] Push OK, code", code);
        sendJsonSuccess();
    } else {
        logMsgInt("[TEST] Push failed, code", code);
        sendJsonError("Push failed");
    }
}

// -----------------------------------------------------------------------------
// Route Handlers - Battery
// -----------------------------------------------------------------------------

void handleBattery() {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"success\":true,\"percent\":%d,\"mv\":%d}",
        batteryPercent, batteryMv
    );
    server.send(200, "application/json", buf);
}

// -----------------------------------------------------------------------------
// Route Handlers - Login/Auth
// -----------------------------------------------------------------------------

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
    
    // Wait for other HTTPS operations to finish
    if (httpsBusy) {
        sendJsonError("HTTPS busy, try again");
        return;
    }
    httpsBusy = true;
    
    logMsg2Val("[AUTH] URL", url, "", "");
    
    if (isHttps) {
        clientSecure.setInsecure();
        clientSecure.setTimeout(15000);  // 15 second timeout
        if (!http.begin(clientSecure, url)) {
            logMsg("[AUTH] HTTPS begin failed");
        appendMonitorLog("[AUTH] HTTPS begin failed");
            httpsBusy = false;
            sendJsonError("Failed to connect to server");
            return;
        }
    } else {
        if (!http.begin(client, url)) {
            logMsg("[AUTH] HTTP begin failed");
            appendMonitorLog("[AUTH] HTTP begin failed");
            httpsBusy = false;
            sendJsonError("Failed to connect to server");
            return;
        }
    }
    
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);
    
    int code = http.POST(payload);
    String resp = "";
    
    if (code > 0) {
        resp = http.getString();
    } else {
        logMsgInt("[AUTH] POST failed, code", code);
        appendMonitorLogInt("[AUTH] POST failed", code);
    }
    http.end();
    clientSecure.stop();
    client.stop();
    
    // Log full response for debugging
    logMsgInt("[AUTH] HTTP code", code);
    logMsg2Val("[AUTH] Body", resp.c_str(), "", "");
    
    // Check if response indicates success
    // Response format: {"success":true,"data":{"access_token":"...","refresh_token":"..."}}
    // Or error: {"success":false,"error":"..."}
    
    if (resp.length() == 0) {
        logMsg("[AUTH] Empty response");
        httpsBusy = false;
        sendJsonError("Empty response from server");
        return;
    }
    
    // Check for success flag (handle both "success":true and "success": true)
    bool success = (strstr(resp.c_str(), "\"success\":true") != NULL || 
                    strstr(resp.c_str(), "\"success\": true") != NULL);
    
    if (!success) {
        // Extract error message
        const char* errKey = "\"error\":\"";
        const char* errPos = strstr(resp.c_str(), errKey);
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
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}", errorMsg);
        server.send(200, "application/json", buf);
        return;
    }
    
    // Extract tokens from response - they're nested in "data"
    // Use static buffers to avoid stack overflow (JWT tokens are 700+ chars)
    static char accessToken[1024];
    static char refreshToken[512];
    accessToken[0] = '\0';
    refreshToken[0] = '\0';
    
    // Simple JSON extraction - look for access_token inside data object
    const char* atKey = "\"access_token\":\"";
    const char* rtKey = "\"refresh_token\":\"";
    
    const char* atPos = strstr(resp.c_str(), atKey);
    if (atPos) {
        atPos += strlen(atKey);
        const char* endQuote = strchr(atPos, '"');
        if (endQuote) {
            int len = endQuote - atPos;
            if (len < (int)sizeof(accessToken)) {
                strncpy(accessToken, atPos, len);
                accessToken[len] = '\0';
            }
        }
    }
    
    const char* rtPos = strstr(resp.c_str(), rtKey);
    if (rtPos) {
        rtPos += strlen(rtKey);
        const char* endQuote = strchr(rtPos, '"');
        if (endQuote) {
            int len = endQuote - rtPos;
            if (len < (int)sizeof(refreshToken)) {
                strncpy(refreshToken, rtPos, len);
                refreshToken[len] = '\0';
            }
        }
    }
    
    if (!charBufIsEmpty(accessToken)) {
        // Save tokens
        charBufSet(agentBearerToken, sizeof(agentBearerToken), accessToken);
        charBufSet(agentRefreshToken, sizeof(agentRefreshToken), refreshToken);
        
        preferences.begin("agent", false);
        preferences.putString("tok", agentBearerToken);
        preferences.putString("rtok", agentRefreshToken);
        preferences.end();
        
        logMsg("[AUTH] Login successful, tokens saved");
        appendMonitorLog("[AUTH] Login OK");
        
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"success\":true,\"message\":\"Logged in\",\"has_refresh\":%s}",
            charBufIsEmpty(refreshToken) ? "false" : "true"
        );
        httpsBusy = false;
        server.send(200, "application/json", buf);
        return;
    }
    
    // Login failed - no token found
    logMsg("[AUTH] No access_token in response");
    appendMonitorLog("[AUTH] No token in response");
    
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"success\":false,\"error\":\"No token in response\"}"
    );
    httpsBusy = false;
    server.send(200, "application/json", buf);
}

void handleLogout() {
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
    String resp = "";
    if (code > 0) {
        resp = http.getString();
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
    
    if (!(code >= 200 && code < 300) || resp.length() == 0) {
        logMsgInt("[AUTH] Token refresh failed, HTTP", code);
        if (resp.length() > 0 && resp.length() < 200) {
            logMsgVal("[AUTH] Response", resp.c_str());
        }
        appendMonitorLogInt("[AUTH] Refresh HTTP", code);
        resumeSmsPolling();
        return false;
    }
    
    static char accessToken[1024];
    static char refreshToken[512];
    accessToken[0] = '\0';
    refreshToken[0] = '\0';
    
    const char* atKey = "\"access_token\":\"";
    const char* rtKey = "\"refresh_token\":\"";
    
    const char* atPos = strstr(resp.c_str(), atKey);
    if (atPos) {
        atPos += strlen(atKey);
        const char* endQuote = strchr(atPos, '"');
        if (endQuote) {
            int len = (int)(endQuote - atPos);
            if (len > 0 && len < (int)sizeof(accessToken)) {
                strncpy(accessToken, atPos, (size_t)len);
                accessToken[len] = '\0';
            }
        }
    }
    
    const char* rtPos = strstr(resp.c_str(), rtKey);
    if (rtPos) {
        rtPos += strlen(rtKey);
        const char* endQuote = strchr(rtPos, '"');
        if (endQuote) {
            int len = (int)(endQuote - rtPos);
            if (len > 0 && len < (int)sizeof(refreshToken)) {
                strncpy(refreshToken, rtPos, (size_t)len);
                refreshToken[len] = '\0';
            }
        }
    }
    
    if (charBufIsEmpty(accessToken)) {
        appendMonitorLog("[AUTH] Refresh failed: no access_token");
        resumeSmsPolling();
        return false;
    }
    
    charBufSet(agentBearerToken, sizeof(agentBearerToken), accessToken);
    if (!charBufIsEmpty(refreshToken)) {
        charBufSet(agentRefreshToken, sizeof(agentRefreshToken), refreshToken);
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

void handleRegisterDevice() {
    // Register device with backend
    if (charBufIsEmpty(agentBaseUrl)) {
        sendJsonError("No base URL configured");
        return;
    }
    
    if (charBufIsEmpty(agentBearerToken)) {
        sendJsonError("Not logged in");
        return;
    }
    
    if (charBufIsEmpty(agentDeviceId)) {
        // Generate default device ID
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
        agentDeviceId, agentDeviceId
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
            http.end();
        }
    }
    
    if (code >= 200 && code < 300) {
        deviceRegistered = true;
        logMsg("[REGISTER] Device registered");
        appendMonitorLog("[REGISTER] Device registered");
        sendJsonSuccess("Device registered");
    } else {
        logMsgInt("[REGISTER] Device registration failed, code", code);
        appendMonitorLogInt("[REGISTER] Device failed", code);
        sendJsonError("Registration failed");
    }
    clientSecure.stop();
    client.stop();
    delay(50);
    httpsBusy = false;
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
// Network Selection Handlers
// -----------------------------------------------------------------------------

void handleScanNetworks() {
    // Scan available networks for a specific SIM slot
    if (!server.hasArg("slot")) {
        sendJsonError("Missing slot parameter");
        return;
    }
    
    int slot = server.arg("slot").toInt();
    int simIdx = slot - 1;
    
    if (simIdx < 0 || simIdx >= SIM_COUNT) {
        sendJsonError("Invalid SIM slot");
        return;
    }
    
    if (!simStates[simIdx].responsive) {
        sendJsonError("SIM not responsive");
        return;
    }
    
    logMsgInt("[NET] Scanning networks for SIM", slot);
    appendMonitorLogVal("[NET] Scan SIM ", String(slot).c_str());
    
    // Select the SIM
    selectSIM(simIdx);
    pauseSmsPolling(30000);
    setSimYieldToWebServer(false);
    
    // Send AT+COPS=? to scan networks (takes 10-15 seconds)
    sendATCapture("AT+COPS=?", 20000);
    setSimYieldToWebServer(true);
    
    char* buf = getSimBuffer();
    
    // Parse response: +COPS: [list],,(stat)
    // Example: +COPS: (2,"SMART","SMART","51503"), (2,"GLOBE","GLOBE","51502"),,0
    
    // Build JSON response
    static char json[1024];
    json[0] = '\0';
    strcat(json, "{\"success\":true,\"networks\":[");
    
    const char* start = strstr(buf, "+COPS:");
    bool first = true;
    
    if (start) {
        start += 6; // Skip "+COPS:"
        
        // Parse each network in parentheses
        const char* p = start;
        while (*p) {
            // Look for (stat,"long","short","numeric")
            const char* openParen = strchr(p, '(');
            if (!openParen) break;
            
            const char* closeParen = strchr(openParen, ')');
            if (!closeParen) break;
            
            // Extract the content between parentheses
            int len = (int)(closeParen - openParen - 1);
            if (len > 0 && len < 100) {
                static char netInfo[128];
                strncpy(netInfo, openParen + 1, (size_t)len);
                netInfo[len] = '\0';
                
                // Parse: stat,"long","short","numeric"
                // stat: 0=unknown, 1=available, 2=current, 3=forbidden
                int stat = 0;
                char longName[32] = "";
                char shortName[32] = "";
                
                // Simple parse: find quoted strings
                const char* q1 = strchr(netInfo, '"');
                if (q1) {
                    q1++;
                    const char* q2 = strchr(q1, '"');
                    if (q2 && (q2 - q1) < 32) {
                        strncpy(longName, q1, (size_t)(q2 - q1));
                        longName[q2 - q1] = '\0';
                    }
                }
                
                // Get status (first number before comma)
                stat = atoi(netInfo);
                
                if (strlen(longName) > 0) {
                    if (!first) strcat(json, ",");
                    first = false;
                    
                    char netJson[128];
                    snprintf(netJson, sizeof(netJson), 
                        "{\"name\":\"%s\",\"status\":%d,\"current\":%s}",
                        longName, stat, (stat == 2) ? "true" : "false");
                    strcat(json, netJson);
                }
            }
            
            p = closeParen + 1;
        }
    }
    
    strcat(json, "]}");
    
    logMsgVal("[NET] Scan result", json);
    server.send(200, "application/json", json);
}

void handleSelectNetwork() {
    // Manually select a network for a SIM slot
    if (!server.hasArg("slot")) {
        sendJsonError("Missing slot parameter");
        return;
    }
    
    int slot = server.arg("slot").toInt();
    String network = server.hasArg("network") ? server.arg("network") : "";
    int simIdx = slot - 1;
    
    if (simIdx < 0 || simIdx >= SIM_COUNT) {
        sendJsonError("Invalid SIM slot");
        return;
    }
    
    if (!simStates[simIdx].responsive) {
        sendJsonError("SIM not responsive");
        return;
    }
    
    // Select the SIM
    selectSIM(simIdx);
    pauseSmsPolling(30000);
    setSimYieldToWebServer(false);
    
    char cmd[64];
    
    // If network is empty, set auto mode (AT+COPS=0)
    if (network.length() == 0) {
        logMsgInt("[NET] Setting auto network mode for SIM", slot);
        appendMonitorLogVal("[NET] Auto SIM ", String(slot).c_str());
        strcpy(cmd, "AT+COPS=0");
    } else {
        logMsg2Val("[NET] Selecting network for SIM", String(slot).c_str(), "network", network.c_str());
        appendMonitorLogVal("[NET] Select ", network.c_str());
        snprintf(cmd, sizeof(cmd), "AT+COPS=1,0,\"%s\"", network.c_str());
    }
    
    sendATCapture(cmd, 15000);
    setSimYieldToWebServer(true);
    
    char* buf = getSimBuffer();
    
    // Check for OK or ERROR
    if (strstr(buf, "OK") != NULL) {
        logMsg("[NET] Network selected successfully");
        // Update COPS in state
        delay(2000); // Wait for registration
        sendATCapture("AT+COPS?", 1500);
        charBufSet(simStates[simIdx].cops, sizeof(simStates[simIdx].cops), getSimBuffer());
        sendJsonSuccess("Network selected");
    } else if (strstr(buf, "ERROR") != NULL || strstr(buf, "+CME ERROR") != NULL) {
        logMsgVal("[NET] Selection failed", buf);
        sendJsonError("Network selection failed - SIM may not allow this network");
    } else {
        logMsgVal("[NET] Selection response", buf);
        sendJsonError("Unknown response from modem");
    }
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

void handleFirmwareCheck() {
    bool updateAvailable = false;
    char remote[32];
    remote[0] = '\0';

    const bool ok = otaCheckForUpdate(&updateAvailable, remote, sizeof(remote));
    char buf[192];
    if (!ok) {
        snprintf(buf, sizeof(buf),
            "{\"success\":false,\"current\":\"%s\",\"message\":\"Could not fetch remote version\"}",
            FIRMWARE_VERSION);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"success\":true,\"current\":\"%s\",\"remote\":\"%s\",\"update_available\":%s}",
            FIRMWARE_VERSION,
            remote,
            updateAvailable ? "true" : "false");
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
    if (!WiFi.isConnected()) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"WiFi required for OTA\"}");
        return;
    }

    const char* url = server.hasArg("url") ? server.arg("url").c_str() : nullptr;
    if (url && url[0]) {
        otaSaveUrlToPreferences(url);
    }

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
