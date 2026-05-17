# CPAP AutoSync v4.2 — Multi-SSID WiFi & Firmware Image Minimization

> **OTA upgrades from v4.0, v4.1, v4.1.1, v4.1.2, or v4.1.3 are fully supported.** There are no partition-table or config.txt changes in this release.

## What's Changed in v4.2

### 📶 Multi-SSID WiFi Support

The device now supports up to **4 configured WiFi networks** and will automatically select the best one.

Credit to **m-kozlowski** for the multi-SSID implementation in **PR #98** and the follow-up cleanup in **PR #101**.

- **Up to 4 networks**: Configure `WIFI_SSID_1` through `WIFI_SSID_4` (with matching `WIFI_PASSWORD_1` through `WIFI_PASSWORD_4`) in your `config.txt`. The legacy `WIFI_SSID` / `WIFI_PASSWORD` keys continue to work for single-network setups.
- **Automatic RSSI-ranked selection**: On connect, the device scans for all configured networks, ranks visible APs by signal strength, and connects to the strongest candidate.
- **Per-candidate retries with fallback**: Each candidate gets up to 2 retries before moving to the next. If all visible APs fail, the device falls back to blind-try mode (useful for hidden SSIDs).
- **Roaming with hysteresis**: When 2+ networks are configured, the device periodically samples RSSI and switches to a stronger AP if it exceeds the current connection by a configurable margin. Roaming is automatically suspended during upload sessions to avoid mid-transfer disruption.
- **Exponential backoff**: Failed reconnect attempts use increasing intervals (30s → 60s → 2m → 5m cap) to avoid hammering the radio.
- **NetworkHints persistence**: BSSID, channel, and PMF flags are cached in NVS for faster reconnects. Hints have a configurable TTL.
- **AP fallback with STA retry**: If the initial boot WiFi connection fails, the device starts a setup AP while continuing to retry STA in the background. Once STA recovers stably for 2 minutes with no AP clients, the device reboots into normal STA-only mode.
- **Setup wizard updated**: The web-based setup wizard now supports configuring up to 4 WiFi networks.
- **Bus-aware reconnect deferral**: On PCNT-capable devices, WiFi reconnect attempts are deferred while the CPAP machine is actively using the SD bus, preventing RF bursts from interfering with ongoing SD I/O.

### 📡 WiFi Stability Fix

Credit to **m-kozlowski** for the WiFi stability fix in **PR #104**.

- **Disabled supplicant auto-reconnect**: The ESP32's built-in WiFi supplicant was autonomously retrying connections after disconnects, competing with the firmware's managed reconnection loop. This produced scan failures, ghost connections the FSM couldn't track, and log storms of repeated disconnect events. The supplicant auto-reconnect is now disabled so the main loop is the sole reconnection owner.
- **Ghost-connect reconciliation**: If the radio reports connected while the managed state machine is idle or failed, the connection is now adopted instead of ignored.
- **Reason-36 mapping**: `WIFI_REASON_STA_LEAVING` (reason code 36) is now logged with a human-readable label instead of `Unknown (36)`.

### 📦 Smaller Web UI Payloads

The runtime web UI is now stored as generated gzip-compressed payloads instead of a large raw `PROGMEM` HTML string.

Credit to **m-kozlowski** for the original web UI gzip/minification work in **PR #100**.

This change:

- Moves dashboard HTML into source HTML files.
- Generates compressed headers at build time.
- Serves the dashboard with `Content-Encoding: gzip`.
- Adds optional `minify-html` support for additional compression.

This was the largest initial image-size reduction and established the new post-web-gzip baseline.

### 🧹 Firmware Size Optimizations

The firmware was further minimized without changing the partition table or removing user-facing features.

Measured post-web-gzip baseline:

```text
App used:      1,603,440 bytes
Free OTA:        231,568 bytes (~226 KB)
```

Final v4.2 minimized build:

```text
App used:      1,514,415 bytes
Free OTA:        320,593 bytes (~313 KB)
Net saving:       89,025 bytes (~87 KB)
```

The final build now uses about **82.5%** of the OTA application slot.

### ⚙️ Build Configuration Reductions

The following no-BLE size reductions were applied:

