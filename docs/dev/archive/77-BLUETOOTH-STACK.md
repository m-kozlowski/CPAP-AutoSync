# 77 — Bluetooth Stack Feasibility Analysis

**Date:** 2026-05-06  
**Status:** Analysis — awaiting decision  

---

## 1. Executive Summary

Adding a Bluetooth Low Energy (BLE) stack to the firmware requires fitting the Bluetooth controller, host stack, and a small application integration layer into the OTA application partition. Each `pico32-ota` app slot is 0x1C0000 = **1,835,008 bytes**.

The first priority was to shrink the existing non-BLE firmware before measuring any Bluetooth overhead. That Track A minimization work is now complete and produced these measured results:

| Build point | App bytes used | Free OTA bytes | Free OTA space |
|-------------|----------------|----------------|----------------|
| Post web-gzip baseline | **1,603,440** | **231,568** | **~226 KB** |
| After Track A minimization | **1,514,415** | **320,593** | **~313 KB** |
| Net Track A saving | **89,025 bytes** | **+89,025 bytes** | **~87 KB** |

**Bottom line:** the current non-BLE OTA build now has about **313 KB** free. A minimal raw ESP-IDF NimBLE central configuration is plausibly within reach, but it must still be measured with an empty feature stub before any application-level BLE work is approved. The document intentionally describes only generic Bluetooth stack feasibility and does not define a product/vendor-specific protocol plan.


## 2. Generic BLE Requirements

The intended Bluetooth capability should be treated as a generic BLE central/client integration. The firmware would initiate a connection to a nearby BLE peripheral, discover the required GATT service and characteristics, exchange data, then disconnect.

### BLE Features Expected to Be Needed

| Feature | Required? | Notes |
|---------|-----------|-------|
| **GATT Client (Central role)** | ✅ Yes | Firmware initiates the BLE connection |
| **Scanning (Observer role)** | ✅ Yes | Needed to find the target peripheral |
| **GATT Service Discovery** | ✅ Yes | Needed after connection |
| **Characteristic Write** | ✅ Yes | Needed for command/request traffic |
| **Characteristic Notify** | ✅ Yes | Needed for response/data traffic |
| **CCCD Write** | ✅ Yes | Needed to enable notifications |
| **ATT MTU Exchange** | Likely | Larger MTU improves transfer efficiency; exact value should be validated later |
| **SMP Pairing/Bonding** | Unknown / avoid if possible | Keeping this disabled saves space if the target workflow allows it |
| **GATT Server (Peripheral)** | ❌ No | Firmware does not need to advertise as a BLE peripheral |
| **Advertising (Broadcaster)** | ❌ No | Firmware does not need to advertise |
| **BLE Mesh** | ❌ No | Not needed |
| **Bluetooth Classic** | ❌ No | BLE only |
| **Multiple connections** | ❌ No | Assume one peripheral connection at a time |

The application code above the BLE stack should remain small if kept self-contained: scanning, connection management, GATT discovery, a transfer state machine, validation, and handoff to the existing upload/storage flow. A placeholder estimate is **5–15 KB** until a real generic prototype is measured.

---

## 3. BLE Library Options

### Option A: NimBLE via ESP-IDF (Recommended if feasible)

NimBLE is the ESP32's lightweight BLE-only stack, replacing the much heavier Bluedroid dual-mode stack.

| Aspect | Detail |
|--------|--------|
| **Library** | ESP-IDF built-in `esp_nimble` (no Arduino wrapper needed) |
| **PlatformIO** | Use `custom_sdkconfig` to enable `CONFIG_BT_NIMBLE_ENABLED=y` |
| **Typical flash cost** | **~200–300 KB** total (controller + host + GATT) |
| **Controller blob** | `libbt.a` — ~80–100 KB (binary, not shrinkable) |
| **NimBLE host** | ~80–120 KB (configurable, text + rodata) |
| **PHY/coexistence** | ~30–50 KB (shared with WiFi, partially already linked) |
| **Advantages** | Smallest possible BLE stack; no C++ wrapper overhead; highly configurable |
| **Disadvantages** | C API (not Arduino-friendly); requires hybrid compile changes |

