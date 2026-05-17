# CPAP AutoSync v3.6i-beta2 — Monitoring UI Fixes & Stealth Mode Improvements

This release builds upon v3.6i-beta1 with focused fixes for the Web UI's SD Activity Monitor tab, while also introducing significant improvements to stealth mode SD card handling.

---

## 🔧 Monitoring UI Fixes

### Monitoring State Restoration (GitHub Issue #26)
- **Problem**: The monitoring state was lost when refreshing the page or navigating away from the SD Activity tab, causing the "Stop" button to disappear even when monitoring was active.
- **Fix**: Added client-side logic to restore the monitoring state from the server FSM on page load. When the server reports `MONITORING` state but the client's `monActive` flag is false, the UI now automatically reactivates monitoring polling and updates the button state.
- **Impact**: Users can now refresh the page or navigate away and back without losing the monitoring UI state.

### Time Display Formatting
- **Problem**: The "In state" time display on the dashboard showed only minutes and seconds (e.g., "60m 0s") when durations exceeded 59 minutes, making it difficult to read longer durations.
- **Fix**: Enhanced the time formatting logic to display hours, minutes, and seconds when the duration exceeds 59 minutes (e.g., "2h 15m 30s").
- **Impact**: Improved readability for long-duration monitoring sessions.

### Start/Stop Button Visibility
- **Problem**: The syncMonBtn() function only managed the "Start" button visibility, leaving the "Stop" button state unhandled in some cases.
- **Fix**: Updated syncMonBtn() to properly manage both the "Start" (btn-mst) and "Stop" (btn-msp) buttons based on the monitoring state and FSM state.
- **Impact**: The correct button is now always displayed based on the current monitoring state.

---

## 🔒 Stealth Mode Improvements (PR by m-kozlowski)

### Overview
m-kozlowski's PR significantly enhanced stealth mode by adding card state capture and restoration capabilities that are used around EVERY upload cycle (both AS10 and AS11). Previously, stealth mode was only used for reading config.txt at boot (AS10 only) and hardcoded card state restoration after uploads.

### Key Benefits
- **Precise Card State Preservation**: The device now captures the exact card state (RCA, bus width, card state) BEFORE `SD_MMC.begin()` sends CMD0, then restores it to that exact state AFTER `SD_MMC.end()`. This ensures the CPAP machine can resume SD access without errors, regardless of whether it was in Standby or Transfer state.
- **Bus Width Detection**: The implementation empirically detects the card's bus width (1-bit or 4-bit) during state capture, allowing accurate restoration instead of assuming a fixed width.
- **Universal Compatibility**: Works on both AirSense 10 (AS10) and AirSense 11 (AS11) CPAP machines, improving reliability across all hardware.
- **Reduced CPAP Interference**: By preserving the exact card state the CPAP left it in, the ESP minimizes disruption to the CPAP's SD access patterns, reducing the likelihood of CPAP power-cycling the ESP.

### Technical Details
- **Bare-Metal Register Access**: Direct manipulation of SDMMC controller registers to avoid ESP-IDF ISR interference
- **State Capture**: `captureCardState()` probes card state (RCA, bus width, card state) without sending CMD0, called before `SD_MMC.begin()`
- **State Restoration**: `restoreToSavedState()` restores card to previously captured state after `SD_MMC.end()`, using ACMD6 to set bus width and CMD7 to manage selected/deselected state
- **Bus Width Detection**: Empirical 4-bit read test to determine card's configured bus width during state capture
- **Fallback Support**: If state capture fails, falls back to the original hardcoded `restoreCardState()` (Standby/1-bit)

### Note: Dual Stealth Mode Approaches
The firmware uses two distinct stealth mode approaches for different purposes:

1. **Boot Config Reading (AS10 only)**: `StealthConfigReader::readConfigTxt()` reads `config.txt` without any SD card initialization (no CMD0). Uses custom FAT32 parser. Returns card to original state found. Used only on AS10 at boot.

2. **Upload State Preservation (AS10 and AS11)**: `captureCardState()`/`restoreToSavedState()` captures card state before `SD_MMC.begin()` (which sends CMD0) and restores it after `SD_MMC.end()`. No FAT32 parsing needed. Used on both AS10 and AS11 during upload cycles.

---

## 📂 Cumulative Changes Since v3.5i

This release includes all features from v3.6i-beta1:

