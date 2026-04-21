# Upload Progress Bar — Analysis & Recommendations

**Tickets:** #58, #61
**Reference commit (marginally-better state, ~v0.9.2):** `ddaab74`
**Scope:** Web dashboard "Upload Progress" card. Analysis only, no code changes.

---

## 1. Summary of the problem

In the current dashboard, when both SMB and CLOUD backends are configured the "Upload Progress" card shows a **single collapsed bar labelled `DUAL`** with counters like `15 / 15 (1 empty) ✓` and a footer `Status: ✓ All synced`. Users consistently find this confusing:

1. **`DUAL` is not an endpoint** — it hides the per-backend state. A user can't tell whether files went to SleepHQ, to the NAS, or to both.
2. **`(1 empty)`** is correct bookkeeping but a leaky abstraction — users don't care about "empty folders" as a concept and shouldn't have to know what they are.
3. **`All synced`** is asserted from a single `incomplete > 0 ? pending : all-good` check against the *primary* state manager only (cloud if present, else SMB). In dual mode it can be truthful for one backend but silent about the other.

The older UI (image 2 in the ticket) displayed two rows: an active bar for one backend and a muted "Next: CLOUD" row with a staleness timestamp. That layout carried more information but was itself a by-product of the old *cycling* orchestrator (one backend per session) rather than a deliberate design.

---

## 2. Where the confusion comes from (code pointers)

### 2.1 Server emits a blended "DUAL" label
`@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp:258-266`
```
const char* mode = hasBothBackends() ? "DUAL" :
                   hasCloudBackend() ? "CLOUD" :
                   hasSmbBackend()   ? "SMB"   : "NONE";
strncpy(g_activeBackendStatus.name, mode, ...);
```
…and again at end-of-session (`FileUploader.cpp:732-735`). During a session the label is briefly overwritten with the real phase (`"CLOUD"` then `"SMB"`), but the steady-state value shown on the dashboard between sessions is `"DUAL"`.

### 2.2 Folder counts are single-backend, mislabelled as "DUAL"
`@/opt/projects/personal/CPAP_data_uploader/src/CpapWebServer.cpp:936-943`
```
if (stateManager) {
    foldersDone    = stateManager->getCompletedFoldersCount();
    foldersPending = stateManager->getPendingFoldersCount();
    foldersTotal   = foldersDone + stateManager->getIncompleteFoldersCount();
}
```
`stateManager` here is `primaryStateManager()` — **cloud if present, otherwise SMB** (`FileUploader.h:99-102`). So the numbers under the `DUAL` label are *not* a blended view; they are literally the cloud backend's numbers with a misleading label.

The SMB state manager *is* already plumbed into the web server via `setSmbStateManager()` (`CpapWebServer.cpp:1094`), but `updateStatusSnapshot()` never reads it.

### 2.3 "All synced" only looks at one backend
`@/opt/projects/personal/CPAP_data_uploader/include/web_ui.h:626`
```
var fst=inc>0?'⚠ '+inc+' folder(s) pending':(done>0?'✓ All synced':'Waiting for first scan');
```
`inc` / `done` come from the primary-backend snapshot above. Same root cause — it can't truthfully describe a dual-backend system.

### 2.4 "(1 empty)" is internal bookkeeping
Empty / pending folders are DATALOG folders with no new files since last upload. They must be tracked so the uploader knows whether to re-scan them, and they're legitimately *excluded* from the progress denominator (per commit `ddaab74`). But surfacing the raw count to the user — with no explanation — is unnecessary cognitive load.

### 2.5 `next_backend` is permanently `NONE` in the new model
The phased orchestrator runs CLOUD then SMB inside **one** session, so the "inactive / next" block that the UI still knows how to render (`web_ui.h:616-625`, `d-next-be`) is never populated any more. `g_inactiveBackendStatus.name` is fixed at `"NONE"` at init and never updated. That's why the old two-row layout disappeared — the data stopped flowing, not the UI.

---

## 3. What data is *already* available

The good news: we already have, or can trivially derive, everything needed for a per-backend display. **No new scan logic is required.**

Per backend (SMB and CLOUD) we can read on every 3 s snapshot:

| Source | Field | Already available? |
| --- | --- | --- |
| `smbStateManager->getCompletedFoldersCount()` | done | yes |
| `smbStateManager->getIncompleteFoldersCount()` | remaining | yes |
| `smbStateManager->getPendingFoldersCount()` | empty | yes |
| `smbStateManager->getLastUploadTimestamp()` | last OK | yes |
| `g_smbSessionStatus.uploadActive / currentFolder / filesUploaded / filesTotal` | live file-level | yes |
| Same four/five fields for `cloudStateManager` / `g_cloudSessionStatus` | yes |

`hasSmbBackend()` / `hasCloudBackend()` tell us which rows to display. That's the entire required dataset.

