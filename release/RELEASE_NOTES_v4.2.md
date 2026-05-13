# CPAP AutoSync v4.2 — Firmware Image Minimization

> **OTA upgrades from v4.0, v4.1, v4.1.1, v4.1.2, or v4.1.3 are fully supported.** There are no partition-table or config.txt changes in this release.

## What's Changed in v4.2

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

- **Size**: Runtime web UI is now generated, minified, gzip-compressed, and served with `Content-Encoding: gzip`.
- **Size**: Applied no-BLE firmware image reductions, improving free OTA space from ~226 KB to ~313 KB.
- **Size**: Reduced final app usage from 1,603,440 bytes to 1,514,415 bytes, saving 89,025 bytes.
- **Build**: Disabled unused WiFi Enterprise, VFS termios/select, error-name lookup, full assertion strings, and C++ exceptions.
- **Build**: Reduced Arduino core debug verbosity to `CORE_DEBUG_LEVEL=1`.
- **SMB**: Replaced `%llu` 64-bit offset logging with Nano-safe hexadecimal high/low formatting.
- **Dashboard**: Preserved and hardened WiFi SSID display by escaping SSID values in status JSON.
- **Credit**: Includes web UI gzip/minification work from **m-kozlowski** in PR #100 and dashboard WiFi SSID display work from **m-kozlowski** in PR #101.
