# CPAP AutoSync v3.6i — Stable Release

> **Note:** v3.6i is a major stable release that transforms the device's configuration experience while introducing critical reliability improvements for both AirSense 10 and 11 hardware. This version consolidates all improvements from the v3.6i-beta cycle.

## What's New in v3.6i

### ✨ Visual Setup Wizard & Captive Portal
The highlight of this release is a complete overhaul of the first-time setup process.
- **Zero-SD-Reader Setup**: You no longer need to edit `config.txt` on your computer. Simply flash the firmware, insert the card, and connect to the **`CPAP-Setup`** WiFi network on your phone.
- **Captive Portal**: Most devices will automatically open the configuration page when you connect.
- **Scanner & Mesh Support**: The wizard includes a WiFi scanner that deduplicates mesh networks and shows signal strength to help you pick the best node.
- **User Benefit**: Dramatically simplifies setup for non-technical users and eliminates "invalid config" errors caused by typos in text files.

### 🛡️ Connectivity & Reconnection Hardening
We've hardened the way the device handles network transitions and browser security.
- **"Heartbeat" Reconnection**: Implemented a new polling strategy that succeeds even when browsers block traffic due to CORS or redirects.
- **Verbose Debug Console**: If a reconnection fails, the "Saving & Rebooting" modal now features a live debug console showing exactly why.
- **[Improved] 10s Reconnection Window**: Increased the timeout and added yellow "Warning" labels for transient polling failures, reducing user anxiety during reboots.
- **Case-Insensitive Keys**: The UI now matches the firmware's flexibility, treating `HOSTNAME` and `hostname` as the same key.

### 🔒 Advanced Stealth Mode (Universal)
Thanks to contributions from **m-kozlowski**, the "Stealth Mode" which protects the CPAP machine's SD access has been greatly enhanced.
- **Universal State Capture**: The device now probes and captures the exact RCA and bus width (1-bit or 4-bit) before starting an upload.
- **Precise Restoration**: After an upload, the card is restored to the *exact* state the CPAP left it in, not just a generic "Standard" state.
- **User Benefit**: This significantly reduces "SD Card Error" messages on AirSense 11 and prevents reboot loops on AirSense 10 more effectively than ever before.

### 🍱 UI & Privacy Refinements
- **SD Activity Tab**: Renamed from "SD Access" to better reflect its monitoring purpose.
- **Secure by Default**: `MASK_CREDENTIALS` now defaults to `true`. Your passwords are automatically moved to the device's secure flash memory (NVS) on first setup and replaced with `***STORED_IN_FLASH***` in your config file.
- **Enhanced Formatting**: Time durations now display in `H M S` format (e.g., `2h 15m 30s`) for easier reading of long monitoring sessions.
- **Password Toggles**: Added 👁️ icons to all sensitive fields to prevent typos during setup.

---

## Configuration Reference

### New & Updated Keys

| Key | Default | Description |
|---|---|---|
| `TZ_NAME` | *(empty)* | Stores the IANA city name (e.g. `Europe/London`) for consistent timezone tracking. |
| `MASK_CREDENTIALS` | `true` | **New Default.** Migrates passwords to secure flash. Set to `false` for raw plaintext storage. |

---

## Upgrade Instructions

### Existing Users (v3.1.x / v3.5.x)
1. **OTA Update**: Upload `firmware-ota-upgrade.bin` via your current web interface.
2. **Setup**: It is highly recommended to visit `http://cpap.local/setup` once after upgrading to verify your **Timezone** and **Hostname**, then click **Save & Restart**.

### Fresh Installations
1. Burn `firmware-ota.bin` via USB using the web flasher.
2. Power on the device in your CPAP.
3. Connect to **`CPAP-Setup`** on your phone and follow the wizard.
