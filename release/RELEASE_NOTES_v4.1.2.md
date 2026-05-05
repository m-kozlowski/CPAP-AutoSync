# CPAP AutoSync v4.1.2 — Retry Detection Fix

> **OTA upgrades from v4.0, v4.1, or v4.1.1 are fully supported.** There are no partition-table or config.txt changes in this release.

## What's Fixed in v4.1.2

### 🔄 Retry Detection Now Works When a Backend Cannot Connect

**This is a targeted bug fix release that corrects an edge case in the v4.1.1 upload retry fix.**

v4.1.1 correctly stopped suppressing retries when a backend still had incomplete folders after a session. However, the retry decision relied on `totalFoldersCount`, which is only populated during the upload phase's folder scan.

That created a failure mode when a backend never reached its folder scan at all — for example, when the NAS was temporarily unreachable and the SMB connection failed before Phase 2 could enumerate folders. In that case, `totalFoldersCount` could remain unset, causing the device to incorrectly believe there were no incomplete folders and suppress further retry attempts.

**The fix:** incomplete-work detection now uses the work probe snapshot (`probeUniverse` vs `probeSynced`) instead of relying only on `totalFoldersCount`. The work probe runs before upload phases and after eligible sessions regardless of whether a backend can connect, so it accurately reflects whether each backend still has unsynced folders.

This ensures the device correctly retries uploads when a destination is temporarily unavailable.

---

## Upgrade Instructions

### Option 1 — OTA (Recommended)

1. Open your device's dashboard at `http://cpap.local` (or its IP address).
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.1.2.bin` asset from the [Releases](https://github.com/ilyakruchinin/CPAP-AutoSync/releases) page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full Flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails.

1. Download `firmware-ota-v4.1.2.bin` from the [Releases](https://github.com/ilyakruchinin/CPAP-AutoSync/releases) page.
2. Open the [ESP Web Flasher](https://esp.huhn.me/) in Chrome/Edge.
3. Connect your ESP32 via USB and select its serial port.
4. Click **Erase** to clear the flash (this resets all settings).
5. Set the flash address to `0x0` and select the downloaded `.bin` file.
6. Click **Program** and wait for the flash to complete.
7. The device will reboot into setup mode — follow the on-screen instructions to configure WiFi and upload settings.

---

## Known Limitations

Unchanged from v4.0 — see `RELEASE_NOTES_v4.0.md` for details.

---

## Changelog Summary (since v4.1.1)

- **Fix**: Incomplete-folder detection now uses the work probe snapshot (`probeUniverse` vs `probeSynced`) so retry suppression works correctly even when a backend fails to connect before its upload-phase folder scan can run.
