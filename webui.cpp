// ============================================================================
// Web UI Server Implementation
// ============================================================================

#include "webui.h"
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

// Web server on port 80
WebServer server(80);

static volatile bool refreshAllInProgress = false;

// Preferences for storing settings (defined in main .ino)
extern Preferences preferences;

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

// HTML page stored in PROGMEM
static const char INDEX_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html>
<head>
  <title>SIM800 Gateway</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    :root { --bg:#0b1220; --card:#0f172a; --muted:#94a3b8; --text:#e5e7eb; --accent:#2563eb; --ok:#16a34a; --warn:#f59e0b; --bad:#dc2626; }
    * { box-sizing: border-box; }
    body { margin:0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: linear-gradient(180deg,#0b1220,#060a13); color: var(--text); padding: 12px; }
    .wrap { max-width: 500px; margin: 0 auto; }
    .card { background: rgba(15,23,42,0.95); border: 1px solid rgba(148,163,184,0.15); border-radius: 12px; padding: 14px; margin-bottom: 12px; }
    h1 { font-size: 18px; margin: 0 0 4px 0; }
    h2 { font-size: 14px; margin: 0 0 10px 0; color: #94a3b8; font-weight: 500; }
    .row { display:flex; gap:8px; align-items:center; justify-content:space-between; flex-wrap:wrap; }
    .btn { background: var(--accent); color: white; border: none; padding: 10px 14px; border-radius: 8px; cursor: pointer; font-size: 14px; font-weight: 500; }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .btn.gray { background: rgba(148,163,184,0.25); }
    .btn.sm { padding: 6px 10px; font-size: 12px; }
    .btn.bad { background: var(--bad); }
    .btn.ok { background: var(--ok); }
    .btn.full { width: 100%; }
    .btn.red { background: var(--bad); }
    input { width: 100%; padding: 10px; border-radius: 8px; border: 1px solid rgba(148,163,184,0.2); background: rgba(0,0,0,0.3); color: var(--text); font-size: 14px; }
    input:focus { outline: none; border-color: var(--accent); }
    select { width: 100%; padding: 10px; border-radius: 8px; border: 1px solid rgba(148,163,184,0.2); background: rgba(0,0,0,0.3); color: var(--text); }
    label { font-size: 12px; color: #94a3b8; display:block; margin-bottom: 4px; }
    .grid { display:grid; gap: 8px; }
    .muted { color: var(--muted); font-size: 11px; }
    .badge { display:inline-block; padding: 4px 10px; border-radius: 20px; font-size: 11px; font-weight: 500; }
    .badge.ok { background: rgba(22,163,74,0.2); color: #4ade80; }
    .badge.bad { background: rgba(220,38,38,0.2); color: #f87171; }
    .badge.warn { background: rgba(245,158,11,0.2); color: #fbbf24; }
    pre { background: rgba(0,0,0,0.3); border-radius: 8px; padding: 10px; max-height: 200px; overflow:auto; font-size: 11px; white-space: pre-wrap; font-family: 'SF Mono', Consolas, monospace; }
    .hide { display:none !important; }
    .net-item { display:flex; align-items:center; justify-content:space-between; padding: 10px; border-radius: 8px; border: 1px solid rgba(148,163,184,0.1); background: rgba(0,0,0,0.2); margin-bottom: 6px; cursor: pointer; }
    .net-item:hover { border-color: var(--accent); background: rgba(37,99,235,0.1); }
    .net-item.selected { border-color: var(--accent); background: rgba(37,99,235,0.15); }
    .net-info { display:flex; flex-direction:column; }
    .net-name { font-weight: 500; }
    .net-signal { font-size: 11px; color: var(--muted); }
    .net-icon { font-size: 18px; }
    .sim-item { display:flex; align-items:center; justify-content:space-between; padding: 8px; border-radius: 6px; border: 1px solid rgba(148,163,184,0.1); background: rgba(0,0,0,0.2); margin-bottom: 4px; }
    .sim-item.disabled { opacity: 0.5; }
    .sim-row { display: flex; flex-direction: column; gap: 2px; }
    .sim-main { display: flex; align-items: center; justify-content: space-between; gap: 8px; flex-wrap: wrap; }
    .sim-left { display:flex; align-items:center; gap: 8px; flex-wrap: wrap; }
    .sim-actions { display:flex; align-items:center; gap: 6px; flex-wrap: wrap; }
    .sim-num { font-family: monospace; color: var(--accent); }
    .sig { display:inline-flex; align-items:flex-end; gap:2px; height: 12px; margin-right: 6px; }
    .sig i { display:inline-block; width:3px; background: rgba(148,163,184,0.35); border-radius:2px; }
    .sig i:nth-child(1) { height: 3px; }
    .sig i:nth-child(2) { height: 5px; }
    .sig i:nth-child(3) { height: 8px; }
    .sig i:nth-child(4) { height: 11px; }
    .sig.b1 i:nth-child(1),
    .sig.b2 i:nth-child(-n+2),
    .sig.b3 i:nth-child(-n+3),
    .sig.b4 i:nth-child(-n+4) { background: rgba(34,197,94,0.85); }
    .sim-info { font-size: 11px; }
    .btn-sm { font-size: 10px; padding: 2px 6px; border-radius: 4px; background: var(--accent); color: #000; border: none; cursor: pointer; }
    .btn-sm:hover { opacity: 0.8; }
    .toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); background: rgba(15,23,42,0.95); border: 1px solid var(--accent); border-radius: 8px; padding: 12px 20px; z-index: 9999; max-width: 90%; text-align: center; }
    .spinner { display: inline-block; width: 14px; height: 14px; border: 2px solid rgba(255,255,255,0.3); border-top-color: #fff; border-radius: 50%; animation: spin 0.8s linear infinite; }
    @keyframes spin { to { transform: rotate(360deg); } }
    .section { margin-bottom: 16px; }
    .divider { height: 1px; background: rgba(148,163,184,0.1); margin: 12px 0; }
    .modal { position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.7); display: flex; align-items: center; justify-content: center; z-index: 1000; }
    .modal-content { background: var(--card); border: 1px solid rgba(148,163,184,0.2); border-radius: 12px; padding: 16px; max-width: 400px; width: 90%; max-height: 80vh; overflow-y: auto; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <div class="row">
        <div>
          <h1>SIM800 Gateway</h1>
          <div class="muted" id="subtitle">Loading...</div>
        </div>
        <div id="statusBadge">...</div>
      </div>
    </div>

    <!-- Login Section -->
    <div class="card" id="loginCard">
      <h2>Login</h2>
      <div id="loginForm" class="grid">
        <input id="loginEmail" type="email" placeholder="Email" />
        <input id="loginPassword" type="password" placeholder="Password" />
        <button class="btn full" id="loginBtn">Login</button>
      </div>
      <div id="authStatus" class="hide">
        <div class="row" style="margin-bottom:8px;">
          <span class="muted">Status</span>
          <span id="authBadge">-</span>
        </div>
        <div class="row" style="margin-bottom:8px;">
          <span class="muted">Device</span>
          <span id="deviceStatus">-</span>
        </div>
        <div class="row" style="margin-bottom:8px;">
          <span class="muted">SIM</span>
          <span id="simRegStatus">-</span>
        </div>
        <div class="row">
          <button class="btn sm" id="registerDeviceBtn">Register Device</button>
          <button class="btn sm" id="registerAllSimsBtn">Register All SIMs</button>
          <button class="btn sm bad" id="logoutBtn">Logout</button>
        </div>
      </div>
    </div>

    <!-- Backend Config -->
    <div class="card">
      <h2>Backend Config</h2>
      <div class="grid">
        <input id="baseUrl" placeholder="Backend URL (e.g. https://api.example.com)" />
        <input id="apiPath" placeholder="API path (/api/agent/incoming-sms)" />
        <button class="btn full" id="saveConfigBtn">Save Backend URL</button>
      </div>
    </div>

    <!-- WiFi Section -->
    <div class="card">
      <h2>WiFi</h2>
      <div id="wifiStatus" class="section">
        <div class="row">
          <span id="wifiStatusText">Not connected</span>
          <button class="btn sm gray" id="disconnectBtn">Disconnect</button>
        </div>
      </div>
      
      <div id="wifiSetup">
        <div class="row" style="margin-bottom:10px;">
          <button class="btn" id="scanBtn">Scan Networks</button>
        </div>
        <div id="scanResults" class="grid"></div>
        
        <div class="divider"></div>
        
        <label>Or enter manually:</label>
        <div class="grid">
          <input id="manualSsid" placeholder="Network name (SSID)" />
          <input id="manualPw" type="password" placeholder="Password" />
          <button class="btn full" id="connectBtn">Connect</button>
        </div>
      </div>
    </div>

    <!-- System Status -->
    <div class="card">
      <h2>System</h2>
      <div class="row" style="margin-bottom:8px;">
        <span class="muted">Battery</span>
        <span id="batteryStatus">-</span>
      </div>
      <div class="row" style="margin-bottom:8px;">
        <span class="muted">Uptime</span>
        <span id="uptimeStatus">-</span>
      </div>
      <div class="row">
        <span class="muted">Pending SMS</span>
        <span id="pendingStatus">-</span>
      </div>
    </div>

    <!-- SIM Slots -->
    <div class="card">
      <div class="row">
        <h2>SIM Slots</h2>
        <button class="btn sm gray" id="checkAllBtn">Refresh All</button>
        <button class="btn sm gray" id="pausePollingBtn">Pause SMS</button>
        <button class="btn sm gray" id="pauseHeartbeatBtn">Pause HB</button>
      </div>
      <div id="simList"></div>
    </div>

    <!-- Device Info -->
    <div class="card">
      <h2>Device Info</h2>
      <div class="grid">
        <div class="row" style="margin-bottom:8px;">
          <span class="muted">Device ID</span>
          <span id="deviceIdDisplay">-</span>
        </div>
        <div class="row" style="margin-bottom:8px;">
          <span class="muted">Bearer Token</span>
          <span id="tokenStatus">-</span>
        </div>
        <div class="row">
          <button class="btn sm gray" id="testPushBtn">Test Push</button>
          <button class="btn sm gray" id="refreshTokenBtn">Refresh Token</button>
        </div>
      </div>
    </div>

    <!-- Monitor -->
    <div class="card">
      <div class="row">
        <h2>Log</h2>
        <button class="btn sm gray" id="clearLogBtn">Clear</button>
      </div>
      <pre id="monitor">Loading...</pre>
    </div>
  </div>

  <div class="toast hide" id="toast"></div>

<script>
let currentNetwork = null;
let statusInterval;
let statusSlowInterval;

function $(id) { return document.getElementById(id); }
function show(el, on) { el.classList.toggle('hide', !on); }
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.remove('hide');
  setTimeout(() => t.classList.add('hide'), 3000);
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
  const r = await fetch(path, {method:'POST', body: new URLSearchParams(data)});
  return r.json();
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
    if (s.sta_connected) {
      $('wifiStatusText').innerHTML = '<span class="badge ok">Connected</span> ' + s.sta_ip;
      $('disconnectBtn').classList.remove('hide');
      show($('wifiSetup'), false);
    } else {
      $('wifiStatusText').innerHTML = '<span class="badge bad">Offline</span> AP: ' + s.ap_ip;
      $('disconnectBtn').classList.add('hide');
      show($('wifiSetup'), true);
    }
    
    $('subtitle').textContent = s.sta_connected ? s.sta_ip : 'AP Mode';
    $('statusBadge').innerHTML = s.sta_connected ? '<span class="badge ok">Online</span>' : '<span class="badge warn">AP</span>';
    
    // System
    const showLowestAsMain = (!s.battery_percent || s.battery_percent <= 0) && (s.lowest_battery_sim > 0) && (s.lowest_battery > 0);
    const batPct = showLowestAsMain ? s.lowest_battery : s.battery_percent;
    const batBadge = batPct > 50 ? 'ok' : batPct > 20 ? 'warn' : 'bad';
    let batHtml = '<span class="badge ' + batBadge + '">' + batPct + '%</span>';
    if (!showLowestAsMain && s.battery_mv && s.battery_mv > 0) {
      batHtml += ' <span class="muted">(' + (s.battery_mv/1000.0).toFixed(3) + 'V)</span>';
    }
    if (!showLowestAsMain && s.lowest_battery_sim > 0) {
      batHtml += ' <span class="muted">(Lowest: SIM' + s.lowest_battery_sim + ' ' + s.lowest_battery + '%)</span>';
    }
    $('batteryStatus').innerHTML = batHtml;
    $('uptimeStatus').textContent = Math.floor(s.uptime_s / 60) + ' min';
    $('pendingStatus').textContent = s.pending_sms;
    
    // Config - only update if elements exist
    if ($('baseUrl')) $('baseUrl').value = s.base_url || '';
    if ($('apiPath')) $('apiPath').value = s.api_path || '/api/agent/incoming-sms';
    
    // Device info
    if ($('deviceIdDisplay')) $('deviceIdDisplay').textContent = s.device_id || '-';
    if ($('tokenStatus')) $('tokenStatus').textContent = (s.bearer_token && s.bearer_token.length > 0) ? '(set)' : 'Not set';

    // Heartbeat pause button state
    if ($('pauseHeartbeatBtn')) {
      if (s.heartbeat_paused) {
        $('pauseHeartbeatBtn').textContent = 'Resume HB';
        $('pauseHeartbeatBtn').classList.add('red');
      } else {
        $('pauseHeartbeatBtn').textContent = 'Pause HB';
        $('pauseHeartbeatBtn').classList.remove('red');
      }
    }
    
  } catch(e) {
    console.error('Status error:', e);
  }
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
  } catch(e) {}
}

async function scanNetworks() {
  $('scanBtn').innerHTML = '<span class="spinner"></span> Scanning...';
  $('scanBtn').disabled = true;
  
  try {
    const nets = await get('/scan');
    const list = $('scanResults');
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
  
  $('scanBtn').textContent = 'Scan Networks';
  $('scanBtn').disabled = false;
}

function selectNetwork(ssid, secure) {
  currentNetwork = ssid;
  $('manualSsid').value = ssid;
  $('manualPw').value = '';
  $('manualPw').focus();
  
  // Highlight selected
  document.querySelectorAll('.net-item').forEach(el => el.classList.remove('selected'));
  event.currentTarget.classList.add('selected');
  
  if (!secure) {
    connectWifi();
  }
}

async function connectWifi() {
  const ssid = $('manualSsid').value.trim();
  const pw = $('manualPw').value;
  
  if (!ssid) {
    toast('Enter network name');
    return;
  }
  
  $('connectBtn').innerHTML = '<span class="spinner"></span> Connecting...';
  $('connectBtn').disabled = true;
  
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
  
  $('connectBtn').textContent = 'Connect';
  $('connectBtn').disabled = false;
}

async function disconnectWifi() {
  await fetch('/disconnect');
  toast('Disconnected');
  setTimeout(refreshStatus, 1000);
}

async function refreshSims() {
  try {
    const s = await get('/sim-config');
    const list = $('simList');
    list.innerHTML = '';
    
    for (let i = 0; i < s.enabled.length; i++) {
      const div = document.createElement('div');
      div.className = 'sim-item' + (s.enabled[i] ? '' : ' disabled');
      
      // Extract signal strength from CSQ (format: +CSQ: xx,yy)
      let signal = '-';
      let network = '-';
      let rssi = null;
      let bars = 0;
      if (s.csq && s.csq[i]) {
        const csqMatch = s.csq[i].match(/\+CSQ:\s*(\d+)/);
        if (csqMatch) {
          rssi = parseInt(csqMatch[1]);
          if (rssi === 99) {
            signal = '?';
            bars = 0;
          } else if (rssi >= 20) { signal = 'Excellent'; bars = 4; }
          else if (rssi >= 15) { signal = 'Good'; bars = 3; }
          else if (rssi >= 10) { signal = 'Fair'; bars = 2; }
          else if (rssi >= 5) { signal = 'Poor'; bars = 1; }
          else { signal = 'Very Poor'; bars = 0; }
        }
      }
      
      // Extract network from COPS
      if (s.cops && s.cops[i]) {
        const copsMatch = s.cops[i].match(/"([^"]+)"/);
        if (copsMatch) network = copsMatch[1];
      }
      
      const num = s.numbers[i] || '-';
      const isResponsive = s.responsive && s.responsive[i];
      const isBackendReg = s.backend_registered && s.backend_registered[i];
      const badge = s.enabled[i] ? (isResponsive ? (s.registered[i] ? 'ok' : 'warn') : 'bad') : 'bad';
      const status = s.enabled[i] ? (isResponsive ? (s.registered[i] ? signal : 'No Net') : 'No Resp') : 'Off';
      const statusText = (isResponsive && s.registered[i] && rssi != null) ? `${signal} (CSQ ${rssi})` : status;
      const barHtml = (isResponsive && s.registered[i] && rssi != null) ? `<span class="sig ${bars?('b'+bars):''}"><i></i><i></i><i></i><i></i></span>` : '';
      const battery = (s.battery_pct && s.battery_pct[i] != null) ? s.battery_pct[i] : -1;
      const batteryMv = (s.battery_mv && s.battery_mv[i] != null) ? s.battery_mv[i] : -1;
      const batteryV = (batteryMv && batteryMv > 0) ? (batteryMv/1000.0).toFixed(3) : '';
      
      div.innerHTML = `
        <div class="sim-row">
          <div class="sim-main">
            <div class="sim-left">
              <b>SIM ${i+1}</b>
              <span class="sim-num" id="simNum${i}">${num}</span>
            </div>
            <div class="sim-actions">
              ${num !== '-' ? `<button class="btn-sm" onclick="copyText('${num}')">Copy</button>` : ''}
              <button class="btn-sm" onclick="toggleSim(${i})" style="background:${s.enabled[i]?'#ef4444':'#22c55e'}">${s.enabled[i]?'Disable':'Enable'}</button>
              ${(isResponsive && s.enabled[i] && num !== '-' && !isBackendReg) ? `<button class="btn-sm" id="regSimBtn${i}" onclick="registerSimSlot(${i})">Register</button>` : ''}
              ${isResponsive ? `<button class="btn-sm" onclick="showNetworkModal(${i})">Network</button>` : ''}
            </div>
          </div>
          <div class="sim-info">
            <span class="muted">${network}</span>
            ${battery >= 0 ? `<span class="muted" style="margin-left:8px">BAT ${battery}%${batteryV ? (' ' + batteryV + 'V') : ''}</span>` : ''}
            ${isBackendReg ? `<span class="badge ok" style="margin-left:8px">Backend</span>` : ''}
          </div>
        </div>
        <span class="badge ${badge}">${barHtml}${statusText}</span>
      `;
      list.appendChild(div);
    }
  } catch(e) {}
}

async function checkAllSims() {
  $('checkAllBtn').innerHTML = '<span class="spinner"></span> Checking...';
  try {
    const r = await get('/check-all-sim');
    // Update UI with fresh data
    await refreshSims();
  } catch(e) {
    toast('Refresh failed');
  }
  $('checkAllBtn').textContent = 'Refresh All';
}

async function togglePollingPause() {
  try {
    const r = await post('/toggle-polling', {});
    if (r.success) {
      toast(r.message || 'Toggled');
      // Update button text based on state
      if (r.paused) {
        $('pausePollingBtn').textContent = 'Resume SMS';
        $('pausePollingBtn').classList.add('red');
      } else {
        $('pausePollingBtn').textContent = 'Pause SMS';
        $('pausePollingBtn').classList.remove('red');
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
      if (r.paused) {
        $('pauseHeartbeatBtn').textContent = 'Resume HB';
        $('pauseHeartbeatBtn').classList.add('red');
      } else {
        $('pauseHeartbeatBtn').textContent = 'Pause HB';
        $('pauseHeartbeatBtn').classList.remove('red');
      }
    } else {
      toast(r.error || 'Failed');
    }
  } catch(e) {
    toast('Failed to toggle');
  }
}

function copyText(text) {
  // Fallback for non-HTTPS contexts
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(() => toast('Copied!')).catch(() => {
      fallbackCopy(text);
    });
  } else {
    fallbackCopy(text);
  }
}

function fallbackCopy(text) {
  const input = document.createElement('input');
  input.value = text;
  document.body.appendChild(input);
  input.select();
  document.execCommand('copy');
  document.body.removeChild(input);
  toast('Copied!');
}

async function toggleSim(idx) {
  const s = await get('/sim-config');
  const newEnabled = !s.enabled[idx];
  const r = await post('/sim-enable', { slot: idx + 1, enabled: newEnabled ? 1 : 0 });
  if (r.success) {
    toast('SIM ' + (idx + 1) + ' ' + (newEnabled ? 'enabled' : 'disabled'));
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
  const list = $('networkList');
  list.innerHTML = '<div class="muted">Click "Scan Networks" to see available networks</div>';
  modal.classList.remove('hide');
}

function closeNetworkModal() {
  $('networkModal').classList.add('hide');
  currentNetSimIdx = -1;
}

async function scanNetworks() {
  if (currentNetSimIdx < 0) return;
  
  const list = $('networkList');
  list.innerHTML = '<div class="muted"><span class="spinner"></span> Scanning... (takes 10-20 seconds)</div>';
  
  try {
    const r = await post('/scan-networks', { slot: currentNetSimIdx + 1 });
    
    if (!r.success) {
      list.innerHTML = '<div class="muted" style="color:var(--bad)">Error: ' + (r.error || 'Scan failed') + '</div>';
      return;
    }
    
    if (!r.networks || r.networks.length === 0) {
      list.innerHTML = '<div class="muted">No networks found</div>';
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
        <span class="net-icon">${n.current ? 'OK' : 'AVAIL'}</span>
      `;
      if (!n.current) {
        div.onclick = () => selectNetwork(n.name);
      }
      list.appendChild(div);
    });
  } catch(e) {
    list.innerHTML = '<div class="muted" style="color:var(--bad)">Connection error</div>';
  }
}

async function selectNetwork(networkName) {
  if (currentNetSimIdx < 0) return;
  
  const list = $('networkList');
  list.innerHTML = '<div class="muted"><span class="spinner"></span> Selecting ' + networkName + '...</div>';
  
  try {
    const r = await post('/select-network', { slot: currentNetSimIdx + 1, network: networkName });
    
    if (r.success) {
      toast('Network selected: ' + networkName);
      closeNetworkModal();
      await refreshSims();
    } else {
      list.innerHTML = '<div class="muted" style="color:var(--bad)">Error: ' + (r.error || 'Selection failed') + '</div>';
    }
  } catch(e) {
    list.innerHTML = '<div class="muted" style="color:var(--bad)">Connection error</div>';
  }
}

async function setAutoNetwork() {
  if (currentNetSimIdx < 0) return;
  
  const list = $('networkList');
  list.innerHTML = '<div class="muted"><span class="spinner"></span> Setting auto mode...</div>';
  
  try {
    const r = await post('/select-network', { slot: currentNetSimIdx + 1, network: '' });
    
    if (r.success) {
      toast('Auto network mode enabled');
      closeNetworkModal();
      await refreshSims();
    } else {
      // Try AT+COPS=0 directly
      toast('Setting auto mode...');
      closeNetworkModal();
    }
  } catch(e) {
    list.innerHTML = '<div class="muted" style="color:var(--bad)">Connection error</div>';
  }
}

async function saveConfig() {
  const r = await post('/agent-config', {
    base_url: $('baseUrl').value,
    api_path: $('apiPath').value,
    device_id: $('deviceId').value,
    bearer_token: $('bearerToken').value
  });
  toast(r.success ? 'Saved' : 'Failed');
}

async function testPush() {
  const r = await get('/test-push');
  toast(r.success ? 'Push OK' : r.error);
}

async function clearLog() {
  await fetch('/clear-monitor');
  $('monitor').textContent = '';
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
    const r = await post('/register-sim', { slot: idx + 1 });
    toast(r.success ? ('SIM ' + (idx + 1) + ' registered') : (r.error || 'Register failed'));
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
  const s = await get('/status');
  const hasToken = s.bearer_token && s.bearer_token.length > 0;

  // Update heartbeat pause button state
  if ($('pauseHeartbeatBtn')) {
    if (s.heartbeat_paused) {
      $('pauseHeartbeatBtn').textContent = 'Resume HB';
      $('pauseHeartbeatBtn').classList.add('red');
    } else {
      $('pauseHeartbeatBtn').textContent = 'Pause HB';
      $('pauseHeartbeatBtn').classList.remove('red');
    }
  }
  
  // Update device info section
  $('deviceIdDisplay').textContent = s.device_id || '-';
  $('tokenStatus').textContent = hasToken ? '(set)' : 'Not set';
  
  if (hasToken) {
    $('loginForm').classList.add('hide');
    $('authStatus').classList.remove('hide');
    
    $('authBadge').innerHTML = '<span class="badge ok">Logged In</span>';
    $('deviceStatus').innerHTML = s.device_registered ? '<span class="badge ok">Registered</span>' : '<span class="badge warn">Not Registered</span>';
    $('simRegStatus').innerHTML = s.sim_registered ? '<span class="badge ok">Registered</span>' : '<span class="badge warn">Not Registered</span>';
  } else {
    $('loginForm').classList.remove('hide');
    $('authStatus').classList.add('hide');
  }
}

// Event listeners
$('scanBtn').onclick = scanNetworks;
$('connectBtn').onclick = connectWifi;
$('disconnectBtn').onclick = disconnectWifi;
$('checkAllBtn').onclick = checkAllSims;
$('pausePollingBtn').onclick = togglePollingPause;
$('pauseHeartbeatBtn').onclick = toggleHeartbeatPause;
$('saveConfigBtn').onclick = saveConfig;
$('testPushBtn').onclick = testPush;
$('clearLogBtn').onclick = clearLog;
$('loginBtn').onclick = doLogin;
$('logoutBtn').onclick = doLogout;
$('refreshTokenBtn').onclick = doRefreshToken;
$('registerDeviceBtn').onclick = doRegisterDevice;
$('registerAllSimsBtn').onclick = doRegisterAllSims;

// Init
refreshStatus();
refreshSims();
checkAuthStatus();
statusInterval = setInterval(refreshMonitor, 2000);
statusSlowInterval = setInterval(refreshStatus, 10000);
setTimeout(refreshMonitor, 300);
</script>

<!-- Network Selection Modal -->
<div id="networkModal" class="modal hide">
  <div class="modal-content">
    <div class="row" style="margin-bottom:12px">
      <h2 style="margin:0">Network Selection</h2>
      <button class="btn gray sm" onclick="closeNetworkModal()">Close</button>
    </div>
    <div class="row" style="margin-bottom:12px">
      <button class="btn" onclick="scanNetworks()">Scan Networks</button>
      <button class="btn gray" onclick="setAutoNetwork()">Auto Mode</button>
    </div>
    <div id="networkList">
      <div class="muted">Click "Scan Networks" to see available networks</div>
    </div>
  </div>
</div>

</body>
</html>
)=====";

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

void initWebUI() {
    // Register routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/monitor", HTTP_GET, handleMonitor);
    server.on("/clear-monitor", HTTP_GET, handleClearMonitor);
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
    char buf[1024];
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
    
    snprintf(buf, bufSize,
        "{"
        "\"sta_connected\":%s,"
        "\"sta_ip\":\"%s\","
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
        "\"heartbeat_paused\":%s"
        "}",
        staConnected ? "true" : "false",
        staConnected ? staIp.toString().c_str() : "",
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
        heartbeatPaused ? "true" : "false"
    );
}

void handleMonitor() {
    static char buf[4096];  // Static to avoid stack overflow
    getMonitorLogText(buf, sizeof(buf));
    server.send(200, "text/plain", buf);
}

void handleClearMonitor() {
    clearMonitorLog();
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
    static char buf[4096];  // Static to avoid stack overflow
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
        activeSim + 1,
        currentMuxSim + 1
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
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : 1;
    int simIdx = slot - 1;
    
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
        slot, cregEsc, copsEsc, csqEsc
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
    static char buf[8192];
    size_t pos = 0;
    
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"success\":true,\"sims\":[");
    
    for (int i = 0; i < SIM_COUNT; i++) {
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"slot\":%d,\"enabled\":%s",
            i + 1,
            simStates[i].enabled ? "true" : "false"
        );
        
        if (simStates[i].enabled) {
            logMsgInt("[SIM] Checking slot", i + 1);
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
    
    setSimYieldToWebServer(true);
    setSimBusy(false);
    resumeSmsPolling();  // Resume SMS polling
    refreshAllInProgress = false;
    logMsg("[SIM] Refresh all complete");
    appendMonitorLog("[SIM] Refresh all done");
    server.send(200, "application/json", buf);
}

void handleSimEnable() {
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : 1;
    int simIdx = slot - 1;
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
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : (activeSim + 1);
    int simIdx = slot - 1;
    
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
    int slot = server.hasArg("slot") ? server.arg("slot").toInt() : (activeSim + 1);
    int simIdx = slot - 1;
    
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
    charBufSet(agentBaseUrl, sizeof(agentBaseUrl), server.arg("base_url").c_str());
    charBufSet(agentApiPath, sizeof(agentApiPath), server.arg("api_path").c_str());
    charBufSet(agentDeviceId, sizeof(agentDeviceId), server.arg("device_id").c_str());
    charBufSet(agentBearerToken, sizeof(agentBearerToken), server.arg("bearer_token").c_str());
    
    // Save to preferences
    preferences.begin("agent", false);
    preferences.putString("base", agentBaseUrl);
    preferences.putString("path", agentApiPath);
    preferences.putString("dev", agentDeviceId);
    preferences.putString("tok", agentBearerToken);
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
        simStates[simIdx].number, agentDeviceId, slot
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
            simStates[simIdx].number, agentDeviceId, slot
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
