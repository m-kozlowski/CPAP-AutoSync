# CPAP AutoSync v3.5i — Changelog

> **Note:** v3.5i is identiacal to v3.5i-beta1. This release is a major reliability release focused on fixing three reported beta bugs (#53, #54, #55) related to PCNT detection and stealth mode, plus a new AS10 card-state restoration feature. These changes improve AirSense 10/11 detection accuracy, eliminate the AS11 "SD Card Error" caused by stealth mode, and enable periodic data re-checks on AS10.

## What's New in v3.5i

### Earliest PCNT Detection (fixes #53)
- **Pre-main PCNT initialization** — The pulse counter now starts in a GCC `__attribute__((constructor))` that runs ~500ms before `setup()`. This captures the CPAP's SD card handshake pulses that were previously missed on AS11 when using blank or simple cards.
- **MUX pin locked before app_main** — Eliminates floating-pin jitter during the first 400ms of boot that could generate false PCNT readings.
- **Unified PCNT unit** — EarlyPCNT and TrafficMonitor now share a single PCNT hardware unit (handed off via `adoptUnit()`). Zero gap in pulse counting from constructor through entire runtime.
- **Unconditional NVS write on power-on boot** — The AS10/AS11 detection result is written to NVS on every power-on boot, supporting users who switch cards between machines.

### Conditional Stealth Mode (fixes #54)
- **AS11 uses regular SD init for config read** — Stealth mode left the AS11 card in 1-bit Transfer state, causing "SD Card Error" when therapy started. AS11 now uses standard `SD_MMC.begin()` for config reads, which the CPAP handles gracefully.
- **AS10 continues using stealth mode** — Card state is perfectly preserved on AS10, preventing the reboot loop caused by regular SD init.
- **Automatic selection** — The firmware uses the PCNT detection result (available before config read) to choose the appropriate path. No user configuration needed.

### AS10 Periodic Re-Checks (fixes #55)
- **Timer-based suppression clear** — On AS10 (where PCNT never fires), the no-work suppression flag is now cleared every `INACTIVITY_SECONDS` (~62s default). This enables periodic re-scans for new therapy data without relying on bus activity that will never occur.
- **"Semi-smart" mode for AS10** — Users who set `UPLOAD_START_HOUR=0` / `UPLOAD_END_HOUR=0` get automatic discovery of new data within ~60 seconds of the CPAP finishing a write.

### Stealth Card-State Restoration (AS10)
- **New `STEALTH_RESTORE` config key** (default: `true`) — After each upload, the firmware restores the SD card to Standby state via a stealth sequence (no CMD0) before handing it back to the CPAP. This prevents AirSense 10 from power-cycling the ESP32 after upload.
- **Automatic on AS10 only** — The restore only runs when PCNT detection identifies the device as AS10 (`!g_pcntCapable`). AS11 is unaffected.
- **Configurable** — Set `STEALTH_RESTORE=false` in config.txt to disable if it causes issues on your hardware.

### Bug Fixes
- **Cleaned up experimental log prefixes** — Removed `===EXPERIMENTAL===` prefixes from PCNT checkpoint logs.

### New Config Key

| Key | Default | Description |
|---|---|---|
| `STEALTH_RESTORE` | `true` | Restore SD card to Standby state after upload via stealth sequence. Prevents AS10 power-cycling the ESP. Only active on AS10. Set `false` to disable. |

### Upgrade Notes
- **v3.1.0i-beta2 or v3.x → v3.5i:** OTA update supported
- **No config.txt changes required** — All new behavior is automatic. The new `STEALTH_RESTORE` key defaults to `true` (enabled) and only affects AS10 devices.

---

**Full Specification:** See `docs/archive/67-PCNT-AS11-PLAN.md` for the detailed implementation plan.

---

## How to Flash (Web Flasher)

**For users upgrading from earlier releases or new installations:**

1. **Prepare the hardware:**
   - Connect the SD WIFI PRO to the development board with switches set to:
     - Switch 1: OFF
     - Switch 2: ON

2. **Use a desktop Chromium-based browser** (Chrome, Edge, or Opera — Firefox/Safari not supported)

3. **Steps:**
   - Extract the release ZIP to a folder on your computer
   - Open https://esptool.spacehuhn.com/ in Chrome, Edge, or Opera
   - Connect the ESP32 board by USB
   - Click **Connect** and choose the serial port:
     - **Windows:** `USB Serial (COM5)`, `USB-SERIAL CH340 (COMx)`, or `Silicon Labs CP210x USB to UART Bridge (COMx)` 
     - **macOS:** `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART` 
     - **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0` 
     - *Tip: If unsure, click Connect first, then plug in the board — the new port is usually the right one*
   - Delete any existing rows if needed, then click **Add** once
   - Make sure the address is **`0x0`**
   - Select **`firmware-ota.bin`** (the complete image for first-time flashing)
   - **DO NOT SKIP THIS STEP:** Click **Erase** — this is mandatory for clean state
   - Click **Program**

**For users upgrading from any v3.x:**

1. Open the web interface at `http://cpap.local` (or `http://<device-ip>/`)
2. Go to the **OTA** tab
3. Upload `firmware-ota-upgrade.bin`
4. Wait for the device to restart (~30 seconds)

**CRITICAL: After a full USB flash (not OTA), BEFORE inserting the ESP32 into your CPAP machine:**

You **must** update `config.txt` on your SD card with your actual WiFi credentials. The **Erase** step wipes all previously stored passwords from the ESP32's flash memory (including any credentials that were migrated to NVS in previous versions). If you skip this, the device will fail to connect with "SSID is empty" or authentication errors.

**Important:** Use `firmware-ota.bin` (not `firmware-ota-upgrade.bin`) when doing a full reflash. The web flasher performs a complete erase automatically.
