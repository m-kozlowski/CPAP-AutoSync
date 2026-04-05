# 67 — Implementation Plan: Earliest PCNT, Conditional Stealth, AS10 Periodic Checks

**Status**: IMPLEMENTED (v3.5i-beta1)  
**Prerequisite**: Docs 60–66, GitHub issues #53, #54, #55  
**Goal**: Fix all three open beta bugs and implement AS10 stealth card restoration

---

## Summary

Five coordinated changes, ordered by priority:

| # | Change | Fixes | Risk |
|---|--------|-------|------|
| 1 | Earliest possible PCNT enablement | #53 (AS11 detection) | Low |
| 2 | Unify EarlyPCNT + TrafficMonitor into single PCNT unit | Simplicity, #53 | Low |
| 3 | Conditional stealth: stealth for AS10, regular SD init for AS11 | #54 (SD Card Error) | Low |
| 4 | AS10 periodic re-checks in scheduled mode | #55 (stuck LISTENING) | Low |
| 5 | Stealth card-state restoration before AS10 MUX handoff (`STEALTH_RESTORE`) | Prevent AS10 power-cycle | Medium |

---

## 1. Earliest Possible PCNT Enablement

### Problem

`EarlyPCNT::init()` currently runs at `main.cpp:368`, after BT memory release, `Serial.begin()`, TLS arena init, and GPIO setup. On AS11, the CPAP's SD card handshake (CMD0/ACMD41/etc.) generates DAT3 pulses in the first few hundred milliseconds after power-on. If the ESP32 is still in framework init during this window, those pulses are lost and `finalPulses == 0`, causing the firmware to write `pcnt_capable = false` to NVS — permanently disabling smart mode until the next power-on boot.

Contributing factors documented in doc 66:
- **Blank/simple cards**: Handshake completes in milliseconds, not seconds.
- **CPAP menu interaction**: AS11 delays SD mount if user is in Settings menu.
- **Floating MUX pin**: During first ~400ms of boot, `SD_SWITCH_PIN` is High-Z.

### Solution: `__attribute__((constructor))` Pre-Main Init

Move two critical operations into a GCC constructor that runs before `app_main()`:

```cpp
// In main.cpp, at file scope (before setup())
__attribute__((constructor(101)))
static void preinitPcntAndMux() {
    // 1. Lock MUX to CPAP immediately — eliminates floating-pin jitter
    gpio_reset_pin((gpio_num_t)SD_SWITCH_PIN);
    gpio_set_direction((gpio_num_t)SD_SWITCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);

    // 2. Release any stale RTC hold on CS_SENSE (legacy ULP residue)
    rtc_gpio_hold_dis((gpio_num_t)CS_SENSE);
    rtc_gpio_deinit((gpio_num_t)CS_SENSE);

    // 3. Start PCNT immediately — capture the very first CPAP bus pulses
    EarlyPCNT::init(CS_SENSE);
}
```

**Why constructor priority 101?** Priorities 0–100 are reserved for system/compiler use. Priority 101 runs before any C++ static constructors and before `app_main()` → `setup()`. This gains ~500ms of additional capture time compared to the current position in `setup()`.

**Removes from setup():** The following lines become redundant and should be removed:
- `rtc_gpio_hold_dis()` / `rtc_gpio_deinit()` (moved to constructor)
- `pinMode(CS_SENSE, INPUT)` (PCNT driver configures the pin)
- `pinMode(SD_SWITCH_PIN, OUTPUT)` / `digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE)` (moved to constructor)
- `EarlyPCNT::init(CS_SENSE)` (moved to constructor)

### NVS Persistence: Unconditional Write on Power-On

The NVS persistence logic is straightforward:

```
ON POWER-ON BOOT:
  Write capable = (pulses > 0) unconditionally
  (earliest PCNT should reliably capture AS11 handshake)

ON NON-POWER-ON BOOT (soft-reboot, brownout, etc.):
  Read from NVS (unchanged)
```

**Rationale**: Users may switch the same SD card between AS10 and AS11 machines (e.g., in separate rooms). A power-on boot is required when moving the card, so writing the detection result unconditionally ensures the NVS always reflects the *current* machine. The earliest PCNT enablement (constructor-level) should reliably capture AS11 handshake pulses, making sticky/promotion logic unnecessary.

