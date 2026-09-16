# SIM800 Gateway → OTPocket V2 Integration Plan

**Status:** Planning only — no firmware or app code has been changed by this document.
**Scope:** (1) Port `sim800_gateway` firmware from V1's HTTPS/JWT agent protocol to V2's MQTT/EMQX protocol. (2) Design a standalone **Gateway Fleet Manager** app (evolution of `s8lgateway`) for hardware-level management over each gateway's own local web server — a separate concern from the existing OTPocket "Provider" app.
**Target hardware:** ESP32-N16R8 (16MB flash / 8MB PSRAM), 12–16× SIM800L slots via CD74HC4067 mux.

---

## 1. Why this is two separate problems, not one

It's important to keep these apart, because they talk to different things over different transports:

| | **OTPocket Provider app** (existing) | **Gateway Fleet Manager** (this plan) |
|---|---|---|
| What it manages | A provider's *account*: inventory as OTPocket sees it, earnings, SIM stock, exclusions | The *physical hardware*: is this ESP32 board alive, which SIM800L modules are wired correctly, OTA, LED/power diagnostics |
| Talks to | OTPocket cloud backend (its own API) | Each gateway's **local web server** directly, LAN-only (`http://<gateway_ip>/...`), same as `sim800_gateway`'s existing Web UI and `s8lgateway`'s design |
| Data source | The database (post-MQTT-ingestion, i.e. what already reached the backend) | The device itself, live, including state the backend never sees (raw AT command failures, per-slot voltage, mux channel health, boot logs) |
| Works without internet? | No — needs the backend | Yes — LAN-only, works even if the gateway's WAN is down |
| Audience | Provider checking their own dashboard/earnings | Whoever is physically racking/fixing/flashing the hardware (could be the provider, could be a technician with no OTPocket account at all) |

Merging these would force the Provider app to reimplement LAN discovery, raw device HTTP, and hardware-specific screens (mux channel debug, per-slot voltage) that have nothing to do with "my OTPocket account" — and would break the "works with no internet" property that makes on-site hardware debugging possible in the first place. So: **two apps, two transports, sharing only device identity (the `device_code`) as the thing that ties a hardware unit to an OTPocket account.**

```
┌─────────────────────┐         ┌─────────────────────┐
│  OTPocket Provider   │  HTTPS  │   OTPocket Backend   │◄──MQTT/EMQX──┐
│  app (existing)      │◄───────►│   (this repo)        │              │
└─────────────────────┘         └─────────────────────┘              │
                                                                        │
┌─────────────────────┐   LAN    ┌──────────────────────┐             │
│  Gateway Fleet       │  HTTP    │  ESP32 sim800_gateway │─────────────┘
│  Manager (new app)   │◄────────►│  (local web server +  │
└─────────────────────┘          │   MQTT client)         │
                                  └──────────────────────┘
                                            │
                                  16× SIM800L via CD74HC4067
```

---

## 2. Current state: V1 firmware vs V2 backend protocol

`sim800_gateway` (current, v1.0.21) talks to the **old V1 backend** exclusively:

| Concern | V1 firmware (current) |
|---|---|
| Transport | HTTPS POST, JSON body |
| Auth | Login → JWT access + refresh token, stored in NVS, `Authorization: Bearer` header |
| Inventory report | `POST /api/agent/heartbeat` every 60s (lightweight ping), full sync every 30 min |
| SMS forward | `POST /api/agent/incoming-sms` per message |
| Online/offline detection | Backend infers from heartbeat gaps (no push signal) |

