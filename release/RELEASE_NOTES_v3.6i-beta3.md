# CPAP AutoSync v3.6i-beta3 — Setup Hardening & mDNS Robustness

This release focuses on hardening the Setup Wizard (`/setup`) experience, improving reconnection reliability across various browsers/networks, and refining the "Stealth Mode" SD card handling logic.

---

## 🛡️ Setup & Reconnection Hardening

### Unified Reconnection Assistant (Option A "Heartbeat")
- **Problem**: In previous versions, the "Saving & Rebooting" progress modal would often fail or hang when accessing the device via IP address due to browser security (CORS) or the device's mDNS-to-IP redirect.
- **Solution**: Implemented a "Heartbeat" detection strategy. The browser now treats any successfully received traffic (even "opaque" responses) as definitive proof that the device is back online. 
- **Impact**: Reconnection is now near-instantaneous and succeeds even in restrictive browsers or networks where mDNS resolution is slow.
- **Increased Buffer**: Poll timeout increased from 4s to **8s** to give sluggish OS resolvers and mesh networks more time to discover the device.

### Real-time Reconnection Debugging
- **Added**: A verbose debug console directly in the "Saving & Rebooting" modal. 
- **Benefit**: If a device fails to reconnect, you can now see exactly what the browser is doing—what URL it's polling, what errors it's getting, and the current connection mode—making remote troubleshooting much easier.

### Case-Insensitive Configuration
- **Fixed**: Synchronized the Web UI's configuration parser with the firmware. The UI now treats keys as **case-insensitive** (e.g., `hostname=`, `HOSTNAME=`, and `HostName=` are all correctly identified).
- **Added**: Official support for the `HOSTNAME` key in the Setup Wizard to ensure your custom device name is preserved correctly.

---

## 🎨 UI & Layout Refinements

### Layout Stability
- **Fixed**: The "jumping" UI issue where the page width would shift slightly on laptops/desktops when modals opened. Added a forced scrollbar gutter and synchronized container widths to the main dashboard standard.
- **Gold Standard Modals**: The Setup Wizard now uses the premium "glow" aesthetics and blue borders found in the Profiler Wizard, providing a unified look across the entire application.

### Configuration Management
- **Centralized Editing**: Removed the redundant "Raw Config" editor from the main dashboard. Users are now redirected to the more robust "Advanced" section of the Setup Wizard for raw configuration edits.
- **Data Integrity**: Fixed a bug where multiple trailing newlines would accumulate in `config.txt` after repeated saves.

---

## 🔒 Stealth Mode & Performance (Contributor: m-kozlowski)

- **Optimized State Probing**: `captureCardState()` now intelligently starts probing from the last known bus width. This significantly reduces the time spent in the "probing" phase during each upload cycle.
- **Backend Clean-up**: Removed redundant credential masking logic from the web server, delegating all security handling to the core `Config` class to prevent string injection bugs.

---

## 📋 Full Changelog

### v3.6i-beta3 (Cumulative since beta2)
- **Added**: Robust "Heartbeat" reconnection logic in `/setup` to bypass CORS/Redirect blocks.
- **Added**: Detailed debug console to the Saving & Rebooting modal window.
- **Added**: Cross-origin support for mDNS-to-IP redirection during reboots.
- **Fixed**: Case-insensitive key lookup in Javascript UI (synced with C++ firmware).
- **Fixed**: Layout "jumps" on desktop by forcing overflow-y and syncing .wrap constraints.
- **Fixed**: Trailing newline accumulation in `config.txt` saves.
- **Improved**: `captureCardState` performance by remembering last-known bus width (m-kozlowski).
- **Improved**: Universal modal styling with "Gold Standard" blue glow and borders.
- **Refactored**: Removed duplicate password masking logic from `CpapWebServer.cpp`.
- **Refactored**: Retired dashboard-level raw config editor; redirected to Setup Wizard.

### v3.6i-beta2
- **Fixed**: Monitoring state restoration on page refresh/navigation (GitHub Issue #26).
- **Fixed**: Time display formatting for durations over 59 minutes.
- **Fixed**: Start/Stop button visibility in SD Activity Monitor tab.
- **Added**: Stealth mode card state capture/restore: Added `captureCardState()` and `restoreToSavedState()` to preserve exact SD card state.
- **Added**: Empirical 4-bit read test to detect card's configured bus width during state capture.

### v3.6i-beta1
- **Added**: AP Setup Wizard with captive portal support.
- **Added**: WiFi scanning with mesh node deduplication.
- **Added**: Timezone IANA name persistence (`TZ_NAME`).
- **Added**: Human-readable UTC offset display on dashboard.
- **Improved**: WiFi connection robustness with dual-attempt logic.
- **Added**: Password visibility toggles on all password fields.

---

## 📂 Upgrade Instructions

### For Users on v3.6i-beta2:
1. Upload `firmware-ota-upgrade.bin` via the standard web interface.
2. It is recommended to visit `/setup` once and verify your **HOSTNAME** if you have customized it.

### For Users on v3.5i or older:
1. Upload `firmware-ota-upgrade.bin`.
2. Visit `http://cpap.local/setup`, verify your Timezone (city name), and click **Save & Restart**.
