# CPAP AutoSync v3.1.0i-beta1 — Changelog

> **Note:** v3.1.0i-beta1 marks a significant architectural milestone for the firmware, introducing a unified stealth boot process, intelligent hardware detection, and a much more reliable NTP synchronization engine.

## 🚀 What's New in v3.1.0i-beta1

### Unified "Stealth" Boot Architecture (AS10 & AS11)
- **Universal Stealth Booting:** The ESP32 now boots fully in "stealth" mode by default. We implemented a unified `StealthConfigReader` that reads `config.txt` utilizing safe, non-intrusive protocols. While it still takes exclusive ownership of the SD Card via the MUX switch at boot, it does so in a way that the CPAP machine is completely unable to detect.
- **Legacy Workarounds Removed:** Removed old, unreliable workarounds such as the cached-config logic, delayed boot loops, and `CMD0`-on-release toggles. The boot phase is now incredibly fast and safe. Previously on AirSense 10 machines, reading `config.txt` via normal SD initialization resulted in an SD card power-cycle reboot loop because the AS10 reacts aggressively to unexpected card state changes. (Conversely, the AirSense 11 simply re-initializes the card). The new stealth mode safely bypasses this detection, completely fixing the reboot loop vulnerability on the AS10.

### Intelligent Hardware Detection (PCNT)
- **Automatic Smart Mode Gating:** The firmware now dynamically uses the ESP32's hardware pulse counter (PCNT) to detect whether the physical hardware setup is capable of monitoring SD card activity (like on the AirSense 11 in 4-bit mode). 
- **AS10 Compatibility:** If the firmware detects that PCNT physical lines are not active (e.g. 1-bit AirSense 10 mode), it intelligently gates "smart" upload mode and defaults to "scheduled" mode. This state is durably saved to NVS flash, preventing the ESP32 from waiting for activity triggers that will never physically arrive.

### Custom NTP & DHCP Option 42 Support
- **Custom NTP Servers:** You can now rigorously define a custom `NTP_SERVER` property within your `config.txt` to point to local time servers (e.g. your router or a Pi-hole).
- **Zero-Config Router Fallback:** If you leave `NTP_SERVER` blank, the firmware now intelligently negotiates with your router's DHCP service (DHCP Option 42) to automatically fetch preferred local NTP servers, ultimately falling back to `pool.ntp.org` if none are found.
- **DHCP Client Hostname Setup:** Integrated the user's `config.txt` `HOSTNAME` property securely into the DHCP lease requests, significantly tidying your router's device tables (instead of displaying a generic `espressif` or bare MAC address identifier).
- **Fail-Safe & Memory Efficient:** We eliminated the previous, blocking time-sync daemon. The new architecture pushes all time synchronization purely to the background native `lwIP` layer with zero heap-memory overhead. If the internet drops, the firmware will gracefully continue running without deadlocking the upload state machine.

### UI/UX Improvements
- **Multi-Device Hostname Web UI:** For users with multiple CPAP uploaders, the Web UI has been updated to dynamically inject your configured `HOSTNAME` directly into the browser tab title and the dashboard subtitle. You can now easily identify which CPAP machine's dashboard you are viewing at a glance.

---

## ⚠️ Upgrade Notice
**Upgrading from v3.0i-stable?** OTA update is fully supported. Use the OTA tab in the web interface and upload `firmware-ota-upgrade.bin`.
