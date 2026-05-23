# SIM800 Gateway (ESP32)

16-SIM ESP32 gateway with CD74HC4067 mux, local web UI, and HTTPS OTA updates (optional via GitHub Releases).

## Hardware

- **ESP32** (UART + WiFi)
- **CD74HC4067** mux (16 channels) — logical slot → mux channel mapping is in `config.h` (`SLOT_TO_MUX_CHANNEL_INIT`)
- **SIM800L** modules (shared TX, muxed RX)
- Default pins: see `config.h`

## Arduino IDE setup

1. Install **ESP32 board support** (Board Manager → `esp32` by Espressif).
2. Open **`sim800_gateway.ino`** (Arduino opens the whole folder).
3. **Tools → Board**: your ESP32 board (e.g. ESP32 Dev Module).
4. **Tools → Partition Scheme**: choose one **with OTA**, e.g.:
   - `Minimal SPIFFS (1.9MB APP with OTA)` — recommended
   - or `Huge APP (3MB No OTA)` only if you do **not** use OTA
5. **Tools → Upload Speed**: 921600 (or 115200 if upload fails).
6. **Port**: select the ESP32 COM port.
7. Click **Upload** (first flash must be USB).

## Create a GitHub repo and upload files

### Option A — GitHub website (easiest)

1. Go to [github.com/new](https://github.com/new).
2. Repository name: `sim800_gateway` (or any name).
3. **Private** recommended (firmware may contain default WiFi in `config.h`).
4. Do **not** add README/license if you already have local files.
5. Create repository.

On your PC (PowerShell), from the sketch folder:

```powershell
cd "C:\Users\Niel Ivan\Documents\Arduino\sim800_gateway"

git init
git add .
git commit -m "Initial SIM800 gateway firmware"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/sim800_gateway.git
git push -u origin main
```

Replace `YOUR_USERNAME` with your GitHub username. Sign in when Git asks.

### Option B — GitHub CLI (`gh`)

```powershell
cd "C:\Users\Niel Ivan\Documents\Arduino\sim800_gateway"
git init
git add .
git commit -m "Initial SIM800 gateway firmware"
gh repo create sim800_gateway --private --source=. --remote=origin --push
```

### What gets uploaded

All `.ino`, `.cpp`, `.h` files in this folder. Images (`icon-logo.png`, etc.) are included if you `git add .`.

**Before pushing:** consider removing real WiFi passwords from `config.h` (`DEFAULT_WIFI_*`) or use only the web UI / NVS after first boot.

---

## Testing (local, USB + WiFi)

### 1. Serial monitor

- **Tools → Serial Monitor**, baud **115200**.
- Reset the board. You should see:

  ```
  SIM800 Gateway Starting...
  [MUX] Initialized
  [SETUP] SIM probe start
  ```

### 2. Web UI (no GitHub needed)

1. After boot, the ESP starts an AP: **`SIM800-Gateway-XXXXXX`** (see Serial for IP).
2. Connect your phone/PC to that AP (or use home WiFi if configured in `config.h` / saved settings).
3. Open browser: **`http://192.168.4.1`** (AP) or the STA IP printed in Serial (e.g. `http://192.168.x.x`).
4. Check:
   - **Dashboard** — WiFi, stats
   - **SIMs** — probe each slot; numbers should match physical positions 1–16
   - **Settings** — backend URL, device ID
   - **Logs** — monitor log

### 3. MUX / slot mapping

Physical bank **1** = logical **SIM 1** (slot 0) → mux channel **3** (see `config.h`).  
If the wrong SIM answers on a slot, adjust `SLOT_TO_MUX_CHANNEL_INIT` only.

### 4. SMS / backend (optional)

- Set **Base URL** + agent token in Settings (or defaults in `config.h`).
- Send a test SMS to a SIM; watch **Logs** and backend.

### 5. OTA (after GitHub + second flash)

OTA needs:

1. Partition scheme **with OTA** (step above).
2. Device on **WiFi with internet** (not AP-only).
3. A **`.bin`** hosted at a public HTTPS URL.

#### GitHub Releases workflow

1. In Arduino IDE: **Sketch → Export compiled Binary** (after a successful compile).  
   - Find the `.bin` under `build/esp32.../` or in the sketch folder (depends on IDE version).  
   - Rename/copy to **`firmware.bin`**.

2. On GitHub: repo → **Releases** → **Create a new release**  
   - Tag: `v1.0.0`  
   - Attach **`firmware.bin`** as a release asset.

3. Optional version check file on `main` branch:

   ```
   firmware/version.txt   → single line: 1.0.1
   ```

4. In `config.h` set:

   ```c
   #define FIRMWARE_VERSION    "1.0.0"
   #define OTA_GITHUB_OWNER    "YOUR_USERNAME"
   #define OTA_GITHUB_REPO     "sim800_gateway"
   #define OTA_FIRMWARE_BIN    "firmware.bin"
   ```

   Or leave owner empty and set URL in the device web UI (**Settings → Firmware**):

   ```
   https://github.com/YOUR_USERNAME/sim800_gateway/releases/latest/download/firmware.bin
   ```

5. On device: **Settings → Firmware (OTA)** → **Check Update** → **Install Update**.

#### Test OTA safely

1. Flash **v1.0.0** via USB with `FIRMWARE_VERSION "1.0.0"`.
2. Bump to **1.0.1** in code, export `firmware.bin`, publish release **v1.0.1**.
3. On device: Check Update should show newer version; Install Update reboots into new build.
4. Serial should show new version; Settings shows updated **Installed** version.

---

## Project files

| File | Role |
|------|------|
| `sim800_gateway.ino` | Main loop, WiFi, heartbeat |
| `config.h` | Pins, slots, OTA, intervals |
| `mux.cpp` / `mux.h` | Mux + slot mapping |
| `sim800.cpp` / `sim800.h` | AT commands |
| `sms.cpp` / `sms.h` | SMS poll, forward |
| `webui.cpp` / `webui.h` | HTTP server + UI |
| `ota.cpp` / `ota.h` | HTTPS OTA |
| `utils.h`, `logger.h` | Helpers |

## Troubleshooting

| Issue | Check |
|-------|--------|
| Wrong SIM on slot | `SLOT_TO_MUX_CHANNEL_INIT` in `config.h` |
| OTA fails | OTA partition scheme, WiFi internet, URL opens in browser |
| Web UI old / no OTA tab | Redeploy firmware; hard-refresh browser |
| All SIMs ₱0 / no response | Power, mux wiring, `MUX_SETTLE_MS`, Serial logs |
| Upload fails | Hold BOOT, lower upload speed, correct COM port |

## License

Private / internal use unless you add a license file.
