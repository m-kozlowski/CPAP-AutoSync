# Early-Boot PCNT Pulse Counting for AS10 vs AS11 Detection

> **Predecessors**: [61-NEW-PLAN-FOR-AS10.md](61-NEW-PLAN-FOR-AS10.md) — Stealth RCA Discovery | [62-DETECT-BUS-WIDTH.md](62-DETECT-BUS-WIDTH.md) — CRC-Based Bus-Width Detection

## Core Insight

When a CPAP machine powers on (or the SD card is inserted), the CPAP provides power to
the SD-WIFI-PRO board via the SD slot's VCC pin. This causes the ESP32 to boot from a
**power-on reset** (`ESP_RST_POWERON`, raw Core0 reason = 1).

At the **exact same time** the ESP32 is booting, the CPAP is initializing the SD card.
During this initialization window:

| Machine | SD Init Behavior | DAT3 (CS_SENSE, GPIO 33) | PCNT Pulses |
|---------|-----------------|--------------------------|-------------|
| **AS11** | Negotiates 4-bit mode successfully | Toggles rapidly during data transfers | **Many** (hundreds to thousands) |
| **AS10** | Attempts 4-bit, falls back to 1-bit | Brief activity during negotiation attempt, then silence | **Few or zero** |
| **Card Reader** | May or may not init | Varies | Varies |

The ESP32's PCNT peripheral can count edges on GPIO 33 **in hardware**, with zero CPU
overhead. If we start PCNT counting at the **earliest possible moment** during boot — before
any delays, before Serial.begin(), before anything — the accumulated pulse count by the
time we reach the electrical stabilization delay will reveal whether the CPAP is using
1-bit or 4-bit mode.

---

## Why This Works

### The Race Condition Is Our Friend

The ESP32 boot sequence takes ~300-500ms from power-on to reaching `setup()`. The CPAP's
SD card initialization takes a similar or longer time. This means:

1. ESP32 powers on → hardware boots → enters `setup()`
2. Meanwhile, CPAP sends CMD0, CMD8, ACMD41, CMD2, CMD3, ACMD6... on the SD bus
3. AS11 begins 4-bit data transfers → DAT3 toggles → PCNT counts
4. AS10 falls back to 1-bit → DAT3 goes silent → PCNT stays near zero

By the time the ESP32 reaches the 8-second electrical stabilization delay, the CPAP has
already settled into its operating bus mode. The PCNT count accumulated during this
window is a passive, non-invasive fingerprint of the machine type.

### No MUX Grab Required

This detection happens **while MUX = CPAP**. The ESP32 is only passively listening on
GPIO 33 (CS_SENSE), which is on the host side of the MUX and always connected. The CPAP
has no idea we're counting its pulses.

### Complements the CRC-Based Probe

This PCNT-based detection runs *before* the CRC-based bus-width probe (doc 62). Together:

| Method | When | Requires MUX? | Invasive? | Reliability |
|--------|------|---------------|-----------|-------------|
| **Early PCNT** | During boot (passive) | No | No | High for fresh power-on; needs field validation |
| **CRC Probe** | After stabilization (active) | Yes | Stealth, but active | High (deterministic CRC match) |

If early PCNT gives a clear signal, the CRC probe can be skipped or used as confirmation.
If the boot was a software reset (not power-on), early PCNT may miss the init window
and the CRC probe becomes the primary method.

---

## Implementation

### PCNT Setup (Earliest Possible)

The PCNT unit must be initialized **before** any blocking operations in `setup()`:

```
setup() {
    setCpuFrequencyMhz(80);          // CPU throttle (non-blocking)
    // ... BT memory release ...
    
    // GPIO 33 setup (already done for MUX safety)
    rtc_gpio_hold_dis(CS_SENSE);
    rtc_gpio_deinit(CS_SENSE);
    pinMode(CS_SENSE, INPUT);
    
    // === EARLY PCNT START (new) ===
    earlyPcntInit(CS_SENSE);          // ~50μs, hardware-only
    
    // ... Serial.begin, logging, etc ...
    
    // Log pulse count BEFORE stabilization delay
    int preDelayPulses = earlyPcntRead();
    LOG("===EXPERIMENTAL=== Pre-stabilization PCNT: %d pulses", preDelayPulses);
    
    delay(8000);  // Electrical stabilization
    
    // Log pulse count AFTER stabilization delay
    int postDelayPulses = earlyPcntRead();
    LOG("===EXPERIMENTAL=== Post-stabilization PCNT: %d pulses", postDelayPulses);
    
    // ... Smart Wait, Bus Width Detection, etc ...
}
```