### Option B: NimBLE-Arduino Wrapper

This is a C++ wrapper around NimBLE.

| Aspect | Detail |
|--------|--------|
| **Library** | NimBLE-Arduino wrapper |
| **PlatformIO** | Add to `lib_deps` |
| **Typical flash cost** | **~250–350 KB** (includes C++ wrapper overhead) |
| **Advantages** | Arduino-compatible API; easier to develop |
| **Disadvantages** | ~50 KB larger than raw ESP-IDF NimBLE; C++ templates add size |

### Option C: Bluedroid (Default ESP32 stack) — NOT VIABLE

| Aspect | Detail |
|--------|--------|
| **Flash cost** | **~500–700 KB** |
| **Why not** | Dual-mode (Classic + BLE); massive; would blow through the partition |

### Option D: Raw HCI / Custom Minimal Stack — EXPERIMENTAL

Write a bare-minimum BLE central using only the BT controller's HCI interface.

| Aspect | Detail |
|--------|--------|
| **Flash cost** | **~100–130 KB** (controller blob + minimal HCI shim) |
| **Advantages** | Smallest possible footprint |
| **Disadvantages** | Enormous development effort; no GATT layer; must implement ATT/GATT/GAP manually; fragile; unmaintainable |

### Recommendation

**Option A (ESP-IDF NimBLE, no Arduino wrapper)** is the best balance. It provides the smallest viable stack with proper GATT client support. The C API is harder to use but we only need a thin application layer on top.

If Option A still doesn't fit, **Option D** is theoretically possible but the development cost is prohibitive.

---

## 4. Size Budget Analysis

### Current State

| Item | Value |
|------|-------|
| OTA partition size | 0x1C0000 = 1,835,008 bytes (1,792 KB) |
| Current firmware size | ~1,603,440 bytes (`pio run`, `pico32-ota`, after web UI gzip work) |
| Free space | **~231,568 bytes (~226 KB)** |

### NimBLE Cost Breakdown (Minimal Central-Only Configuration)

| Component | Estimated Size | Notes |
|-----------|---------------|-------|
| BT Controller (`libbt.a`) | ~80–100 KB | Binary blob, not reducible |
| NimBLE Host (GATT client) | ~60–80 KB | With peripheral/broadcaster disabled |
| PHY/coexistence overhead | ~20–40 KB | Shared BT/WiFi radio coordination |
| Generic BLE application layer | ~5–15 KB | Connection flow, GATT operations, transfer state machine |
| **Total BLE addition** | **~165–235 KB** | |

### The Gap

| Scenario | BLE Cost | Free Space | Delta |
|----------|----------|------------|-------|
| Best case (aggressive optimization) | ~165 KB | 226 KB | **+61 KB margin** ✅ |
| Typical case | ~200 KB | 226 KB | **+26 KB margin** ⚠️ |
| Worst case | ~235 KB | 226 KB | **-9 KB over** ❌ |

> [!WARNING]
> The situation has improved since this document was first written: the typical raw ESP-IDF NimBLE case may now fit. However, the margin is still small enough that a library update, debug option, or Arduino-wrapper choice can push the image over the OTA slot.

---

## 5. Optimization Strategy: Making Room

To fit BLE comfortably, we should still shrink the existing firmware or expand the partition — or both. The levers below are ordered by expected impact. Claims marked as estimates must be validated with `pio run` because ESP-IDF Kconfig interactions can make savings non-linear.

### 5.1 High-Impact Changes (Existing Firmware Shrinks)

#### A. Disable C++ Exceptions — Save ~20–40 KB

**Current:** `CONFIG_COMPILER_CXX_EXCEPTIONS=y` (enabled)

C++ exception handling adds significant EH table metadata and runtime support code. ESP-IDF explicitly recommends disabling this for size-constrained builds.

```ini
# In custom_sdkconfig:
CONFIG_COMPILER_CXX_EXCEPTIONS=n
```

