# 72 — Smart Mode Revamp: Quiet Hours & Error Handling

## 1. Problem Statement

### 1.1 Mid-Therapy Upload Collision

Users wake up in the middle of the night (e.g., to get water), pausing therapy.
The PCNT activity monitor detects the pause as "CPAP idle" and triggers an upload.
The user returns and resumes therapy while the upload is still running. The AS11
cannot write to the SD card (ESP holds the MUX) and enters an **unrecoverable
"SD Card Error" state**.

**Root cause:** Smart mode monitors bus activity 24/7. It has no concept of
"therapy hours" — any idle period triggers an upload attempt, including brief
pauses during the night.

### 1.2 Prolonged Upload Sessions on Backend Failure

When the SMB server is unavailable (e.g., user's laptop is asleep), the upload
session spends significant time on connection retries, WiFi recovery cycles,
and per-file retry attempts before finally giving up. During this entire period,
the ESP holds the SD card MUX.

**Root cause:** The retry logic is designed for daytime reliability (server
temporarily busy, flaky WiFi, etc.) and does not distinguish between "scheduled
hours where retrying is worth the risk" and "overnight where minimizing SD hold
time is critical."

Combined, these two issues mean that overnight Smart mode uploads are both
**unnecessary** (users don't need their data uploaded at 3am) and **dangerous**
(prolonged SD hold time + mid-therapy resume = SD Card Error).

---

## 2. Proposed Fix 1: Smart Mode Quiet Hours

### 2.1 Concept

Introduce a configurable hour that defines when Smart mode should transition
from IDLE to LISTENING. Between `UPLOAD_END_HOUR` and this new hour, Smart mode
behaves exactly like Scheduled mode: it stays in IDLE, does not monitor bus
activity, and does not attempt uploads. This gives users a safe window to
pause/resume therapy without risk of upload collision.

**Timeline example** (UPLOAD_START_HOUR=9, UPLOAD_END_HOUR=21, new_param=6):

```
 21:00 ──────────────────── 06:00 ──────── 09:00 ──────────────── 21:00
   │                          │               │                     │
   │  SMART QUIET (IDLE)      │  SMART ACTIVE │  SMART ACTIVE       │
   │  No upload attempts      │  Fresh only   │  Fresh + Old data   │
   │  Therapy-safe            │  (outside     │  (inside upload     │
   │                          │   upload      │   window)           │
   │                          │   window)     │                     │
```

### 2.2 Parameter Naming

The new parameter must coexist with `UPLOAD_START_HOUR` and `UPLOAD_END_HOUR`
in the config file and web UI. Key naming criteria:

- Clearly related to Smart mode (not applicable to Scheduled mode)
- Self-explanatory without reading documentation
- Fits the `*_HOUR` naming convention

| Candidate | Pros | Cons |
|-----------|------|------|
| `SMART_START_HOUR` | Parallels `UPLOAD_START_HOUR`; short | "Start" of what? Could be confused with upload window |
| `SMART_START_HOUR` | Clear: "the hour smart mode becomes active" | Slightly longer |
| `SMART_EARLIEST_HOUR` | Very explicit | Verbose; "earliest" implies "could be later" |
| `SMART_QUIET_UNTIL` | Describes the quiet period directly | Breaks `*_HOUR` convention |
| `THERAPY_END_HOUR` | User-friendly concept | Too assumption-laden; therapy may not end at a fixed time |
| `NO_UPLOAD_BEFORE_HOUR` | Most explicit | Verbose; negative phrasing |

**Selected: `SMART_START_HOUR`**

Rationale:
- Directly parallels `UPLOAD_START_HOUR` / `UPLOAD_END_HOUR`
- Self-documenting in the config file:
  ```
  UPLOAD_MODE=smart
  UPLOAD_START_HOUR=9
  UPLOAD_END_HOUR=21
  SMART_START_HOUR=6
  ```
  Reading this, it's intuitive: "Smart mode starts at 6am, upload window is 9am-9pm."

### 2.3 Semantics

| Config State | Meaning |
|-------------|---------|
| `SMART_START_HOUR` not set (default: 6) | Smart mode is IDLE from `UPLOAD_END_HOUR` until 6am; then LISTENING |
| `SMART_START_HOUR = 0` | Smart mode is IDLE from `UPLOAD_END_HOUR` until midnight; then LISTENING |
| `SMART_START_HOUR = 6` | Smart mode is IDLE from `UPLOAD_END_HOUR` until 6am; then LISTENING |
| `SMART_START_HOUR = UPLOAD_END_HOUR` | Smart mode is never quiet (equivalent to 24/7) |
| Scheduled mode | Parameter is ignored entirely |

**Default value: `6`** (6am). `0` means midnight, NOT disabled. The parameter
is always applied in Smart mode. To disable the quiet period entirely, set
`SMART_START_HOUR` equal to `UPLOAD_END_HOUR`.

### 2.4 The Quiet Period

The quiet period is the interval from `UPLOAD_END_HOUR` to `SMART_START_HOUR`.
During this period:

- FSM state: **IDLE** (same as Scheduled mode outside its window)
- PCNT / TrafficMonitor: **suspended** (saves power, prevents false triggers)
- No SD card access, no upload attempts
- Users can freely pause/resume therapy

**Cross-midnight handling:** The quiet period always crosses midnight (e.g.,
21:00 → 06:00). This is the natural use case — therapy is overnight.

If `SMART_START_HOUR >= UPLOAD_END_HOUR` (e.g., both set to 21), there is no
quiet period and Smart mode is effectively 24/7 active.

If `SMART_START_HOUR > UPLOAD_START_HOUR` (e.g., active at 10, window at 9),
the quiet period extends past the upload window opening. Smart mode will not
start LISTENING until 10am even though old data could be uploaded from 9am. This
is a valid user choice ("I sometimes sleep in until 10am").

### 2.5 Transition: IDLE → LISTENING at SMART_START_HOUR

When the clock crosses `SMART_START_HOUR`:

1. **Reset idle tracking** — call `trafficMonitor.resetIdleTracking()`
2. **Resume PCNT** — `trafficMonitor.resume()` (if suspended during IDLE)
3. **Transition to LISTENING**
4. **Require full inactivity period** before triggering upload

**Why reset idle tracking?** If the bus was quiet for hours during the quiet
period (therapy ended at 5am, SMART_START_HOUR=6), we do NOT want to
immediately trigger an upload at 6:00:00. The inactivity period (default 62s)
acts as a safety buffer — if the user is still in bed and happens to resume
therapy in those 62 seconds, the upload won't start. The cost of waiting 62s
is negligible.

**Why NOT carry over pre-6am bus activity?** PCNT is suspended during the quiet
period (IDLE state), so there is no meaningful activity data to carry over.
Starting fresh is both simpler and safer.

### 2.6 Interaction with Existing Schedule Logic

| Time of Day | `canUploadFreshData()` | `canUploadOldData()` | FSM State |
|-------------|----------------------|---------------------|-----------|
| Quiet period (21:00-06:00) | **false** (new) | false | IDLE |
| Smart active, outside window (06:00-09:00) | true | false | LISTENING |
| Smart active, inside window (09:00-21:00) | true | true | LISTENING |

The key change: `canUploadFreshData()` currently returns `true` unconditionally
in Smart mode. With the quiet period, it must return `false` during quiet hours.

### 2.7 Edge Cases

**User wakes up at 5am (before SMART_START_HOUR=6):**
- Smart mode is IDLE. No upload attempt. Safe.
- At 6am, transition to LISTENING. If bus is now quiet (user left for work), upload triggers after 62s.

**User is still asleep at 6am:**
- Transition to LISTENING. Bus is active (therapy running). No upload triggered.
- User stops therapy at 7am. 62s of silence → upload triggers. Correct.

**Force Upload button pressed during quiet period:**
- Should still work — manual trigger overrides quiet hours (same as Scheduled mode behavior where Force Upload works outside the window, limited to recent data).

**Cooldown expires during quiet period:**
- Currently, `handleCooldown()` transitions Smart mode to LISTENING unconditionally.
- **Must change:** If in quiet period, transition to IDLE instead.

**Upload completes at 20:59, cooldown ends at 21:09 (past UPLOAD_END_HOUR):**
- Cooldown expires → check if in quiet period → yes → transition to IDLE.

---

## 3. Proposed Fix 2: Reduced Retries Outside Scheduled Hours

### 3.1 Current Retry Behavior

**SMB (per-file level):**
- `SMB_UPLOAD_MAX_ATTEMPTS = 2` — each file gets 2 attempts
- On transport error: disconnect → reconnect SMB (150ms delay)
- On reconnect failure: WiFi recovery cycle (disconnect + reconnect WiFi, 45s cooldown, then up to 3 SMB reconnect attempts with escalating delays)
- Worst case per file: ~60-90 seconds of retry activity

**SMB (session level):**
- Iterates all fresh folders, then old folders
- If one folder fails (`sessionHadFailure = true`), continues to next folder
- Session returns `TIMEOUT` (not `ERROR`)
- No early abort on first failure

**Cloud:**
- `begin()` fails → `cloudImportFailed = true` → entire cloud phase skipped
- Per-file: no retry mechanism (single attempt)
- TLS connect failure: non-fatal during pre-warm, but fatal during `begin()`
- Session continues to SMB phase even if Cloud fails

**FSM level:**
- `ERROR` → RELEASING → COOLDOWN → LISTENING (no suppression)
- `TIMEOUT` → RELEASING → COOLDOWN → LISTENING (no suppression, but `uploadCycleHadTimeout = true`)
- Cooldown: configurable, default 10 minutes

### 3.2 The Problem: Unnecessary SD Hold Time

When the SMB server is unreachable at 3am (laptop asleep):

```
t=0s      Bus silence detected → ACQUIRING → UPLOADING
t=1s      SD card mounted, work probe finds fresh folders
t=2s      Cloud phase: begin() fails (or skipped) — fast
t=3s      SMB phase: connect attempt 1...
t=18s     SMB connect timeout (15s) — FAIL
t=18.5s   SMB reconnect attempt... 
t=33s     SMB reconnect timeout — FAIL
t=34s     WiFi recovery: disconnect WiFi...
t=79s     WiFi recovery: cooldown (45s)...
t=80s     WiFi reconnect...
t=82s     SMB reconnect attempt 1/3...
t=97s     SMB reconnect timeout — FAIL
t=98s     SMB reconnect attempt 2/3...
t=113s    SMB reconnect timeout — FAIL
t=114s    SMB reconnect attempt 3/3...
t=129s    SMB reconnect timeout — FAIL
t=130s    File 1 upload fails. Move to file 2...
          (repeat entire cycle for each file in each folder)
```

**Total SD hold time for a single failed session: 2-5+ minutes**, depending on
number of files. During this entire time, the CPAP cannot access the SD card.

### 3.3 Proposed Solution: Context-Aware Retry Policy

Introduce a boolean flag `outsideScheduledHours` that modifies the retry behavior:

**Outside scheduled hours (night):**
- SMB: **1 attempt per file** (no retry). On first connect failure, **abort the
  entire session immediately** — don't try remaining files.
- Cloud: Same as current (already single-attempt). On `begin()` failure, skip.
- WiFi recovery: **Skip entirely**. If the server is unreachable, it's almost
  certainly because the server is offline (laptop asleep), not because WiFi is
  flaky. WiFi recovery adds 45+ seconds of SD hold time for no benefit.
- **Session-level early abort:** On first backend failure (SMB connect fails OR
  first file upload fails), return `UploadResult::ERROR` immediately. Don't
  iterate remaining folders.

**Inside scheduled hours (day):**
- Current behavior unchanged (2 attempts, WiFi recovery, continue on failure).

**Result:** Failed overnight session completes in **~18 seconds** (one SMB
connect timeout) instead of 2-5+ minutes.

### 3.4 Determining "Outside Scheduled Hours"

Use the existing `ScheduleManager::isInUploadWindow()`:
- Inside window → full retry policy
- Outside window → reduced retry policy

This works for both Smart and Scheduled modes:
- Smart mode outside window: reduced retries (the target scenario)
- Scheduled mode outside window: shouldn't be uploading anyway (only via Force Upload, which is user-initiated and should also use reduced retries to minimize risk)
- Both modes inside window: full retries

### 3.5 Implementation Approach

**Option A: Pass a flag through the call chain**

Add a `bool reducedRetries` parameter to `FileUploader::runFullSession()`, which
propagates to the SMB/Cloud upload methods. This is explicit and testable.

**Option B: Query ScheduleManager at decision points**

Each retry decision point calls `scheduleManager->isInUploadWindow()` directly.
Less parameter threading but couples retry logic to schedule logic.

**Recommendation: Option A.** The flag is computed once at session start and
passed through. Clearer, no repeated time checks, and allows Force Upload
to explicitly set the flag.

### 3.6 State Transition After Failed Session

**Current behavior on ERROR/TIMEOUT:**
```
UPLOADING → RELEASING → COOLDOWN → LISTENING
```

This is already correct. On ERROR/TIMEOUT:
- `g_noWorkSuppressed` is NOT set (only set on COMPLETE/NOTHING_TO_DO)
- FSM goes to COOLDOWN (default 10 min) → LISTENING
- Idle tracking is reset → requires fresh inactivity period
- If bus is active (therapy resumed), upload won't trigger

**No change needed to FSM transitions.** The existing flow is safe:
1. Upload fails fast (new: reduced retries)
2. SD card released (RELEASING)
3. Wait in COOLDOWN (10 min default — gives user time to settle back into therapy)
4. Resume LISTENING (check for bus silence before next attempt)
5. If therapy is ongoing → bus is active → no upload triggered

**However**, with the quiet hours feature (Fix 1), we need one addition:

If cooldown expires and we are in the **quiet period**, transition to **IDLE**
instead of LISTENING. This prevents repeated failed attempts throughout the
night (e.g., failing at 3am, cooldown to 3:10am, try again at 3:11am, fail
again, etc.)

### 3.7 Should We Reset g_noWorkSuppressed on Error?

**No.** `g_noWorkSuppressed` is only set on COMPLETE and NOTHING_TO_DO — it means
"we confirmed there is nothing new to upload." An ERROR means "we don't know if
there's work because the upload failed." The correct behavior is to NOT suppress
and allow the next cycle to try again, which is what already happens.

The user's concern about "resetting the flag so it doesn't wait for more PCNT
pulses" is already handled: on ERROR/TIMEOUT, `g_noWorkSuppressed` is **never
set**, so the FSM naturally returns to LISTENING without waiting for new PCNT
activity. The only gate is the inactivity check (62s of bus silence), which is
the correct safety mechanism.

---

## 4. Summary of Changes

### 4.1 New Config Parameter

| Key | Default | Range | Description |
|-----|---------|-------|-------------|
| `SMART_START_HOUR` | `6` | 0–23 | Hour (local time) when Smart mode begins activity monitoring. `0` = midnight. Smart mode stays IDLE from `UPLOAD_END_HOUR` until this hour, preventing uploads during therapy. Set equal to `UPLOAD_END_HOUR` to disable quiet period (24/7 active). Only applies to Smart mode; ignored in Scheduled mode. |

### 4.2 Config File Example

```ini
UPLOAD_MODE=smart
UPLOAD_START_HOUR=9
UPLOAD_END_HOUR=21
SMART_START_HOUR=6
INACTIVITY_SECONDS=62
COOLDOWN_MINUTES=10
```

Reading: "Smart mode, uploads during 9am-9pm (all data) and 6am-9am (fresh only).
Between 9pm and 6am, no upload attempts (safe for therapy)."

### 4.3 CONFIG_REFERENCE.md Entry

```markdown
| `SMART_START_HOUR` | `6` | The hour (0–23, local time) when Smart mode 
begins monitoring for CPAP inactivity. Set this to the earliest time you 
typically wake up. Between `UPLOAD_END_HOUR` and this hour, Smart mode stays 
idle — no upload attempts, safe for therapy interruptions (getting water, 
bathroom breaks, etc.). `0` = midnight (not disabled). Set equal to 
`UPLOAD_END_HOUR` to disable the quiet period. Only applies when 
`UPLOAD_MODE=smart`; ignored in scheduled mode. |
```

---

## 5. Implementation Plan

### Phase 1: Smart Mode Quiet Hours

**Files to modify:**

1. **`include/Config.h`** — Add `smartStartHour` field (int, default 6)
2. **`src/Config.cpp`** — Parse `SMART_START_HOUR`, validate 0–23, add getter
3. **`src/ScheduleManager.h`** — Add `isSmartQuietPeriod()` method
4. **`src/ScheduleManager.cpp`**:
   - Add `isSmartQuietPeriod()`: returns true if Smart mode AND current hour is
     in the quiet window (between `UPLOAD_END_HOUR` and `SMART_START_HOUR`)
   - Modify `canUploadFreshData()`: return false during quiet period
5. **`src/main.cpp`**:
   - `handleListening()`: If Smart mode and in quiet period → transition to IDLE
   - `handleCooldown()`: If Smart mode and in quiet period → transition to IDLE
     instead of LISTENING
   - `handleIdle()`: Add edge-trigger for SMART_START_HOUR crossing (similar to
     existing upload window edge-trigger) — transition IDLE → LISTENING when the
     quiet period ends
6. **`src/CpapWebServer.cpp`** — Expose `smart_start_hour` in `/api/config`
7. **`src/web/setup.html`** — Add `SMART_START_HOUR` field in Upload Schedule
   section (visible only when Smart mode is selected)
8. **`docs/CONFIG_REFERENCE.md`** — Document the new parameter

**Validation rules:**
- Range: 0–23 (clamp with warning, same as other `*_HOUR` params)
- `0` = midnight (NOT disabled)
- If `SMART_START_HOUR == UPLOAD_END_HOUR` → effectively disabled (no quiet period)

### Phase 2: Reduced Retries Outside Scheduled Hours

**Files to modify:**

1. **`include/FileUploader.h`** — Add `bool reducedRetries` parameter to
   `runFullSession()` signature (or store as session-level field)
2. **`src/FileUploader.cpp`**:
   - Accept `reducedRetries` flag in `runFullSession()`
   - **SMB session:** On first SMB connect failure when `reducedRetries=true`,
     skip remaining SMB folders and mark session failed
   - **SMB per-file:** When `reducedRetries=true`, set effective max attempts to 1
   - **SMB reconnect:** When `reducedRetries=true`, skip WiFi recovery cycle
   - **Cloud session:** No change needed (already single-attempt)
   - On any backend connection failure when `reducedRetries=true`, stop the session
     early (don't iterate remaining folders)
3. **`src/SMBUploader.cpp`** — No change needed if retry logic is controlled at
   FileUploader level (don't call `upload()` for remaining files after first failure).
   Alternatively, add a `maxAttempts` parameter to `upload()` to override
   `SMB_UPLOAD_MAX_ATTEMPTS`.
4. **`src/main.cpp`**:
   - In `handleUploading()` where `UploadTaskParams` is constructed: set
     `reducedRetries = !scheduleManager->isInUploadWindow()`
   - For Force Upload outside window: also set `reducedRetries = true`

### Phase 3: Web UI Integration

- Show `SMART_START_HOUR` in the Setup Wizard (conditionally visible when
  Smart mode is selected)
- Show quiet period in the dashboard status explanation (the mode help text)
- Consider showing "Quiet until 6:00 AM" in the FSM state display when in
  quiet-period IDLE

---

## 6. Testing Strategy

### 6.1 Smart Mode Quiet Hours

| Test | Steps | Expected Result |
|------|-------|-----------------|
| Quiet period → IDLE | Set SMART_START_HOUR=6, UPLOAD_END_HOUR=21. At 22:00, verify FSM is IDLE | FSM stays IDLE, no upload attempts |
| SMART_START_HOUR crossing | Set SMART_START_HOUR=6. Wait for 06:00 | FSM transitions IDLE → LISTENING, log shows "Smart mode active — transitioning to LISTENING" |
| Therapy during quiet | Generate PCNT activity during quiet period | No FSM transition, activity ignored |
| Bus silence after active hour | Stop CPAP activity after SMART_START_HOUR. Wait 62s | Upload triggers normally |
| Cooldown into quiet period | Trigger upload at 20:55. Upload completes. Cooldown expires at 21:05 | FSM goes to IDLE (not LISTENING) |
| Force Upload during quiet | Press Force Upload at 3am | Upload runs (recent data only), same as scheduled mode behavior |
| SMART_START_HOUR=UPLOAD_END_HOUR | Set SMART_START_HOUR=21 (same as UPLOAD_END_HOUR) | Smart mode behaves 24/7 (no quiet period) |

### 6.2 Reduced Retries

| Test | Steps | Expected Result |
|------|-------|-----------------|
| SMB fail outside window | Disable SMB server. Trigger upload outside window | Session fails in ~18s (one connect timeout), SD released |
| SMB fail inside window | Disable SMB server. Trigger upload inside window | Session retries with WiFi recovery (current behavior) |
| Partial success | First folder uploads, SMB drops mid-session (outside window) | Session aborts after first failure, partial progress saved |
| Cloud fail + SMB ok | Cloud auth fails, SMB works (outside window) | Cloud skipped, SMB completes normally |

---

## 7. Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| Breaking existing Smart mode behavior | Medium | Default SMART_START_HOUR=6 introduces quiet period for existing users; document in release notes |
| Quiet period too long (user misses upload window) | Low | User controls the parameter; set equal to UPLOAD_END_HOUR to disable |
| Reduced retries → missed uploads at night | Low | Data is not lost; next cycle will pick it up after cooldown |
| Clock/NTP drift causing wrong quiet period boundary | Low | Same risk as existing UPLOAD_START_HOUR/UPLOAD_END_HOUR; tolerable at hour granularity |
| Force Upload during quiet period doesn't work | Medium | Must explicitly allow Force Upload to override quiet period (same pattern as Scheduled mode) |
