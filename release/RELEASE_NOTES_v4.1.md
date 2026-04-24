# CPAP AutoSync v4.1 — UI Clarity & SMB Performance

> **OTA upgrades from v4.0 are fully supported.** There are no partition-table changes in this release.

## What's New in v4.1

### 📊 Rewritten Upload Progress Card
We've completely overhauled the dashboard's "Upload Progress" card in response to user feedback. The previous design could be confusing—especially when both SleepHQ and a network share (NAS/SMB) were configured, lumping everything into a single "DUAL" status.

- **Clear, Separate Rows**: You'll now see dedicated progress rows for **SleepHQ Cloud** and **NAS (SMB)**, complete with independent progress bars and their own "Last upload" timestamps.
- **Accurate Progress Numbers**: The progress numbers now mean exactly what you expect:
  - **Right side**: The total number of valid DATALOG folders found on the SD card.
  - **Left side**: The number of folders fully synced to that specific destination.
- **Smooth Live Updates**: The progress bar now fills smoothly and monotonically during an active upload, eliminating previous visual jumping and oscillation.
- **Better Status Summaries**: "All synced" has been renamed to **"Up to date"**. If there's pending work, the summary explicitly names the destination (e.g., `⚠ 1 on NAS pending`).

### 🚀 Massive SMB Upload Speed Improvements
NAS users will notice significantly faster upload speeds in this release.
- **Persistent Connections**: The firmware now establishes a single connection to your NAS and reuses it for the entire upload session, rather than reconnecting for every individual folder.
- **Write Pipelining & Adaptive Buffering**: The device now pushes multiple chunks of data over the network simultaneously (pipelining) and dynamically sizes its buffers based on available memory.
- **Result**: Drastically reduced upload times for SMB network shares, especially for multi-day uploads or large data files.

### 📡 Remote Syslog (UDP)
Advanced users and network administrators can now stream all device logs to a remote syslog server (e.g., rsyslog, Graylog, Papertrail) in real time.
- **Real-Time Monitoring**: Useful for fleet monitoring, persistent off-device log collection, or configuring webhook triggers on your server.
- **Configuration**: Simply add `SYSLOG_HOST=<your_ip>` (and optionally `SYSLOG_PORT=514`) to your `config.txt`.
- **Zero Impact**: This feature uses a "fire-and-forget" UDP protocol with zero memory footprint, ensuring it never impacts device stability.

### ⏱️ Default Timing Adjustments
- The default **Cooldown Minutes** has been reduced from `10` to `5`. This reduces the wait time between upload cycles without significantly impacting the CPAP machine's access to the SD card.

---

## Important Bug Fixes
- **Smart-Mode Force Upload**: Clicking "Force Upload" during a smart-mode quiet period now correctly runs a recent-data-only session, rather than doing nothing.
- **SMB Timestamps**: When uploading to a NAS, the device will now preserve the original sleep-study file timestamps by default, ensuring your backup files display the correct historical dates. *(If you prefer upload-time timestamps, you can disable this by setting `SMB_PRESERVE_TIMESTAMPS=false` in your config.txt).*
- **SMB Last Upload Time**: The "Last upload" timestamp for SMB now correctly updates immediately after the SMB phase finishes.
- **Status Display**: Fixed an issue where the dashboard could incorrectly display "Waiting for first scan" even after successfully scanning the SD card if no new data was found. It will now correctly say "Up to date" or "No data on card".

---

## Upgrade Instructions

### Option 1 — OTA (Recommended from v4.0)

1. Open your device's dashboard at `http://cpap.local` (or its IP address).
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.1.bin` asset from the Releases page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails. Follow the Full Flash Instructions in the v4.0 release notes — the process is identical; just use the `firmware-ota-v4.1.bin` file at address `0x0`, and remember to click **Erase** before **Program**.

---

## Known Limitations

Unchanged from v4.0 — see `RELEASE_NOTES_v4.0.md` for details.

---

## Changelog Summary (since v4.0)
- **Web UI**: Rewrote the Upload Progress card into separate, backend-aware rows.
- **Performance**: Implemented SMB persistent sessions, write pipelining, and adaptive buffer sizing for vastly improved throughput.
- **Feature**: Added UDP syslog forwarding for advanced remote monitoring.
- **Config**: Reduced default `COOLDOWN_MINUTES` from 10 to 5.
- **Fix**: Progress bar no longer oscillates or jumps during active uploads; numbers correctly reflect "synced / total".
- **Fix**: Force Upload now behaves correctly during the Smart Mode quiet period.
- **Fix**: Dashboard status now correctly displays "Up to date" after a scan finds no new data.
- **API**: Cleaned up `/api/status`, removing obsolete legacy fields and structuring backend status hierarchically.
- **Internals**: Demoted non-critical system logs to `DEBUG` level to reduce log spam.
