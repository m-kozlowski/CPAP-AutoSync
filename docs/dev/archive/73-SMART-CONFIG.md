# 73 — Smart Mode Configuration Validation & UI

## 1. Problem Statement

The three Smart mode time parameters must maintain a strict ordering:

```
SMART_START_HOUR < UPLOAD_START_HOUR < UPLOAD_END_HOUR
```

| Parameter | Meaning | Example |
|---|---|---|
| `SMART_START_HOUR` | Earliest wake-up — Smart monitoring resumes | 6:00 |
| `UPLOAD_START_HOUR` | Latest wake-up — full upload window opens | 9:00 |
| `UPLOAD_END_HOUR` | Earliest sleep — uploads stop, quiet period begins | 23:00 |

The **quiet period** spans from `UPLOAD_END_HOUR` → `SMART_START_HOUR` (wraps midnight). This is when the CPAP is in use and no uploads are attempted.

### Invalid Configuration

A misconfiguration occurs when `SMART_START_HOUR` falls **inside** the upload window:

```
UPLOAD_START_HOUR < SMART_START_HOUR < UPLOAD_END_HOUR
```

Example from the bug report (screenshot): `SMART_START_HOUR=9`, `UPLOAD_START_HOUR=8`, `UPLOAD_END_HOUR=23` → **invalid** (9 is between 8 and 23).

This happens when the user manually edits `config.txt` without understanding the constraint, or when a previously-valid config becomes invalid after editing one value.

---

## 2. Three-Part Solution

### Part A — Server-Side: Auto-Downgrade to SCHEDULED

**Where:** `Config::validate()` (already exists) or at the end of `Config::load()`.

**Logic:**

```
if mode == SMART and SMART_START_HOUR is in range (UPLOAD_START_HOUR, UPLOAD_END_HOUR):
    set an internal flag: smartConfigInvalid = true
    the mode effectively behaves as SCHEDULED
    (SMART_START_HOUR is ignored entirely)
```

Expose on `/api/status`:
```json
"smart_config_invalid": true
```

**Why downgrade rather than clamp?**
Clamping silently changes a value the user set, which is surprising and hard to debug. Downgrading to SCHEDULED is transparent, safe (the CPAP never loses SD card access), and the dashboard clearly communicates what happened and why.

**Implementation checklist:**
- [ ] Add `bool smartConfigInvalid` field to `Config`
- [ ] Validate in `Config::validate()` — set flag when `UPLOAD_START_HOUR < SMART_START_HOUR < UPLOAD_END_HOUR`
- [ ] In all mode-check callsites, treat mode as SCHEDULED when `smartConfigInvalid` is true — the cleanest place is `Config::isSmartMode()` which would return `false` when `smartConfigInvalid`
- [ ] Add `smart_config_invalid` to `/api/status` JSON
- [ ] Log a clear warning at boot: `[Config] SMART_START_HOUR (%d) is inside upload window [%d, %d] — downgrading to SCHEDULED`

---

### Part B — Dashboard: Indicator Badge

Mirrors the existing PCNT-unavailable indicator pattern (already in `web_ui.h`).

**Upload mode field** (currently shows `SMART`):

When `smart_config_invalid` is true, append an `ⓘ` badge next to the mode label:

```
SCHEDULED ⓘ
```

The badge has a tooltip on hover: `"SMART_START_HOUR is inside the upload window — downgraded to SCHEDULED. Fix in Config."`.

**Mode explanation panel** (the dynamic text block under the upload engine fields):

Add a new branch in `generateModeExplanation()`:

```
⚠ Scheduled mode — Smart config invalid
SMART_START_HOUR (X:00) falls inside the upload window (A:00–B:00).
Smart mode requires: SMART_START_HOUR ≤ UPLOAD_START_HOUR.
Fix this in Config, then reboot to re-enable Smart mode.
[Fix in Config →]   [Force Upload]
```

This mirrors the PCNT downgrade UX: terse badge on the stat row + clear explanation in the help panel below.

---

### Part C — Setup Page: Timeline Slider UI

Replace the three separate number-input fields with a **single stacked-slider timeline** that makes the constraint visually obvious and impossible to violate.

#### Layout