The `/api/status` endpoint (`CpapWebServer.cpp:1004-1041`) currently packs only the *primary* backend's three folder counts and one `live_*` group. To render two honest rows it needs to pack both.

---

## 4. Recommendation

**Primary recommendation: restore a per-backend two-row layout, driven by both state managers, and remove internal bookkeeping from the user's view.**

### 4.1 UI (the main change)

Replace the single "DUAL" card with one row **per configured backend**:

```
UPLOAD PROGRESS
  SleepHQ Cloud                                  15 / 15  ✓
  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
  Last upload: 5 min ago

  NAS (SMB)                                      14 / 15  ⚠ 1 left
  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▒▒▒▒▒▒
  Uploading 24/48 · DATALOG/20260420

  Status                                         ✓ Up to date
```

Key properties:

- Each configured backend renders its own labelled row. No blended numbers.
- In single-backend configurations, exactly one row renders — the layout degenerates cleanly to today's single-bar look, just with a real label (`SleepHQ Cloud` / `NAS (SMB)`) instead of `CLOUD` / `SMB`.
- The "live" (in-flight) row should show the per-file counter inline under the bar **only for the backend that's currently uploading** (`live_folder`, `live_up`, `live_total`). The other row stays in its steady-state "last upload X ago" view.
- Empty folders are no longer shown as a counter. They are silently excluded from the denominator (already the case) and, at most, surfaced on hover/tooltip ("N folders had no new data"), or dropped entirely. I recommend dropping them.
- Overall "Status" line becomes an **aggregate** over all configured backends:
  - If any backend has `incomplete > 0` → `⚠ N folder(s) pending on <backend>` (or `… on multiple backends`).
  - Else if any backend has ever completed an upload (`last_ts > 0`) → `✓ Up to date`.
  - Else → `Waiting for first scan`.
  - Wording: **"Up to date"** is clearer than **"All synced"** (which reads like a Dropbox/Drive marketing phrase and invites "synced with *what*?"). Any of "Up to date", "All uploads complete", or "Nothing pending" work.

### 4.2 Backend — minimal JSON changes

I recommend a **small, additive** change to `/api/status`. Current format keeps the `active_backend` / `folders_*` / `next_*` / `live_*` fields (for backwards compatibility with any external monitoring), and adds explicit per-backend objects:

```json
{
  ...existing fields unchanged...,
  "backends": {
    "cloud": {
      "enabled": true,
      "done": 15, "total": 15, "pending": 1,
      "last_ts": 1745012345,
      "live": { "active": false, "folder": "", "up": 0, "total": 0 }
    },
    "smb": {
      "enabled": true,
      "done": 14, "total": 15, "pending": 1,
      "last_ts": 1745010000,
      "live": { "active": true, "folder": "DATALOG/20260420", "up": 24, "total": 48 }
    }
  }
}
```

That's the *only* server change required. It fits inside the existing `WEB_STATUS_BUF_SIZE=1024` budget with room to spare (≈250 bytes added). Nothing else in the status buffer needs to move.

Implementation touch points (for later, not in this doc):
- `CpapWebServer.cpp::updateStatusSnapshot()`: read from both `smbStateManager` *and* the currently-unread cloud state manager. This means plumbing a `setCloudStateManager()` setter alongside the existing `setSmbStateManager()`, mirroring what already exists.
- `web_ui.h` JS: iterate `d.backends` and render one row per enabled backend. The old per-row markup (`d-ab-*` / `d-nb-*`) can be templated/cloned, or the two rows can be hard-coded (since there are at most two).
- Drop the `"DUAL"` sentinel from `FileUploader.cpp`. During a phased session, `active_backend` can still report the *currently running* phase (`CLOUD` or `SMB`) for any external consumer; at rest it's informational only.

### 4.3 What to remove

- `DUAL` as a value of `active_backend` — it never carried useful information.
- The `(N empty)` parenthetical in the folder counter.
- `next_backend` / `next_*` / `d-next-be` block — obsolete since the move to single-session phased uploads. The "two rows" layout replaces it properly.
- `"All synced"` wording → `"Up to date"`.

### 4.4 What to keep

- The 3 s snapshot cadence and the zero-heap `snprintf`-into-static-buffer pattern — there is no pressure on either.
- The `live_*` per-file counter mechanism — just route each backend's live data into its own row instead of into a single shared slot.
- Exclusion of empty/pending folders from the denominator (as fixed in `ddaab74`). This is correct; only the *display* of the empty count should go.

---

## 5. Why not just go back to "SMB + Next: CLOUD"

The old two-row layout (image 2) was an accurate *view* but a misleading *model*: it suggested backends took turns session-by-session, with one "stale" until its turn came around. Under the current phased orchestrator both backends run every session, so the "Next / stale" framing is actively wrong. The correct conceptual model today is **two parallel pipelines, both alive**, which is exactly what the proposal in §4.1 shows.

