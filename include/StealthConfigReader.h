#ifndef STEALTH_CONFIG_READER_H
#define STEALTH_CONFIG_READER_H

#include <Arduino.h>

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
    // Reads config.txt from the SD card via stealth mode.
    // Grabs MUX, does stealth init, reads FAT32, releases MUX.
    // Returns raw file content as a String, or empty string on failure.
    // Caller must NOT hold the MUX — this function manages MUX internally.
    String readConfigTxt();
}

#endif // STEALTH_CONFIG_READER_H
