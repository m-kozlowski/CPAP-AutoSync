# 64 — Stealth Mode Experiment: Definitive Results

**Status**: CONCLUDED — Not viable for production  
**Builds**: dev+20 through dev+25  
**Devices tested**: 3 (1× AS11, 2× AS10 from different users)  
**Total test boots**: 8

## Background

The goal was to determine whether the ESP32 can read the CPAP's SD card **without sending CMD0** (GO_IDLE_STATE), which resets the card and disrupts the CPAP's active session. If viable, this "stealth mode" would allow reading `config.txt` without the CPAP ever knowing we touched its card.

The approach: after the CPAP finishes using the SD card, grab the MUX, initialize the SDMMC peripheral without CMD0, brute-force the card's RCA via CMD13 (SEND_STATUS), then read sector 0 via CMD17.

## Test Architecture

**`stealthTest()` in `BusWidthDetector.cpp`** runs three phases:

| Phase | What it tests | How |
|-------|--------------|-----|
| **Phase 1** | True stealth RCA discovery | Mask ISR, sweep CMD13 across all 65535 RCAs |
| **Phase 1b** | Stealth sector read | CMD7 select + CMD17 read sector 0 |
| **Phase 2** | Positive control | `sdmmc_card_init()` + bare-metal CMD13 |
| **Phase 3a** | MUX isolation | MUX round-trip, SDMMC stays running |
| **Phase 3b** | Stealth init isolation | Full deinit + `initHardware(stealth)` |
| **Phase 3c** | CMD0 isolation | Full deinit + `initHardware(normal)` with init clocks |

Phase 1 runs FIRST (before any CMD0 is sent) to preserve the CPAP's card state.

## Results Summary

### Phase 3 Isolation (100% consistent across all 8 boots)

| Test | Result | Meaning |
|------|--------|---------|
| **3a** (MUX only) | ✅ PASS | MUX round-trip does NOT kill the card |
| **3b** (stealth init) | ✅ PASS | SDMMC deinit + reinit without CMD0 is safe |
| **3c** (normal init) | ❌ FAIL | Init clocks (which secretly send CMD0) kill the card |

### Phase 1 Stealth RCA Discovery

| Device | Boot type | Phase 1 | Notes |
|--------|-----------|---------|-------|
| AS11 (yours) | Cold boot | ❌ FAIL | CPAP inits card (710 PCNT pulses) but resets it |
| AS11 (yours) | Soft-reset (<15s) | ✅ PASS | Card retains RCA 0x1388 from previous Phase 2 |
| AS11 (yours) | Soft-reset (>56s) | ❌ FAIL | CPAP periodic housekeeping resets card |
| AS10 User 1 | Cold boot | ❌ FAIL | CPAP never inits card (PCNT=0) |
| AS10 User 1 | Soft-reset | ❌ FAIL | CPAP sends CMD0 on bus re-acquire |
| AS10 User 2 | Cold boot | ❌ FAIL | Same as User 1 |
| AS10 User 2 | Soft-reset | ❌ FAIL | Same as User 1 |

**Stealth RCA success rate**: 2/8 boots (25%), both on AS11 with <15s timing window.

## Key Technical Findings

### 1. ISR Masking Fix (dev+20)
Setting `SDMMC.intmask.val = 0` after `sdmmc_host_init()` prevents the ESP-IDF SDMMC ISR from consuming CMD_DONE interrupts. Bare-metal CMD13 then works by polling `RINTSTS` (raw interrupt status register).

### 2. Hidden CMD0 Bug (dev+22)
The `initHardware()` "80 init clocks" command used `sdmmc_hw_cmd_t` which zero-initializes to `cmd_index = 0` (CMD0). The DWC SDMMC controller sends init clocks *then* executes the command — so it was secretly sending CMD0 every time, resetting the card. Fixed with a `stealth` parameter that skips init clocks.

### 3. RCA Is Always 0x1388
All three devices produce RCA 0x1388 (decimal 5000) via `sdmmc_card_init()`. This is a hardware-determined value from the card's CMD3 response. The sweep finds it at position 5000 in ~3.3 seconds.

### 4. Sweep Timeout at RCA 40960
Not significant — this is simply the sweep speed limit. At 400kHz, CMD13 takes ~0.76ms per RCA. In the 30-second timeout, ~40000 RCAs can be tested.

### 5. Ghost Responses Are Electrical Noise
All ghost responses show `resp=0x00000000` with `state=0`. These are not real card responses — they're electrical noise on floating data lines that passes through the RTO detection but fails the state check.

### 6. Read Failures (DCRC)
Phase 1b reads failed with `rintsts=0x0002008C` (DCRC = Data CRC Error). Cause: the normal boot sequence from the previous boot configured the card for 4-bit mode, but our stealth read attempted 1-bit mode. The CRC is computed per-line in 4-bit mode, causing a mismatch when read in 1-bit mode. DMA also needed to be disabled (`ctrl.use_internal_dma = 0`) since `sdmmc_host_init()` enables IDMAC by default.

## Why Stealth Fails

### AS10 CPAPs
- **Cold boot**: PCNT shows 0 pulses — the CPAP either never initializes the card at boot, or uses SPI mode (no RCA concept in SPI mode). Either way, the card has no RCA to find.
- **Soft-reset**: After we initialize the card (Phase 2, RCA 0x1388) and release to CPAP, the CPAP immediately sends CMD0 when it re-acquires the bus, wiping the RCA. Confirmed on both AS10 units.

### AS11 CPAPs
- **Cold boot**: CPAP initializes the card (710 PCNT pulses on DAT3 = 4-bit mode activity), but sends CMD0 before going idle. The card loses its RCA.
- **Soft-reset**: Time-dependent. If <15s since SD release, the card may retain its RCA (CPAP hasn't done housekeeping yet). If >56s, the CPAP's periodic housekeeping resets the card.

### Root Cause
The CPAP controls the card's lifecycle. By the time we grab the MUX, the card is always in Idle state (no RCA) because the CPAP has sent CMD0. There is no reliable timing window where the card has an active RCA that we can discover.

## Conclusion

**Stealth mode is NOT viable for production use.**

The bare-metal stealth mechanism itself works perfectly:
- ISR masking ✅
- Stealth init (no CMD0) ✅
- CMD13 sweep ✅
- MUX safety ✅

But the precondition — that the card has an active RCA when we grab the MUX — is never reliably met. Both AS10 and AS11 CPAPs reset the card's state, making the RCA unrecoverable without sending CMD0.

**The current production approach is correct**: grab MUX → `sdmmc_card_init()` (sends CMD0, full initialization) → read config → release MUX. The CPAP handles the card re-initialization gracefully when it next needs the card.

## Files Modified (Experimental)

- `src/BusWidthDetector.cpp` — `stealthTest()`, `initHardware(bool stealth)`
- `src/BusWidthDetector.h` — `initHardware(bool stealth = false)` declaration
- `src/main.cpp` — re-enabled `detect()` call for experiment

**Cleanup**: Remove `stealthTest()`, revert `initHardware()` stealth parameter, restore production `detect()` stub.