---

## 2. Unify EarlyPCNT + TrafficMonitor into a Single PCNT Unit

### Problem

Currently two independent PCNT units exist on the same GPIO:
- **EarlyPCNT** (unit A): created at boot, torn down at `main.cpp:589`
- **TrafficMonitor** (unit B): created at `main.cpp:484`, lives throughout runtime

This wastes a PCNT hardware unit (ESP32 has 8) and creates a **gap**: between EarlyPCNT teardown and TrafficMonitor's first `update()` call, any CPAP pulses could be missed. More importantly, during stealth mode operations (which involve `sdmmc_host_init()` and GPIO reconfigurations), the TrafficMonitor's PCNT unit may be disturbed — this is the likely root cause of issue #53.

### Solution: Single Unit, Handed Off

Instead of two units, create **one** PCNT unit at the earliest possible moment (in the constructor from §1) and hand it to TrafficMonitor later.

**Phase A — Boot (constructor):** `EarlyPCNT::init()` creates the PCNT unit and starts counting. No change to its API.

**Phase B — TrafficMonitor adoption:** Add a new method `TrafficMonitor::adoptUnit(pcnt_unit_handle_t, pcnt_channel_handle_t)` that takes ownership of the pre-existing unit instead of creating a new one in `begin()`.

```cpp
// New method in TrafficMonitor
void TrafficMonitor::adoptUnit(pcnt_unit_handle_t unit, pcnt_channel_handle_t chan) {
    _pcntUnit = unit;
    _pcntChannel = chan;
    _initialized = true;
    // Don't clear count — preserve accumulated pulses for the first idle calculation
    _lastSampleTime = millis();
    _lastSecondTime = millis();
}
```

**Phase C — Handoff in setup():**

```cpp
// Instead of:
//   trafficMonitor.begin(CS_SENSE);    // creates new unit
//   ...
//   EarlyPCNT::teardown();             // destroys old unit

// Do:
trafficMonitor.adoptUnit(EarlyPCNT::detach());  // transfers ownership
// EarlyPCNT no longer owns the unit — no teardown needed
```

`EarlyPCNT::detach()` returns the handle pair and sets its internal pointers to null (preventing double-free).

**Power management**: TrafficMonitor's `suspend()` / `resume()` continue to work exactly as before — they disable/enable the same unit. The only change is *who created it*.

**Key benefit**: The PCNT counter is never destroyed and recreated. There is zero gap in pulse counting from the moment of `__attribute__((constructor))` through the entire runtime. The stealth mode GPIO reconfigurations cannot interfere with the PCNT unit because PCNT uses the GPIO matrix (not direct IO), and `gpio_reset_pin()` in StealthConfigReader only affects the digital IO mux — the PCNT peripheral's connection to the physical pin is managed by the PCNT driver and survives GPIO reconfigurations.

**Verification needed**: Confirm that `sdmmc_host_init_slot()` (called by StealthConfigReader) does not reconfigure GPIO 33 (CS_SENSE). Looking at the slot config in `scrInitHardware()`, the pins configured are SD_CMD(15), SD_CLK(14), SD_D0(2), SD_D1(4), SD_D2(12), SD_D3(13). **GPIO 33 (CS_SENSE) is NOT in this list** — it is a separate sense pin on the CPAP side of the MUX. Therefore stealth mode operations cannot interfere with PCNT counting on GPIO 33.

---

## 3. Conditional Stealth: AS10 Gets Stealth, AS11 Gets Regular SD Init

### Problem (Issue #54)

Stealth mode leaves the SD card in an unclean state on AS11, causing "SD Card Error" if therapy starts before the first upload. The stealth read sequence:

1. Grabs MUX
2. CMD13(0x1388) — card is in **Transfer state (4)** on AS11
3. ACMD6(0) — forces card to **1-bit mode**
4. Reads config.txt sectors
5. Cleanup: since `cardState != 3` (it was 4), does **NOT** deselect
6. Does **NOT** restore 4-bit mode
7. Tri-states pins, deinits SDMMC, releases MUX

The AS11 CPAP expects the card in **4-bit Transfer state**. It gets back a card in **1-bit Transfer state** — an unexpected configuration that triggers the SD Card Error.

