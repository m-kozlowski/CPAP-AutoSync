# 65 — Implementation Plan: Stealth Boot + PCNT Gating + Cleanup

**Status**: PLAN — awaiting approval  
**Prerequisite**: Stealth mode proven on both AS10 and AS11 (dev+27, see doc 64)

---

## Summary

Four coordinated changes that simplify the boot architecture, remove AS10 workarounds, and unify the codebase:

1. **PCNT capability detection** → persist to NVS → gate "smart" upload mode
2. **Universal stealth config read** → replace cached-config workaround → remove AS10 reboot-loop mitigations
3. **Documentation updates** → specs, README, config reference
4. **Post-stealth flow unchanged** → bus silence → regular SD mount → upload

---

## New Boot Flow (Unified AS10 + AS11)

```
┌──────────────────────────────────────────────────────────┐
│ POWER ON                                                 │
├──────────────────────────────────────────────────────────┤
│ 1. EarlyPCNT::init()          ← start counting DAT3     │
│ 2. Boot housekeeping (LittleFS, TrafficMonitor, etc.)    │
│ 3. Electrical stabilization (8s on cold boot)            │
│ 4. Smart Wait (5s bus silence)                           │
│ 5. PCNT final read + teardown                            │
│                                                          │
│ 6. ┌─ PCNT DECISION ─────────────────────────────────┐   │
│    │ IF power-on boot:                                │   │
│    │   pulses > 0  → NVS: pcnt_capable = true         │   │
│    │   pulses == 0 → NVS: pcnt_capable = false        │   │
│    │ ELSE (soft-reboot, watchdog, brownout):           │   │
│    │   Read pcnt_capable from NVS                     │   │
│    └──────────────────────────────────────────────────┘   │
│                                                          │
│ 7. ┌─ STEALTH CONFIG READ ───────────────────────────┐   │
│    │ Grab MUX                                         │   │
│    │ initHardware(stealth=true)  ← no CMD0            │   │
│    │ Mask ISR (INTMASK=0)                             │   │
│    │ CMD13(0x1388) → check card alive                 │   │
│    │ IF alive:                                        │   │
│    │   DMA off, ACMD6(0) → 1-bit                     │   │
│    │   FAT32 read → full config.txt content           │   │
│    │   Parse into Config object                       │   │
│    │ Tri-state pins, deinitHardware                   │   │
│    │ Release MUX to CPAP (CPAP unaware)               │   │
│    └──────────────────────────────────────────────────┘   │
│                                                          │
│ 8. IF stealth config failed:                             │
│    │ Fall back: regular SD mount → loadFromSD()        │  │
│    │ (This sends CMD0 — acceptable as one-time event)  │  │
│                                                          │
│ 9. Apply PCNT gating:                                    │
│    │ IF !pcnt_capable AND upload_mode == "smart":      │  │
│    │   Override to "scheduled", log warning             │  │
│                                                          │
│ 10. Connect WiFi, NTP, web server, etc.                  │
│ 11. FSM: bus silence detection → regular SD mount →      │
│     upload workflow (unchanged)                          │
└──────────────────────────────────────────────────────────┘
```

---

## Requirement 1: PCNT Capability Detection

### What
Detect whether the CPAP uses 4-bit SD mode (AS11, DAT3 pulses visible) or 1-bit/SPI mode (AS10, no DAT3 activity). Persist this to NVS so non-power-on boots can use it. Use it to gate "smart" upload mode.

### Why
Smart mode relies on the TrafficMonitor (PCNT on DAT3) to detect when the CPAP finishes using the SD card. AS10 CPAPs use 1-bit or SPI mode — DAT3 is never toggled — so the TrafficMonitor never sees activity. Smart mode would either never trigger uploads (if it waits for activity-then-silence) or trigger them blindly.

### Implementation

#### A. NVS persistence (`main.cpp`)

After the final PCNT reading (step 5 in the flow above), before stealth config read:

```cpp
// Persist PCNT capability to NVS
bool pcntCapable = false;
{
    Preferences pcntPrefs;
    pcntPrefs.begin("pcnt_cap", false);
    if (resetReason == ESP_RST_POWERON) {
        // Power-on boot: PCNT had the full init window to count
        pcntCapable = (finalPulses > 0);
        pcntPrefs.putBool("capable", pcntCapable);
        LOG_INFOF("[PCNT] Power-on detection: %s (%d pulses)",
                  pcntCapable ? "CAPABLE (AS11)" : "NOT CAPABLE (AS10)", finalPulses);
    } else {
        // Non-power-on: read from NVS (PCNT missed the init window)
        pcntCapable = pcntPrefs.getBool("capable", false);
        LOG_INFOF("[PCNT] Using cached capability: %s", pcntCapable ? "CAPABLE" : "NOT CAPABLE");
    }
    pcntPrefs.end();
}
```