**Impact:** ~20–40 KB flash savings  
**Risk:** Low-to-medium — production code currently has no C++ `try`/`catch` blocks; the only C++ exception use found is in native test mocks. This is likely one of the safest high-impact savings, but the full embedded build must still be compiled after disabling exceptions.

#### B. Disable IPv6 — Save ~10–15 KB

**Current:** `CONFIG_LWIP_IPV6=y` (enabled)

Our device connects to home WiFi networks for SMB/HTTPS uploads. IPv6 is not required for any of our protocols.

```ini
CONFIG_LWIP_IPV6=n
```

**Impact:** ~10–15 KB flash savings  
**Risk:** Not currently low in this codebase — a Track A build with `CONFIG_LWIP_IPV6=n` failed because `libsmb2` still compiles IPv6 socket paths that reference `struct sockaddr_in6`. IPv6 can only be disabled after patching/configuring `libsmb2` to exclude IPv6-aware code, so it is **deferred** for now.

#### C. Disable WiFi Enterprise Support — Save ~5–10 KB

**Current:** `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=y` (enabled)

Enterprise WiFi (802.1X/RADIUS) is not a supported feature. No user connects a CPAP uploader to a corporate RADIUS network.

```ini
CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n
```

**Impact:** ~5–10 KB flash savings  
**Risk:** None — we don't support or document enterprise WiFi.

#### D. Disable VFS Termios + Select — Save ~4.5 KB

**Current:** Both `CONFIG_VFS_SUPPORT_TERMIOS=y` and `CONFIG_VFS_SUPPORT_SELECT=y`

We use VFS for SD card file I/O but never use `termios` or `select()` on file descriptors.

```ini
CONFIG_VFS_SUPPORT_TERMIOS=n
CONFIG_VFS_SUPPORT_SELECT=n
```

**Impact:** ~4.5 KB flash savings (1.8 + 2.7 KB)  
**Risk:** None — we don't use these APIs.

#### E. Silent Assertions — Save ~3–8 KB

**Current:** `CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2` (full)

Silent assertions remove the filename/line strings but still abort. Failed asserts can be traced via the PC address.

```ini
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y
```

**Impact:** ~3–8 KB flash savings  
**Risk:** Low — assertion failures are still caught; debugging requires addr2line lookup instead of reading the log.

#### F. Disable Error-to-Name Lookup — Save ~3–5 KB

**Current:** `CONFIG_ESP_ERR_TO_NAME_LOOKUP=y` (enabled)

This lookup table converts ESP error codes to human-readable strings. We already log our own error descriptions.

```ini
CONFIG_ESP_ERR_TO_NAME_LOOKUP=n
```

**Impact:** ~3–5 KB flash savings  
**Risk:** Low — error logs will show numeric codes instead of names like `ESP_ERR_NO_MEM`. Our Logger already wraps errors with context.

#### G. Newlib Nano Formatting — Save ~25–50 KB

**Current:** `# CONFIG_NEWLIB_NANO_FORMAT is not set` (disabled / using full newlib)

Nano formatting uses a smaller `printf` implementation (partially in ROM).

```ini
CONFIG_NEWLIB_NANO_FORMAT=y
```

**Impact:** ~25–50 KB flash savings  
**Risk:** Medium — this is valuable, but it has one known blocker.

Plain-English explanation of the `%llu` issue:

- `%llu` is a `printf`/`snprintf` format for printing a 64-bit unsigned integer.
- The current SMB pipelined-write logging uses `%llu` to print 64-bit file offsets in `SMBUploader.cpp`.
- Newlib Nano is a smaller formatting library. On ESP32, it does **not** support 64-bit integer formatting in `printf`.
- If we enable Nano without changing those log lines, the firmware may compile but print wrong values, corrupt log output, or behave unpredictably in those formatting calls.

The proposed fix is small and isolated: before enabling `CONFIG_NEWLIB_NANO_FORMAT=y`, replace the few `%llu` SMB offset logs with Nano-safe formatting. Good options are:

1. log the 64-bit offset as two 32-bit hex halves, or
2. add a tiny helper that converts `uint64_t` to decimal text without relying on `printf`, or
3. remove the exact offset from those rare error logs if we decide it is not worth the code.

> [!IMPORTANT]
> **Measured Track A result:** the `%llu` logging was made Nano-safe and `CONFIG_NEWLIB_NANO_FORMAT=y` was tested. In this PlatformIO/Arduino build it produced no measurable application-size reduction (`1514415` bytes before and after), so Nano should remain disabled for now. The `%llu` cleanup is still useful because it removes a future blocker.

### 5.2 Medium-Impact Changes

#### H. Stack Check Mode — Save ~2–5 KB

**Current:** `CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y`

Stack canary checking adds instrumentation to every function. Disabling it saves flash and a few cycles per call.

```ini
CONFIG_COMPILER_STACK_CHECK_MODE_NONE=y
```

**Impact:** ~2–5 KB flash savings  
**Risk:** Medium — loses stack overflow detection. Production builds often disable this.

#### I. Reduce CORE_DEBUG_LEVEL — Save ~5–15 KB

**Current:** `-DCORE_DEBUG_LEVEL=3` (INFO level)

This controls Arduino's internal log verbosity. Level 1 (ERROR only) or 0 (NONE) strips many format strings.

```
-DCORE_DEBUG_LEVEL=1
```

**Impact:** ~5–15 KB flash savings (depends on how many Arduino-level logs exist)  
**Risk:** Low for production — we have our own logging system.


#### J. Keep Runtime Web UI as Generated Gzip — Already Saves ~40 KB

**Current:** Implemented locally from PR #100. `include/web_ui.h` has been replaced by `src/web/web_ui.html` plus generated `include/web_ui_gz.h`.

This was the largest non-sdkconfig saving already applied. The latest measured build is ~1,603,440 bytes, leaving ~226 KB free in the 1,835,008-byte OTA slot. The original baseline in this document (~185 KB free) is now stale.

**Impact:** Already realized; roughly +40 KB additional OTA headroom compared with the old raw `WEB_UI_HTML` PROGMEM string.  
**Risk:** Low — build passed; browser smoke testing should verify `/`, `/status`, `/logs`, `/config`, and `/monitor`.

#### K. Consider Disabling or Shrinking Core Dump Support — Save Partition Space, Not App Flash

**Current:** `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, 64 KB `coredump` partition.

Disabling flash coredumps does not directly reduce the app binary by much, but it can free partition-table space if we later restructure OTA slots. It also removes one diagnostic tool, so this is better treated as a partition-layout decision than a first-line code-size optimization.

**Impact:** Up to 64 KB partition space if the partition is removed; only part of that can become OTA app space depending on alignment.  
**Risk:** Medium — loses post-crash ELF coredump diagnostics.

#### L. Audit mbedTLS Feature Set Before BLE

The project already applies important TLS savings: asymmetric 16 KB/4 KB TLS buffers, no peer certificate retention, and ChaCha/Poly1305/ChaChaPoly disabled. Further mbedTLS pruning may be possible but is risky because HTTPS and WebDAV depend on modern TLS interoperability.

Candidates to measure only after a known-good TLS test matrix:

- unused key exchanges / certificate algorithms
- TLS 1.3 if enabled by the base config and not required
- unused PEM/PKCS parsing paths

**Impact:** Unknown; potentially 5–30 KB.  
**Risk:** High — can silently break HTTPS endpoints.

### 5.3 NimBLE-Specific Minimization

When enabling NimBLE, use these settings to minimize its footprint:

```ini
# Enable NimBLE, disable Bluedroid
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y

# Controller: BLE only, single connection
CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y
CONFIG_BTDM_CTRL_BLE_MAX_CONN=1

# NimBLE Host: Central + Observer only
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=n

# Single connection
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1

# Disable pairing/bonding only if the final target workflow allows it
CONFIG_BT_NIMBLE_SM_LEGACY=n
CONFIG_BT_NIMBLE_SM_SC=n

