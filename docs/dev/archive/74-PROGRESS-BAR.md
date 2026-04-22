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

---

## 8. Implementation log (v4.1-beta1)

This section is the paper trail for what actually shipped. The analysis in §§1-7 is untouched; everything below is post-hoc.

### 8.1 What was implemented from §4

Delivered in v4.1-beta1:

- **Per-backend rows** — `@/opt/projects/personal/CPAP_data_uploader/include/web_ui.h`, the Upload Progress card now renders `SleepHQ Cloud` and `NAS (SMB)` as two independent rows, each hidden unless that backend is configured.
- **`backends` object in `/api/status`** — `@/opt/projects/personal/CPAP_data_uploader/src/CpapWebServer.cpp::updateStatusSnapshot` emits `backends.{cloud,smb} = { enabled, done, total, pending, last_ts, live: { active, folder, up, total } }`. Legacy fields (`active_backend`, `folders_*`, `live_*`) retained for back-compat; `next_*` removed (dead since the move to phased orchestration).
- **`setCloudStateManager()` plumbed** — mirrors the existing `setSmbStateManager()`. `@/opt/projects/personal/CPAP_data_uploader/src/main.cpp` wires both after uploader creation.
- **`"DUAL"` sentinel dropped** — `g_activeBackendStatus.name` now reports the currently-running phase (`CLOUD` / `SMB`) or the primary at rest (`@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp`).
- **`"(N empty)"` counter and `Next: …` row removed** — both were leaky abstractions; replaced by silent accounting in `pending` and a backend-aware aggregate Status line (`Up to date` / `⚠ N on cloud, M on NAS pending`).
- **Buffer bump** — `WEB_STATUS_BUF_SIZE` 1024 → 1536 in `@/opt/projects/personal/CPAP_data_uploader/include/WebStatus.h` to fit the new block with headroom.

### 8.2 `MINIMIZE_REBOOTS` — decision from §7

Chose **Option (1)**: hide it, keep it internal. The key and its default (`true`) are preserved unchanged; only the user-facing docs were updated:
- Removed from `docs/user/configuration-ui.md` (replaced with a developer-only HTML comment).
- User-facing mention removed from `docs/dev/architecture.md` (now references the `max_alloc < 32 KB` safety valve instead).

No code change. Zero regression risk.

### 8.3 Force Upload in Smart-mode quiet period (follow-up bug)

**Symptom:** User pressed Force Upload at ~00:20 local time in Smart mode (`SMART_START_HOUR=6`, window 9–18). FSM logged `No data category eligible, releasing` and no upload ran. The dashboard copy (`Force Upload → forces an upload of recent data now`) and the Danger Zone warning (`takes control of the SD card …`) both already promised exactly what the user expected; only the trigger handler wasn't following through.

**Options considered:**

| # | Approach | Verdict |
|---|---|---|
| A | Mirror the Scheduled-outside-window rule: set `g_forceRecentOnlyFlag` whenever `!canUploadFreshData()`. One predicate covers both Scheduled-out-of-window and Smart-quiet-period. | **Chosen.** ~5-line diff in `handleTriggerUpload()`. No new flags, no FSM states, matches existing UI copy. |
| B | Always set `g_forceRecentOnlyFlag` on any web trigger. | Rejected — would silently demote in-window Force Upload from `ALL_DATA` to `FRESH_ONLY`, breaking the "catch up on backlog" use case. |
| C | Introduce a second flag `g_forceUploadFlag` that bypasses all eligibility checks. | Rejected — strictly more surface area than A with no functional win; the only setter of `g_forceRecentOnlyFlag` is already user-initiated web trigger, so the user-vs-automated distinction A provides is already clean. |
| D | New config key `FORCE_UPLOAD_BYPASSES_QUIET`, default `false`. | Rejected — defeats the purpose of a button explicitly called *Force Upload*, doubles user cognitive load, and the Danger Zone copy already carries the consent language. |
| E | Add a `bool forced` parameter to `ScheduleManager::canUploadFreshData()`. | Rejected — muddles a read-only predicate with caller intent; wrong layer; touches every caller. |

**Shipped (A)** at `@/opt/projects/personal/CPAP_data_uploader/src/CpapWebServer.cpp::handleTriggerUpload`:

```cpp
if (scheduleManager && !scheduleManager->canUploadFreshData()) {
    const char* why = scheduleManager->isSmartMode()
        ? "Smart mode quiet period"
        : "Scheduled mode outside window";
    LOGF("[WebServer] %s — force upload limited to recent data", why);
    g_forceRecentOnlyFlag = true;
}
```