**OTPocket V2** (confirmed by reading this repo's actual ingestion code) uses something structurally different:

| Concern | V2 backend (actual, current) |
|---|---|
| Transport | MQTT over TLS (mqtts, port 8883) to EMQX, `clientId: device:<device_code>` |
| Auth | Per-device 6-character credential used as the **MQTT password**; also echoed as an MQTT v5 user-property `auth` on every publish (EMQX forwards this to the backend webhook, which re-checks it per message) |
| Inventory report | Publish a JSON **array** to `device/<code>/inventory`: `[{slot, number, signal?, networkType?, carrier?, excludedApps?}, ...]` — the backend does change-detection itself; publishing identical state again performs zero DB writes, so there's no need for the firmware to suppress duplicates itself |
| SMS forward | Publish JSON to `device/<code>/sms`: `{direction?, from?, to?, text, providerSmsId?, receivedAt?}` — `to` is which SIM number received it (this is how the backend knows which of the 16 SIMs a message belongs to) |
| Status/heartbeat | `device/<code>/status` topic (lightweight, e.g. battery/uptime — mirrors what `/status` already reports locally) |
| Online/offline detection | **Push-based**, not poll-based: EMQX itself tells the backend when the MQTT client connects/disconnects (LWT), with a 30s grace window before the backend marks a device offline — no heartbeat-timeout guessing needed |
| Pairing | A provisioning call returns `{deviceId, assignedHost, mqtt: {host, port, clientId, topics}}` plus the device's credential — this becomes the one-time "login" instead of JWT |

The SIM-handling core (`mux.cpp`, `sim800.cpp`, `sms.cpp`, `ussd.cpp`, `calls.cpp`) doesn't know or care which backend protocol is in use — **only the transport/auth layer changes.**

---

## 3. Firmware migration plan (phased)

### Phase 0 — Prep (no behavior change)
- Add an MQTT client library (PubSubClient, or ESP-IDF's native `esp-mqtt` for better TLS/PSRAM behavior) alongside the existing HTTPS client — don't rip anything out yet.
- Add a `PROTOCOL_MODE` build flag (`AGENT_HTTPS` / `AGENT_MQTT`) so the firmware can run either backend during the transition, selectable in `config.h` or via NVS — this lets a batch of already-deployed V1 gateways keep working on the old backend while new/updated units switch over, with no hard cutover date required.

### Phase 1 — Pairing over MQTT
- Replace the JWT login screen (Web UI `Settings → Agent → Login`) with a "Pair with OTPocket" flow that calls the V2 provisioning endpoint and stores the returned `{mqtt.host, mqtt.port, mqtt.clientId, credential}` in NVS instead of access/refresh tokens.
- Keep the existing `agent/dev`, `agent/base` NVS keys conceptually (device id, backend host) — just repurpose their contents.

### Phase 2 — MQTT connect + status/LWT
- Connect to EMQX with the stored credential as MQTT password.
- Set an LWT (or rely purely on EMQX's connect/disconnect events, matching how other V2 devices already behave — confirm which the backend actually expects before committing to a specific LWT payload).
- Publish periodic `device/<code>/status` messages (battery, uptime, firmware version) — mirrors the existing `/status` JSON already served locally, just also pushed to MQTT.

### Phase 3 — Inventory over MQTT
- Replace the `POST /api/agent/heartbeat` call in `maintenance.cpp` with a publish to `device/<code>/inventory` — same SIM-state data the firmware already assembles for the JSON heartbeat body today, just re-shaped to the `[{slot, number, signal, networkType, carrier}]` array format and a different transport call.
- Drop the "full sync every 30 min vs lightweight ping every 60s" distinction — the backend already no-ops on unchanged state, so a single consistent publish cadence (recommend every 60–120s) is sufficient and simpler than V1's two-tier scheme.

### Phase 4 — SMS over MQTT
- Replace the `POST /api/agent/incoming-sms` call in `sms.cpp` with a publish to `device/<code>/sms` per delivered message, `to` set to the receiving SIM's number.
- Missed-call → Viber-OTP forwarding (`calls.cpp`) publishes through the same `sms` topic/shape — no separate path needed.

### Phase 5 — Cutover + cleanup
- Once a batch of gateways is confirmed stable on MQTT, remove the HTTPS agent code path and `PROTOCOL_MODE` flag.
- OTA (`ota.cpp`), USSD (`ussd.cpp`), Web UI (`webui.cpp`), and LittleFS logging are **unchanged** by any of the above — they're independent of which backend protocol is active.

### Resolved against the live backend (checked directly in the OTPocket V2 repo)

1. **LWT topic/payload — none needed at the application level.** The backend relies entirely on EMQX's own broker-level `client.connected`/`client.disconnected` events (matched via `clientid = device:<code>`), never on a device-published topic — confirmed in `MqttIngressService.handleConnect`/`handleDisconnect` and the webhook's EVENT-mode branch. `device/<code>/status` is not part of online/offline detection at all; it's optional supplementary data (battery/uptime) processed the same as any other data topic. **Firmware action:** just connect with `clientId: device:<code>`; setting a standard MQTT CONNECT-packet will-message is good practice so EMQX still fires its disconnect event on an ungraceful drop, but no specific will-message payload/topic is required or read by the backend.

2. **Full state required, every publish — no delta/partial support.** `InventoryService.syncInventory()`'s own doc comment is explicit: *"Upserts each payload SIM by (device_id, slot), re-enables missing ones, and DISABLES SIMs absent from the payload."* Any slot the firmware omits from a given publish is treated as "no longer present" and gets disabled — **the firmware must always publish the complete current slot state**, not just what changed. Also: one malformed/unnormalizable phone number anywhere in the array rejects the *entire* payload (nothing partial is written), so validate every entry client-side before publishing.

3. **No rate limit applies to this hardware.** A throttle mechanism exists in the type signature (`BeatResult.sync: "durable" | "throttled" | "none"`) but only ever applies to `device_class = "small"` — confirmed via a test explicitly titled "never throttles a gateway heartbeat (always durable)". A 16-SIM board is unambiguously `device_class = "gateway"`, so every inventory publish is processed durably with no backend-side throttling. No application-enforced payload size cap either — a 16-slot JSON array is a few KB, trivial against EMQX's own broker-level message size limits. **Recommended heartbeat interval from §3 Phase 3 (60–120s) stands with no adjustment needed.**

---

## 4. Hardware: ESP32-N16R8, 12–16 slots

- **Flash (16MB):** comfortably fits dual OTA app partitions (matching the existing `partitions.csv` approach) plus a larger LittleFS region for message/error logs than V1's board allowed — no more tight partition tuning.
- **PSRAM (8MB):** absorbs MQTT+TLS connection buffers and the larger `MultipartSms`/SIM-state structures for 16 concurrent slots without competing with the Web UI and OTA buffers for internal SRAM — this was a real constraint on V1's board choice; the N16R8 removes it.
- **Mux wiring:** identical to V1 — CD74HC4067, same `S0–S3` GPIO scheme, same shared-TX/muxed-RX topology. No hardware redesign needed there, only confirm GPIO numbers don't collide with N16R8's PSRAM-reserved pins (verify against the specific N16R8 dev board pinout before finalizing `config.h`).
- **Power:** unchanged guidance from V1 — 5V 10A+ for a full 16-SIM populated board, 100µF decoupling per SIM800L.

## 5. LED status design

Recommend an addressable RGB (WS2812/NeoPixel), one per unit, replacing V1's single status LED — enough distinct states to diagnose from across a room without opening a laptop:

| State | Pattern |
|---|---|
| Booting | Dim white, slow pulse |
| WiFi connecting | Blue, slow blink |
| WiFi up, MQTT connecting | Blue, solid |
| MQTT connected, all SIMs healthy | Green, solid |
| MQTT connected, some SIMs down/unregistered | Amber, solid |
| All SIMs down / no signal | Red, solid |
| OTA in progress | Purple, fast blink |
| Fault (mux/power brownout detected) | Red, fast blink |

---

## 6. Gateway Fleet Manager app (replaces/evolves `s8lgateway`)

Confirmed distinct from the OTPocket Provider app (see §1). Talks **only** to each gateway's local web server (LAN), never to the OTPocket backend directly — this is what keeps it useful even when a gateway's WAN is down, which is exactly the situation a technician most needs it for.

### Carries over from `s8lgateway`'s existing design (already well-specified there)
- Multi-gateway dashboard, LAN discovery/pairing, SIM Manager grid, SMS/log viewer, OTA manager (single + bulk), Drift local cache, offline-first sync.
- Reuse `s8lgateway`'s `LegacyGatewayApiClient` (`/status`, `/sim-config`, etc.) as-is — none of that changes just because the firmware's *backend* protocol is moving to MQTT; the ESP32's *local* Web UI/API is a separate, unaffected surface.

### What's new/changes because of this plan
- **`/status` gains protocol + LED-state fields**: `protocol_mode` ("https"/"mqtt"), `mqtt_connected`, `led_state` — so the Fleet Manager app can show MQTT connectivity and LED state without a second endpoint.
- **New OTA-adjacent screen: "Protocol Migration"** — for the transition window in §3, show each gateway's current `PROTOCOL_MODE` and let the technician flip a unit from HTTPS to MQTT remotely (over the same LAN connection already used for OTA), rather than needing physical USB access per unit.
- **Bridge to Provider app via `device_code` only**: the Fleet Manager app never needs OTPocket login — but showing "this hardware unit = this OTPocket device_code" next to each gateway (read from `/status`) is what lets a technician correlate a physical unit with what the provider sees in their own Provider app dashboard, without the two apps needing to talk to each other.

---

## 7. Suggested build order

1. Firmware Phase 0–2 (MQTT client + pairing + status/LWT) on one test unit, confirm against a real EMQX broker and the backend's `/api/webhooks/mqtt` ingestion.
2. Firmware Phase 3–4 (inventory + SMS over MQTT), same test unit, confirm SIM/SMS actually land correctly in the OTPocket admin inventory UI end-to-end.
3. LED hardware change + `led_state` in `/status`.
4. Fleet Manager app: port `s8lgateway`'s Phase 1–2 (foundation + gateway list) unmodified, since none of that depends on the firmware's backend protocol.
5. Fleet Manager app: add the Protocol Migration screen once Phase 0's `PROTOCOL_MODE` flag exists in firmware to flip.
6. Roll Phase 5 (HTTPS code removal) only after a batch of field units have run stable on MQTT for a real deployment cycle.
