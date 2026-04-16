# CPAP AutoSync v4.0-beta1 — Major Beta Release

> **⚠️ IMPORTANT: This release changes the partition table. OTA upgrades from v3.6i or earlier versions are NOT possible. You must perform a full flash via USB using the web flasher. See Upgrade Instructions below.**

## What's New in v4.0-beta1

### 🧠 Smart Mode with Quiet Period
The headline feature of v4.0 is an entirely updated Smart Mode designed to only be active closer to the end of the therapy schedule.
- **Therapy Protection**: A configurable "quiet period" prevents uploads from running while you're actively using therapy, avoiding the risk of SD Card errors on AirSense 11.
- **Reduced Retries Outside Window**: When outside your configured upload window, Smart mode uses a reduced retry strategy to avoid prolonged upload sessions that could interfere with therapy.

### 🛡️ Smart Mode Configuration Validation
We've added server-side validation to prevent misconfigurations that could cause uploads during therapy hours.
- **Auto-Downgrade**: If `SMART_START_HOUR` falls within or at the boundary of the upload window, the firmware automatically downgrades to Scheduled mode at boot.
- **Dashboard Indicator**: When Smart mode is invalid, the dashboard displays a red ⓘ badge with an explanation.
- **Setup Wizard Protection**: The new timeline slider UI enforces a minimum 1-hour gap between "earliest wake-up" and "latest wake-up" to prevent invalid configurations at the input stage.

### 🔍 Hardware-Aware Smart Mode Detection
Smart mode requires 4-bit SD bus activity detection (PCNT peripheral). Not all devices support this.
- **Auto-Detection**: The firmware detects at boot whether the device supports PCNT-based activity monitoring.
- **Graceful Fallback**: On devices without PCNT capability (typically AirSense 10), Smart mode is automatically disabled and the setup wizard shows an inline explanation.
- **User-Friendly UI**: When Smart mode is unavailable, the radio button is greyed out with a clear message explaining why.

### ⏱️ Advanced Timing Controls
Both **`EXCLUSIVE_ACCESS_MINUTES`** and **`COOLDOWN_MINUTES`** are now configurable in the setup wizard. They are hidden under a "Timing Settings" accordion on the setup page to keep the interface clean.

### 🎨 Redesigned Timeline Slider
The upload schedule UI has been completely overhauled for clarity and usability.
- **Intuitive Labels**: Replaced technical terms with plain-English descriptions: "What time do you usually finish your therapy session?", "When are you absolutely done with therapy?", and "What is the earliest time you go to sleep?".
- **Visual Timeline Bar**: A colored gradient bar shows the quiet period (dark blue), recent-data-only window (light blue), and full-archive window (green).
- **Hour Tick Marks**: Time markers (0, 3, 6, 9, 12, 15, 18, 21) below the bar provide visual reference points.
- **Color Legend**: A static legend explains what each color means without requiring hover interaction.
- **Mode-Aware**: The blue legend entry and "keep gap small" tip only appear in Smart mode; Scheduled mode shows a simplified view.

### ✅ Setup Wizard Form Validation
The setup wizard now validates inputs before saving to prevent invalid configurations.
- **Endpoint Requirement**: You must configure either SleepHQ OR SMB server (or both) before saving. A modal error explains this if you try to save without an endpoint.
- **Numeric Range Validation**: `COOLDOWN_MINUTES` and `EXCLUSIVE_ACCESS_MINUTES` are clamped to their valid ranges (1–60 and 1–30 respectively) before saving.
- **User-Friendly Feedback**: Validation errors appear in a styled modal matching the rest of the UI.

### 🔧 Unified Stealth SD Card Access (AS10 + AS11)
The firmware no longer uses two separate approaches for safe SD card access. All device types now go through a single stealth-aware path at both boot and during upload cycles.
- **Capture Before Mount**: Before `SD_MMC.begin()` sends CMD0, the card's RCA, state, and bus width are captured via a stealth probe (`captureCardState()`).
- **Restore After Unmount**: After `SD_MMC.end()`, the card is restored to its exact pre-mount state via `restoreToSavedState()`, so the CPAP machine resumes seamlessly.
- **AS10 Boot Simplified**: Previously, AS10 at boot used a custom FAT32 parser to read `config.txt` without touching the SDMMC driver. This has been replaced by the same capture/mount/restore path used during uploads — less code, same card safety guarantee.
- **No Behavioral Change for End Users**: The CPAP machine continues to see the SD card in exactly the state it left it.

### 🌐 VPN-Friendly Reconnection
The "Saving & Rebooting" modal now includes intelligent fallback logic for users accessing the device via VPN or networks without mDNS.
- **Two-Phase Polling**: First attempts hostname-based reconnection (`cpap.local`) for 30 seconds, then automatically switches to IP-based polling for the remaining 90 seconds.
- **Automatic IP Detection**: The device's IP is automatically extracted from the browser's current URL — no manual input required.
- **AP Mode Unchanged**: In Access Point (SoftAP) mode, the wizard continues to use hostname-based polling exclusively.
- **Transparent Logging**: The debug console shows when and why the reconnection strategy switches, making troubleshooting easier.