---

## 6. Suggested order of work (non-binding)

1. Add per-backend fields to `/api/status` (additive, no breakage).
2. Add `setCloudStateManager()` wiring so the web server can read both state managers, same pattern as `setSmbStateManager()`.
3. Rewrite the "Upload Progress" card in `web_ui.h` to render N rows from `d.backends`.
4. Replace "All synced" wording; drop `(N empty)`; drop `DUAL`.
5. Retire the `next_backend` / `d-next-be` block.
6. Optional follow-up: user-friendly backend labels (`SleepHQ Cloud`, `NAS (SMB)`) instead of the raw `CLOUD` / `SMB` tokens — this is the cheapest readability win in the whole change.

---

## 7. Related question: what does `MINIMIZE_REBOOTS` still do?

### 7.1 What it used to do
In the old *cycling* orchestrator (pre-`754cc77`, pre-phased), each upload session ran exactly **one** backend, chosen by `selectActiveBackend()` based on whichever `.backend_summary.*` file had the older `sessionStartTs`. With `MINIMIZE_REBOOTS=false`, the device rebooted after every session, which had a useful *side-effect*: on the next boot the selector would naturally pick the *other* backend, so SMB and CLOUD alternated session-by-session. The reboot was about heap recovery; the alternation was a consequence of the selection algorithm, not a flag.

### 7.2 What it does today
The cycling orchestrator is gone. `runFullSession()` now runs **CLOUD then SMB inside one session** (phased) — `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp:289-305`. The order is fixed and memory-driven: cloud first (TLS arena active, heap clean), then TLS is torn down via `resetConnection()` to release ~40 KB and clear the lwIP socket table for libsmb2. Running SMB first would starve the cloud phase of heap.

So today, `MINIMIZE_REBOOTS`:

- **Does not** affect which backend runs, in what order, or how often.
- **Does not** affect the "active backend" / "stale backend" display.
- **Only** controls whether `handleReleasing()` calls `esp_restart()` at the end of a session (`@/opt/projects/personal/CPAP_data_uploader/src/main.cpp:1426-1451`).

Concretely:

| `MINIMIZE_REBOOTS` | Behaviour after a session that did real work |
| --- | --- |
| `true` (default) | Stay alive. Go straight to `COOLDOWN → LISTENING`. Runtime is reused. A **heap safety valve** at `ma < 32 KB` *forces* a reboot anyway, with a warning at `ma < 35 KB`. |
| `false` | Always `esp_restart()` after a real upload session ("reboot for heap recovery"). |

Two existing carve-outs bypass the flag and are orthogonal to it:
- *Nothing uploaded* (`g_nothingToUpload`): never reboots, regardless of flag (`main.cpp:1418-1425`).
- *Monitoring requested*: goes to `MONITORING` instead (`main.cpp:1407-1414`).

### 7.3 Is it still worth being user-configurable?

Strictly from user value: **probably not**. The knob's original purpose (controlling backend alternation) is gone, and the remaining purpose (elective reboot for heap recovery) is already handled correctly by the safety valve that triggers on `ma < 32 KB` regardless of the flag. Leaving it on is a strict win on every normal session: faster turnaround, no boot delay, no connection re-establishment, no lost log continuity.

Options, in order of preference:

1. **Hide it, keep it internal.** Remove the knob from user-facing docs and the setup wizard, but keep the config key and its default (`true`) as a developer escape hatch for debugging heap issues. This is the lowest-risk choice and reflects how the flag is actually used today.
2. **Remove it entirely.** Always skip the elective reboot; rely on the safety valve. Simpler code, one fewer state to reason about. Safe because the valve already enforces the heap floor that the reboot path was there to guarantee.
3. **Keep it, relabel it.** Rename to something that reflects its current meaning — e.g. `FORCE_REBOOT_AFTER_UPLOAD` (default `false`) — and document it as a diagnostic toggle for heap-leak hunting, not a tuning parameter. Only worth it if you want users to still be able to toggle it.

I'd pick **(1)**: hide the flag, default stays `true`, no behaviour change, no docs burden. `(2)` is cleaner but touches more files for marginal benefit. `(3)` only makes sense if you want to keep user control, which I don't think is justified here.

### 7.4 Impact on the progress-bar proposal

None. The progress bar shows *what the state managers know*, not *whether the device rebooted*. The proposal in §4 stands regardless of how `MINIMIZE_REBOOTS` is handled. If anything, moving to a per-backend UI makes it easier to explain the current behaviour in text next to the card ("both backends upload every cycle"), which further undermines the case for a user-facing reboot knob.

No build-system, scan-logic, or state-manager changes are required.