# Logging: silent
CONFIG_BT_NIMBLE_LOG_LEVEL_NONE=y

# MTU: choose the smallest value that satisfies the final transfer design
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247

# Disable mesh, ext advertising, etc.
CONFIG_BT_NIMBLE_EXT_ADV=n
CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT=n
```

Disabling peripheral + broadcaster roles saves **~20 KB**. Disabling SMP saves **~5–10 KB**. Silent logging saves **~5–10 KB** of format strings.

### 5.4 Summary: Total Potential Savings

| Optimization | Estimated Saving | Risk |
|-------------|-----------------|------|
| Keep runtime Web UI gzipped | ~40 KB | Already applied |
| Track A low-risk sdkconfig/build-flag bundle, excluding IPv6 | **83,561 bytes measured** | Low |
| Disable C++ exceptions | **5,484 bytes measured** | Low-to-medium |
| Newlib Nano formatting | 0 bytes measured in this build | Deferred |
| Disable IPv6 | 10–15 KB estimated | Deferred: `libsmb2` compile issue |
| Additional mbedTLS pruning | 5–30 KB | High |
| Core dump partition removal/shrink | partition only | Medium |
| **Measured Track A app-flash savings so far** | **89,025 bytes** excluding prior web-gzip work | |

Measured after Track A changes:
- **Original post-web-gzip baseline:** 1,603,440 bytes used, ~226 KB free
- **After low-risk bundle + C++ exceptions disabled + `%llu` cleanup:** 1,514,415 bytes used, ~313 KB free
- **Measured additional saving:** 89,025 bytes from the current code delta, within rounding of the step totals above
- **Generic NimBLE central + BLE application layer estimated cost:** ~165–235 KB
- **Projected post-BLE margin from measured Track A baseline:** ~78–148 KB ✅

> [!TIP]
> With the measured Track A optimizations, there is a realistic path to fitting a minimal NimBLE BLE central feature **while preserving OTA updates**, but the actual stack overhead must still be measured with an empty stub.

---

## 6. Alternative: Partition Restructuring

If the optimization approach falls short, the partition table can be restructured.

### Current Layout

```
# Name,    Type, SubType, Offset,    Size
nvs,       data, nvs,     0x9000,    0x5000     (20 KB)
otadata,   data, ota,     0xe000,    0x2000     (8 KB)
app0,      app,  ota_0,   0x10000,   0x1C0000   (1,792 KB)
app1,      app,  ota_1,   0x1D0000,  0x1C0000   (1,792 KB)
spiffs,    data, spiffs,  0x390000,  0x60000    (384 KB)
coredump,  data, coredump,0x3F0000,  0x10000    (64 KB)
```

### Option: Reclaim from SPIFFS and Coredump

Current LittleFS/SPIFFS data partition is 384 KB. Reducing it to 256 KB frees 128 KB, which can cleanly grow each OTA app slot by 64 KB (0x1C0000 → 0x1D0000).

The original version of this section claimed coredump could be reduced to 32 KB and split as an extra +80 KB per app slot, but that was misleading: the example table kept coredump at 64 KB and grew the apps by only 64 KB. Treat coredump removal/shrinking as a separate option that needs alignment validation and a deliberate loss-of-diagnostics decision.

```
# Name,    Type, SubType, Offset,    Size
nvs,       data, nvs,     0x9000,    0x5000     (20 KB)
otadata,   data, ota,     0xe000,    0x2000     (8 KB)
app0,      app,  ota_0,   0x10000,   0x1D0000   (1,856 KB = +64 KB)
app1,      app,  ota_1,   0x1E0000,  0x1D0000   (1,856 KB = +64 KB)
spiffs,    data, spiffs,  0x3B0000,  0x40000    (256 KB)
coredump,  data, coredump,0x3F0000,  0x10000    (64 KB)
```

> [!CAUTION]
> Partition restructuring requires a full reflash (not OTA). All users would need to re-flash via USB for this change. It is still the most reliable way to create durable BLE headroom, but it should be avoided unless measured NimBLE overhead exceeds the optimized app slot.

---

## 7. WiFi + BLE Coexistence

The ESP32 shares a single radio between WiFi and Bluetooth. When both are active simultaneously, the radio time-division multiplexes between the two protocols.

### Implications for Our Architecture

1. **BLE scan + WiFi STA:** Works, but WiFi throughput drops during active BLE scanning.
2. **BLE connected + WiFi STA:** Works. The ESP-IDF coexistence arbiter handles scheduling. File transfers over BLE will be slower if WiFi is also transmitting.
3. **AP setup mode:** Must avoid BLE scans while the captive-portal AP has clients connected; scans can disrupt the shared radio and user configuration flow.
4. **Recommended approach:** Run BLE sync as a separate phase — after WiFi uploads complete, or before they start. Prefer stopping WiFi or at least avoiding WiFi upload traffic during BLE file transfer. Do not run BLE during SD/CPAP bus-active windows.

### RAM Implications

NimBLE uses significantly less RAM than Bluedroid (~100 KB less), but it still needs heap for connection state, buffers, mbufs, and controller/host task stacks. Buffer sizing matters: larger MTU values improve throughput but increase RAM pressure. Current free heap and `max_alloc` should be measured before BLE init, after BLE init, while connected, and during representative transfer activity.

---

## 8. Implementation Architecture (If Approved)

### Phased Approach

This plan has two tracks. **Track A is the recommended next work and does not integrate NimBLE at all.** Track B only starts later if/when a generic BLE feature is approved.

#### Track A — Firmware minimization now, no BLE

Goal: increase OTA headroom while keeping the current feature set unchanged.

1. **A0 — Freeze the baseline:** keep the web-gzip change, record the current firmware size, and save the exact `pio run` output.
2. **A1 — Apply safest sdkconfig cuts first:** disable WiFi Enterprise, VFS termios/select, and error-to-name lookup; reduce `CORE_DEBUG_LEVEL`; enable silent assertions. Build and record the byte delta. Do **not** disable IPv6 yet because `libsmb2` currently fails to compile without IPv6 types.
3. **A2 — Disable C++ exceptions:** production code currently has no C++ `try`/`catch`; disable exceptions and build. If it compiles and tests pass, keep it.
4. **A3 — Prepare Newlib Nano:** make the SMB `%llu` offset logs Nano-safe. This is a small code cleanup, not a BLE integration.
5. **A4 — Test Newlib Nano, but keep only if it saves space:** in the measured Track A build, Nano produced no application-size reduction, so leave it disabled for now.
6. **A5 — Stop unless more space is needed:** do not touch NimBLE yet. At this point we have a smaller non-BLE firmware and a measured headroom number.

Recommended order for the immediate optimization PRs:

1. low-risk sdkconfig bundle from A1, excluding IPv6
2. C++ exceptions disabled from A2
3. SMB `%llu` logging cleanup from A3
4. Newlib Nano measured but not kept unless a future toolchain/build change shows real savings

#### Track B — BLE feasibility later

Only after Track A is measured:

1. **B1:** Enable raw ESP-IDF NimBLE with the minimal configuration from §5.3 and an empty `#ifdef ENABLE_BLE_OXIMETER_SYNC` stub. Measure the true stack overhead.
2. **B2:** Implement the generic BLE peripheral integration layer as a self-contained module.
3. **B3:** Integrate BLE sync into the upload FSM as a separate phase, gated away from AP-setup clients and CPAP/SD bus activity.

