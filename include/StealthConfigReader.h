#ifndef STEALTH_CONFIG_READER_H
#define STEALTH_CONFIG_READER_H

#include <Arduino.h>

// Captured SD card state — saved before SD_MMC.begin() destroys it
struct SavedCardState {
    bool     valid;       // true if capture succeeded
    uint16_t rca;         // card's RCA (typically 0x1388)
    uint8_t  cardState;   // R1 bits [12:9]: 3=Standby, 4=Transfer, etc.
    uint8_t  busWidth;    // 1 or 4
};

// ============================================================================
// StealthConfigReader — reads config.txt from SD card WITHOUT sending CMD0.
//
// Uses the proven stealth sequence:
//   1. Grab MUX → stealth SDMMC init (no init clocks = no CMD0)
//   2. Mask ESP-IDF SDMMC ISR (INTMASK=0)
//   3. CMD13(0x1388) to verify card is alive and get state
//   4. CMD7 to select card if in Standby
//   5. Disable DMA, force 1-bit via ACMD6(0)
//   6. Minimal FAT32 reader: MBR → BPB → root dir → config.txt
//   7. Tri-state pins, deinit, release MUX to CPAP
//
// The CPAP never knows the card was accessed. No CMD0 is sent.
// Proven on both AirSense 10 and AirSense 11 (dev+27).
// ============================================================================
namespace StealthConfigReader {
    // readConfigTxt() — SUPERSEDED. Replaced by SDCardManager::takeControl() +
    // captureCardState()/restoreToSavedState() which achieves the same card-state
    // preservation without a custom FAT32 parser, and works for both AS10 and AS11.
    // String readConfigTxt();

    // Restores the SD card to Standby state after SD_MMC.end().
    // Re-inits SDMMC in stealth mode (no CMD0), verifies card at RCA 0x1388,
    // forces 1-bit mode, then deselects (CMD7(0)) → card in Standby.
    // Caller MUST still hold the MUX on ESP side (before releaseControl handoff).
    // Returns true if the card was successfully restored to Standby.
    bool restoreCardState();

    // Captures the card's current state (RCA, card state, bus width) via
    // stealth probe.  Called AFTER MUX switch to ESP but BEFORE SD_MMC.begin()
    // which would destroy the state with CMD0.
    // Bus width is detected empirically via a 4-bit sector read test.
    // Caller MUST hold the MUX on ESP side.
    bool captureCardState(SavedCardState* out);

    // Restores card to a previously captured state after SD_MMC.end().
    // Sets bus width via ACMD6 and selects/deselects to match saved state.
    // Caller MUST hold the MUX on ESP side.
    bool restoreToSavedState(const SavedCardState& saved);
}

#endif // STEALTH_CONFIG_READER_H