On AS10: the card starts in **Standby state (3)**, we select it, read, deselect back to Standby. AS10 uses 1-bit mode natively, so ACMD6(0) is effectively a no-op. No problem.

### Solution: Use PCNT Decision to Gate Config Read Strategy

After §1 and §2, the PCNT capability flag is reliable and available before the config read. The boot flow becomes:

```
PCNT DECISION (already computed)
  │
  ├─ pcntCapable == true (AS11)
  │   └─ Regular SD init: sdManager.takeControl() → config.loadFromSD() → sdManager.releaseControl()
  │      AS11 handles this gracefully — it re-inits the card when MUX returns
  │
  └─ pcntCapable == false (AS10)
      └─ Stealth config read: StealthConfigReader::readConfigTxt()
         AS10 never notices — card state is perfectly preserved
```

**Implementation in main.cpp** (replacing the current unconditional stealth block):

```cpp
bool stealthConfigOK = false;

if (g_pcntCapable) {
    // AS11: regular SD init is safe — AS11 gracefully re-inits the card
    LOG_INFO("[Config] AS11 detected — using regular SD init for config read");
    if (sdManager.takeControl()) {
        stealthConfigOK = config.loadFromSD(sdManager.getFS());
        sdManager.releaseControl();
        if (stealthConfigOK) {
            LOG_INFO("[Config] Config loaded via regular SD init");
        }
    }
} else {
    // AS10: stealth mode required — AS10 power-cycles ESP on regular init
    LOG_INFO("[Config] AS10 detected — using stealth config read");
    String rawConfig = StealthConfigReader::readConfigTxt();
    if (!rawConfig.isEmpty()) {
        stealthConfigOK = config.loadFromString(rawConfig);
    }
}

// Fallback for either path
if (!stealthConfigOK) {
    LOG_WARN("[Config] Primary config read failed — falling back to regular SD mount");
    // ... existing fallback code ...
}
```

**Edge case — first-ever boot (no NVS value):** On the very first power-on with a blank NVS, `g_pcntCapable` defaults to `false` (from `pcntPrefs.getBool("capable", false)`). This means the first boot always uses stealth mode, which is safe for both AS10 and AS11 (AS11 will show the SD Card Error only if therapy starts before the first upload — unlikely on first-ever boot). On the second power-on boot, NVS will have the correct value.

**Mitigation for first-boot edge case:** If earliest PCNT (§1) captures pulses on the first boot, `g_pcntCapable` will be `true` and the regular path is used. The only scenario where first-boot stealth affects AS11 is if the CPAP's handshake completes before even the constructor-level PCNT can capture it — extremely unlikely with the constructor approach.

---

## 4. AS10 Periodic Re-Checks in Scheduled Mode (Issue #55)

### Problem

After all folders are uploaded, `g_noWorkSuppressed = true`. In `handleListening()`, suppression is cleared only when `trafficMonitor.isBusy() || trafficMonitor.hasActivityLatch()` — i.e., PCNT detects new bus activity. On AS10, PCNT never fires (DAT3 is unused in 1-bit mode), so suppression is never cleared. If new therapy data is written mid-day, it won't be uploaded until the next power-on or manual Force Upload.

From issue #55 comment by @ilyakruchinin:
> *"IF, for AS10, we modify 'scheduled mode' to NOT rely on bus activity detection and do the checks periodically, that would simply mean more checks, but I don't anticipate any issues."*

### Solution: Timer-Based Fallback for Non-PCNT-Capable Devices

Add a periodic suppression clear in `handleListening()` for AS10 only:

```cpp
// In handleListening(), inside the g_noWorkSuppressed block:
if (g_noWorkSuppressed) {
    // ... existing PCNT-based clearing (for AS11) ...

    // AS10 fallback: no PCNT activity possible, so clear suppression on a timer
    if (!g_pcntCapable) {
        static unsigned long lastPeriodicClearMs = 0;
        unsigned long periodicIntervalMs = (unsigned long)config.getInactivitySeconds() * 1000UL;
        if (millis() - lastPeriodicClearMs >= periodicIntervalMs) {
            lastPeriodicClearMs = millis();
            g_noWorkSuppressed = false;
            LOG("[FSM] AS10 periodic check — clearing no-work suppression (no PCNT available)");
        }
    }

    // ... rest of existing logic ...
}
```

