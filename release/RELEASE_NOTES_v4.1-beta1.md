# CPAP AutoSync v4.1-beta1 — UI Clarity Update

> **OTA upgrades from v4.0 are supported.** No partition-table change in this release.

## What's New in v4.1-beta1

### 📊 Rewritten Upload Progress Card
The dashboard's "Upload Progress" card has been completely reworked in response to user feedback (issues #58 and #61). When both SleepHQ and a network share (SMB) were configured, the card previously collapsed into a single ambiguous row labelled `DUAL` with one set of numbers that, confusingly, belonged to only one of the two backends. That is gone.

- **One row per backend**: Configured backends now each render their own labelled progress row — `SleepHQ Cloud` and `NAS (SMB)` — with their own counts, progress bar, and last-upload timestamp. Single-backend setups still show a single row, just with a real label instead of a raw token.
- **Honest per-backend numbers**: Each row's counts come directly from that backend's own state manager. No more blended or mismatched denominators.
- **Per-row live status**: The backend that is currently uploading shows inline per-file progress (e.g. `Uploading 24/48 · DATALOG/20260420`) while the other row stays in its steady "Last upload: 5 min ago" view.
- **Dropped**: the `DUAL` sentinel label, the unhelpful `(N empty)` counter in the denominator, and the obsolete `Next: CLOUD (stale)` row (which belonged to the old cycling orchestrator and has been meaningless since the move to phased single-session uploads).
- **Aggregate status line**: `"All synced"` is now `"Up to date"`. When work is pending it names the backend(s): e.g. `⚠ 1 on NAS pending`.

### 🔌 `/api/status` — additive changes
For anyone polling the status JSON:
- **Added**: a `backends` object with per-backend `{ enabled, done, total, pending, last_ts, live: { active, folder, up, total } }` for `cloud` and `smb`.
- **Retained (unchanged)**: `active_backend`, `folders_done`, `folders_total`, `folders_pending`, `live_active` / `live_folder` / `live_up` / `live_total`. The only behavioural change: `active_backend` no longer emits the string `"DUAL"` — it reports the currently-running phase (`CLOUD` or `SMB`) or, at rest, the primary backend.
- **Removed**: `next_backend`, `next_done`, `next_total`, `next_empty`, `next_ts`. These fields belonged to the old cycling orchestrator and were permanently zero/`"NONE"` in v4.0. Any external integration that was still reading them was already seeing dead data.

### 🧹 `MINIMIZE_REBOOTS` — reclassified as internal
The `MINIMIZE_REBOOTS` config key was originally meaningful back when each upload session ran exactly **one** backend and rebooting was the only way for the next session to pick the *other* backend. Under the current phased orchestrator, both backends upload inside one session (always `CLOUD → SMB`), and the flag now only controls whether the FSM performs an elective `esp_restart()` at the end. The heap safety valve already enforces a reboot at `max_alloc < 32 KB` regardless of the flag.

- The key and its default (`true`) are **unchanged** — your existing configs continue to work with no behaviour change.
- It has been **removed from the user configuration reference** and is no longer documented as a tuning parameter. Treat it as a developer-only diagnostic toggle.

### � Fixed: Force Upload now honours its name in Smart mode
Previously, pressing **Force Upload** during Smart mode's quiet period (before `SMART_START_HOUR`) silently no-op'd with `No data category eligible, releasing`. The dashboard already advertised this as "forces an upload of recent data now" and the Danger Zone already carried the SD-access warning — only the web trigger handler was not following through.

- Force Upload in **Smart mode quiet period** now runs a normal phased upload session restricted to recent data (`FRESH_ONLY`), same semantics as Force Upload already had in **Scheduled mode outside the upload window**.
- The automated FSM path is untouched: the quiet period still fully suppresses scheduled uploads. Only the user-initiated web trigger bypasses it, matching existing UI copy.
- Standard safeguards still apply: `EXCLUSIVE_ACCESS_MINUTES` time budget, pre-flight work probe, and the `nothing-to-upload` short-circuit.

### 🚫 No other behaviour changes to uploads
Upload scheduling, smart-mode quiet period (for automated uploads), SD-card access, SMB/cloud transport, and state tracking are otherwise identical to v4.0.

---

## Changelog (since v4.0)