---

## Partition Table Changes

This release increases the app partition size to accommodate future development and shrinks the SPIFFS partition to a size that still provides healthy wear-leveling headroom.

| Partition | Before | After | Change |
|-----------|--------|-------|--------|
| app0/app1 | 1.625 MB | **1.835 MB** | +210 KB each |
| spiffs | 768 KB | **384 KB** | -384 KB |
| coredump | 64 KB | 64 KB | unchanged |

**Why this matters:**
- The firmware has reached 99.7% of the old app partition size, leaving only 5 KB of headroom for future features.
- The new app partitions provide ~200 KB of headroom for ongoing development.
- SPIFFS at 384 KB still provides 2.5× the working set size (logs + state files), ensuring healthy wear-leveling.

---

## Configuration Reference

### New Keys

| Key | Default | Range | Description |
|---|---|---|---|
| `SMART_START_HOUR` | `6` | 0–23 | Earliest hour Smart mode monitoring begins (quiet period ends). Must be strictly less than `UPLOAD_START_HOUR`. |

---

## Upgrade Instructions

### ⚠️ OTA Upgrades Are NOT Possible

Due to the partition table change, you **cannot** upgrade from v3.6i (or any earlier version) via OTA. A full flash via USB is required.

### Full Flash Instructions

1. **Backup Your Configuration**
   - Before flashing, note your current configuration settings (WiFi credentials, SleepHQ/SMB details, timezone, etc.).
   - Your config file on the SD card (`/config.txt`) will be preserved, but it's good practice to have a backup.

2. **Prepare the Hardware (Flash Mode)**
   - Connect the SD WIFI PRO to the development board with switches set to:
     - Switch 1: **OFF**
     - Switch 2: **ON**

3. **Flash via Web Flasher**
**Use a desktop Chromium-based browser** (Chrome, Edge, or Opera — Firefox and Safari are not supported).

**Steps:**
   - Extract the release ZIP to a folder on your computer.
   - Open [https://esptool.spacehuhn.com/](https://esptool.spacehuhn.com/) in Chrome, Edge, or Opera.
   - Connect the ESP32 board via USB.
   - Click **Connect** and choose the serial port:
     - **Windows:** `USB Serial (COM5)`, `USB-SERIAL CH340 (COMx)`, or `Silicon Labs CP210x USB to UART Bridge (COMx)` 
     - **macOS:** `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART` 
     - **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0` 
     - *Tip: If unsure, click Connect first, then plug in the board — the new port is usually the right one.*
   - Delete any existing rows if needed, then click **Add** once.
   - Ensure the address is set to **`0x0`**.
   - Select **`firmware-ota.bin`** (the complete image for first-time flashing).
   - **⚠️ DO NOT SKIP THIS STEP:** Click **Erase** — this is mandatory to reinitialize the partition table.
   - Click **Program**.

4. **First Boot**
   - Power on the device. The LittleFS partition will be reformatted on first boot.
   - Connect to the **`CPAP-Setup`** WiFi network; you should be automatically redirected to the Setup page. If not, open `http://192.168.4.1/setup` in your browser.
   - ⚠️ **MANDATORY RE-CONFIGURATION**: You **must** re-enter your WiFi credentials and other secret configuration values (SleepHQ/SMB credentials, etc.) if they were previously encrypted. Because the partition change requires a full flash with erase, all secrets previously stored in secure memory (NVS) are lost.
   - Verify your configuration and click **Save & Restart**.

---

## Known Limitations

- **Smart Mode**: Only available on devices with PCNT peripheral support (typically AirSense 11). AirSense 10 devices will automatically fall back to Scheduled mode.
- **Partition Change**: This is a one-time migration. Future v4.x releases will support OTA upgrades from v4.0-beta1.

---

## Changelog (since v3.6i)

- **Partitioning**: Increased app partitions to 1.835 MB to ensure future development headroom; reduced SPIFFS to 384 KB.
- **Smart Mode**: Implemented "Quiet Period" and reduced retries to prevent interference with active therapy.
- **Validation**: Added server-side config validation and dashboard alerts for invalid Smart Mode settings.
- **UI/UX**: Redesigned the setup wizard timeline with intuitive labels, hour tick marks, and a color legend.
- **Safety**: Added form validation to ensure at least one upload endpoint is configured before saving.
- **Hardware**: Implemented auto-detection of PCNT capability with graceful fallback for older hardware.
- **SD Access**: Unified stealth SD access path for both AS10 and AS11; retired custom FAT32 boot reader in favour of `captureCardState()`/`restoreToSavedState()` for all mounts.
- **Reconnection**: VPN-friendly setup wizard reconnection — automatically falls back to IP-based polling after 30 s of failed hostname attempts (regular WiFi mode only).