#### B. Upload mode gating (`main.cpp`, after config is loaded)

```cpp
// Gate smart mode on PCNT capability
if (!pcntCapable && config.isSmartMode()) {
    LOG_WARN("[PCNT] Smart mode not supported (no DAT3 activity detected) — forcing scheduled mode");
    config.overrideUploadMode("scheduled");
}
```

Requires adding `Config::overrideUploadMode(const String& mode)` — a simple setter.

#### C. Expose in `/api/status` JSON (`CpapWebServer.cpp`)

Add `pcnt_capable` boolean to the status snapshot:
```json
{ ..., "pcnt_capable": true, ... }
```

Store `pcntCapable` in a global (e.g., `g_pcntCapable`) accessible to the web server.

#### D. Web UI changes (`web_ui.h`)

In the dashboard's "Upload Engine" card, after the "Upload mode" row:
- If `pcnt_capable == false` AND config `upload_mode == "smart"`:
  - Show the mode as **"SCHEDULED"** with a small info icon/badge
  - Add a subtle note: _"Smart mode requires 4-bit SD bus activity (not available on this CPAP unit). Using scheduled mode instead."_
- Implementation: a small `⚠` icon next to the mode text, clickable to show a tooltip or inline text. Keep it minimal — not a modal, not a banner.

Example rendering:
```
Upload mode    SCHEDULED ⓘ
```
Where ⓘ expands to: _"Your CPAP uses 1-bit SD communication. Smart mode (which detects SD bus activity) is not available. Scheduled mode uploads at the configured window."_

#### E. Config page / config editor

When `pcnt_capable == false`, the config page should indicate that `UPLOAD_MODE=smart` will be overridden. No hard block on editing — just an informational note.

---

## Requirement 2: Universal Stealth Config Read + Remove Cached Config

### What
On every boot, read `config.txt` via stealth mode (no CMD0) before any regular SD access. Remove the AS10 cached-config workaround, the consecutive-POR tracking for reboot-loop detection, and the `AS10=true` config flag.

### Why
- Stealth mode is proven on both AS10 and AS11 (dev+27 logs confirm full success on both).
- The "deadly embrace" reboot loop was caused by the boot-time SD init sending CMD0, which disrupted the AS10's session. Stealth mode avoids CMD0 entirely.
- With stealth config read, the cached-config workaround is unnecessary — the ESP32 always gets config without disturbing the card.
- Unified code path: no AS10-specific branching needed.

### Implementation

#### A. New stealth config reader

Refactor the existing `BusWidthDetector::readConfigTxt()` into a production-ready function. Two options:

**Option 1 (preferred): Standalone `StealthConfigReader` module**
- New files: `src/StealthConfigReader.cpp`, `include/StealthConfigReader.h`
- Extracts the proven stealth sequence from `BusWidthDetector.cpp`:
  - `initHardware(stealth)`, ISR masking, CMD13, ACMD6, DMA disable
  - FAT32 reader (MBR → BPB → root dir → file read)
- Returns the **full raw content** of `config.txt` (not just WIFI_SSID)
- Must handle files > 512 bytes: follow FAT32 cluster chain (read consecutive sectors within the same cluster; for multi-cluster files, follow the FAT chain — config files are typically < 2KB so 1 cluster is sufficient on most cards)

Public API:
```cpp
namespace StealthConfigReader {
    // Reads config.txt from SD card WITHOUT sending CMD0.
    // Grabs MUX, does stealth init, reads FAT32, releases MUX.
    // Returns raw file content, or empty string on failure.
    String readConfigTxt();
}
```

**Option 2: Keep in BusWidthDetector, return full content via DetectionResult**
- Less clean separation, but fewer new files.

#### B. Extend FAT32 reader to return full file content

The current `readConfigTxt()` only reads the first 512-byte sector and only extracts `WIFI_SSID`. Extend it to:
1. Read up to 4 sectors (2KB) — enough for any reasonable `config.txt`
2. Follow cluster chain if file spans multiple clusters (unlikely but handle it)
3. Return the raw text as a `String`

#### C. Integration in `main.cpp`

Replace the current boot flow:

```cpp
// OLD (remove):
// if (resetReason == ESP_RST_POWERON && consecutivePOR >= 2) { ... cached boot ... }
// if (!usedCachedConfig) { ... Smart Wait ... takeControl() ... loadFromSD() ... }

// NEW:
// 1. Smart Wait (already present)
// 2. PCNT teardown + NVS persist (Requirement 1)
// 3. Stealth config read:
String rawConfig = StealthConfigReader::readConfigTxt();
bool stealthConfigOK = false;
if (!rawConfig.isEmpty()) {
    stealthConfigOK = config.loadFromString(rawConfig);
    if (stealthConfigOK) {
        LOG_INFO("[Stealth] Config loaded successfully via stealth read");
    }
}

// 4. Fallback: regular SD mount if stealth failed
if (!stealthConfigOK) {
    LOG_WARN("[Stealth] Stealth config read failed — falling back to regular SD mount");
    while (!sdManager.takeControl()) { delay(1000); }
    if (!config.loadFromSD(sdManager.getFS())) {
        // ... existing error handling ...
    }
    sdManager.releaseControl();
}

// 5. Apply PCNT gating (Requirement 1)
// 6. Continue with WiFi, NTP, etc.
```

#### D. Config parser update

Rename `Config::loadFromCachedString()` → `Config::loadFromString()` (it's no longer AS10-specific). Or keep both and deprecate the old one. The method itself is fine — it parses raw config text line by line and handles masked credentials.

#### E. Code to REMOVE

| Item | Location | What to remove |
|------|----------|---------------|
| `cacheConfigToNVS()` | `main.cpp` | Entire function |
| `loadCachedConfigFromNVS()` | `main.cpp` | Entire function |
| `usedCachedConfig` logic | `main.cpp` | The `if (consecutivePOR >= 2)` block |
| `consecutivePOR` tracking | `main.cpp` | The NVS `consec_por` increment/check (keep `total` boot counter) |
| `Config::loadFromCachedString()` | `Config.cpp` | Rename to `loadFromString()` or remove if merged |
| `Config::getAS10Mode()` | `Config.h/cpp` | Getter + `as10Mode` member + `parseLine` handling |
| `AS10=true` config key | `Config.cpp` | `parseLine()` case |
| `SD_CMD0_ON_RELEASE` config key | `Config.cpp` | `parseLine()` case + getter + member |
| `config.getSdCmd0OnRelease()` | `SDCardManager.cpp` | The CMD0-before-release block in `releaseControl()` |
| `cfg_cache` NVS namespace | Runtime | Will become orphaned (harmless, or erase on first boot) |
| `BusWidthDetector::stealthTest()` | `BusWidthDetector.cpp` | The experimental test function (replaced by production StealthConfigReader) |
| `BusWidthDetector::detect()` call | `main.cpp` | The experimental detect() call in boot flow |

#### F. Code to KEEP (from doc 59 improvements)

| Item | Location | Why keep |
|------|----------|---------|
| Bus pin pull-ups after SD_MMC.end() | `SDCardManager::releaseControl()` | Prevents floating lines during MUX handoff — universal benefit, does NOT disturb card |
| Drive strength restore after MUX switch | `SDCardManager::releaseControl()` | Prevents bus contention during handoff — universal benefit, invisible to CPAP |
| Reduced drive strength on takeControl() | `SDCardManager::takeControl()` | Reduces current spikes — universal benefit |

These improvements are read-path safe. They only affect the electrical characteristics of the handoff, not the SD protocol. The CPAP cannot detect them.

#### G. What about `ENABLE_1BIT_SD_MODE`?

Keep it. This controls the *upload-phase* SD mount mode (`SD_MMC.begin()` 1-bit vs 4-bit). It's independent of stealth mode. Some users may want 1-bit for compatibility.

---

## Requirement 3: Documentation Updates

### A. Update `docs/archive/64-STEALTH-MODE-RESULTS.md`

Add AS10 dev+27 results:
- AS10 cold boot (#1318): Phase 1 PASS (Stby state 3), Phase 1b PASS, Phase 1c PASS (WIFI_SSID='T4C')
- AS10 soft-reset (#1319): Phase 1 PASS (Tran state 4), all phases PASS

Update status from "PROVEN VIABLE on AS11 — AS10 testing pending" to **"PROVEN VIABLE on AS10 and AS11"**.

### B. Update `docs/CONFIG_REFERENCE.md`

- Remove `AS10=true` documentation
- Remove `SD_CMD0_ON_RELEASE` documentation
- Add note about `UPLOAD_MODE=smart` being auto-overridden to `scheduled` on AS10 CPAPs
- Document the PCNT-based detection

### C. Update `README.md`

Add a section on CPAP compatibility:
```
## CPAP Compatibility

| Feature | AirSense 10 (AS10) | AirSense 11 (AS11) |
|---------|--------------------|--------------------|
| Upload mode: Smart | ❌ Not supported* | ✅ Supported |
| Upload mode: Scheduled | ✅ Supported | ✅ Supported |
| Stealth config read | ✅ Works | ✅ Works |
| SD bus mode | 1-bit (no DAT3) | 4-bit (DAT3 active) |

*AS10 uses 1-bit SD communication. The ESP32 cannot detect CPAP SD bus
 activity on DAT3, which Smart mode requires for automatic upload triggering.
 If UPLOAD_MODE=smart is set, the firmware automatically falls back to
 scheduled mode on AS10 units.
```

### D. Update `docs/FEATURE_FLAGS.md`

Remove `AS10` and `SD_CMD0_ON_RELEASE` feature flags.

### E. Archive `docs/archive/60-CACHED-CONFIG.md`

Add a header note: _"SUPERSEDED by stealth config read (doc 65). Cached config and AS10 reboot-loop mitigation removed."_

---

## Requirement 4: Post-Stealth Flow (Unchanged)

After the stealth config read returns the MUX to the CPAP:

1. **Bus silence detection** — The TrafficMonitor + Smart Wait runs as normal. On AS10 it won't see activity (DAT3 is idle), so it passes through quickly. On AS11 it waits for CPAP to finish. **Same code path for both.**

2. **Regular SD mount** — When the FSM transitions to ACQUIRING, `sdManager.takeControl()` does a full `SD_MMC.begin()` (which sends CMD0 + full init). This is intentional and acceptable because:
   - The CPAP is confirmed idle (bus silence detected)
   - A single CMD0/reinit is fine — the CPAP handles it gracefully on next access
   - This is NOT a reboot loop — the loop was caused by boot-time CMD0 before the CPAP was ready

3. **Upload workflow** — Completely unchanged. Scan folders, upload files, release SD.

### Spec clarification needed

The existing FSM docs should note:
- Boot-time config read uses stealth mode (no CMD0, no card disturbance)
- Upload-time SD access uses regular mode (full init with CMD0)
- These are intentionally different because their timing contexts differ

---

## Implementation Order

| Step | Task | Risk | Depends on |
|------|------|------|------------|
| 1 | Create `StealthConfigReader` module (extract from BusWidthDetector) | Medium — core change | — |
| 2 | Extend FAT32 reader to return full file content (>512 bytes) | Low | Step 1 |
| 3 | Add `Config::loadFromString()` (rename from `loadFromCachedString`) | Low | — |
| 4 | Wire stealth config read into `main.cpp` boot flow | Medium | Steps 1-3 |
| 5 | Add PCNT NVS persistence | Low | — |
| 6 | Add upload mode gating (smart → scheduled on AS10) | Low | Step 5 |
| 7 | Add `pcnt_capable` to `/api/status` + web UI changes | Low | Step 6 |
| 8 | Remove cached config code (functions, NVS, AS10 flag, CMD0 toggle) | Medium — many touchpoints | Step 4 |
| 9 | Remove experimental `stealthTest()` / `detect()` code | Low | Step 1 |
| 10 | Documentation updates | Low | Steps 1-9 |
| 11 | Build + test on AS10 and AS11 | — | All |

Steps 1-4 are the critical path. Steps 5-7 are independent and can be done in parallel with step 4. Step 8 is cleanup after step 4 is confirmed working.

---

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| Stealth read fails on some card/CPAP combo | Fallback to regular SD mount (step 8 in flow diagram) |
| Config file > 2KB | Follow FAT cluster chain; cap at 4KB (more than enough) |
| RCA is not 0x1388 on some cards | CMD13 failure triggers fallback; log the error for diagnosis |
| Card not initialized by CPAP at boot | CMD13 returns RTO → fallback to regular mount |
| NVS `pcnt_capable` stale after CPAP swap | Next power-on boot refreshes it automatically |

---

## Files Affected (Summary)

**New:**
- `src/StealthConfigReader.cpp`
- `include/StealthConfigReader.h`

**Modified:**
- `src/main.cpp` — new boot flow, remove cached config, add PCNT NVS
- `src/Config.cpp` — rename loadFromCachedString, remove AS10/CMD0 keys
- `include/Config.h` — remove AS10 members, add overrideUploadMode
- `src/SDCardManager.cpp` — remove CMD0-on-release block
- `src/CpapWebServer.cpp` — add pcnt_capable to status JSON
- `include/web_ui.h` — smart mode unavailable indicator
- `docs/archive/64-STEALTH-MODE-RESULTS.md` — add AS10 results
- `docs/CONFIG_REFERENCE.md` — remove AS10/CMD0, add PCNT note
- `docs/README.md` — CPAP compatibility table
- `docs/FEATURE_FLAGS.md` — remove flags

**Removed (code):**
- `BusWidthDetector::stealthTest()` — experimental, replaced by StealthConfigReader
- `BusWidthDetector::detect()` call in main.cpp — experimental
- Optionally: entire `BusWidthDetector` class if no longer needed (EarlyPCNT stays)