- **Web UI**: Rewrote the Upload Progress card as per-backend rows; dropped `DUAL` label, `(N empty)` counter, and the stale `Next:` block.
- **Web UI**: Renamed `"All synced"` to `"Up to date"`; made the aggregate status line backend-aware.
- **API**: Added `backends.{cloud,smb}` object to `/api/status`; removed obsolete `next_*` fields. Plumbed `setCloudStateManager()` alongside the existing `setSmbStateManager()` so the status snapshot reads both state managers directly.
- **Internals**: `g_activeBackendStatus.name` no longer emits `"DUAL"`; set to `CLOUD` or `SMB` at rest and to the currently-running phase mid-session.
- **Buffer**: `WEB_STATUS_BUF_SIZE` grown from 1024 → 1536 to fit the new per-backend block.
- **Docs**: `MINIMIZE_REBOOTS` removed from the user configuration reference and architecture guide; now commented as developer-only.
- **Fix**: Force Upload in Smart-mode quiet period now runs a recent-data-only session instead of silently no-op'ing. Same semantics as Force Upload in Scheduled mode outside the window.
- **Fix**: SMB "Last upload" timestamp is now updated on every successful phased session. Previously only the primary (cloud) backend got stamped, so the NAS row kept showing a stale "N days ago" even right after a successful upload.
- **Fix**: Per-backend progress bar now fills smoothly during an active upload. Previously it sat frozen at `N / N ✓` while the `Uploading k/m · <folder>` detail ticked underneath, because the state manager counts the currently-uploading folder as "already done" whenever it's a refresh of a recent folder (the dominant case in Force / FRESH_ONLY sessions). Full decision trail in `docs/dev/archive/74-PROGRESS-BAR.md` §8.5.
- **Major UX fix**: The progress counts now mean what users expect. Right side = **DATALOG folders on the card within `MAX_DAYS`** (stable, same for both backends). Left side = **folders from that window that are fully synced to this backend** (a refreshed folder with new content drops out of this count until re-upload finishes). This eliminates the long-standing confusion of two backends showing different denominators (`11 / 11` vs `9 / 9`) even when both were fully caught up: they now share a single denominator and any divergence in the numerator is real and informative. An idle device with no DATALOG folders renders `No data on card yet` instead of `0 / 0`. Implementation: a new single-pass work probe in `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp::hasWorkToUpload` enumerates `/DATALOG` once, consults both backends per folder, and publishes an in-memory snapshot via new `UploadStateManager::setProbeSnapshot()`. The `-1 done` client-side synthesis from the progress-bar fix above is no longer needed and has been removed. Full decision trail in `docs/dev/archive/74-PROGRESS-BAR.md` §9.
- **Fix**: When both Cloud and SMB are configured, the Cloud progress row now updates as soon as the Cloud phase finishes, instead of waiting for the SMB phase to complete. Previously, if SMB took several minutes (or ran into a slow failure because the user's NAS is off at night), the Cloud row stayed frozen at its pre-session value even though Cloud was already done. Fix adds a mid-session probe refresh at the seam between Phase 1 (Cloud) and Phase 2 (SMB) in `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp::runFullSession`, guarded by `if (smbWork)` so it only runs when an SMB phase actually follows. Full decision trail in `docs/dev/archive/74-PROGRESS-BAR.md` §9.8.
- **Fix**: Progress bar no longer oscillates or jumps during an active upload phase. Previously, `done` was frozen at the last probe snapshot while the web UI added fractional folder progress (`live.up / live.total`), causing the bar to flicker between e.g. `3` and `4` as each folder completed and reset. Cloud also jumped from the middle to the end when the post-cloud probe finally refreshed. Fix adds a `probeSnapshotCompletedCount` field to `UploadStateManager` that captures `completedCount` at probe time; `updateStatusSnapshot()` now adds newly-completed folders since the snapshot to the live `done` count, so the numerator advances monotonically and smoothly throughout the phase. Full decision trail in `docs/dev/archive/74-PROGRESS-BAR.md` §9.9.
- **Fix**: SMB uploads preserve original file timestamps from the SD card by default. After each file is successfully written to the NAS, the remote file's `creation_time`, `last_access_time`, `last_write_time`, and `change_time` are set to match the local file's `lastWrite` timestamp, so the backup appears with the correct original date (e.g., the actual sleep-study night) rather than the upload time. Implementation uses `SMB2_SET_INFO` with `FILE_BASIC_INFORMATION` via the existing `libsmb2` raw API (`smb2_cmd_set_info_async`). **Enabled by default** — set `SMB_PRESERVE_TIMESTAMPS=false` in `config.txt` to disable if upload-time timestamps are preferred. If the timestamp update fails, the upload itself is still considered successful — a warning is logged but the session continues. One additional SMB round-trip per file (~1–5 ms on a local NAS, negligible next to multi-second file writes).
- **Fix**: SMB timestamp-preservation helper (`smb2_set_basic_info_ev`) was missing the required `smb2_queue_pdu()` call after `smb2_cmd_set_info_async()`. Without this the SET_INFO PDU was created but never sent on the wire, so `smb2_run_event_loop()` waited forever for a response that never came, causing the SMB phase to hang indefinitely after the first file's `smb2_open` succeeded. Fix adds the missing queue call in `@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp`.
- **Fix**: Post-session work probe now also runs when the upload timer expires (`TIMEOUT`). Previously it only ran on `COMPLETE` and `NOTHING_TO_DO`, so a partial session that uploaded several folders but ran out of time left the progress bar frozen at the stale pre-session snapshot. For SMB (which runs in Phase 2 after Cloud), this meant the bar could roll all the way back to `0 / N` even though multiple folders were successfully uploaded — the live delta (`completedCount - probeSnapshotCompletedCount`) stopped applying once `live.active` went false, and `probeSynced` still held the pre-SMB value. Fix in `@/opt/projects/personal/CPAP_data_uploader/src/main.cpp` adds `UploadResult::TIMEOUT` to the post-session probe guard so the snapshot is refreshed before the SD card is released.
- **Performance**: Work probe is now **a single enumeration of `/DATALOG` shared across both backends**, where previously it ran a full pass per configured backend. In the common "everything synced" case this halves SD hold time at probe entry. In the "work found" case we no longer short-circuit (we need the full count), adding ~100–200 ms typical — trivially small next to a 100+ second upload session. No schema change; existing state files on upgrade.
- **Performance (SMB)**: Major SMB upload throughput improvements via persistent sessions, TCP tuning, adaptive buffer sizing, and write pipelining. Implementation based on `docs/dev/archive/75-SMB-IMPROVEMENTS.md` (Phases A, B, C). Changes:
  - **Persistent SMB session**: The SMB connection is now established once at the start of Phase 2 and reused across all folder uploads, eliminating repeated connect/disconnect overhead (previously incurred per folder).
  - **TCP_NODELAY**: Enabled `TCP_NODELAY` on the libsmb2 socket after connection to disable Nagle's algorithm, reducing latency for small PDUs.
  - **Removed artificial drain delay**: Eliminated the `SMB_WRITE_TCP_DRAIN_BYTES` delay band-aid (a `delay(10)` every 16 KB) in favor of native TCP flow control.
  - **Adaptive buffer sizing**: The upload buffer size is now dynamically chosen based on both heap availability and the server's advertised maximum write size (`smb2_get_max_write_size()`). The top band for chunk sizing has been extended to 32 KB when heap permits.
  - **Pipelined writes**: Implemented write pipelining with depth 2 using `smb2_pwrite_async` with explicit file offsets, allowing up to two SMB write PDUs to be in-flight simultaneously. This leverages SMB2 credits for better throughput. A contiguous buffer is carved into pipeline slots to avoid heap fragmentation. Falls back to serial writes if pipelined buffer allocation fails.
  - These changes are expected to significantly improve SMB upload throughput, especially for sessions with multiple folders and larger files.

---

## Upgrade Instructions

### Option 1 — OTA (recommended from v4.0)

1. Open your device's dashboard at `http://cpap.local` (or its IP address).
2. Go to the **OTA** tab.
3. Either point the URL uploader at the `firmware-ota-upgrade-v4.1-beta1.bin` asset from the Releases page, or download the file and upload it manually.
4. The device will reboot into the new firmware. Configuration and upload state are preserved.

### Option 2 — Full flash via USB

Only needed if you are upgrading from v3.6i or earlier, or if OTA fails. Follow the Full Flash Instructions in the v4.0 release notes — the process is identical; just use the `firmware-ota-v4.1-beta1.bin` file at address `0x0`, and remember to click **Erase** before **Program**.

---

## Known Limitations

Unchanged from v4.0 — see `RELEASE_NOTES_v4.0.md` for details.