**Why use `INACTIVITY_SECONDS` as the interval?** It's the natural "silence" threshold already configured by the user. On AS10, there's no PCNT to detect silence, so we use the same interval as a timer-based proxy. Default is 62 seconds — reasonable for periodic re-checks.

**Impact**: On AS10 in scheduled mode, after all work is complete, the FSM will re-attempt an upload cycle every ~62 seconds (or whatever `INACTIVITY_SECONDS` is set to). If there's truly nothing new, the upload task returns `NOTHING_TO_DO` quickly (a few seconds for SD mount + scan), and suppression is re-enabled. The overhead is minimal.

**"Semi-smart" mode for AS10**: As noted in the issue comment, this naturally enables a "semi-smart" behavior for AS10 users who set `UPLOAD_START_HOUR=0` / `UPLOAD_END_HOUR=0`. The periodic re-checks will discover new therapy data within `INACTIVITY_SECONDS` after the CPAP finishes writing, without requiring PCNT.

**AS11 is unaffected**: The `!g_pcntCapable` guard ensures this code path is never taken on AS11, which continues to use the existing PCNT-based activity detection.

---

## 5. Stealth Card-State Restoration Before AS10 MUX Handoff

### Problem

After a regular upload cycle on AS10:
1. `SD_MMC.begin()` sends CMD0 (via init clocks) → resets card → re-initializes with CMD2/CMD3/CMD7
2. Upload runs normally
3. `SD_MMC.end()` unmounts FATFS and calls `sdmmc_host_deinit()` — this deinits the peripheral but does **NOT** send CMD0
4. `releaseControl()` tri-states pins, sets pull-ups, releases MUX
5. AS10 resumes — finds card in a state it doesn't expect → power-cycles the ESP

The power-cycle is the fundamental reason AS10 needed stealth mode for config reading. But it also means after every upload, the AS10 reboots the ESP. Currently this is handled by the firmware soft-rebooting itself first (or `MINIMIZE_REBOOTS` entering cooldown and accepting the next power-cycle).

### Feasibility Analysis

After `SD_MMC.end()`, the SDMMC peripheral is deinitialized but the card itself should still retain its last state. The card was initialized by `SD_MMC.begin()` with RCA 0x1388 (deterministic), and `SD_MMC.end()` does not send card-level commands — it only unmounts the VFS and deinits the host.

**Theory**: If we re-init the SDMMC host in stealth mode (no CMD0) after `SD_MMC.end()`, the card should still be accessible at RCA 0x1388. We can then restore it to the exact state the CPAP expects:

```
After SD_MMC.end():
  1. scrInitHardware()          — stealth init (no CMD0, no init clocks)
  2. CMD13(0x1388)              — verify card is alive, check state
  3. ACMD6(0)                   — ensure 1-bit mode (AS10 native)
  4. CMD7(0)                    — deselect card → Standby state (AS10 expects this)
  5. scrDeinitHardware()        — deinit SDMMC peripheral
  6. Tri-state pins + pull-ups  — bus lines high (SD idle convention)
  7. Release MUX                — AS10 finds card exactly as it left it
```

### Risk Assessment

- **Medium risk**: `SD_MMC.end()` internal behavior could vary between ESP-IDF versions. If it ever starts sending CMD0 on unmount, this approach breaks silently. Need to verify with current pioarduino/ESP-IDF 5.5.2.
- **Testing required**: Must verify on real AS10 hardware that the card retains RCA 0x1388 after `SD_MMC.end()`.
- **Partial benefit**: Even if this works, the AS10 CPAP may have other reasons to power-cycle (it periodically resets the SD card every ~60s during therapy). This fix would only help for the upload-handoff scenario.

### Implementation (v3.5i-beta1)

Implemented in `SDCardManager::releaseControl()` behind a config guard. The new `STEALTH_RESTORE` config key (default: `true`) controls whether the restore runs. Only active on AS10 (`!g_pcntCapable`).

```cpp
// In SDCardManager::releaseControl(), after SD_MMC.end():
if (!g_pcntCapable && config.getStealthRestore()) {
    StealthConfigReader::restoreCardState();
}
```

