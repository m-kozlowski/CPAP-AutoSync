# CPAP AutoSync v3.6i-beta1 — Master Changelog (since v3.5i)

This release introduces the **AP Setup Wizard**, a major upgrade to the device's first-time configuration and recovery experience, alongside significant improvements to timezone handling and connection robustness.

---

## 🚀 Major Feature: AP Setup Wizard & Captive Portal

The uploader now features a built-in **Access Point (AP) Setup Mode**. If the device cannot find a valid configuration or fails to connect to WiFi on boot, it will create its own WiFi network: `CPAP-AutoSync`.

- **Automatic Trigger**: Starts automatically on a fresh power-up if no WiFi is configured.
- **Captive Portal Support**: When you connect your phone or laptop to the `CPAP-AutoSync` network, a configuration page will automatically pop up (supported on iOS, Android, and Windows).
- **Dark Mode UI**: The setup page has been completely overhauled to match the main dashboard's aesthetic, featuring animated branding and a mobile-friendly layout.
- **Manual Access**: Accessible at `http://192.168.4.1/setup` in AP mode, or `http://cpap.local/setup` in normal operation.

---

## 📶 WiFi & Connection Robustness

- **Dual-Attempt Connection**: The device now makes two attempts to connect to your WiFi (with a 3-second delay) before giving up. This provides better stability for mesh networks or slow router handshakes.
- **SSID Scanning & Deduplication**: The Setup Wizard features a "Scan" button that displays nearby networks. Duplicate SSIDs (from mesh nodes) are automatically filtered server-side, showing only the strongest signal.
*   **Cold-Boot Guard**: AP mode is now strictly restricted to **power-on** events. Soft reboots (from the web UI) or firmware crashes will no longer trigger an unsolicited AP, preventing "network hijacking" if your credentials are wrong.
- **Credential Warning**: If you save incorrect WiFi details, the device now provides a clear warning: *"⚠️ If the device cannot connect, power-cycle it to re-enter setup mode."*

---

## 🌍 Timezone & Localization Improvements

*   **Robust IANA Persistence**: Fixed a bug where the timezone would "flip" to a similar city (e.g., Melbourne → Sydney). The uploader now saves the exact IANA name (`TZ_NAME`) to `config.txt`.
*   **Human-Readable Dashboard**: The dashboard **Timezone** field now displays a friendly UTC offset (e.g., `UTC+10:00 (DST active)`) instead of cryptic POSIX strings.
*   **Server-Side DST Calculation**: The UTC offset is computed live on the device based on your current POSIX rule, ensuring the dashboard always shows the correct local time status.
*   **Improved TZ Database**: Migrated to a native JSON-based timezone database (`zones.json`), improving load speed and reliability of the setup dropdown.

---

## 🛠️ UI & Configuration UX

- **Password Visibility**: Added 👁️ toggle buttons to all password fields (`WiFi`, `SMB`, `SleepHQ`) to make entry easier and more accurate.
- **SleepHQ Help**: Repositioned API credential instructions above the form with a clear ℹ️ info banner.
- **Atomic Config Saving**: Improvements to the config writing process on the SD card to ensure data integrity.
- **Live Preview**: The Setup Wizard shows a real-time preview of the `config.txt` file as you fill in the form.

---

## 🆕 New Configuration Keys

| Key | Default | Description |
|---|---|---|
| `TZ_NAME` | *(empty)* | Stores the IANA city name (e.g. `Europe/London`). Used by the web UI to maintain consistent dropdown selection. |

---

## 📂 Upgrade Instructions

### For Users on v3.5i or older:

1. **OTA Update**: Upload `firmware-ota-upgrade.bin` via the standard web interface.
2. **First Boot**: The device will continue using your existing `config.txt`. 
3. **Finish Setup**: It is recommended to visit `http://cpap.local/setup`, verify your Timezone (which will now show the city name), and click **Save & Restart** to generate the new `TZ_NAME` entry.

### For Fresh Installations:

1. Burn the firmware via USB.
2. Power on the device.
3. Connect to the `CPAP-AutoSync` WiFi network on your phone.
4. Follow the setup wizard to configure your WiFi, Timezone, and SleepHQ/SMB credentials.
