# CPAP AutoSync v3.1.0i-beta2 — Changelog

> **Note:** v3.1.0i-beta2 is a maintenance release that focuses on code quality improvements and bug fixes. The stealth mode architecture and PCNT detection features from v3.1.0i-beta1 remain fully functional and stable.

## 🚀 What's New in v3.1.0i-beta2

### 🔧 Improvements
- **Unified NTP Implementation** - Consolidated synchronization logic using `configTzTime()` while maintaining full feature compatibility
- **Code Quality** - Removed duplicate parsing logic and streamlined callback methods for better maintainability  
- **Clean Architecture** - Resolved merge conflicts between stealth mode and NTP features

### Bug Fixes
- **Fixed Duplicate NTP_SERVER Parsing:** Resolved an issue where the `NTP_SERVER` configuration key was being parsed twice in the Config class, with the second entry never being reached.
- **Fixed ntpServer Initialization:** Corrected the constructor initialization order to properly initialize the `ntpServer` field with its default value of "pool.ntp.org".
- **Removed Unused Getter Declarations:** Cleaned up header files by removing duplicate `getNtpServer()` declarations that were no longer needed after code refactoring.

### 📋 Upgrade Notes
- **v2.x or v3.X → v3.1.0i-beta2:** OTA update supported

---

**Full Release Notes:** See `release/RELEASE_NOTES_v3.1.0i-beta2.md` for detailed installation instructions and upgrade notices.

---

## 🔄 How to Flash (Web Flasher)

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
   - **⚠️ DO NOT SKIP THIS STEP:** Click **Erase** — this is mandatory for clean state
   - Click **Program**

**For users upgrading from any v3.1.0i-beta1:**

1. Open the web interface at `http://cpap.local` (or `http://<device-ip>/`)
2. Go to the **OTA** tab
3. Upload `firmware-ota-upgrade.bin`
4. Wait for the device to restart (~30 seconds)

**🚨 CRITICAL: After a full USB flash (not OTA), BEFORE inserting the ESP32 into your CPAP machine:**

You **must** update `config.txt` on your SD card with your actual WiFi credentials. The **Erase** step wipes all previously stored passwords from the ESP32's flash memory (including any credentials that were migrated to NVS in previous versions). If you skip this, the device will fail to connect with "SSID is empty" or authentication errors.

**Important:** Use `firmware-ota.bin` (not `firmware-ota-upgrade.bin`) when doing a full reflash. The web flasher performs a complete erase automatically.
