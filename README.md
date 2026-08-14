# SIM800 Gateway (ESP32)

**Version:** 1.0.21  
**Architecture:** 16-SIM ESP32 Gateway with CD74HC4067 Multiplexer  
**Purpose:** SMS gateway for OTP forwarding with web UI, HTTPS OTA, and backend integration

---

## Table of Contents

1. [Overview](#overview)
2. [Hardware Requirements](#hardware-requirements)
3. [Architecture](#architecture)
4. [Quick Start](#quick-start)
5. [Configuration](#configuration)
6. [Web UI Features](#web-ui-features)
7. [Backend Integration](#backend-integration)
8. [OTA Updates](#ota-updates)
9. [Project Structure](#project-structure)
10. [API Reference](#api-reference)
11. [Troubleshooting](#troubleshooting)
12. [Advanced Topics](#advanced-topics)

---

## Overview

The SIM800 Gateway is an ESP32-based firmware that manages up to 16 SIM800L GSM modules through a CD74HC4067 analog multiplexer. It provides:

- **16-SIM SMS Gateway** - Polls all SIMs for incoming SMS and forwards to backend
- **Web UI** - Local HTTP server for configuration, monitoring, and SIM management
- **Dual WiFi Mode** - Simultaneous AP (for setup) + STA (for backend connectivity)
- **HTTPS OTA** - Over-the-air updates via GitHub Releases or custom URL
- **Missed Call Forwarding** - Converts missed calls to Viber-style OTP messages
- **USSD Balance Check** - Bulk *143# balance checking across all SIMs
- **Persistent Storage** - LittleFS for message logs and error logs
- **Heartbeat System** - Regular backend sync with full inventory every 30 minutes
- **SIM Watchdog** - Auto-recovery of unresponsive SIMs

---

## Hardware Requirements

### Core Components

| Component | Specification | Purpose |
|-----------|--------------|---------|
| **ESP32** | Any ESP32 Dev Module | Main controller (UART + WiFi) |
| **CD74HC4067** | 16-channel analog multiplexer | Routes 16 SIM RX lines to single ESP32 UART |
| **SIM800L** | 16x GSM modules | SMS/voice/calls (shared TX, muxed RX) |
| **Power Supply** | 5V 10A+ (critical!) | Powers all SIM800L modules (peak 2A each) |

### Pin Connections

#### ESP32 to CD74HC4067 Mux
```
ESP32 GPIO5  (TX)  → All SIM800L RX pins (shared)
ESP32 GPIO4  (RX)  → CD74HC4067 COM (common output)
ESP32 GPIO9   (S0) → CD74HC4067 S0
ESP32 GPIO10  (S1) → CD74HC4067 S1
ESP32 GPIO11  (S2) → CD74HC4067 S2
ESP32 GPIO12  (S3) → CD74HC4067 S3
ESP32 GPIO23  (RST) → SIM reset control (optional)
ESP32 GPIO2   (LED) → Status LED (built-in)
```

#### CD74HC4067 to SIM800L Modules
```
Mux Channel 0  → SIM Slot 1  (physical bank 1)
Mux Channel 1  → SIM Slot 2
Mux Channel 2  → SIM Slot 3
Mux Channel 3  → SIM Slot 4
...
Mux Channel 15 → SIM Slot 16
```

**Note:** Logical slot → mux channel mapping is defined in `config.h` (`LOGICAL_TO_MUX_INIT`). Physical bank 1 = logical SIM 1 (slot 0) → mux channel 3 by default.

### Power Considerations

⚠️ **CRITICAL:** SIM800L modules draw up to 2A during transmission. Use a robust 5V power supply:

- **Minimum:** 5V 5A for 4-8 SIMs
- **Recommended:** 5V 10A for full 16-SIM setup
- **Decoupling:** Add 100µF capacitor per SIM800L near module
- **Voltage:** Must stay above 3.7V (SIM800L brown-out at ~3.4V)

---

## Architecture

### Communication Flow

```
┌─────────────┐
│   Backend   │ (HTTPS API)
│   Server    │
└──────┬──────┘
       │ HTTPS (heartbeat, SMS forward)
       │
┌──────▼──────────────────────────────┐
│         ESP32 (WiFi + UART)         │
│  ┌────────────────────────────────┐ │
│  │   Web UI (HTTP Server)        │ │
│  │   Port 80 (AP: 192.168.4.1)   │ │
│  └────────────────────────────────┘ │
│  ┌────────────────────────────────┐ │
│  │   Heartbeat / SMS Queue       │ │
│  │   LittleFS Logs               │ │
│  └────────────────────────────────┘ │
└──────┬──────────────────────────────┘
       │ UART TX (shared) → All SIM RX
       │ UART RX ← MUX COM ← SIM TX (muxed)
       │
┌──────▼──────────────────────────────┐
│   CD74HC4067 Mux (16 channels)      │
│   S0-S3 controlled by ESP32 GPIO    │
└──────┬──────────────────────────────┘
       │
   ┌───┴───┬───┬───┐ ... ┌───┐
   │ SIM1  │SIM2│SIM3│     │SIM16│
   └───────┴───┴───┘     └────┘
```

### Software Architecture

The firmware is modular, with each `.cpp/.h` pair handling a specific domain:

| Module | Responsibility |
|--------|----------------|
| `sim800_gateway.ino` | Main loop, WiFi, heartbeat, global state |
| `config.h` | All constants, pins, timing, data structures |
| `mux.cpp/h` | Multiplexer control, slot mapping |
| `sim800.cpp/h` | AT command interface, SIM initialization |
| `sms.cpp/h` | SMS polling, parsing, forwarding, multipart handling |
| `webui.cpp/h` | HTTP server, route handlers, HTML UI |
| `ota.cpp/h` | HTTPS OTA updates from GitHub/custom URL |
| `calls.cpp/h` | Missed call detection, call log |
| `ussd.cpp/h` | USSD *143# balance checking |
| `maintenance.cpp/h` | Backend maintenance API polling |
| `status_led.cpp/h` | LED status indicators |
| `utils.h` | String helpers, JSON escaping, phone normalization |
| `logger.h` | Logging macros and helpers |

---

## Quick Start

### 1. Arduino IDE Setup

1. Install **ESP32 board support**:
   - Arduino IDE → File → Preferences → Additional Board Manager URLs
   - Add: `https://dl.espressif.com/dl/package_esp32_index.json`
   - Tools → Board → Boards Manager → Search "ESP32" → Install

2. Open **`sim800_gateway.ino`** (Arduino opens the entire folder automatically)

3. Select board:
   - **Tools → Board:** ESP32 Dev Module (or your specific board)
   - **Tools → Partition Scheme:** **Minimal SPIFFS (1.9MB APP with OTA)** ⚠️ REQUIRED for OTA
   - **Tools → Upload Speed:** 921600
   - **Tools → Port:** Select ESP32 COM port

4. Click **Upload** (first flash must be via USB)

### 2. Initial Configuration

After flashing:

1. **Connect to AP:** ESP32 creates WiFi AP `SIM800-Gateway-XXXXXX` (password: empty)
2. **Open Web UI:** Navigate to `http://192.168.4.1`
3. **Configure WiFi:** Settings → WiFi → Scan → Select your network → Save
4. **Configure Backend:** Settings → Agent → Enter Base URL + Device ID
5. **Test SIMs:** SIMs tab → Check All SIMs → Verify numbers appear

### 3. Verify Operation

- **Dashboard:** Shows WiFi status, uptime, SMS stats
- **SIMs Tab:** Each slot shows number, signal, network type, battery
- **Logs Tab:** Real-time monitor log + persistent error log
- **Messages Tab:** Stored SMS history (last 30 messages)

---

## Configuration

### config.h Key Settings

#### Hardware
```c
#define USE_DUAL_UART   0          // 0 = 16-SIM mux, 1 = 2-SIM dual UART
#define UART_BAUD_RATE  115200     // SIM800L baud rate
#define UART_RX_PIN     4          // GPIO4 - RX from mux
#define UART_TX_PIN     5          // GPIO5 - TX to all SIMs
#define MUX_S0          9          // GPIO9-12 - Mux control pins
#define MUX_S1          10
#define MUX_S2          11
#define MUX_S3          12
#define RESET_PIN       23         // GPIO23 - SIM reset
```

#### Timing
```c
#define SMS_POLL_INTERVAL_MS        400     // Min gap between SIM polls
#define SMS_SLOT_POLL_COOLDOWN_MS   3000    // Min gap before re-polling same slot
#define HEARTBEAT_INTERVAL_MS       60000   // 60s between heartbeats
#define HEARTBEAT_FULL_SYNC_INTERVAL_MS (30UL * 60UL * 1000UL)  // Full sync every 30min
#define MUX_SETTLE_MS               500     // Mux channel settle time (ms)
```

#### Backend
```c
#define DEFAULT_BASE_URL        "https://seller.otpocket.app"
#define DEFAULT_API_PATH        "/api/agent/incoming-sms"
#define AGENT_BEARER_TOKEN_SIZE   2048    // JWT tokens can be large
#define AGENT_REFRESH_TOKEN_SIZE  768
```

#### OTA
```c
#define OTA_ENABLED         1        // Enable HTTPS OTA (requires large partition)
#define OTA_FIRMWARE_URL_DEFAULT \
    "https://trfifrcfdtaxyuvsbfql.supabase.co/storage/v1/object/public/app/firmware.bin"
#define OTA_VERSION_URL \
    "https://raw.githubusercontent.com/Vanflame/sim800-gateway/main/firmware/version.txt"
#define OTA_GITHUB_OWNER    "Vanflame"
#define OTA_GITHUB_REPO     "sim800-gateway"
#define OTA_FIRMWARE_BIN    "firmware.bin"
```

#### Missed Call → Viber OTP
```c
#define MISSED_CALL_FORWARD_DEFAULT true    // Enable by default
#define MISSED_CALL_VIBER_SENDER    "Viber"
#define MISSED_CALL_SCAN_INTERVAL_MS 200    // Fast scan tick
#define MISSED_CALL_PRIORITY_LISTEN_MS 4000 // Dwell on priority SIM
```

### Persistent Settings (NVS)

Settings are saved in ESP32 NVS (Non-Volatile Storage) and persist across reboots:

| Key | Description |
|-----|-------------|
| `wifi/ssid` | WiFi SSID |
| `wifi/pw` | WiFi password |
| `agent/base` | Backend base URL |
| `agent/dev` | Device ID |
| `agent/tok` | Bearer token (JWT) |
| `agent/rtok` | Refresh token |
| `agent/sim` | Agent SIM number |
| `agent/slot` | Agent SIM slot |
| `agent/path` | API path |
| `agent/auth` | Auth enabled flag |
| `agent/hb_pause` | Heartbeat paused |
| `agent/mcall` | Missed call forward enabled |
| `agent/mcall_slot` | Missed call watch slot |
| `agent/sim_off` | SIM disable mask (bitmask) |
| `store/lfs_sz` | LittleFS partition size |
| `store/lfs_magic` | LittleFS format magic |

---

## Web UI Features

### Dashboard
- WiFi status (AP + STA IP, SSID)
- Uptime, free heap, largest allocatable block
- SMS statistics (received, forwarded, failed)
- Firmware version, installed version
- Modem gateway status (running/stopped)

### SIMs Tab
- **Per-SIM Status:** Number, signal strength, network type, battery, registration
- **Actions:**
  - Check Single SIM (probe specific slot)
  - Check All SIMs (full scan)
  - Enable/Disable individual SIMs
  - USSD *143# balance check (single or bulk)
  - Missed call toggle (ON/OFF)
- **Auto-Disable:** SIMs with 3+ consecutive poll errors are auto-disabled

### Settings Tab
- **WiFi:** Scan networks, save credentials, disconnect
- **Agent Config:** Base URL, device ID, API path, auth toggle
- **Login/Logout:** Backend authentication (JWT + refresh token)
- **Firmware (OTA):** Check updates, install updates, configure OTA URL
- **Modem Control:** Start/Stop modem gateway manually

### Logs Tab
- **Monitor Log:** Last 20 events (RAM, circular buffer)
- **Error Log:** Persistent errors (LittleFS, auto-pruned)
- **Clear buttons:** Clear monitor log or error log

### Messages Tab
- Stored SMS history (last 30 messages in LittleFS)
- Auto-prunes oldest when full
- Shows: timestamp, SIM slot, sender, message (clipped to 200 chars)

### Calls Tab
- Missed call log (last 15 entries)
- Make call (number + SIM slot)
- Hang up current call
- Clear call log

---

## Backend Integration

### Heartbeat API

The gateway sends heartbeats to the backend at regular intervals:

#### Full Inventory Sync (every 30 minutes)
```http
POST /api/agent/heartbeat
Content-Type: application/json
Authorization: Bearer <token>

{
  "device_id": "ESP32_XXXXXX",
  "battery_level": 85,
  "inventory_sync": true,
  "sims": [
    {
      "number": "+639123456789",
      "slot": 0,
      "status": "ACTIVE",
      "signal_strength": 18,
      "network_type": "2G"
    },
    ...
  ]
}
```

#### Lightweight Ping (between full syncs)
```http
POST /api/agent/ping
Content-Type: application/json
Authorization: Bearer <token>

{
  "device_id": "ESP32_XXXXXX",
  "battery_level": 85
}
```

### SMS Forward API

When an SMS is received, it's forwarded to:

```http
POST /api/agent/incoming-sms
Content-Type: application/json
Authorization: Bearer <token>

{
  "device_id": "ESP32_XXXXXX",
  "sim_number": "+639123456789",
  "sender": "Viber",
  "message": "Viber: Your verification code is 482917. Missed call — the last 6 digits of the caller number are 482917.",
  "timestamp": "2024-01-15 10:30:45",
  "sim_slot": 0
}
```

### Missed Call → Viber OTP

When missed call forwarding is enabled:

1. SIM receives incoming call → `+CLIP: "+639876543210",129,...`
2. Gateway captures last 6 digits: `654321`
3. Hangs up immediately
4. POSTs to `/api/agent/incoming-sms` with:
   - **sender:** `Viber`
   - **message:** `Viber: Your verification code is 654321. Missed call — the last 6 digits of the caller number are 654321.`

### Authentication Flow

1. **Login:** POST `/api/agent/login` with device_id + credentials → returns `access_token` + `refresh_token`
2. **Heartbeat:** Uses `access_token` in `Authorization: Bearer` header
3. **Token Refresh:** When access token expires (or 401 response), uses `refresh_token` to get new access token
4. **Token Storage:** Both tokens stored in NVS, auto-loaded on boot

---

## OTA Updates

### Prerequisites

1. **Partition Scheme:** Must use `Minimal SPIFFS (1.9MB APP with OTA)` or custom partition with OTA slots
2. **Firmware Size:** ~800KB–1.5MB (OTA partition must accommodate)
3. **Network:** Device must be on WiFi with internet access

### GitHub Releases Workflow

#### 1. Export Firmware Binary

In Arduino IDE:
```
Sketch → Export compiled Binary
```

Files generated in sketch folder:
- `sim800_gateway.ino.bin` ← **USE THIS** (~1MB, magic byte 0xE9)
- `sim800_gateway.ino.merged.bin` ← Full flash image (USB only)
- `sim800_gateway.ino.bootloader.bin` ← Bootloader
- `sim800_gateway.ino.partitions.bin` ← Partition table
- `sim800_gateway.ino.boot_app0.bin` ← Boot app

**Rename for release:**
```bash
cp sim800_gateway.ino.bin firmware.bin
```

#### 2. Create GitHub Release

1. Go to repo → **Releases** → **Create a new release**
2. Tag: `v1.0.0` (semantic versioning recommended)
3. Attach **`firmware.bin`**
4. Publish release

**Download URLs:**
- Tag-specific: `https://github.com/USER/REPO/releases/download/v1.0.0/firmware.bin`
- Latest: `https://github.com/USER/REPO/releases/latest/download/firmware.bin`

#### 3. Configure OTA in Firmware

In `config.h`:
```c
#define FIRMWARE_VERSION    "1.0.0"
#define OTA_GITHUB_OWNER    "YOUR_USERNAME"
#define OTA_GITHUB_REPO     "sim800_gateway"
#define OTA_FIRMWARE_BIN    "firmware.bin"
```

Or set full URL in device Web UI: **Settings → Firmware → OTA Firmware URL**

#### 4. Perform OTA Update

On device Web UI:
1. **Settings → Firmware (OTA)**
2. Click **Check Update** (compares `FIRMWARE_VERSION` to remote `version.txt`)
3. If update available, click **Install Update**
4. Device downloads, verifies, and reboots into new firmware

### OTA Safety Features

- **Partition validation:** Checks magic byte before flashing
- **Rollback protection:** OTA partition scheme prevents bricking
- **Stall detection:** 180s timeout on download
- **HTTPS verification:** TLS certificate validation (insecure mode for self-signed)
- **Version check:** Compares local `FIRMWARE_VERSION` to remote `version.txt`

---

## Project Structure

```
sim800_gateway/
├── sim800_gateway.ino      # Main program (loop, setup, WiFi, heartbeat)
├── config.h                # All configuration, pins, timing, data structures
├── build_opt.h             # Build options (optimization flags)
├── mux.cpp / mux.h         # CD74HC4067 multiplexer control
├── sim800.cpp / sim800.h   # AT command interface, SIM initialization
├── sms.cpp / sms.h         # SMS polling, parsing, forwarding, multipart
├── webui.cpp / webui.h     # HTTP server, route handlers, HTML UI
├── ota.cpp / ota.h         # HTTPS OTA updates
├── calls.cpp / calls.h     # Missed call detection, call log
├── ussd.cpp / ussd.h       # USSD *143# balance checking
├── maintenance.cpp / maintenance.h  # Backend maintenance API
├── status_led.cpp / status_led.h   # LED status indicators
├── utils.h                 # String helpers, JSON, phone normalization
├── logger.h                # Logging macros
├── partitions.csv          # Custom partition table (1.9MB app + OTA)
├── PARTITION.txt           # Partition info
├── firmware.bin            # Pre-built firmware (for OTA releases)
├── icon-logo.png           # Web UI favicon
├── background-logo.png     # Web UI background
├── README.md               # This file
├── .gitignore
├── .vscode/
│   └── arduino.json        # VS Code Arduino config (partition scheme)
└── build/                  # Build output (gitignored)
```

---

## API Reference

### Web UI HTTP Endpoints

#### Status & Monitoring
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/` | Main dashboard page |
| GET | `/status` | JSON system status |
| GET | `/monitor` | Monitor log (text) |
| GET | `/clear-monitor` | Clear monitor log |

#### WiFi Management
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/scan` | Scan WiFi networks (JSON) |
| POST | `/save-wifi` | Save WiFi credentials |
| GET | `/disconnect` | Disconnect from WiFi |
| POST | `/network-ping` | Test DNS + HTTPS reachability |

#### SIM Management
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/sim-config` | Get all SIM states (JSON) |
| GET | `/check-sim?slot=N` | Check specific SIM |
| GET | `/check-all-sim` | Check all SIMs |
| POST | `/sim-enable?slot=N&enabled=1/0` | Enable/disable SIM |
| POST | `/sim-disable-all` | Disable all SIMs |
| POST | `/ussd-check?slot=N` | Run *143# on SIM |
| GET | `/ussd-manual-status` | USSD progress/result |
| POST | `/ussd-bulk` | Run *143# on all SIMs |
| GET | `/ussd-bulk-status` | Bulk USSD progress |
| POST | `/toggle-missed-call?enabled=1/0` | Toggle missed call forward |

#### Calls
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/calls` | Get call log (JSON) |
| GET | `/clear-calls` | Clear call log |
| POST | `/call?number=XXX&slot=N` | Make a call |
| POST | `/hangup` | Hang up current call |

#### SMS
| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/send-sms?number=XXX&message=XXX&slot=N` | Send SMS |
| GET | `/messages` | Get message history (JSON) |
| GET | `/clear-messages` | Clear message history |

#### Agent & Backend
| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/agent-config` | Save agent configuration |
| POST | `/login` | Backend login (JWT) |
| GET | `/logout` | Logout (clear tokens) |
| POST | `/refresh-token` | Refresh access token |
| POST | `/register-device` | Register device with backend |
| POST | `/register-sim?slot=N` | Register SIM with backend |
| POST | `/heartbeat` | Manual heartbeat trigger |
| POST | `/toggle-polling` | Pause/resume SMS polling |
| POST | `/toggle-heartbeat` | Pause/resume heartbeat |

#### Firmware (OTA)
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/firmware-check` | Check for updates |
| POST | `/firmware-update` | Download and flash firmware |
| POST | `/firmware-config` | Save OTA URL to NVS |

#### Modem Control
| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/modem-start` | Start SIM800 init + polling |
| POST | `/modem-stop` | Stop SMS poll / watchdog |

---

## Troubleshooting

### Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| **Wrong SIM on slot** | Incorrect mux channel mapping | Adjust `SLOT_TO_MUX_CHANNEL_INIT` in `config.h` |
| **OTA fails / Verify Bin Header Failed** | Uploaded wrong binary | Upload **only** `sim800_gateway.ino.bin` (~1MB), not partitions/merged |
| **Web UI old / no OTA tab** | Old firmware | Redeploy latest firmware; hard-refresh browser |
| **All SIMs show ₱0 / no response** | Power issue, mux wiring, or settle time | Check 5V 10A PSU, verify mux wiring, increase `MUX_SETTLE_MS` |
| **Upload fails** | Bootloader conflict | Hold BOOT button during upload, lower baud rate to 115200 |
| **WiFi won't connect** | Wrong credentials / router isolation | Use AP mode (192.168.4.1) → Scan → Re-save WiFi |
| **SMS not forwarding** | Backend not configured / auth failed | Check Settings → Agent → Base URL + Login |
| **SIM auto-disabled** | 3+ consecutive poll errors | Check SIM signal, power, or disable manually in UI |
| **Heartbeat failing** | No WiFi / backend down / auth expired | Check WiFi status, backend URL, re-login if needed |

### Debug Tips

1. **Serial Monitor:** 115200 baud, reset board, watch boot logs
2. **Monitor Log:** Web UI → Logs tab (last 20 events)
3. **Error Log:** Persistent errors in LittleFS (survives reboot)
4. **Heap Monitoring:** Low heap warnings logged when < 25KB free
5. **Watchdog:** Force-clears `simBusy` after 15s, `httpsBusy` after 90s

### Serial Boot Log Example

```
========================================
SIM800 Gateway Starting...
========================================
[BOOT] Starting...
[MUX] Initialized
[SETUP] SIM probe start
[STORE] LittleFS mounted
[WIFI] AP started SIM800-Gateway-XXXXXX IP 192.168.4.1
[WIFI] Connecting to MyWiFi
[WIFI] Connected on try 1
[WIFI] IP 192.168.1.100 GW 192.168.1.1
[MODEM] Auto-start on boot
[MODEM] SIM init starting...
[SIM] Slot 1: +639123456789
[SIM] Slot 2: +639987654321
...
[MODEM] Running — SMS poll active
[HEARTBEAT] Deferred until modem/SIM stable
```

---

## Advanced Topics

### Dual UART Mode (2-SIM, No Mux)

For simpler 2-SIM setups without multiplexer:

```c
// In config.h
#define USE_DUAL_UART   1

// Pins
#define UART1_RX_PIN    4    // SIM1 RX
#define UART1_TX_PIN    5    // SIM1 TX
#define UART2_RX_PIN    16   // SIM2 RX
#define UART2_TX_PIN    17   // SIM2 TX
```

- No mux control needed
- `SIM_COUNT` automatically set to 2
- Each SIM on independent UART

### Custom Partition Table

For advanced users needing specific partition layout:

1. Edit `partitions.csv`
2. In Arduino IDE: **Tools → Partition Scheme → Custom**
3. Select `partitions.csv` from sketch folder

Example `partitions.csv` for 16MB flash:
```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x1400000,
ota_0,    app,  ota_0,   0x1500000, 0x1400000,
ota_1,    app,  ota_1,   0x2900000, 0x1400000,
littlefs, data, littlefs,0x3D00000, 0x300000,
```

### SIM Recovery

Unresponsive SIMs are automatically probed every 45 seconds (`SIM_RECOVERY_PROBE_INTERVAL_MS`). Recovery attempts:

1. Re-initialize SIM (AT+CFUN=1)
2. Check for response (AT)
3. Re-register network (AT+CREG?)
4. If successful, re-enable in UI

### Multipart SMS Handling

Long SMS (>160 chars) are split by carrier into multiple parts:

1. Parts are buffered in `MultipartSms` struct (max 3 parts, 2 concurrent)
2. Combined when all parts received (within 30s window)
3. Forwarded as single message to backend
4. All parts deleted from SIM after processing

### LittleFS Logs

- **messages.log:** Last 30 SMS (auto-prunes oldest when full)
- **errors.log:** Persistent error log (no size limit, but pruned on mount)
- **Format:** `timestamp|sim_slot|sender|message` (pipe-delimited)

### Power Management

- **WiFi Sleep:** Disabled during HTTPS (`WIFI_PS_NONE`), restored after
- **Modem Auto-Start:** Delayed 20s after boot (`MODEM_AUTO_START_DELAY_MS`)
- **Battery Monitoring:** Reads SIM800L `AT+CBC` every 60s per SIM

---

## Related Projects

- **sim800_fast/** - Lean OTP-focused firmware variant with faster polling
- **OTPocketSMSAgent/** - Android companion app for SMS management
- **OTP fast bot/** - Telegram bot for OTP distribution

---

## License

Private / internal use. Add license file if distributing.

---

## Support

For issues, check:
1. Serial monitor logs (115200 baud)
2. Web UI → Logs tab
3. This README troubleshooting section
4. GitHub Issues (if public repo)

---

**Last Updated:** 2024-01-15  
**Firmware Version:** 1.0.21