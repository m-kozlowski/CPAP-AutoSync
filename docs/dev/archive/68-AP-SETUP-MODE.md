# Implementation Plan: Universal AP Setup Mode & Web Configurator

## 1. Executive Summary
To simplify initial setup and recovery, the firmware will introduce a robust **AP Setup Mode** alongside a **Universal Web GUI**. This ensures users never strictly *need* to edit `config.txt` manually, while fully preserving the ability to do so.

The core philosophy of this feature is **Client-Side Heavy, ESP-Side Light**. By shifting all configuration parsing, UI generation, and payload reconstruction to the user's browser via JavaScript, the ESP32's heap and flash impact remains near zero. The same Universal Web GUI will be used in both AP Mode (initial setup/recovery) and STA Mode (normal operation).

## 2. Trigger Conditions & Boot Flow
The device will automatically enter AP Mode under specific fallback conditions, ensuring the user is never permanently locked out.

**AP Trigger Conditions:**
1. **Missing `config.txt`:** If the SD card has no config file, or the card cannot be mounted during the initial boot phase.
2. **WiFi Connection Failure:** If the device attempts to connect to the configured WiFi network but fails (e.g., password changed, router replaced, or initial blank config).

**AP Mode Behavior:**
- Starts a WiFi Access Point named `CPAP-AutoSync` (open, or with a default password documented in the manual).
- Starts a **Captive Portal** DNS server. Any DNS request from a connected client resolves to the ESP32's IP address (usually `192.168.4.1`), prompting the OS (iOS, Android, Windows) to automatically open the setup page.
- Serves the Universal Web GUI.

## 3. Universal Web GUI Architecture
The configurator will be a single-page application (SPA) built with vanilla HTML/JS/CSS to ensure minimal footprint. 

### 3.1. Delivery Mechanism
To guarantee the setup page is available even if the SD card is corrupted or missing, the entire HTML/JS/CSS payload must be baked into the firmware.
- The web assets will be minified, gzip-compressed, and stored as a `PROGMEM` byte array (e.g., `web_configurator_html_gz`).
- Serving gzipped content requires zero heap overhead for decompression on the ESP32, as the browser handles decompression natively.

### 3.2. Client-Side Parsing & Payload Generation
The ESP32 only needs to provide two existing endpoints:
- `GET /api/config-raw`: Returns the raw `config.txt` text.
- `POST /api/config-raw`: Accepts raw text and saves it to `config.txt`.

**The JavaScript Flow:**
1. **Fetch:** JS requests `GET /api/config-raw`.
2. **Parse:** JS parses the text line-by-line, maintaining an array of objects representing lines (Type: Key-Value, Comment, or Blank).
3. **Generate UI:** JS uses a predefined "Configuration Schema" to build the UI dynamically.
   - Known keys (e.g., `WIFI_SSID`, `CLOUD_ENABLED`) are mapped to user-friendly inputs (text, passwords, checkboxes).
   - Unknown keys are safely placed into an "Advanced / Raw Entries" text area.
   - Existing comments in the file are tied to the line they precede, ensuring they aren't lost when the file is rewritten.
4. **Save:** When the user clicks "Save", the JS reconstructs the exact text payload—updating values for known keys, keeping comments intact, and preserving the order. It then sends this payload via `POST /api/config-raw`.

### 3.3. Dynamic UI & Schema
The UI will be organized into logical sections (e.g., WiFi, Schedule, Cloud, SMB, Advanced).
- **Conditional Visibility:** JS will dynamically show/hide relevant sections. For example, if `CLOUD_ENABLED` is unchecked, all Cloud-specific settings disappear, reducing visual clutter.
- **Password Masking:** Existing `******` masking logic is naturally supported; JS will only write the password back to the payload if the user modifies the input field.

## 4. Timezone Selector (External API Integration)
To provide a user-friendly timezone selector without bloating the ESP32 with a massive timezone database:
- The JS will directly fetch the authoritative POSIX database from GitHub:
  `https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv`
- **CORS Compatibility:** GitHub Raw sets `Access-Control-Allow-Origin: *`, so modern browsers will allow this cross-origin request seamlessly.
- **UI Experience:** The JS parses the CSV and generates a searchable dropdown (e.g., `<datalist>` or custom select). The user selects "America/New_York", and the JS writes the corresponding POSIX string (`EST5EDT,M3.2.0,M11.1.0`) to the `TZ_STRING` key.

## 5. Wi-Fi Scanning (Stretch Goal)
Implementing a "Select WiFi Network" feature is highly feasible and significantly improves UX.

**Implementation:**
1. **ESP-Side (`GET /api/wifi-scan`):**
   - Triggers `WiFi.scanNetworks(true, true)` (async, show hidden).
   - ESP streams the JSON results directly to the HTTP response to prevent heap fragmentation (avoiding large intermediate strings).
2. **Client-Side:**
   - JS polls or waits for the scan to complete.
   - Populates a dropdown next to the `WIFI_SSID` field, showing SSIDs and signal strength (RSSI).

## 6. Simultaneous AP + STA (Evaluation)
**Recommendation: DO NOT use simultaneous `WIFI_AP_STA` mode for setup verification.**

*Why?* The ESP32 has a single 2.4GHz radio. If the AP is broadcasting on Channel 1, but the target router the user wants to connect to is on Channel 6, the ESP must constantly channel-hop. This causes the Captive Portal AP to drop packets, disconnect clients, or become completely unresponsive.

**Proposed Alternative Flow:**
1. User enters credentials in the AP Captive Portal and clicks "Save & Restart".
2. JS posts the config and shows a "Rebooting and connecting to WiFi... Please check your router for the new IP" message.
3. ESP reboots into STA mode.
4. If the connection fails (wrong password, etc.), the trigger conditions in Section 2 automatically kick in, and the ESP falls back to AP Mode, allowing the user to try again.

## 7. Summary of Requirements & Constraints Check
- **Zero/Minimal Heap Impact:** Yes. ESP only serves a static PROGMEM gzip and streams raw text.
- **Client-Side JS Focus:** Yes. All UI logic, parsing, and serialization are browser-side.
- **Preserve Comments:** Yes. Line-by-line parsing array structure ensures comments are retained.
- **Universal Interface:** Yes. The exact same HTML payload can be served at `http://<device-ip>/setup` during normal operation and `http://192.168.4.1/` during AP mode.