### 🚀 AP Setup Wizard & Captive Portal
- **Automatic Trigger**: Starts automatically on fresh power-up if no WiFi is configured
- **Captive Portal Support**: Configuration page auto-opens on iOS, Android, and Windows when connecting to `CPAP-AutoSync` network
- **Dark Mode UI**: Overhauled setup page matching main dashboard aesthetic with animated branding
- **Manual Access**: Available at `http://192.168.4.1/setup` in AP mode or `http://cpap.local/setup` in normal operation

### 📶 WiFi & Connection Robustness
- **Dual-Attempt Connection**: Two WiFi connection attempts with 3-second delay for better mesh network stability
- **SSID Scanning & Deduplication**: "Scan" button shows nearby networks with automatic mesh node filtering
- **Cold-Boot Guard**: AP mode restricted to power-on events only, preventing unsolicited AP on soft reboots
- **Credential Warning**: Clear warning when incorrect WiFi credentials are saved

### 🌍 Timezone & Localization
- **Robust IANA Persistence**: Fixed timezone "flipping" by saving exact IANA name (`TZ_NAME`) to config
- **Human-Readable Dashboard**: Timezone field displays friendly UTC offset (e.g., `UTC+10:00 (DST active)`)
- **Server-Side DST Calculation**: UTC offset computed live based on POSIX rules
- **Improved TZ Database**: Migrated to JSON-based timezone database for faster, more reliable setup

### 🛠️ UI & Configuration UX
- **Password Visibility**: 👁️ toggle buttons on all password fields (WiFi, SMB, SleepHQ)
- **SleepHQ Help**: Repositioned API credential instructions with clear ℹ️ info banner
- **Atomic Config Saving**: Improved config writing process for data integrity
- **Live Preview**: Setup wizard shows real-time `config.txt` preview

### 🆕 New Configuration Keys
| Key | Default | Description |
|---|---|---|
| `TZ_NAME` | *(empty)* | IANA city name (e.g. `Europe/London`). Used by web UI for consistent dropdown selection. |

---

## 📂 Upgrade Instructions

### For Users on v3.6i-beta1:

1. **OTA Update**: Upload `firmware-ota-upgrade.bin` via the standard web interface.
2. **Verification**: Navigate to the SD Activity Monitor tab and verify that the monitoring state persists across page refreshes.

### For Users on v3.5i or older:

1. **OTA Update**: Upload `firmware-ota-upgrade.bin` via the standard web interface.
2. **First Boot**: The device will continue using your existing `config.txt`.
3. **Finish Setup**: It is recommended to visit `http://cpap.local/setup`, verify your Timezone (which will now show the city name), and click **Save & Restart** to generate the new `TZ_NAME` entry.

### For Fresh Installations:

1. Burn the firmware via USB.
2. Power on the device.
3. Connect to the `CPAP-AutoSync` WiFi network on your phone.
4. Follow the setup wizard to configure your WiFi, Timezone, and SleepHQ/SMB credentials.

---

## 📋 Full Changelog

### v3.6i-beta2
- **Fixed**: Monitoring state restoration on page refresh/navigation (GitHub Issue #26)
- **Fixed**: Time display formatting for durations over 59 minutes
- **Fixed**: Start/Stop button visibility in SD Activity Monitor tab
- **Added**: Stealth mode card state capture/restore: Added `captureCardState()` and `restoreToSavedState()` functions to preserve exact SD card state (RCA, bus width, card state) around `SD_MMC.begin()`/`SD_MMC.end()` cycles (PR by m-kozlowski)
- **Bus width detection**: Empirical 4-bit read test to detect card's configured bus width during state capture
- **Universal stealth restore**: Card state restoration now works on both AS10 and AS11 CPAP machines, improving reliability across all hardware

**Note**: Stealth mode uses two distinct approaches: (1) Boot config reading without SD init/CMD0 (AS10 only, custom FAT32 parser), and (2) Upload state preservation around SD_MMC.begin()/end() with CMD0 (AS10/AS11, no FAT32 parsing).

### v3.6i-beta1 (Cumulative)
- **Added**: AP Setup Wizard with captive portal support
- **Added**: WiFi scanning with mesh node deduplication
- **Added**: Timezone IANA name persistence (`TZ_NAME`)
- **Added**: Human-readable UTC offset display on dashboard
- **Improved**: WiFi connection robustness with dual-attempt logic
- **Improved**: Timezone database with JSON-based lookup
- **Improved**: Config writing atomicity and data integrity
- **Added**: Password visibility toggles on all password fields
- **Fixed**: Timezone flipping bug (Melbourne → Sydney)
- **Fixed**: AP mode triggering on soft reboots (now power-on only)
