# 71 — Rapid SD Yield: Research & Implementation Plan

## 1. Problem Statement

When the ESP32 holds the SD bus for uploading (MUX pin GPIO26 = LOW), the CPAP machine (AirSense 11 specifically) may attempt to access the card at any time — periodic EDF flushes, RTC updates, post-therapy writes. Without yielding, the MUX silently blocks the CPAP. The AS11 reacts to this by entering an **unrecoverable "SD Card Error" state**, permanently abandoning SD card usage for the remainder of that session.

**Goal:** Detect the CPAP's *intention* to write and return the MUX back to the CPAP as fast as possible — ideally within the SD host command timeout window (estimated 100–500 ms).

Reference: [PR #45](https://github.com/amanuense/CPAP_data_uploader/pull/45)

---

## 2. Hardware Context

### 2.1 MUX & Signal Routing

```
                   ┌────────────┐
 CPAP SD Host ───► │  CS_SENSE  │◄── GPIO33 (upstream of MUX, CPAP side)
                   │            │
                   │   MUX      │──── SD Card
                   │  (GPIO26)  │
 ESP32 SDMMC  ───►│            │
                   └────────────┘
```

- **GPIO26** (`SD_SWITCH_PIN`): MUX control. LOW = ESP owns bus, HIGH = CPAP owns bus.
- **GPIO33** (`CS_SENSE`): Tapped on the **CPAP side** of the MUX, upstream of the switch. When ESP owns the bus, this pin only sees CPAP-originated activity. ESP's own SD traffic goes through the ESP side of the MUX and does NOT appear on CS_SENSE.
- A **FALLING edge** on GPIO33 while ESP owns the bus = CPAP is actively attempting to assert chip-select and being silently blocked.

### 2.2 Current CS_SENSE Usage

CS_SENSE is used by `TrafficMonitor` via the PCNT (Pulse Counter) peripheral:
- PCNT counts edges on GPIO33 at hardware speed (no CPU overhead)
- `TrafficMonitor::update()` samples the counter every 100 ms
- Used for idle detection in LISTENING state (>62 s of silence → trigger upload)
- **PCNT update is NOT called during UPLOADING state** (only LISTENING, COOLDOWN, MONITORING)
- PCNT unit remains technically running during upload but is not being read

**Key insight:** PCNT hardware counting and GPIO edge interrupts can coexist on the same pin. The PCNT peripheral uses its own edge-detection logic (APB clock domain), while GPIO ISR uses the GPIO interrupt matrix. No conflict.

### 2.3 False Trigger Analysis — REVISED (Apr 2026)

**Original assumption (WRONG):**
> ESP SD traffic does NOT appear on CS_SENSE. Zero false-trigger risk.

**Actual behavior (confirmed by testing):**

The MUX is a bus switch with per-channel parasitic capacitance. When ESP drives
DAT3 (GPIO13) at MHz speeds in **4-bit mode**, the signal couples through the
MUX's DAT3 channel capacitance to the CPAP side — where GPIO33 is tapped.

| Source | Signal on GPIO33 | Detectable? |
|--------|------------------|:-----------:|
| ESP 4-bit DAT3 activity | Coupling through MUX | **Yes — FALSE POSITIVE** |
| CPAP blocked access (CMD+CLK only) | Nothing (DAT3 idle in cmd phase) | **No — UNDETECTABLE** |
| CPAP active data transfer (has bus) | Real DAT3 toggles at MHz | Yes (PCNT uses this) |

The original PR assumed SPI-mode semantics where CS = DAT3 is asserted for every
transaction. But **both AS11 and ESP use 4-bit SD mode**, where DAT3 is a data
line, NOT chip-select. During blocked access attempts the CPAP only drives CMD
and CLK — DAT3 remains idle.

**Root cause of ALL false-positive/missed-detection issues:**
1. False positives: ESP's own DAT3 couples to GPIO33 in 4-bit mode
2. Missed detection: CPAP does not drive DAT3 during blocked command phase

### 2.4 The 1-Bit Mode Solution

**Key insight** ([Issue #50](https://github.com/ilyakruchinin/CPAP-AutoSync/issues/50)):
If ESP mounts the SD card in **1-bit mode** during upload:

- ESP only uses DAT0, CLK, CMD — **DAT3 is not driven** by the SDMMC host
- DAT3 (GPIO13 on ESP side, GPIO33 on CPAP side) sits at logic HIGH via weak
  pull-up (internal WPU + external 10kΩ per SD spec)
- **No coupling** from ESP activity on DAT3 → no false positives
- CPAP continues to use 4-bit mode (the mode the card was in before ESP took
  over), so CPAP **WILL drive DAT3** during its access attempts
- CPAP's DAT3 assertion easily overcomes the weak pull-up → **clean falling edge**

**Confirmed by Espressif documentation and ESP-IDF source:**

| Question | Answer |
|----------|--------|
| Does ESP32 SDMMC host drive DAT3 in 1-bit mode? | ❌ No — host releases the line entirely |
| What holds DAT3 high in 1-bit mode? | Weak internal pull-up (WPU) + external 10kΩ |
| Can CPAP pull DAT3 low against that? | ✅ Yes — driven output overcomes weak pull-up |
| Will GPIO33 falling edge ISR detect CPAP activity? | ✅ Yes — clean, unambiguous signal |
| Risk of bus contention? | ❌ None — pull-up vs driven output |

**Trade-off:** 1-bit mode has ~25% lower SD read throughput than 4-bit, but the
upload bottleneck is network I/O (WiFi → SMB/Cloud), so the impact on overall
upload speed is negligible.

**Verification note:** GPIO33 is not the standard Slot 1 DAT3 pin (GPIO13) — it
is a separate tap on the CPAP side of the MUX routed through the GPIO matrix.
When `SD_MMC.begin()` is called with `mode1bit=true`, the SDMMC host does not
configure or drive GPIO13 (DAT3). GPIO33 remains a passive input with pull-up,
ready for ISR edge detection.

---

## 3. Current Release Flow & Timing

The existing `SDCardManager::releaseControl()` sequence:

| Step | Operation | Estimated Time |
|------|-----------|----------------|
| 1 | `SD_MMC.end()` — unmount VFS, deinit SDMMC driver | 10–50 ms |
| 2 | Stealth restore: `scrInitHardware()` | ~260 ms (includes `sdmmc_host_init` + slot config + delays) |
| 3 | Stealth restore: CMD13 + ACMD6 + CMD7 | ~5 ms |
| 4 | Stealth restore: `scrDeinitHardware()` | ~1 ms |
| 5 | Bus lines → pull-up (INPUT mode) | <1 ms |
| 6 | `setControlPin(false)` + 300 ms settle delay | **300 ms** |
| **Total** | | **~620–680 ms** |

**This is far too slow.** The CPAP's SD host timeout is estimated at 100–500 ms. By the time the normal release completes, the CPAP may have already declared the card missing.

---

## 4. Proposed Detection Mechanism

### 4.1 ISR on GPIO33 (FALLING Edge)

**Prerequisite:** ESP must mount the SD card in **1-bit mode** during upload
(see Section 2.4). In 1-bit mode, ESP does not drive DAT3, so GPIO33 is quiet
and only shows genuine CPAP activity. No software filtering or debouncing needed.

```cpp
volatile bool g_cpapYieldRequest = false;

void IRAM_ATTR cpapYieldISR() {
    g_cpapYieldRequest = true;
}
```

**Arming/disarming:**
- **Arm** immediately after `sdManager.takeControl()` succeeds:
  ```cpp
  g_cpapYieldRequest = false;
  attachInterrupt(digitalPinToInterrupt(CS_SENSE), cpapYieldISR, FALLING);
  ```
- **Disarm** before `releaseControl()`:
  ```cpp
  detachInterrupt(digitalPinToInterrupt(CS_SENSE));
  g_cpapYieldRequest = false;
  ```

### 4.2 PCNT Coexistence

PCNT continues running in hardware during upload. It just isn't being sampled by `TrafficMonitor::update()`. After the yield and return to LISTENING, PCNT resumes normal operation. No special handling needed — the hardware counter doesn't interfere with the GPIO ISR matrix.

---

## 5. MUX Return Strategy (Issue 1)

### 5.1 Option 1b: Immediate MUX Flip (No Restore) — RECOMMENDED FOR INITIAL TESTING

The absolute fastest yield — **<1 ms**:

```
ISR fires
  └─► g_cpapYieldRequest = true
Upload task (next chunk boundary, ~10-50ms later):
  └─► Close any open File objects
  └─► gpio_set_level(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE)  // instant MUX flip
  └─► Return YIELD_NEEDED
```

**Card state when CPAP receives it:**
- Card is in **Transfer state** (state 4) — was actively doing ESP's I/O
- Bus width: **4-bit** (ESP default on AS11)
- RCA: **0x1388** (card assigns this itself; same across ESP and CPAP init)
- The SDMMC host driver on the ESP side is still technically initialized (SD_MMC.end() hasn't been called yet)

**What the CPAP sees:**
- Card responds to commands but is at a different protocol state than expected
- CPAP may need to send CMD0 to reset the card, or CMD12 to abort a pending transfer
- If AS11 has robust error recovery (sends CMD0 on any error), the card will reset and CPAP can re-initialize normally

**Risk:** If AS11 does NOT send CMD0 and simply throws "SD Card Error", this option fails. But it's the fastest option and worth testing first.

**Enhancement — ISR-level MUX flip (even faster, ~0 ms):**

For absolute minimum latency, the ISR itself could flip the MUX:

```cpp
void IRAM_ATTR cpapYieldISR() {
    // Direct register write — ISR-safe, ~100ns
    GPIO.out_w1ts = (1 << SD_SWITCH_PIN);  // GPIO26 HIGH = CPAP
    g_cpapYieldRequest = true;
}
```

This makes the MUX flip happen in **<1 µs** after CPAP's CS assertion. However:
- The ESP's SDMMC driver is mid-transaction; the bus disappears from under it
- SD_MMC file reads will return errors
- Need to handle the resulting cascading failures gracefully

**Recommendation:** Start testing with the ISR-level MUX flip. The cascading errors are manageable (see Section 6).

### 5.2 Option 1a: Fast Restore Then Flip (~5-15 ms) — FALLBACK IF 1b FAILS

If AS11 cannot recover from receiving the card in Transfer state, we can do a **minimal bare-metal restore** before flipping the MUX:

```
Upload task (next chunk boundary):
  └─► SDMMC.intmask.val = 0           // Mask ESP-IDF SDMMC ISR (~0 µs)
  └─► scrSendCmd(55, ...)  + CMD6(0)  // ACMD6(0): force 1-bit mode (~2 ms)
  └─► scrSendCmd7(0)                  // CMD7(0): deselect → Standby (~2 ms)
  └─► Bus pins → INPUT + PULLUP       // Idle bus convention (~0 µs)
  └─► GPIO26 → HIGH                   // MUX flip to CPAP (~0 µs)
  └─► Return YIELD_NEEDED
```

**Key optimization:** Skip `SD_MMC.end()` and `scrInitHardware()`. Instead:
- The SDMMC peripheral is ALREADY initialized (ESP was using it for I/O)
- Just mask the ISR to prevent ESP-IDF's driver from interfering
- Send CMD7(0) + ACMD6(0) directly via bare-metal register pokes
- These bare-metal commands work because we're bypassing the driver (same technique as stealth mode)

**Total time:** ~5–15 ms. Well within the 100–500 ms CPAP timeout.

**Card state when CPAP receives it:**
- Card is in **Standby state** (state 3) — deselected via CMD7(0)
- Bus width: **1-bit** — forced via ACMD6(0)
- This is the state the CPAP typically expects (same as normal `restoreToSavedState()`)

**Risk:** The ~5-15 ms delay (plus up to ~50 ms for the upload task to notice the flag) gives a total of ~20-65 ms. Still well within the timeout window.

### 5.3 Timing Budget Analysis

```
CPAP asserts CS (FALLING edge on GPIO33)
  t=0 µs    ISR fires, sets g_cpapYieldRequest = true
  
  [Option 1b - ISR MUX flip]
  t=0.1 µs  MUX flipped in ISR (register write)
  t=done    CPAP has bus immediately
  
  [Option 1b - task-level MUX flip]  
  t=0-50 ms Upload task reaches next chunk boundary check
  t+0.1 ms  Close open File objects
  t+0.2 ms  MUX flipped via GPIO
  t=done    CPAP has bus (total: 0-50 ms)
  
  [Option 1a - fast restore then flip]
  t=0-50 ms Upload task reaches next chunk boundary
  t+2 ms    ACMD6(0) sent
  t+4 ms    CMD7(0) sent
  t+4.1 ms  Bus pullups set
  t+4.2 ms  MUX flipped
  t=done    CPAP has bus (total: 4-55 ms)
```

All options are well within the 100–500 ms estimated CPAP timeout.

### 5.4 Decision Matrix

| Option | Yield Time | Card State for CPAP | Risk | Complexity |
|--------|-----------|---------------------|------|-----------|
| 1b (ISR flip) | <1 µs | Transfer/4-bit (needs CMD0) | AS11 may not send CMD0 | Low |
| 1b (task flip) | 0–50 ms | Transfer/4-bit (needs CMD0) | Same | Low |
| 1a (fast restore) | 4–55 ms | Standby/1-bit (ideal) | Bare-metal CMD conflict | Medium |

**Recommendation:** Test in this order: 1b (ISR flip) → 1b (task flip) → 1a (fast restore).

---

## 6. ESP Cleanup After Yield (Issue 2)

Once the MUX is flipped back to CPAP, the ESP is in a messy state. The SD card bus has been pulled out from under the SDMMC driver. We need to clean up without leaking resources, corrupting memory, or leaving zombie connections.

### 6.1 State Inventory During Upload

| Resource | Owner | State During Upload | Cleanup Required |
|----------|-------|---------------------|------------------|
| SD_MMC VFS mount | ESP-IDF | Mounted at `/sdcard` | `SD_MMC.end()` |
| SDMMC peripheral | ESP-IDF driver | Active, configured | Deinit via `SD_MMC.end()` |
| Open `File` objects | Upload code | 0-1 open for reading | `.close()` (will fail gracefully) |
| SMB context (`smb2`) | libsmb2 | Connected, possibly mid-write | `smb2_disconnect_share` + `smb2_destroy_context` |
| SMB upload buffer | heap | 4-8 KB allocated | `freeBuffer()` |
| TLS connection | WiFiClientSecure | Connected, possibly mid-write | `resetTLS()` / `tlsClient->stop()` |
| Upload task | FreeRTOS Core 0 | Running | Must signal + wait for exit |
| PCNT unit | Hardware | Running (not sampled) | No action needed |
| GPIO ISR on CS_SENSE | GPIO matrix | Armed | `detachInterrupt()` |

### 6.2 Cleanup Strategy: Graceful Abort (Preferred)

The existing `g_abortUploadFlag` mechanism already provides cooperative upload cancellation. We extend it:

```
1. g_cpapYieldRequest is set (ISR or upload task detects)
2. Upload task:
   a. Closes any open File objects (close() on a gone bus = harmless error)
   b. If option 1a: fast-restore + MUX flip
   c. If option 1b: MUX flip (or already flipped by ISR)
   d. Sets uploadTaskResult = YIELD_NEEDED
   e. Falls through to normal task exit
3. FSM (handleUploading) sees YIELD_NEEDED:
   a. detachInterrupt(CS_SENSE)
   b. SD_MMC.end() — safe even though bus is gone; just tears down VFS/driver
   c. SMB disconnect (if connected)
   d. Cloud disconnect (if connected) 
   e. Free upload buffer
   f. Transition to COOLDOWN → LISTENING
```

**Why this works:**
- `SD_MMC.end()` doesn't need the physical bus — it deinits the driver and unmounts VFS
- `File::close()` on a gone bus just returns an error; the VFS handle is freed
- SMB disconnect sends a network packet (doesn't need SD card)
- TLS disconnect sends a network packet (doesn't need SD card)
- The upload buffer is just `free()`'d

### 6.3 SMB-Specific Cleanup

During SMB upload, the typical state is:
```
localFile = sd.open(path)      ← open File on SD (the one that matters)
remoteFile = smb2_open_ev(...) ← open SMB file handle (network, not SD)
```

Cleanup sequence:
1. `localFile.close()` — will fail silently (bus gone), but File destructor frees handle
2. `smb2_close_ev(smb2, remoteFile)` — sends SMB Close over network (works fine)
3. `smb2_disconnect_share_ev(smb2)` — TCP-level disconnect
4. `smb2_destroy_context(smb2)` — frees all libsmb2 internal state

**Concern:** If the upload task is blocked inside `localFile.read()` when the bus disappears, the SDMMC driver may hang waiting for a DMA transfer that will never complete. The ESP-IDF SDMMC driver has internal timeouts (typically 1–2 seconds) that should eventually cause it to return an error. But this is a potential blocking point.

**Mitigation:** If using the ISR-level MUX flip (option 1b), the SD card read will fail at the SDMMC driver level with a timeout/error, which propagates up as `bytesRead = 0` or an error return. The upload code already handles read failures.

### 6.4 Cloud-Specific Cleanup

During SleepHQ upload:
```
file = sd.open(filePath)           ← open File on SD
tlsClient->write(buffer, size)     ← sending data over TLS
```

Cleanup sequence:
1. `file.close()` — same as SMB, fails silently
2. The HTTP multipart upload is now incomplete — server will timeout and discard
3. `sleephqUploader->resetConnection()` / `resetTLS()` — tears down TLS
4. The import can be retried on the next upload cycle (SleepHQ handles duplicate imports)

**Cloud concern:** If `tlsClient->write()` is blocked, it has a 10-second `SO_SNDTIMEO`. This is independent of the SD bus — it's a TCP socket issue. The yield from SD doesn't affect TLS write behavior. The upload task will eventually notice `g_cpapYieldRequest` after the write returns.

### 6.5 Fallback: Soft Reboot

If graceful cleanup fails (e.g., SDMMC driver hangs indefinitely on a timed-out DMA transfer), fall back to soft reboot:

```cpp
if (cleanupTimedOut) {
    LOG_WARN("[Yield] Graceful cleanup timed out — forcing soft reboot");
    setRebootReason("Rapid yield cleanup timeout");
    Logger::getInstance().flushBeforeReboot();
    delay(200);
    esp_restart();
}
```

The soft reboot path already handles all resource cleanup via hardware reset. Fast-boot (ESP_RST_SW) skips cold-boot delays, so recovery is ~5–8 seconds instead of ~20 seconds.

### 6.6 Memory Safety

Key concerns:
- **Open file handles:** `SD_MMC.end()` unmounts VFS, which invalidates all open file handles. Any subsequent `File` operations will fail gracefully (return -1/0). No memory leak — Arduino `File` wrapper holds a `shared_ptr` to the VFS file descriptor.
- **SDMMC DMA buffers:** The SDMMC driver manages its own DMA buffers internally. `SD_MMC.end()` → `sdmmc_host_deinit()` frees them.
- **libsmb2 state:** `smb2_destroy_context()` frees all internal allocations. Safe to call even if mid-transfer.
- **TLS state:** `resetTLS()` deletes the `WiFiClientSecure` object and all associated mbedTLS state. Safe to call anytime.
- **Upload buffer:** Explicitly freed via `freeBuffer()`. Already managed by SMBUploader destructor as backup.

**No memory leaks expected** from the graceful cleanup path.

---

## 7. FSM Changes

### 7.1 New UploadResult Value

```cpp
enum class UploadResult {
    COMPLETE,
    TIMEOUT,
    ERROR,
    NOTHING_TO_DO,
    YIELD_NEEDED    // NEW: CPAP requested bus mid-upload
};
```

### 7.2 FSM Handling

```
handleUploading():
  if uploadTaskComplete && result == YIELD_NEEDED:
    detachInterrupt(CS_SENSE)
    // SD_MMC.end() already called by upload task, or call here
    // Backend disconnects already done by upload task, or do here
    log "CPAP requested bus — yielded"
    transitionTo(RELEASING)

handleReleasing():
  // Normal path already handles SD release + cooldown
  // For yield: skip the usual reboot, go straight to cooldown
  // Clear noWorkSuppressed so we retry soon
```

### 7.3 Yield Check Insertion Points

The yield flag must be checked at **every point where latency matters**:

1. **SMBUploader::upload()** — inner read loop, every chunk (every 4–8 KB)
2. **SleepHQUploader::httpMultipartUpload()** — inner read loop, every chunk
3. **FileUploader::runFullSession()** — between folders
4. **FileUploader::uploadDatalogFolderSmb()** — between files
5. **FileUploader::uploadDatalogFolderCloud()** — between files

The existing `g_abortUploadFlag` checks are at positions 3–5. We add the inner-loop checks (positions 1–2) for tighter response time.

---

## 8. Implementation Plan

### Phase 1: ISR + Detection (Low Risk)

**Goal:** Prove that CS_SENSE FALLING edge reliably detects CPAP access attempts during ESP bus ownership.

- Add `g_cpapYieldRequest` volatile flag + ISR
- Arm ISR after `takeControl()`, disarm before `releaseControl()`
- **Log-only mode:** ISR sets flag, upload code logs it but does NOT yield
- Deploy to AS11 for testing — confirm detection works without disrupting uploads
- Measure: how often does CPAP try to access? At what times? How many edges?

### Phase 2: MUX Yield + SMB Cleanup (Medium Risk)

**Goal:** Prove that immediate MUX yield works for SMB uploads on AS11.

- Option 1b first: ISR flips MUX directly (register write in IRAM)
- Add yield checks in `SMBUploader::upload()` inner loop
- Graceful cleanup: close files → SD_MMC.end() → SMB disconnect → COOLDOWN
- Add `YIELD_NEEDED` to `UploadResult` and FSM handling
- Test on AS11: Does CPAP recover after immediate MUX flip?

### Phase 3: Card State Restore (If Needed)

**Goal:** If Phase 2 shows AS11 cannot recover from Transfer-state card, add fast bare-metal restore.

- Implement fast restore in upload task (mask ISR → ACMD6(0) → CMD7(0) → MUX flip)
- Reuse existing `scrSendCmd()` helpers from StealthConfigReader
- This runs BEFORE SD_MMC.end(), using the still-initialized SDMMC peripheral
- Test timing: should be 5–15 ms total

### Phase 4: Cloud Upload Support

**Goal:** Extend yield support to SleepHQ uploads.

- Add yield checks in `httpMultipartUpload()` inner loop
- Handle mid-upload TLS connection cleanup
- Handle incomplete SleepHQ import (server-side timeout + retry)
- Test: Cloud upload interrupted by CPAP access, then resumed on next cycle

### Phase 5: Hardening & Edge Cases

- Handle: yield during folder scan (before any upload starts)
- Handle: yield during `hasWorkToUpload()` probe
- Handle: yield during web server config save (handleApiConfigRawPost has SD access)
- Handle: multiple rapid yields (CPAP accesses card frequently)
- Add metrics: yield count, yield response time, CPAP recovery time
- Expose in web UI: "SD bus yields this session: N"

---

## 9. Risks & Mitigations

### 9.1 SDMMC Driver Hang on Bus Loss

**Risk:** The ESP-IDF SDMMC driver may hang if the bus disappears mid-DMA transfer.

**Mitigation:**
- ESP-IDF SDMMC has internal command timeouts (~100 ms per command, configurable)
- DMA transfers that don't complete trigger Data Timeout (DRTO) interrupts
- Worst case: driver blocks for 1–2 seconds, then returns an error
- If this is unacceptable, the ISR can also mask SDMMC interrupts to prevent the driver from waiting, and the cleanup code does a full controller reset

### 9.2 Partial SMB Write

**Risk:** SMB write was mid-transfer when yield happened. File on server is incomplete.

**Mitigation:**
- SMB uses `O_WRONLY | O_CREAT | O_TRUNC` — next upload attempt overwrites completely
- Per-file upload tracking already handles this (file only marked complete on success)
- No server-side corruption risk

### 9.3 Partial Cloud Upload

**Risk:** HTTP multipart upload was mid-stream. Server receives partial data.

**Mitigation:**
- SleepHQ import is not processed until `processImport()` is called
- If upload was interrupted, the import is incomplete and eventually times out server-side
- Next upload cycle creates a new import and re-uploads the files
- Per-file tracking ensures already-uploaded files aren't re-uploaded

### 9.4 Yield Storm (CPAP Accesses Card Repeatedly)

**Risk:** CPAP tries to access every few seconds, causing constant yield-retry loops.

**Mitigation:**
- After a yield, enter COOLDOWN for the configured period (default: minutes)
- If CPAP keeps trying, we naturally back off
- Add a yield counter — if >N yields in a row, extend cooldown or log a warning
- The upload will make progress between CPAP access attempts (most of the upload time is network I/O, not SD reads)

### 9.5 GPIO33 Noise / Spurious Triggers — RESOLVED

**Original risk:** ESP's SDMMC DAT3 activity in 4-bit mode couples through MUX
parasitic capacitance to GPIO33, causing false FALLING edges.

**Resolution:** ESP now mounts in **1-bit mode** during upload. DAT3 is not
driven by the SDMMC host — held HIGH by weak pull-up only. No coupling, no
false positives. See Section 2.4 for full analysis.

**Residual risk:** PCB crosstalk from CMD/CLK traces to DAT3 trace. This is
much weaker than same-channel MUX coupling and has not been observed in testing.
The PCNT 125 ns glitch filter provides additional protection.

---

## 10. Testing Strategy

### 10.1 Lab Testing (Development Board)

- Manually toggle GPIO33 with a jumper wire while upload is running
- Verify ISR fires and upload aborts
- Verify cleanup completes without memory leaks (check heap before/after)
- Verify FSM returns to LISTENING and can start a new upload cycle

### 10.2 AS11 Testing

1. Start SMB upload via web UI
2. Wait for upload to be in progress (check logs)
3. Use CPAP to trigger SD access (e.g., start a therapy session)
4. Observe:
   - Does the ESP detect the yield request?
   - How quickly does the MUX flip?
   - Does the CPAP recover or still show "SD Card Error"?
   - Does the ESP cleanly return to LISTENING?
   - Does the next upload cycle work normally?

### 10.3 Soak Testing

- Leave the system running for days with CPAP in active use
- Monitor yield events, recovery times, heap usage
- Verify no memory leaks over repeated yield cycles
- Verify upload eventually completes despite interruptions

---

## 11. Summary of Recommendations

1. **Start with Option 1b** (immediate MUX flip, no card state restore) — simplest, fastest, test if AS11 can self-recover
2. **ISR should flip MUX directly** for minimum latency (<1 µs response time)
3. **Use cooperative abort** (`g_cpapYieldRequest` flag) for upload task cleanup — the existing `g_abortUploadFlag` pattern is proven and safe
4. **Graceful cleanup preferred** (no reboot): close files → SD_MMC.end() → backend disconnect → COOLDOWN
5. **Soft reboot as fallback** only if graceful cleanup hangs
6. **Test with SMB first** — simpler backend, easier to verify, and the user can test on AS11
7. **Add Cloud support after** SMB is proven
8. **Phase 1 should be log-only** — detect and measure without yielding, to gather real-world data on CPAP access patterns

---

## 12. Answers to Open Questions (User Feedback)

1. **AS11 SD host timeout value:** Unknown. We must aim for the fastest possible yield.
2. **AS11 error recovery:** AS11 sends CMD0 to re-init the card — **it can recover**, as long as it did not timeout trying to get access.
3. **CPAP access frequency:** AS11 accesses the SD card **every 58 seconds** during therapy.
4. **AS10 behavior:** AS10 **power-cycles the SD card slot**, which reboots the ESP (and thus recovers naturally). AS10 is 1-bit mode; CS_SENSE/DAT3 is unused for data, but the ISR on CS_SENSE may still work for detecting AS10 access attempts.
5. **Multi-yield resilience:** Each yield MUST be honoured. After a yield, the ESP should **gracefully restore its functionality and go to COOLDOWN**. One yield per upload session; the session is abandoned and retried after cooldown.

---

## 13. Revised Recommendation (Based on Feedback)

### Rationale

Since AS11 sends CMD0 to recover (can self-reinit), the immediate MUX flip (Option 1b) would likely work. However, the user strongly prefers **restoring the card to its original state before flipping the MUX** for maximum compatibility with other CPAP vendors beyond AS10/AS11. This is the correct long-term approach.

### Selected Approach: Option 1a — Restore Card State, Then Flip MUX

The implementation uses the **existing `releaseControl()` logic** with one optimization: the 300 ms settle delay in `setControlPin()` is skipped during rapid yield. The stealth restore (scrInitHardware + CMD sequences + scrDeinitHardware) takes ~270–320 ms, which should be within the CPAP's timeout window.

**Yield timing budget:**
```
t=0 ms     ISR fires on GPIO33 FALLING edge
t=0 ms     g_cpapYieldRequest = true
t=0-50 ms  Upload task reaches next chunk boundary check
t+10 ms    Close open File objects
t+10 ms    SD_MMC.end() (~10-50 ms)
t+60 ms    scrInitHardware() (~60 ms for host_init + slot_init + delays)
t+65 ms    CMD13 + ACMD6 + CMD7 bare-metal restore (~5 ms)
t+66 ms    scrDeinitHardware() (~1 ms)
t+66 ms    Bus pullups set
t+66 ms    MUX flipped to CPAP (GPIO write, instant)
───────────────────────────────────────────
Total: ~66–120 ms (well under estimated 100-500 ms timeout)
```

**Per-session policy:** One yield per upload session. After yielding, the upload task returns `YIELD_NEEDED`, the FSM enters COOLDOWN, and the upload is retried in the next cycle. Given CPAP accesses every 58 seconds, there is ample time between accesses for upload progress.

---

## 14. Implementation Status

### Critical Fix: 1-Bit Mode for Yield Detection (Apr 2026)

**Problem:** The original ISR approach used 4-bit SD mode, which caused ESP's
own DAT3 activity to couple through the MUX to GPIO33. All software filtering
attempts (idle windows, pin-level checks, quiet-period confirmation) failed
because the coupling occurs during ALL SD operations.

**Proposed Solution (from GitHub Issue #50):** Force 1-bit SD mode during upload.
The theory was that ESP would not drive DAT3 in 1-bit mode, allowing CPAP's DAT3
assertions to produce clean falling edges on GPIO33.

**Implementation:**
- `src/SDCardManager.cpp` — `takeControl()` forces 1-bit mode during upload
  (overrides `config.getEnable1BitSdMode()`)
- `src/main.cpp` — Simple ISR: `g_cpapYieldRequest = true` (no pin-level
  checks, no idle-window guards)
- `include/UploadFSM.h` — Simplified: single `g_cpapYieldRequest` flag,
  `confirmCpapYieldRequest()` returns it directly

**Test Result (Apr 14, 2026):** **FAILED - Protocol Limitation**

The 1-bit mode change successfully eliminated false positives (ESP no longer
couples to GPIO33), but detection still fails because CPAP does not drive
DAT3 during blocked access attempts.

**Root Cause:** In SD 4-bit mode, DAT3 is a data line, not chip-select. The host
only drives DAT3 during the data transfer phase, which only starts after a
successful command-response exchange. When the MUX blocks the CPAP from the card:
1. CPAP sends command on CMD + CLK
2. Command fails (no response, card disconnected)
3. CPAP never reaches data transfer phase
4. DAT3 is never driven → no falling edge on GPIO33

**Conclusion:** There is no reliable way to detect CPAP's blocked SD card access
attempts via GPIO33 (DAT3 tap), regardless of ESP's bus mode. The SD protocol
simply doesn't produce the signal we're trying to detect. The fundamental
assumption in Issue #50 was incorrect.

**Code Review (Apr 14, 2026):** No code-level bugs found. GPIO33 is correctly
configured as INPUT with external pull-ups, ISR is properly attached, and PCNT
coexists with GPIO ISR without conflict. The failure is due to the protocol
limitation, not a code bug.

### Phase 1+2: ISR Detection + SMB Yield (IMPLEMENTED)

Files modified:
- `include/UploadFSM.h` — `g_cpapYieldRequest` flag, ISR declaration
- `include/FileUploader.h` — `YIELD_NEEDED` added to `UploadResult`
- `include/SDCardManager.h` — `rapidYield()` method added
- `src/SDCardManager.cpp` — `rapidYield()` implementation (releaseControl minus settle delay)
- `src/main.cpp` — ISR definition, arm/disarm in FSM, `YIELD_NEEDED` handling
- `src/SMBUploader.cpp` — yield check in upload inner loop
- `src/FileUploader.cpp` — yield checks between folders in `runFullSession()`

### Phases 3-5: Deferred

- Cloud (SleepHQ) yield support
- Edge case hardening (yield during folder scan, web config save, etc.)
- Metrics and web UI integration