- Disabled WiFi Enterprise support.
- Disabled unused VFS `termios` support.
- Disabled unused VFS `select()` support.
- Disabled ESP error-name lookup tables.
- Switched assertions to silent mode.
- Disabled C++ exceptions.
- Reduced Arduino core debug verbosity from `CORE_DEBUG_LEVEL=3` to `CORE_DEBUG_LEVEL=1`.

IPv6 was evaluated but intentionally left enabled because the SMB library currently requires IPv6 socket types at compile time.

### 📝 SMB Logging Cleanup

SMB pipelined-write offset logging no longer uses `%llu` formatting for 64-bit offsets. The offsets are now logged as two 32-bit hexadecimal halves.

This avoids a known compatibility issue with smaller `printf` implementations and keeps the SMB logs safe for future size experiments.

### 🧪 Newlib Nano Evaluated but Not Kept

`CONFIG_NEWLIB_NANO_FORMAT=y` was tested after the SMB logging cleanup. In this PlatformIO/Arduino build it produced no measurable application-size reduction, so it was left disabled.

### 🌐 Dashboard WiFi SSID Escaping

The dashboard status JSON now escapes the WiFi SSID before returning it to the browser. This prevents malformed JSON if the SSID contains quotes, backslashes, or other special characters.

Credit to **m-kozlowski** for the dashboard WiFi SSID display work in **PR #101**; this release keeps that improvement and hardens the server-side JSON output.

---

## Upgrade Instructions

### Option 1 — OTA (Recommended)

1. Open your device's dashboard at `http://cpap.local` or its IP address.
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.2.bin` asset from the Releases page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full Flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails.

1. Download `firmware-ota-v4.2.bin` from the Releases page.
2. Open the ESP Web Flasher in Chrome/Edge.
3. Connect your ESP32 via USB and select its serial port.
4. Click **Erase** to clear the flash. This resets all settings.
5. Set the flash address to `0x0` and select the downloaded `.bin` file.
6. Click **Program** and wait for the flash to complete.
7. The device will reboot into setup mode. Follow the on-screen instructions to configure WiFi and upload settings.

---

## Known Limitations

Unchanged from v4.1.3.

---

## Changelog Summary (since v4.1.3)

- **WiFi**: Multi-SSID support with up to 4 configured networks, RSSI-ranked selection, per-candidate retries, and hidden-SSID blind fallback.
- **WiFi**: Roaming with RSSI hysteresis between configured networks, suspended during uploads.
- **WiFi**: Exponential backoff for reconnect attempts (30s → 60s → 2m → 5m cap).
- **WiFi**: NetworkHints persistence (BSSID, channel, PMF flags) in NVS for faster reconnects.
- **WiFi**: AP fallback mode with background STA retry and stable-quiet teardown after recovery.
- **WiFi**: Bus-aware reconnect deferral on PCNT-capable devices during active CPAP SD I/O.
- **WiFi**: Setup wizard updated to support up to 4 WiFi networks.
- **WiFi**: Disabled ESP32 supplicant auto-reconnect to eliminate ghost-connect races and log storms.
- **WiFi**: Added ghost-connect reconciliation for radio connections outside the managed path.
- **Size**: Runtime web UI is now generated, minified, gzip-compressed, and served with `Content-Encoding: gzip`.
- **Size**: Applied no-BLE firmware image reductions, improving free OTA space from ~226 KB to ~313 KB.
- **Size**: Reduced final app usage from 1,603,440 bytes to 1,514,415 bytes, saving 89,025 bytes.
- **Build**: Disabled unused WiFi Enterprise, VFS termios/select, error-name lookup, full assertion strings, and C++ exceptions.
- **Build**: Reduced Arduino core debug verbosity to `CORE_DEBUG_LEVEL=1`.
- **SMB**: Replaced `%llu` 64-bit offset logging with Nano-safe hexadecimal high/low formatting.
- **Dashboard**: Preserved and hardened WiFi SSID display by escaping SSID values in status JSON.
- **Credit**: Multi-SSID WiFi from **m-kozlowski** in PR #98, WiFi stability fix from **m-kozlowski** in PR #104, web UI gzip/minification from **m-kozlowski** in PR #100, and dashboard WiFi SSID display from **m-kozlowski** in PR #101.