```
┌──────────────────────────────────────────────────┐
│  UPLOAD SCHEDULE                                  │
│                                                   │
│  ┌── Timeline ─────────────────────────────────┐ │
│  │▓▓▓▓▓▓░░░░░░░░░▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│ │
│  │ Quiet │ Monitor│      Upload Window    │Quiet│ │
│  └─────────────────────────────────────────────┘ │
│    0h                                        24h  │
│                                                   │
│  Earliest wake-up (Smart mode starts)    06:00    │
│  [────────O────────────────────────────────────]  │
│                                                   │
│  Latest wake-up (Full uploads start)     09:00    │
│  [───────────O──────────────────────────────────] │
│                                                   │
│  Earliest sleep (Uploads stop)           23:00    │
│  [───────────────────────────────────────────O──] │
│                                                   │
└──────────────────────────────────────────────────┘
```

#### Why this approach

- **Zero external dependencies** — native `<input type="range">`, CSS `linear-gradient`, ~50 lines of vanilla JS
- **Mobile-friendly** — three vertical tracks give a large touch target on narrow screens; the OS handles drag physics
- **Constraint enforcement** — JS clamps adjacent sliders when they would cross, so illegal combinations are impossible
- **Immediate visual feedback** — the timeline bar color zones update live as sliders move

#### Color zones on the timeline bar

| Zone | Color | Meaning |
|---|---|---|
| 0 → SMART_START_HOUR | `#1e293b` (dark) | Quiet period — CPAP in use, no monitoring |
| SMART_START_HOUR → UPLOAD_START_HOUR | `#38bdf8` (cyan) | Smart monitoring only (fresh data) |
| UPLOAD_START_HOUR → UPLOAD_END_HOUR | `#10b981` (green) | Full upload window (all data) |
| UPLOAD_END_HOUR → 24 | `#1e293b` (dark) | Quiet period — CPAP in use, no monitoring |

#### Slider labels (user-facing language)

| Slider | Label | Tooltip |
|---|---|---|
| SMART_START_HOUR | **Earliest wake-up** | "Smart mode starts monitoring from this hour. Set this to the earliest you could possibly be awake." |
| UPLOAD_START_HOUR | **Latest wake-up** | "Full backlog uploads start from this hour. By this time, you'll definitely be awake." |
| UPLOAD_END_HOUR | **Earliest sleep** | "Uploads stop at this hour. Set this to the earliest you might go to bed." |

This framing makes the semantics clear without exposing internal parameter names to the user.

#### Constraint enforcement in JS

All three sliders enforce a **mandatory 1-hour minimum gap** between adjacent values.

```
on slider 1 (SMART_START_HOUR) change:
    clamp val to max = UPLOAD_START_HOUR - 1
    update timeline

on slider 2 (UPLOAD_START_HOUR) change:
    if val <= SMART_START_HOUR + 1: push SMART_START_HOUR down to val - 1
    if val >= UPLOAD_END_HOUR - 1: clamp val to UPLOAD_END_HOUR - 1
    update timeline

on slider 3 (UPLOAD_END_HOUR) change:
    if val <= UPLOAD_START_HOUR + 1: push UPLOAD_START_HOUR down (cascade to SMART_START_HOUR)
    update timeline
```

The cascade ensures that dragging Slider 3 leftward automatically moves Sliders 1 and 2 leftward too. The invariant `SSH + 1 ≤ USH` and `USH + 1 ≤ UEH` is physically impossible to violate through the UI.

#### In SCHEDULED mode

Sliders 1 (SMART_START_HOUR) is hidden. Only Sliders 2 and 3 are shown (Start and End of upload window). The timeline bar shows only two zones: dark and green.

---

## 3. Resolved Decisions

1. **Equal hours edge case** — **INVALID.** `SMART_START_HOUR` must be strictly less than `UPLOAD_START_HOUR`. Equal values are not permitted. The slider enforces a 1-hour minimum gap, and the server-side validator treats equality as misconfiguration.

2. **Scheduled mode and the new slider UI** — **Agreed.** The timeline slider replaces the current number inputs for both Smart and Scheduled modes. In Scheduled mode, Slider 1 (SMART_START_HOUR) is hidden; only Sliders 2 and 3 are shown.

3. **Slider min gap** — **1 hour mandatory.** Sliders may never be closer than 1 hour apart. This is enforced in JS (cascade/clamp) and separately validated server-side.

4. **Config tab raw editor** — **Unchanged.** The raw `config.txt` textarea on the Advanced tab is preserved as-is. Manual edits are the user's responsibility; the server-side downgrade is the safety net.

---

## 4. Implementation Order

1. **Server-side downgrade** (C++) — Config validation flag + isSmartMode() check (safest, deploy first)
2. **Dashboard badge** (web_ui.h) — show downgrade reason when `smart_config_invalid`
3. **Setup page timeline slider** (setup.html) — replace three number inputs

Steps 1 and 2 together cover the manual-edit-config.txt attack surface. Step 3 prevents the problem at the UI level going forward.