### PCNT Configuration

- **GPIO**: 33 (CS_SENSE) — hardwired to DAT3 on host side of MUX
- **Edge action**: Count on both rising AND falling edges (maximum sensitivity)
- **Glitch filter**: 125ns (filters sub-100ns electrical noise)
- **Counter range**: 0 to 32767 (int16 max, sufficient for boot window)
- **Sampling**: Read accumulated count at key checkpoints, no periodic polling needed

### Separate From TrafficMonitor

This early PCNT uses a **separate PCNT unit** from the TrafficMonitor (which is
initialized later for ongoing idle detection). The ESP32 has multiple PCNT units
(up to 8 on IDF 5.x), so this doesn't conflict. The early unit is torn down after
the boot detection phase completes and before TrafficMonitor.begin() claims its own unit.

---

## Expected Results (To Be Validated By Field Testing)

### Power-On Boot (ESP_RST_POWERON)

| Scenario | Pre-Stabilization Pulses | Post-Stabilization Pulses | Interpretation |
|----------|-------------------------|--------------------------|----------------|
| **AS11 in therapy** | 100–10000+ | Same or higher | 4-bit mode confirmed |
| **AS10 in therapy** | 0–50 | 0–50 | 1-bit mode (DAT3 idle) |
| **Card reader** | 0–varies | 0–varies | No CPAP, or brief init burst |
| **CPAP off** | 0 | 0 | No host activity |

### Software Reset (ESP_RST_SW)

On software reboot, the CPAP has already finished initialization. PCNT will only
capture ongoing data transfer activity (if any), not the init burst. Results are
less definitive for AS10 detection but still useful for AS11 (which has continuous
DAT3 activity during therapy).

---

## Decision Logic (Future)

Once field data confirms the thresholds:

```
if (resetReason == ESP_RST_POWERON) {
    int pulses = earlyPcntRead();
    if (pulses > THRESHOLD_4BIT) {
        // High confidence: AS11 (4-bit mode)
        detectedMode = BUS_4BIT;
    } else if (pulses < THRESHOLD_1BIT) {
        // High confidence: AS10 (1-bit mode) or no CPAP
        // Use CRC probe to distinguish AS10 from card reader
        detectedMode = BUS_1BIT_OR_UNKNOWN;
    } else {
        // Ambiguous — fall through to CRC probe
        detectedMode = UNKNOWN;
    }
}
```

Thresholds will be determined from field testing logs. The `===EXPERIMENTAL===` log
tags make it easy to grep for these values across multiple test reports.

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| ESP32 boots too late to catch init burst | Miss AS11 detection on power-on | CRC probe as fallback |
| PCNT unit conflicts with TrafficMonitor | One of them fails to initialize | Use separate PCNT units; tear down early unit before TrafficMonitor.begin() |
| Threshold varies between SD cards | False classification | Collect data from multiple users; use wide margins; CRC probe as tiebreaker |
| Software reset misses init window | No early PCNT data | Already handled: skip early PCNT logic on non-POWERON resets |
| AS10 has brief 4-bit negotiation burst | Small pulse count could be misread as 4-bit | Set 4-bit threshold high enough to exclude brief bursts; CRC probe confirms |

---

## Relationship to Existing Detection

This early PCNT approach is **additive**, not a replacement:

1. **Early PCNT** (passive, boot-time) → quick hint about bus mode
2. **CRC Probe** (active, post-stabilization) → deterministic confirmation
3. **NVS Cache** (persistent) → skip both on subsequent boots

The firmware uses all available signals to build confidence in the detection result
before committing it to NVS cache.
