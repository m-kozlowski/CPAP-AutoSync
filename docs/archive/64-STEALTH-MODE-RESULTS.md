# 64 — Stealth Mode Experiment: Results

**Status**: PROVEN VIABLE on AS11 — AS10 testing pending  
**Builds**: dev+20 through dev+26  
**Devices tested**: 3 (1× AS11, 2× AS10 from different users)  
**Total test boots**: 10

## Background

The goal is to read the CPAP's SD card (`config.txt`) **without sending CMD0** (GO_IDLE_STATE), which resets the card and disrupts the CPAP's active session. By preserving the CPAP's established RCA and card state, the ESP32 can transparently query the card and return the MUX without the CPAP registering a disruption.

## Test Architecture

**`stealthTest()` in `BusWidthDetector.cpp`** runs multiple phases:

| Phase | What it tests | How |
|-------|--------------|-----|
| **Phase 1** | True stealth RCA check | Mask ISR, CMD13(0x1388) directly |
| **Phase 1b** | Stealth sector read | ACMD6(0)→1-bit, CMD17 read sector 0 |
| **Phase 1c** | Stealth config.txt read | Minimal FAT32 reader → extract WIFI_SSID |
| **Phase 2** | Positive control | `sdmmc_card_init()` + bare-metal CMD13 |
| **Phase 3a** | MUX isolation | MUX round-trip, SDMMC stays running |
| **Phase 3b** | Stealth init isolation | Full deinit + `initHardware(stealth)` |
| **Phase 3c** | CMD0 isolation | Full deinit + `initHardware(normal)` with init clocks |

Phase 1 runs FIRST (before any CMD0) to preserve the CPAP's card state.

## Results Summary

### Phase 3 Isolation (100% consistent across all 10 boots)

| Test | Result | Meaning |
|------|--------|---------|
| **3a** (MUX only) | ✅ PASS | MUX round-trip does NOT kill the card |
| **3b** (stealth init) | ✅ PASS | SDMMC deinit + reinit without CMD0 is safe |
| **3c** (normal init) | ❌ FAIL | Init clocks (which secretly send CMD0) kill the card |

### AS11 Stealth Results (dev+26 — direct RCA approach)

| Boot type | Phase 1 | Phase 1b | Card state | Notes |
|-----------|---------|----------|------------|-------|
| Cold boot (#31) | ✅ PASS | ✅ READ OK | Tran (state 4) | RCA 0x1388, 0x55AA valid, instant |
| Soft-reset (#32) | ✅ PASS | ✅ READ OK | Tran (state 4) | 7s gap, instant response |

**Full end-to-end stealth proven**: grab MUX → stealth init → CMD13(0x1388) → ACMD6(1-bit) → CMD17 → valid MBR. No CMD0 sent. Card state fully preserved.

### Earlier Results (dev+20–25 — brute-force sweep approach, NOW SUPERSEDED)

The brute-force RCA sweep (CMD13 for all 65535 RCAs) failed on cold boot and was unreliable on soft-reset. **dev+26 proved the sweep itself was the problem** — rapid-fire CMD13s to wrong RCAs disrupted the card. The direct CMD13(0x1388) approach works instantly on both cold boot and soft-reset.

### AS10 Results (dev+24 — brute-force sweep, needs re-test with dev+26)

| Device | Boot type | Phase 1 | Notes |
|--------|-----------|---------|-------|
| AS10 User 1 | Cold boot | ❌ FAIL | PCNT=0, needs re-test with direct RCA |
| AS10 User 1 | Soft-reset | ❌ FAIL | Needs re-test with direct RCA |
| AS10 User 2 | Cold boot | ❌ FAIL | Same — sweep may have been the issue |
| AS10 User 2 | Soft-reset | ❌ FAIL | Same |

**AS10 re-testing with dev+26 (direct RCA) is pending.**

## Key Technical Findings

### 1. ISR Masking Fix (dev+20)
Setting `SDMMC.intmask.val = 0` after `sdmmc_host_init()` prevents the ESP-IDF SDMMC ISR from consuming CMD_DONE interrupts. Bare-metal CMD13 then works by polling `RINTSTS` (raw interrupt status register).

### 2. Hidden CMD0 Bug (dev+22)
The `initHardware()` "80 init clocks" command used `sdmmc_hw_cmd_t` which zero-initializes to `cmd_index = 0` (CMD0). The DWC SDMMC controller sends init clocks *then* executes the command — so it was secretly sending CMD0 every time, resetting the card. Fixed with a `stealth` parameter that skips init clocks.

### 3. RCA Is Always 0x1388
All three devices produce RCA 0x1388 (decimal 5000). This is the card's CMD3 response. Using the known RCA directly (instead of sweeping) eliminates the brute-force delay entirely.

### 4. Brute-Force Sweep Was the Problem (dev+26)
The 30-second CMD13 sweep across 65535 RCAs failed even when the card had an active RCA. Replacing it with a direct CMD13(0x1388) works instantly on cold boot AND soft-reset. The rapid-fire barrage of CMD13s to wrong RCAs likely confused the card or caused timing issues in the SDMMC controller.

### 5. DMA Must Be Disabled for FIFO Reads (dev+25)
`sdmmc_host_init()` enables the IDMAC by default, which drains the FIFO before our polling loop reads it. Fix: `SDMMC.ctrl.use_internal_dma = 0; SDMMC.bmod.enable = 0;`

### 6. Card Bus Width Must Be Forced to 1-bit (dev+25)
The CPAP's normal boot configures the card for 4-bit mode. Our stealth init only has 1-bit host. CRC is computed per-line in 4-bit mode, causing DCRC when read in 1-bit. Fix: CMD55+ACMD6(0) forces the card to 1-bit mode. This is transparent — the CPAP will re-configure 4-bit when it next uses the card.

### 7. CMD8 Diagnostic (dev+26)
CMD8 (SEND_IF_COND) only works in Idle state. When Phase 1 fails:
- CMD8 responds → card is in Idle (was CMD0'd by something)
- CMD8 RTO → card not in Idle (not initialized, Inactive, or SPI mode)

## The Stealth Sequence (Proven on AS11)

```
1. PCNT confirms bus silence (CPAP idle)
2. Grab MUX (SD_SWITCH_PIN → ESP32)
3. sdmmc_host_init() + sdmmc_host_init_slot() — GPIO & clock only
4. SKIP init clocks (stealth=true) — no CMD0!
5. Mask SDMMC interrupts (INTMASK=0)
6. CMD13(0x1388) — card responds instantly (state=Transfer)
7. Disable DMA (IDMAC off)
8. CMD55+ACMD6(0) — force card to 1-bit mode
9. CMD17 sector reads — MBR, BPB, root dir, config.txt
10. Parse WIFI_SSID from config.txt
11. Restore card state, tri-state pins, release MUX
```

Total time: <500ms. No CMD0 sent. Card state preserved.

## Files Modified

- `src/BusWidthDetector.cpp` — `stealthTest()`, `initHardware(bool stealth)`, `readConfigTxt()`
- `src/BusWidthDetector.h` — `initHardware(bool stealth = false)` declaration
- `src/main.cpp` — re-enabled `detect()` call for experiment
