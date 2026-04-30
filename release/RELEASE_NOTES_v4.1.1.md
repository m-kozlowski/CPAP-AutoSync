# CPAP AutoSync v4.1.1 — Upload Retry Fix

> **OTA upgrades from v4.0 or v4.1 are fully supported.** There are no partition-table or config.txt changes in this release.

## What's Fixed in v4.1.1

### 🔄 Uploads Now Retry When Backends Have Incomplete Folders

**This is a targeted bug fix release with a single, high-impact correction.**

Previously, when an upload session ended — whether all files were processed or the session ran out of time — the device would unconditionally **suppress further upload attempts** and wait for new CPAP SD card bus activity (PCNT pulses) before trying again. This was correct when everything was fully synced, but **caused uploads to stall indefinitely** when a backend still had pending work.

**The most common symptom:** Your NAS (SMB) destination shows "2 left" on the dashboard, but the device never retries — even though it's within the upload window and has nothing else to do. The device sits in LISTENING state for hours, waiting for CPAP therapy activity that may never come.

**Common triggers:**
- SMB/NAS server temporarily unreachable (rebooting, network glitch)
- Upload session ran out of its time budget before finishing all folders
- One backend (e.g., SleepHQ Cloud) completed successfully while the other (e.g., NAS) failed

**The fix:** The device now checks whether any backend still has incomplete folders after each upload session. If work remains, the device will **continue retrying after the cooldown period** instead of waiting for new PCNT activity. If all backends are fully synced, suppression still activates as before — preventing unnecessary SD card scans when there's nothing to do.

This fix applies to **both Smart and Scheduled upload modes**.

---

## Upgrade Instructions

### Option 1 — OTA (Recommended)

1. Open your device's dashboard at `http://cpap.local` (or its IP address).
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.1.1.bin` asset from the [Releases](https://github.com/ilyakruchinin/CPAP-AutoSync/releases) page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full Flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails.

1. Download `firmware-ota-v4.1.1.bin` from the [Releases](https://github.com/ilyakruchinin/CPAP-AutoSync/releases) page.
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

## Changelog Summary (since v4.1)

- **Fix**: Upload retry suppression now checks whether backends still have incomplete folders before activating. Previously, suppression was unconditional after any `COMPLETE` or `NOTHING_TO_DO` result, which could stall uploads indefinitely when a backend was temporarily unreachable or the session timed out with work remaining.
