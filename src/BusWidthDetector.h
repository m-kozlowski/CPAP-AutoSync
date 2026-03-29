#ifndef BUS_WIDTH_DETECTOR_H
#define BUS_WIDTH_DETECTOR_H

#include <Arduino.h>
#include <driver/pulse_cnt.h>

// ============================================================================
// Detection result returned by detectBusWidth()
// ============================================================================
struct DetectionResult {
    int      busWidth;       // 0 = unknown/failed, 1 = 1-bit (AS10), 4 = 4-bit (AS11)
    uint16_t rca;            // Discovered RCA (0 if none found)
    uint8_t  cardState;      // SD card state from CMD13 R1 response [12:9]
    uint32_t sweepTimeMs;    // How long the RCA sweep took
    String   wifiSSID;       // Extracted from config.txt via stealth read (empty if failed)
};

// ============================================================================
// Early PCNT — lightweight standalone pulse counter for boot-time detection.
// Uses its own PCNT unit, separate from TrafficMonitor.
// Must be initialized as early as possible in setup(), torn down before
// TrafficMonitor.begin() is called.
// ============================================================================
namespace EarlyPCNT {
    void init(int gpio);     // Create PCNT unit on gpio, start counting immediately
    int  read();             // Read accumulated pulse count (non-destructive)
    void teardown();         // Delete PCNT unit to free resources
}

// ============================================================================
// BusWidthDetector — stealth SD card detection without CMD0
//
// Grabs MUX, brute-forces the RCA via bare-metal CMD13, determines bus width
// via CRC-based CMD17 probing, optionally reads config.txt via minimal FAT32
// sector reader, restores card state, and returns MUX to CPAP.
//
// All operations are read-only. No CMD0, no ACMD6, no writes, no timestamps.
// The CPAP's SD session is preserved transparently.
// ============================================================================
class BusWidthDetector {
public:
    static DetectionResult detect();
    static void stealthTest(); // Validate zero-CMD0 stealth: ISR mask + RCA sweep + sector read

private:
    // Low-level bare-metal SDMMC command dispatch (no ESP-IDF transaction API)
    static bool     initHardware(bool stealth = false);
    static void     deinitHardware();
    static bool     sendCmd(uint8_t cmdIdx, uint32_t arg, uint32_t flags, uint32_t* resp, uint32_t timeoutUs);
    static bool     sendCmd13(uint16_t rca, uint32_t* status);
    static bool     sendCmd7(uint16_t rca);
    static bool     sendCmd12();
    static bool     readSector(uint32_t sector, uint8_t* buf);

    // RCA brute-force sweep
    static uint16_t sweepRCA(uint32_t* outStatus);

    // Bus-width CRC probe (tries 1-bit then 4-bit)
    static int      probeBusWidth(uint16_t rca, uint8_t* sectorBuf);

    // Minimal FAT32 sector-level reader
    static bool     readConfigTxt(int busWidth, uint16_t rca, uint8_t* sectorBuf, String& outSSID);

    // Hardware bus-width configuration
    static void     setHostBusWidth(int bits);
    static void     setHostClock(int freqKHz);

    // Cleanup and MUX return
    static void     restoreAndRelease(uint16_t rca, uint8_t origState, bool wasSelected);
};

#endif // BUS_WIDTH_DETECTOR_H
