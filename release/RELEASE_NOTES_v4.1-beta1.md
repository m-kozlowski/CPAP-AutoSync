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

### 🚫 No behaviour changes to uploads
This release is UI-layer and documentation only. Upload scheduling, smart-mode quiet period, SD-card access, SMB/cloud transport, and state tracking are all identical to v4.0.

---

## Changelog (since v4.0)

- **Web UI**: Rewrote the Upload Progress card as per-backend rows; dropped `DUAL` label, `(N empty)` counter, and the stale `Next:` block.
- **Web UI**: Renamed `"All synced"` to `"Up to date"`; made the aggregate status line backend-aware.
- **API**: Added `backends.{cloud,smb}` object to `/api/status`; removed obsolete `next_*` fields. Plumbed `setCloudStateManager()` alongside the existing `setSmbStateManager()` so the status snapshot reads both state managers directly.
- **Internals**: `g_activeBackendStatus.name` no longer emits `"DUAL"`; set to `CLOUD` or `SMB` at rest and to the currently-running phase mid-session.
- **Buffer**: `WEB_STATUS_BUF_SIZE` grown from 1024 → 1536 to fit the new per-backend block.
- **Docs**: `MINIMIZE_REBOOTS` removed from the user configuration reference and architecture guide; now commented as developer-only.

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
