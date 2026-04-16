# Issue Analysis: AS11 PCNT Detection Failure (Smart Mode Bypass)

## 1. Problem Description
Users of AirSense 11 (AS11) units occasionally report that the firmware incorrectly identifies their machine as an AirSense 10 (AS10), disabling "Smart Mode" and forcing "Scheduled Mode." 

Log analysis shows that during a `POWERON` reset, the `EarlyPCNT` counter sometimes records **0 pulses**, despite being on AS11 hardware. This causes the firmware to overwrite the NVS capability flag to `false`, a state that persists across subsequent reboots.

## 2. Log Analysis & Observations

### The "Successful" Boot (AS11 Detected)
*   **Boot #2104:** `Early PCNT: 2156`, `Post-stabilization: 8018`.
*   **Behavior:** The CPAP was actively scanning the card throughout the first 9 seconds of boot. This typically occurs when a card has an existing ResMed directory structure (`/DATALOG`) with many files, forcing a longer metadata scan by the CPAP.

### The "Failure" Boot (AS11 Missed)
*   **Boot #2106:** `Reset reason: Power-on reset`, `Early PCNT: 0 pulses`.
*   **Behavior:** The bus was silent for the entire 10-second detection window. 
*   **Context:** This occurred immediately after a "State Reset" and while the user was interacting with the AS11's internal menus to change the timezone.

## 3. Root Cause: The "Quiet Handshake" Race

The AirSense 11 uses DAT3 pulses for its SD card handshake (CMD0/ACMD41/etc.). However, several factors can make this window too narrow for the ESP32 to catch:

1.  **Filesystem Complexity:** A "Blank Card" (no ResMed folders) reduces the AS11's handshake time from seconds to milliseconds. The CPAP reads the MBR, sees the root is empty, and stops bus activity before the ESP32 has even finished its internal bootloader.
2.  **Menu Suppression:** If a user is actively in the AS11 Settings menu (e.g., Timezone), the CPAP firmware may delay its SD card mount until the menu is exited. If this delay exceeds the ESP32's 10-second `EarlyPCNT` window, the pulses are missed.
3.  **Floating MUX Jitter:** During the first ~400ms of ESP32 boot, the `SD_SWITCH_PIN` is floating (High-Z). If it jitter-triggers the MUX to the ESP side during the CPAP's initial handshake, the CPAP sees an error and gives up, resulting in 0 pulses.
4.  **Power Discontinuity:** An ESP-only power-on reset (while the CPAP remains powered) means the CPAP doesn't re-initialize the card. In this scenario, the ESP32 correctly sees 0 pulses, but incorrectly concludes that the hardware is non-capable.

## 4. Technical Recommendations

### A. Earliest Possible Start (Static Constructor)
Implement a C++ `__attribute__((constructor))` in `main.cpp`. This runs before the Arduino framework and even before `app_main()`.
*   **Immediate MUX Lock:** Explicitly drive `SD_SWITCH_PIN` to `SD_SWITCH_CPAP_VALUE` (HIGH) in the constructor to eliminate "floating pin" jitter.
*   **Immediate Counter Start:** Call `EarlyPCNT::init()` here to capture the first ~500ms of boot currently lost to framework overhead.

### B. Extended Integration Window
Modify `setup()` to keep the `EarlyPCNT` counter running throughout the **8s Stabilization** and **5s Smart Wait** cycles. Do not call `teardown()` until the FSM officially transitions to `ACQUIRING` for the first time.

### C. "Sticky-True" Capability Flag
Refine the NVS persistence logic:
*   **Promotion Only:** If `pulses > 0`, set `capable = true` in NVS. 
*   **Preservation:** If `pulses == 0` during a `POWERON` reset, check NVS. If it was *ever* `true`, keep it `true`. 
*   **Reasoning:** An AS11 can be "quiet," but an AS10 can never be "noisy." Real pulses are a definitive positive; silence is an ambiguous negative.

### D. Self-Noise Awareness
Acknowledge that the ESP32's own SDMMC transfers generate pulses on the `CS_SENSE` pin. While this explains later log activity (like `idle=726449ms`), it is not the cause of the boot-time detection failure because the `EarlyPCNT` window currently closes before the ESP32 touches the card.

## 5. Summary of Proposed Workflow
1.  **Power On:** MUX locks to CPAP and Counter starts (Microseconds after boot).
2.  **10-15s Window:** Firmware stabilizes while the counter accumulates any CPAP activity.
3.  **Final Decision:** If pulses happened *at any point*, Smart Mode is enabled. If the device was *ever* Smart Mode capable in the past, it stays enabled.