### Module Design

```
include/
  BleOximeterSync.h    — public API: init(), sync(), deinit()
  BleOximeterProtocol.h — generic protocol/transfer helpers
src/
  BleOximeterSync.cpp   — BLE scan, connect, GATT operations
  BleOximeterProtocol.cpp — pure-function protocol/transfer helpers
```

The protocol/transfer helper layer should be portable and testable on native where possible. The BLE integration should remain behind `#ifdef ENABLE_BLE_OXIMETER_SYNC`.

---

## 9. Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| BLE stack doesn't fit in current OTA headroom | Medium | Current headroom is ~226 KB; apply optimizations first and measure raw ESP-IDF NimBLE before committing |
| Newlib Nano breaks `printf` formats | Medium | Known `%llu` usage exists in `SMBUploader.cpp`; fix before enabling Nano |
| Disabling C++ exceptions causes crashes | Medium | Audit codebase for `try`/`catch`; convert to error codes |
| WiFi/BLE coexistence causes instability | Medium | Run BLE in a separate phase; test extensively |
| Partition restructuring needed | Low | Only if optimizations are insufficient; requires USB reflash |
| BLE peripheral compatibility edge cases | Medium | Keep the first integration isolated; test with representative hardware before enabling by default |

---

## 10. Recommendation

### Recommended next step: minimize firmware first, do not add BLE yet