Automated FSM path untouched: Smart quiet period still fully suppresses scheduled uploads. Only user-initiated Force Upload bypasses it.

### 8.4 SMB "Last upload" timestamp staleness (follow-up bug)

**Symptom:** After a clean dual-backend session, the SleepHQ Cloud row updated to `Last upload: just now` but the NAS (SMB) row kept showing `Last upload: 4 days ago`.

**Root cause:** `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp::runFullSession()` only stamped `primaryStateManager()` (cloud when both are configured). The SMB state manager's `lastUploadTimestamp` was never written.

**Fix:** Stamp every configured backend's state manager on a clean session, using the existing `hasIncompleteFolders() == false` guard (which was already two-backend-aware, so if it passes both backends legitimately finished):

```cpp
if (cloudStateManager) cloudStateManager->setLastUploadTimestamp((unsigned long)endNow);
if (smbStateManager)   smbStateManager->setLastUploadTimestamp((unsigned long)endNow);
if (scheduleManager)   scheduleManager->setLastUploadTimestamp((unsigned long)endNow);
```

`scheduleManager` also mirrored so any other consumer of "last upload" stays consistent.

### 8.5 Progress-bar live-fill flicker (follow-up UX)

Once §8.1 was in, users saw a live per-file `Uploading 5/12 · <folder>` line underneath the per-backend row while the bar itself sat at `15 / 15 ✓` and never moved during the upload. Two iterations were needed.

**Root cause (state manager):** `@/opt/projects/personal/CPAP_data_uploader/src/UploadStateManager.cpp:575-585` — `getIncompleteFoldersCount()` is `totalFoldersCount - completedCount - pendingCount`, clamped to 0. And `totalFoldersCount` is set from `folders.size()` at `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp:901`, i.e. the number of folders *this scan decided it needed to touch*, not the true universe of folders. As a result the UI's derived `total = done + incomplete` can read equal to `done` even while a folder is being uploaded. Two distinct sub-cases:

- **Case A — net-new folder** (e.g. tonight's `20260421` appearing for the first time): `totalFoldersCount = 1`, `completedCount` stays at 15 → `incomplete = max(0, 1-15-0) = 0` → UI sees `15/15` until the folder finishes, then snaps to `16/16`.
- **Case B — recent-only refresh** (RECENT_FOLDER_DAYS re-iterating today's already-completed folder to pick up appended files): counters don't move at all; `done == total == N` throughout the session. The dominant case in FRESH_ONLY / Force Upload runs.

**First attempt — "+1 total" synthesis (shipped briefly):** when `live.active && done >= total`, render as `done + frac` over `total + 1`. Worked for Case A. For Case B it introduced a visible `16 / 16 ✓ → 16 / 17 uploading → 16 / 16 ✓` flicker because the synthesised `+1` slot vanished at the end of the session.

**Options considered for the fix:**

| # | Approach | Verdict |
|---|---|---|
| 1 | Flip synthesis to **`-1 done`** when `done >= total && live.active`. Pre-subtract the folder being refreshed. | **Chosen.** ~3-line JS diff. Case B: `16/16 ✓ → 15/16 uploading → 16/16 ✓` — matches user's mental model. Case A: `15/15 ✓ → 14/15 uploading → 16/16 ✓` — one-off visual jump of 2 at completion but bar behaviour is monotone and correct. |
| 2 | Add a `live.refresh` (or `live.new_folder`) boolean to `/api/status`. Server knows whether the currently-uploading folder is already in that backend's `completedFolders`; client picks `+1` vs `-1` accordingly. | Held in reserve. Correct for every case, requires a small `UploadStateManager::isFolderCompleted(String)` getter plus ~3 lines in `updateStatusSnapshot()`. Worth doing only if (1)'s Case-A visual jump becomes user-visible. |
| 3 | Fix the root cause — change `UploadStateManager::totalFoldersCount` semantics so it tracks `done + incomplete + pending`, not scan-result size. | Rejected. Touches the state manager hot path and the persisted field semantics, meaningful regression risk, existing devices would see a one-time inconsistency on upgrade. Not justified by a cosmetic UI polish. |
| 4 | Revert to pre-beta1 behaviour (no synthesis). | Rejected — the user explicitly flagged the jumpy `N/N → (N+1)/(N+1)` as unacceptable. |

**Shipped (1)** at `@/opt/projects/personal/CPAP_data_uploader/include/web_ui.h::renderBe`:

```js
if(live.active && live.total>0){
  frac = Math.min(1, Math.max(0, live.up/live.total));
  if(done >= total && done > 0) dispDone = done - 1 + frac;  // re-upload case
  else dispDone = done + frac;                                // promoted-mid-scan case
}
var shownDone = live.active ? Math.floor(dispDone) : done;
```

The `done > 0` guard prevents a negative display during the first-ever upload on a virgin device.

**Assumption documented:** the live folder in a FRESH_ONLY / recent-only session is one already counted in `done`. This is safe because `scanDatalogFolders()` sorts newest-first (`@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp:890-892`) and Force Upload in quiet-period uses `FRESH_ONLY` (see §8.3). If we ever need to be exact, promote to Option 2.

### 8.6 Summary of regressions checked

- **Upload scheduling** — untouched; Smart-mode automated path still fully quiet before `SMART_START_HOUR`.
- **Phased orchestrator / state managers** — untouched beyond the one-line additional stamping in §8.4.
- **External `/api/status` consumers** — legacy fields preserved; `next_*` removal is the only break, and those were zero/`"NONE"` in v4.0 (dead fields).
- **`MINIMIZE_REBOOTS`** — config key, default, and behaviour all unchanged.
- **Heap / flash** — status buffer +512 B `.bss`; flash usage 88.8% (flat vs v4.0).

Release notes: `release/RELEASE_NOTES_v4.1-beta1.md`.

---

## 9. Meaningful progress numbers — Option 1 shipped

Follow-up to §8.5.  The `-1 done` synthesis in §8.5 was a client-side papering over a state-manager quirk: `total` collapsed to `done` whenever the last scan returned 0 folders, so `N / N ✓` became the resting display for every backend regardless of how much data actually lived on the card.  Users looking at two backends with different historical completed-counts (e.g. `11 / 11 ✓` vs `9 / 9 ✓`) had no way to tell whether they were equivalently in sync or significantly divergent.

Fixed in v4.1-beta1 by:

1. **Redefining the two numbers** with meanings that map directly onto a user's mental model:
   - **Right side (`universe`)**: DATALOG folders on the SD card within the configured `MAX_DAYS` window.  Stable across both backends (same value), moves only when the CPAP writes a new day or an old one rolls off the cliff.
   - **Left side (`synced`)**: folders from that universe that are fully in sync for this backend.  A folder counts as synced when it is completed AND — if it falls inside `RECENT_FOLDER_DAYS` — no `.edf` has changed since last upload.

2. **Rewriting the work probe** (`@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp::hasWorkToUpload`) as a **single-pass enumeration** of `/DATALOG` that, for every folder in the `MAX_DAYS` window, consults **both** state managers and computes per-backend `hasWork`, `universe`, `synced`.  The old two-pass (one per backend) probe is gone.  The short-circuit-on-first-hit is also gone — we now always enumerate the full window — because we need the complete counts.

3. **Publishing the snapshot** into each `UploadStateManager` via a new `setProbeSnapshot(universe, synced)` + `getProbeUniverse() / getProbeSynced()` API (`@/opt/projects/personal/CPAP_data_uploader/include/UploadStateManager.h:81-88, 147-150`).  In-memory only, not persisted — each boot's first probe refreshes it.

4. **Reading the snapshot** in `CpapWebServer::updateStatusSnapshot` via a small lambda that prefers the probe snapshot and falls back to the legacy `done / done+incomplete` math only when no probe has run yet this boot (pre-first-session UX stays coherent).

### 9.1 SD hold-time impact

Measured expectation:

| Case | Old probe | New probe | Delta |
|---|---|---|---|
| No work found | Enumerate all folders, two passes (one per backend) | Enumerate all folders, **one pass** | **Reduced** by ~50% of the enumeration cost when two backends are configured. |
| Work found on first folder | Short-circuit immediately, two passes | Full enumeration, one pass | +50–500 ms in the worst case; typical ~100–200 ms. |
| Work found in a completed+recent folder | Short-circuit on first `hasFileChanged=true` | Continue enumerating remaining folders to tally counts | Same bounded cost as above. |

Session context: actual upload holds the SD card for 100–200+ seconds; the probe accounts for <1 s of that.  The probe's role is to decide whether to create the upload task at all — adding ~100 ms to it is immaterial next to skipping the upload entirely in the no-work case (which saves tens of seconds of SD hold time).

The bonus single-pass optimisation means the **two-backend probe is now cheaper than the old single-backend probe** in the no-work case, which is the dominant case in a well-synced device.

### 9.2 Edge cases handled

- **No SD, or `/DATALOG` missing**: probe logs warning and returns `{hasWork=false, universe=-1, synced=-1}`; UI falls back to legacy math.
- **Clock not yet synced**: MAX_DAYS cutoff calculation is guarded by `now > 24*3600`; if that fails we fall back to "no cutoff" and count every folder on the card.  Self-corrects on the next probe once NTP succeeds.
- **Empty folder, never uploaded**: `folderHasEdf()` returns false for incomplete folders → not counted as work, not counted as synced.  Pending-folder bookkeeping in the state manager handles promotion separately.
- **First boot / virgin card**: `universe=0`.  Client renders "No data on card yet" instead of a `0/0` ratio (`@/opt/projects/personal/CPAP_data_uploader/include/web_ui.h:628-630`).
- **RECENT_FOLDER_DAYS re-discovery**: a completed-recent folder where any `.edf` has changed since last upload drops out of `synced` immediately — user sees `24/25` during re-sync, returns to `25/25` when done.  No client-side synthesis required; the state manager owns the truth.
- **Migration**: no schema change; the probe snapshot is in-memory only.  Existing state files untouched.  Users will notice the `/N` value shift on first session after the upgrade (e.g. `/17` → `/25`) because the semantics changed.

### 9.3 What the v4.1-beta1 UI looked like before vs after

| Moment | Before (pre-§9) | After (§9) |
|---|---|---|
| All synced, two backends | `11 / 11 ✓` and `9 / 9 ✓` — different, unexplainable | `25 / 25 ✓` and `25 / 25 ✓` (or e.g. `25 / 25` vs `24 / 25` exposing a real one-folder divergence) |
| Idle, nothing uploaded yet | `0 / 0` displayed as `— ✓` | `0 / 25` or `No data on card yet` |
| RECENT_FOLDER_DAYS refresh of today's folder | `16 / 16 → 15 / 16 uploading → 16 / 16 ✓` via `-1 done` synthesis | `25 / 25 → 24 / 25 uploading → 25 / 25 ✓` — state manager drives the transition |
| Force Upload in Smart quiet period (§8.3) | Same drop pattern via client synthesis | Same, but now honest: server knows the folder isn't synced |

### 9.4 Why the old `-1 done` synthesis could be retired

It existed purely to paper over a broken denominator.  Now that the denominator is `universe` (stable) and the numerator is `synced` (which the probe recomputes with full `hasFileChanged` knowledge), the "folder being refreshed" case is captured in the state manager itself.  Reverting the synthesis in `@/opt/projects/personal/CPAP_data_uploader/include/web_ui.h::renderBe` left a much simpler function: plain `(done + live.up/live.total) / total`.

### 9.5 Code delta summary

| File | Lines changed | Notes |
|---|---|---|
| `@/opt/projects/personal/CPAP_data_uploader/include/UploadStateManager.h` | +11 | Two new in-memory fields, three new public methods. |
| `@/opt/projects/personal/CPAP_data_uploader/src/UploadStateManager.cpp` | +14 | Field init + getter/setter bodies. |
| `@/opt/projects/personal/CPAP_data_uploader/include/FileUploader.h` | +10 / −3 | Extended `WorkProbeResult` with `universe`, `cloudSynced`, `smbSynced`. |
| `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp` | ~+130 / −115 | Rewrote `hasWorkToUpload()` as single-pass two-backend probe.  Docstring rewritten. |
| `@/opt/projects/personal/CPAP_data_uploader/src/CpapWebServer.cpp` | +17 / −12 | `readBackendCounts` lambda prefers probe snapshot; legacy math is fallback. |
| `@/opt/projects/personal/CPAP_data_uploader/include/web_ui.h` | +9 / −15 | Removed `-1 done` synthesis, added "No data on card yet". |

Flash usage: 88.8% → 88.9% (`pico32-ota`, esp32doit-devkit-v1).  RAM `.bss`: +8 B per state manager instance (two `int` fields).

### 9.6 Follow-up bug: phantom "1 left" on empty stub folders

First post-deploy test surfaced:

```
[WorkProbe] Result: cloud=0 smb=0 | universe=12 cloudSynced=11 smbSynced=11
```

— yet the UI rendered `11 / 12 · 1 left` for both backends, contradicting `hasWork=false`.

**Root cause.** The universe filter was too permissive: *every* in-window DATALOG subdirectory was counted, including empty stubs with no `.edf` content that no backend had ever uploaded. Such a folder hit the incomplete-but-empty branch of `evaluateBackend`, which neither flagged it as work nor counted it as synced, leaving `total − done = 1` with nothing actually behind it.

This is common in practice — the CPAP pre-creates `DATALOG/YYYYMMDD` for tonight's session slightly before any file lands in it.

**Fix.** Added an explicit universe filter before `universe++`:

```cpp
if (!anyCompleted && !checkEdf()) continue;
```

A folder only counts toward the shared denominator when it is either (a) completed for at least one configured backend, or (b) currently contains at least one `.edf` file. Empty-never-uploaded stubs are invisible until they grow content.

Side-effects of the refactor:

- `isFolderCompleted` is now looked up once per folder per backend (memory-only, no SD cost), not once inside each `evaluateBackend` call.
- `folderHasEdf` is wrapped in a lazy `checkEdf` closure so it runs **at most once per folder** regardless of how many backends consult it — a small additional SD-hold-time win over the §9 baseline when both backends are configured.

**Semantics preserved:** the "incomplete + old + outside upload window" case still counts in the universe and surfaces as `1 left`, because in that scenario the folder *does* contain real data and the `1 left` message is correct (it just can't be uploaded this session).

### 9.7 Follow-up bug: stale snapshot after successful upload

Second post-deploy test surfaced:

- Pre-upload: `12 / 12 ✓` for both backends.
- New CPAP activity: 2 new `.edf` files land in today's folder.
- Force Upload runs. During the session: `11 / 12 · 1 left` (correct — the folder has changed content).
- Session completes successfully, files uploaded, folder marked completed.
- **UI remains stuck at `11 / 12 · 1 left`** indefinitely. Only the next full upload session (triggered later) corrects it back to `12 / 12`.

**Root cause.** `FileUploader::hasWorkToUpload()` is the sole producer of the `(universe, synced)` snapshot, and it only runs **once per session** at the pre-upload work-probe step (`@/opt/projects/personal/CPAP_data_uploader/src/main.cpp:1193`). Between the pre-upload probe and the end of the session, `markFolderCompleted()` is called multiple times but **does not touch the probe snapshot** — it's a derived-cache that nothing refreshes at session end. The stale cache survives until the next pre-upload probe overwrites it.

**Fix.** Add a post-session probe refresh in the upload task, immediately after `runFullSession()` returns and before releasing the SD card. The SD is still mounted, so no additional exclusive-access cost; the probe itself is ~100–200 ms on a typical card.

```cpp
// src/main.cpp — upload task, step 4b
if ((result == UploadResult::COMPLETE || result == UploadResult::NOTHING_TO_DO)
    && params->sdManager->hasControl()) {
    params->uploader->hasWorkToUpload(params->sdManager->getFS());
    esp_task_wdt_reset();
    g_uploadHeartbeat = millis();
}
```

Conditional on `COMPLETE` / `NOTHING_TO_DO` because:

- On `ERROR`, the state is already partially volatile (folder marked completed but import didn't finalize, etc.). A fresh probe would lock in potentially inconsistent numbers. Wait for the next session.
- On `TIMEOUT`, the exclusive-access window expired mid-upload. Leave the pre-session snapshot until the next cycle — the user knows a timeout happened and will trigger another session.

**Why not refresh the snapshot incrementally** (i.e. bump `probeSynced` inside `markFolderCompleted`)? Considered and rejected: `markFolderCompleted` doesn't know whether the folder counted toward `universe` (it might be an empty stub; see §9.6), doesn't know whether the folder had changed `.edf` content that the probe had excluded, and would need a parallel decrement path for `removeFolderFromCompleted`. The probe already encapsulates all that logic correctly — running it once more at session end is cheaper to write and easier to trust than mirroring its rules in three places.

**Result.** After session completion, the UI reflects the fresh post-upload counts in the next `/api/status` poll — typically within 1 second of `RELEASING → COOLDOWN`.

### 9.8 Future work

If users report the one-off "left side drops by 1 unexpectedly" case (Case A from §8.5 — a genuinely net-new folder appearing), we can promote Option 2 from §8.5: add a `live.refresh` hint to `/api/status` so the client can disambiguate.  Not needed for v4.1-beta1 because the state manager's `hasFileChanged` path naturally handles the dominant case.