`StealthConfigReader::restoreCardState()` implements the sequence from the feasibility analysis:
1. `scrInitHardware()` — stealth SDMMC init (no CMD0)
2. `CMD13(0x1388)` — verify card alive, get state
3. `ACMD6(0)` — force 1-bit mode if in Transfer state
4. `CMD7(0)` — deselect → Standby
5. Verify final state via CMD13
6. `scrDeinitHardware()` — deinit SDMMC peripheral

The caller (`releaseControl`) then proceeds with the existing tri-state + MUX release.

---

## Proposed Boot Flow (After All Changes)

```
┌──────────────────────────────────────────────────────────────┐
│ __attribute__((constructor(101)))                              │
│   1. Lock MUX to CPAP (GPIO 26 → HIGH)                       │
│   2. Release RTC hold on CS_SENSE (GPIO 33)                   │
│   3. EarlyPCNT::init(CS_SENSE) — counting starts NOW          │
├──────────────────────────────────────────────────────────────┤
│ setup()                                                       │
│   4. Serial, BT release, TLS arena, logging                  │
│   5. sdManager.begin()                                        │
│   6. trafficMonitor.adoptUnit(EarlyPCNT::detach())            │
│      └─ Single PCNT unit, no gap, no teardown needed          │
│   7. 8s electrical stabilization (cold boot) or skip (fast)   │
│   8. Smart Wait (5s bus silence via trafficMonitor)            │
│                                                               │
│   9. PCNT DECISION (read trafficMonitor accumulated count):    │
│      ├─ Power-on: write capable=(pulses>0) unconditionally    │
│      └─ Non-power-on: read from NVS                           │
│                                                               │
│  10. CONFIG READ (conditional on PCNT decision):               │
│      ├─ g_pcntCapable (AS11): regular SD init                 │
│      │   sdManager.takeControl() → loadFromSD() → release     │
│      └─ !g_pcntCapable (AS10): stealth config read            │
│          StealthConfigReader::readConfigTxt()                  │
│                                                               │
│  11. PCNT gating: if !capable && smart → force scheduled      │
│  12. WiFi, NTP, web server, etc.                              │
│  13. FSM starts (LISTENING or IDLE)                           │
├──────────────────────────────────────────────────────────────┤
│ FSM Runtime                                                   │
│  LISTENING:                                                   │
│   ├─ AS11: PCNT-based idle detection (unchanged)              │
│   └─ AS10: periodic re-check every INACTIVITY_SECONDS         │
│  UPLOADING → RELEASING:                                       │
│   ├─ AS11: SD_MMC.end() + release (AS11 re-inits gracefully)  │
│   └─ AS10: SD_MMC.end() + stealth restore + release            │
└──────────────────────────────────────────────────────────────┘
```

---

## Implementation Order

All five changes implemented in v3.5i-beta1:

1. **§1 + §2**: Earliest PCNT + unified unit. Fixes #53.
2. **§3**: Conditional stealth (AS10=stealth, AS11=regular SD). Fixes #54.
3. **§4**: AS10 periodic re-checks. Fixes #55.
4. **§5**: Stealth card restoration (`STEALTH_RESTORE=true` default). AS10 optimization.

---

## Testing Plan

### §1 + §2: Earliest PCNT
- **AS11**: Power-on with blank card → verify `finalPulses > 0` (was previously 0 in some cases)
- **AS11**: Power-on while in CPAP Settings menu → verify pulses still captured
- **AS10**: Power-on → verify `finalPulses == 0` (no false positives)
- **Both**: Soft-reboot → verify NVS value is read correctly

### §3: Conditional Stealth
- **AS11**: Power-on → verify regular SD init used, no "SD Card Error" on therapy start
- **AS10**: Power-on → verify stealth mode used, config loaded, no reboot loop
- **First boot (blank NVS)**: Both devices → verify fallback behavior is acceptable

### §4: AS10 Periodic Re-Checks
- **AS10**: Upload all folders → wait for suppression → verify re-check triggers after INACTIVITY_SECONDS
- **AS10**: Add new therapy data mid-day → verify it's discovered and uploaded within ~2 minutes
- **AS11**: Verify this code path is never taken (log check)

### §5: Stealth Card Restoration (future)
- **AS10**: Upload → verify card state restored → verify AS10 does NOT power-cycle ESP
- **AS10**: Multiple upload cycles with MINIMIZE_REBOOTS → verify no power-cycles between cycles
