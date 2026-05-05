# CPAP AutoSync v4.1.3 — Status Timestamp and Hostname Fixes

> **OTA upgrades from v4.0, v4.1, v4.1.1, or v4.1.2 are fully supported.** There are no partition-table or config.txt changes in this release.

## What's Fixed in v4.1.3

### 📊 Per-Backend "Last upload" Now Reflects Actual No-Work Completion

The dashboard's Upload Progress card shows separate `Last upload` values for SleepHQ Cloud and NAS (SMB). In some successful sessions, those timestamps could remain stale even though the post-session work probe showed all configured backends were fully synced.

**The symptom:** the dashboard could show both backends as `9 / 9 ✓` and `Up to date`, but still report `Last upload: 5 days ago` or similar after an upload completed today.

**The cause:** backend timestamps were stamped only in the older session-completion path and were not always persisted after being updated. The newer, authoritative work probe could prove that no work remained (`cloud=0 smb=0`, with `cloudSynced == universe` and `smbSynced == universe`), but that proof was not used to update each backend's saved `last_ts`.

**The fix:** after the post-session work probe runs, each backend now updates and saves its own `Last upload` timestamp independently when that backend has no remaining work:

- SleepHQ Cloud updates when Cloud has no work left and `cloudSynced == universe`.
- NAS (SMB) updates when SMB has no work left and `smbSynced == universe`.
- The session/day is marked complete only when all configured backends have no remaining work.

This means Cloud and SMB timestamps now correctly reflect the last time each backend individually reached the fully-synced state.

### 🌐 DHCP Hostname Is Applied Before Station Mode Starts

Fixed a WiFi hostname timing issue contributed by m-kozlowski in PR #94.

Previously, the firmware called `WiFi.mode(WIFI_STA)` before applying the configured hostname. In the Arduino WiFi layer, the hostname is applied during `WiFi.mode(WIFI_STA)` using the cached value from `NetworkManager::setHostname()`.

Although `setHostname()` sets or resets the hostname, `WiFi.mode(WIFI_STA)` is asynchronous. That means DHCP DISCOVER could already have been sent using the cached default hostname before the configured hostname was applied.

**The fix:** the configured hostname is now set before `WiFi.mode(WIFI_STA)`, so DHCP DISCOVER uses the configured hostname instead of a cached default.

---

## Upgrade Instructions

### Option 1 — OTA (Recommended)

1. Open your device's dashboard at `http://cpap.local` (or its IP address).
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.1.3.bin` asset from the [Releases](https://github.com/ilyakruchinin/CPAP-AutoSync/releases) page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full Flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails.

1. Download `firmware-ota-v4.1.3.bin` from the [Releases](https://github.com/ilyakruchinin/CPAP-AutoSync/releases) page.
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

## Changelog Summary (since v4.1.2)

- **Fix**: Per-backend `Last upload` timestamps now update and persist after the post-session work probe proves that backend has no work left. Cloud and SMB are stamped independently.
- **Fix**: Set configured hostname before `WiFi.mode(WIFI_STA)` so DHCP DISCOVER uses the configured hostname instead of the cached default.