The best immediate path is **not** to start NimBLE. First create a smaller, cleaner non-BLE firmware and measure each saving.

1. **Keep the web-gzip change** as the new baseline.
2. **Apply low-risk sdkconfig reductions** from §5.1/§5.2 and record exact size deltas. In the measured Track A pass, IPv6 was deferred because `libsmb2` failed to compile without IPv6 socket types.
3. **Disable C++ exceptions** if the embedded build confirms no production dependency.
4. **Fix the `%llu` SMB logging blocker** so Newlib Nano can be evaluated safely.
5. **Test Newlib Nano, but keep it only if it actually saves space.** The measured Track A pass showed no size reduction, so it should remain disabled for now.
6. **Stop there** unless/until BLE work is approved.

BLE should be a separate later decision. If the optimized non-BLE build has **≥ 300 KB** free, then raw ESP-IDF NimBLE is worth measuring with an empty stub. If it leaves less than ~40 KB post-BLE margin, revisit partition restructuring (§6).

### Key Numbers to Validate

| Metric | Target | How to Measure |
|--------|--------|----------------|
| Current free OTA space after Track A | ~313 KB measured | `1835008 - firmware.elf reported used bytes` |
| Post-optimization free space | ≥ 300 KB preferred | Achieved by measured Track A build |
| NimBLE minimal central overhead | ≤ 200 KB preferred | Build with NimBLE enabled, empty BLE sync stub |
| Post-BLE image margin | ≥ 40 KB | OTA slot size minus final image size |
| Runtime free heap with BLE | ≥ 40 KB | `esp_get_free_heap_size()` after BLE init |
| Runtime max alloc with BLE | ≥ largest TLS/SMB allocation need | `ESP.getMaxAllocHeap()` during BLE + upload phases |

---

## Appendix A: Relevant sdkconfig Entries (Current State)

| Setting | Current Value | Recommended | Saving |
|---------|--------------|-------------|--------|
| `CONFIG_BT_ENABLED` | `n` | `y` (NimBLE) | — |
| `CONFIG_COMPILER_CXX_EXCEPTIONS` | `y` | `n` | 20–40 KB |
| `CONFIG_LWIP_IPV6` | `y` | Keep `y` for now | Deferred: `libsmb2` compile issue |
| `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT` | `y` | `n` | 5–10 KB |
| `CONFIG_ESP_WIFI_SOFTAP_SUPPORT` | `y` | Keep `y` | — (needed for AP setup) |
| `CONFIG_VFS_SUPPORT_TERMIOS` | `y` | `n` | 1.8 KB |
| `CONFIG_VFS_SUPPORT_SELECT` | `y` | `n` | 2.7 KB |
| `CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL` | `2` (full) | Silent | 3–8 KB |
| `CONFIG_ESP_ERR_TO_NAME_LOOKUP` | `y` | `n` | 3–5 KB |
| `CONFIG_NEWLIB_NANO_FORMAT` | not set | Keep disabled for now | 0 bytes measured after `%llu` fix |
| `CONFIG_COMPILER_STACK_CHECK` | `NORM` | `NONE` | 2–5 KB |
| `CORE_DEBUG_LEVEL` | `3` (INFO) | `1` (ERROR) | 5–15 KB |
