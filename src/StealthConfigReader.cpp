#include "StealthConfigReader.h"
#include "pins_config.h"
#include "Logger.h"
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include "soc/sdmmc_reg.h"
#include "soc/sdmmc_struct.h"
#include <driver/gpio.h>

// ============================================================================
// ESP32 SDMMC register access via the memory-mapped struct
// ============================================================================
#define SDMMC_FIFO_ADDR  (DR_REG_SDMMC_BASE + 0x200)
#define SDMMC_FIFO       (*(volatile uint32_t*)SDMMC_FIFO_ADDR)

// Interrupt status bits
#define SCR_INT_CMD_DONE   BIT(2)
#define SCR_INT_DTO        BIT(3)   // Data Transfer Over
#define SCR_INT_RXDR       BIT(5)   // RX FIFO Data Ready
#define SCR_INT_RCRC       BIT(6)   // Response CRC error
#define SCR_INT_DCRC       BIT(7)   // Data CRC error
#define SCR_INT_RTO        BIT(8)   // Response Timeout
#define SCR_INT_DRTO       BIT(9)   // Data Read Timeout
#define SCR_INT_HTO        BIT(10)  // Data starvation Host Timeout
#define SCR_INT_FRUN       BIT(11)  // FIFO underrun/overrun
#define SCR_INT_HLE        BIT(12)  // Hardware Locked write Error
#define SCR_INT_SBE        BIT(13)  // Start Bit Error
#define SCR_INT_EBE        BIT(15)  // End Bit Error

#define SCR_CMD_ERR_FLAGS  (SCR_INT_RTO | SCR_INT_RCRC | SCR_INT_HLE)
#define SCR_DATA_ERR_FLAGS (SCR_INT_DCRC | SCR_INT_DRTO | SCR_INT_SBE | SCR_INT_EBE | SCR_INT_HTO | SCR_INT_FRUN)

// SD card states (R1 response bits [12:9])
static const char* SCR_SD_STATE_NAMES[] = {
    "Idle", "Ready", "Ident", "Stby", "Tran", "Data", "Rcv", "Prg", "Dis"
};
#define SCR_SD_STATE_NAME(s) ((s) < 9 ? SCR_SD_STATE_NAMES[s] : "Unknown")

// Maximum config.txt size we'll read (4 sectors = 2KB, more than enough)
static constexpr int MAX_CONFIG_SECTORS = 4;
static constexpr int MAX_CONFIG_BYTES = MAX_CONFIG_SECTORS * 512;

// ============================================================================
// Low-level bare-metal SDMMC helpers (static, file-scoped)
// ============================================================================

static bool scrSendCmd(uint8_t cmdIdx, uint32_t arg, uint32_t extraFlags,
                       uint32_t* resp, uint32_t timeoutUs)
{
    SDMMC.rintsts.val = 0xFFFFFFFF;
    SDMMC.cmdarg = arg;

    sdmmc_hw_cmd_t hw = {};
    hw.cmd_index      = cmdIdx;
    hw.card_num       = SDMMC_HOST_SLOT_1;
    hw.use_hold_reg   = 1;
    hw.wait_complete  = 1;
    hw.start_command  = 1;
    uint32_t cmdVal   = *(uint32_t*)&hw;
    cmdVal           |= extraFlags;
    *(volatile uint32_t*)&SDMMC.cmd = cmdVal;

    uint32_t t0 = (uint32_t)esp_timer_get_time();
    while (SDMMC.cmd.start_command) {
        if (((uint32_t)esp_timer_get_time() - t0) > timeoutUs) return false;
    }

    while (true) {
        uint32_t sts = SDMMC.rintsts.val;
        if (sts & (SCR_INT_CMD_DONE | SCR_CMD_ERR_FLAGS)) {
            if (resp) *resp = SDMMC.resp[0];
            return (sts & SCR_INT_CMD_DONE) && !(sts & SCR_CMD_ERR_FLAGS);
        }
        if (((uint32_t)esp_timer_get_time() - t0) > timeoutUs) return false;
    }
}

static bool scrSendCmd13(uint16_t rca, uint32_t* status) {
    uint32_t flags = (1 << 6) | (1 << 8);  // response_expect, check_crc
    return scrSendCmd(13, (uint32_t)rca << 16, flags, status, 5000);
}

static bool scrSendCmd7(uint16_t rca) {
    uint32_t flags = (1 << 6) | (1 << 8);
    uint32_t resp = 0;
    return scrSendCmd(7, (uint32_t)rca << 16, flags, &resp, 50000);
}

static bool scrSendCmd12() {
    uint32_t flags = (1 << 6) | (1 << 14);  // response_expect + stop_abort_cmd
    uint32_t resp = 0;
    return scrSendCmd(12, 0, flags, &resp, 50000);
}

static bool scrReadSector(uint32_t sector, uint8_t* buf) {
    SDMMC.blksiz.block_size = 512;
    SDMMC.bytcnt = 512;

    // Reset FIFO
    SDMMC.ctrl.fifo_reset = 1;
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    while (SDMMC.ctrl.fifo_reset) {
        if (((uint32_t)esp_timer_get_time() - t0) > 10000) return false;
    }

    SDMMC.rintsts.val = 0xFFFFFFFF;
    SDMMC.cmdarg = sector;

    // CMD17 READ_SINGLE_BLOCK
    sdmmc_hw_cmd_t hw = {};
    hw.cmd_index       = 17;
    hw.response_expect = 1;
    hw.check_response_crc = 1;
    hw.data_expected   = 1;
    hw.wait_complete   = 1;
    hw.card_num        = SDMMC_HOST_SLOT_1;
    hw.use_hold_reg    = 1;
    hw.start_command   = 1;
    *(volatile uint32_t*)&SDMMC.cmd = *(uint32_t*)&hw;

    t0 = (uint32_t)esp_timer_get_time();
    while (SDMMC.cmd.start_command) {
        if (((uint32_t)esp_timer_get_time() - t0) > 100000) return false;
    }

    uint32_t sts = SDMMC.rintsts.val;
    if (sts & SCR_CMD_ERR_FLAGS) return false;

    uint32_t* buf32 = (uint32_t*)buf;
    uint32_t wordsRead = 0;
    const uint32_t wordsNeeded = 128;
    t0 = (uint32_t)esp_timer_get_time();

    while (wordsRead < wordsNeeded) {
        sts = SDMMC.rintsts.val;
        if (sts & SCR_DATA_ERR_FLAGS) return false;

        uint32_t fifoCount = SDMMC.status.fifo_count;
        while (fifoCount > 0 && wordsRead < wordsNeeded) {
            buf32[wordsRead++] = SDMMC_FIFO;
            fifoCount--;
        }

        if (sts & SCR_INT_DTO) break;

        if (((uint32_t)esp_timer_get_time() - t0) > 500000) return false;
    }

    // Wait for DTO
    if (!(SDMMC.rintsts.val & SCR_INT_DTO)) {
        t0 = (uint32_t)esp_timer_get_time();
        while (!(SDMMC.rintsts.val & SCR_INT_DTO)) {
            if (SDMMC.rintsts.val & SCR_DATA_ERR_FLAGS) return false;
            if (((uint32_t)esp_timer_get_time() - t0) > 100000) return false;
        }
    }

    sts = SDMMC.rintsts.val;
    if (sts & (SCR_INT_DCRC | SCR_INT_SBE | SCR_INT_EBE)) return false;

    return (wordsRead == wordsNeeded);
}

// ============================================================================
// Host bus configuration helpers
// ============================================================================

static void scrSetHostBusWidth(int bits) {
    if (bits == 4)
        SDMMC.ctype.card_width |= BIT(SDMMC_HOST_SLOT_1);
    else
        SDMMC.ctype.card_width &= ~BIT(SDMMC_HOST_SLOT_1);
}

static void scrSetHostClock(int freqKHz) {
    sdmmc_host_set_card_clk(SDMMC_HOST_SLOT_1, freqKHz);
    delayMicroseconds(100);
}

// ============================================================================
// Hardware init/deinit
// ============================================================================

static bool scrInitHardware() {
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOG_ERRORF("[Stealth] sdmmc_host_init failed: 0x%x", err);
        return false;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk   = (gpio_num_t)SD_CLK_PIN;
    slot.cmd   = (gpio_num_t)SD_CMD_PIN;
    slot.d0    = (gpio_num_t)SD_D0_PIN;
    slot.d1    = (gpio_num_t)SD_D1_PIN;
    slot.d2    = (gpio_num_t)SD_D2_PIN;
    slot.d3    = (gpio_num_t)SD_D3_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) {
        LOG_ERRORF("[Stealth] sdmmc_host_init_slot failed: 0x%x", err);
        sdmmc_host_deinit();
        return false;
    }

    scrSetHostBusWidth(1);
    scrSetHostClock(400);
    delay(10);

    // Stealth mode: skip init clocks (which secretly send CMD0)
    LOG_INFO("[Stealth] Stealth init: skipping init clocks (would send CMD0)");
    delay(50);

    return true;
}

static void scrDeinitHardware() {
    scrSetHostBusWidth(1);
    sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_1);
    sdmmc_host_deinit();
}

// ============================================================================
// Cleanup: tri-state pins + release MUX
// ============================================================================

static void scrCleanupAndReleaseMux() {
    const int sdPins[] = { SD_CMD_PIN, SD_CLK_PIN, SD_D0_PIN,
                           SD_D1_PIN, SD_D2_PIN, SD_D3_PIN };
    for (int pin : sdPins) {
        gpio_reset_pin((gpio_num_t)pin);
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    }

    scrDeinitHardware();

    // Return MUX to CPAP
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
    delay(5);
    LOG_INFO("[Stealth] MUX returned to CPAP.");
}

// ============================================================================
// FAT32 helpers
// ============================================================================

static uint32_t readLE16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t readLE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

// ============================================================================
// readConfigTxt — the main entry point
// ============================================================================

String StealthConfigReader::readConfigTxt() {
    LOG_INFO("[Stealth] === Stealth config.txt read ===");

    // ── Step 0: Grab MUX ──
    LOG_INFO("[Stealth] Grabbing SD MUX...");
    pinMode(SD_SWITCH_PIN, OUTPUT);
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_ESP_VALUE);
    delay(200);

    // ── Step 1: Stealth hardware init (no CMD0) ──
    if (!scrInitHardware()) {
        LOG_ERROR("[Stealth] Hardware init failed");
        digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
        return "";
    }

    // ── Step 2: Mask SDMMC ISR so polling works ──
    uint32_t savedIntMask = SDMMC.intmask.val;
    SDMMC.intmask.val = 0;

    // ── Step 3: CMD13(0x1388) — check card is alive ──
    const uint16_t KNOWN_RCA = 0x1388;
    uint32_t r1 = 0;
    bool ok = scrSendCmd13(KNOWN_RCA, &r1);
    uint8_t cardState = (r1 >> 9) & 0x0F;

    if (!ok || cardState < 3 || r1 == 0) {
        // Try a few nearby RCAs before giving up
        LOG_WARNF("[Stealth] CMD13(0x%04X) failed (ok=%d state=%d) — scanning nearby...",
                  KNOWN_RCA, ok, cardState);
        bool found = false;
        uint16_t scanRanges[][2] = { {0x0001, 0x0020}, {0x1380, 0x1398} };
        for (auto& range : scanRanges) {
            for (uint16_t rca = range[0]; rca <= range[1] && !found; rca++) {
                uint32_t r = 0;
                if (scrSendCmd13(rca, &r)) {
                    uint8_t s = (r >> 9) & 0x0F;
                    if (s >= 3 && r != 0) {
                        // Confirm
                        uint32_t r2 = 0;
                        if (scrSendCmd13(rca, &r2) && ((r2 >> 9) & 0x0F) >= 3 && r2 != 0) {
                            LOG_INFOF("[Stealth] Found RCA 0x%04X via scan, state=%d", rca, (r2 >> 9) & 0x0F);
                            r1 = r2;
                            cardState = (r2 >> 9) & 0x0F;
                            found = true;
                        }
                    }
                }
            }
        }
        if (!found) {
            LOG_ERROR("[Stealth] Card not responding — stealth read failed");
            SDMMC.intmask.val = savedIntMask;
            scrCleanupAndReleaseMux();
            return "";
        }
    } else {
        LOG_INFOF("[Stealth] CMD13(0x%04X): state=%d(%s) — card alive",
                  KNOWN_RCA, cardState, SCR_SD_STATE_NAME(cardState));
    }

    // ── Step 4: Disable DMA (IDMAC drains FIFO before our polling loop) ──
    SDMMC.ctrl.use_internal_dma = 0;
    SDMMC.bmod.enable = 0;

    // ── Step 5: Select card if in Standby ──
    if (cardState == 3) {
        LOG_INFO("[Stealth] Card in Standby — selecting with CMD7...");
        if (!scrSendCmd7(KNOWN_RCA)) {
            LOG_ERROR("[Stealth] CMD7 SELECT failed");
            SDMMC.intmask.val = savedIntMask;
            scrCleanupAndReleaseMux();
            return "";
        }
        delay(2);
    }

    // ── Step 6: Force card to 1-bit via ACMD6(0) ──
    {
        uint32_t resp55 = 0, resp6 = 0;
        uint32_t acmdFlags = (1u << 6) | (1u << 8) | (1u << 13) | (1u << 29);
        scrSendCmd(55, (uint32_t)KNOWN_RCA << 16, acmdFlags, &resp55, 50000);
        scrSendCmd(6, 0, acmdFlags, &resp6, 50000);
    }
    scrSetHostBusWidth(1);
    scrSetHostClock(400);

    // ── Step 7: Read sector 0 (MBR or VBR) ──
    uint8_t sec[512] __attribute__((aligned(4)));
    memset(sec, 0, sizeof(sec));

    // Try read probes: 1-bit@400kHz first, then escalate
    struct { int bits; int khz; const char* label; } readProbes[] = {
        { 1,   400, "1-bit@400kHz" },
        { 1, 25000, "1-bit@25MHz"  },
        { 4,   400, "4-bit@400kHz" },
        { 4, 25000, "4-bit@25MHz"  },
    };

    int workingBits = 0;
    int workingKhz = 0;
    bool readOK = false;

    for (auto& p : readProbes) {
        scrSetHostBusWidth(p.bits);
        scrSetHostClock(p.khz);

        if (scrReadSector(0, sec)) {
            bool validSig = (sec[510] == 0x55 && sec[511] == 0xAA);
            LOG_INFOF("[Stealth] Sector 0 read OK (%s), boot sig %s",
                      p.label, validSig ? "valid" : "MISSING");
            workingBits = p.bits;
            workingKhz = p.khz;
            readOK = true;
            break;
        } else {
            scrSendCmd12();
            delay(2);
            SDMMC.rintsts.val = 0xFFFFFFFF;
            SDMMC.ctrl.fifo_reset = 1;
            while (SDMMC.ctrl.fifo_reset) {}
            SDMMC.ctrl.dma_reset = 1;
            while (SDMMC.ctrl.dma_reset) {}
        }
    }

    if (!readOK) {
        LOG_ERROR("[Stealth] All read probes failed — cannot read config.txt");
        // Restore card to standby if we selected it
        if (cardState == 3) scrSendCmd7(0);
        SDMMC.intmask.val = savedIntMask;
        scrCleanupAndReleaseMux();
        return "";
    }

    // ── Step 8: FAT32 parse — MBR → BPB → root dir → config.txt ──
    // Configure to working speed for data reads
    scrSetHostBusWidth(workingBits);
    scrSetHostClock(workingKhz);

    // Determine if this is MBR or VBR
    uint32_t vbrSector = 0;
    bool isFAT = (memcmp(sec + 0x36, "FAT", 3) == 0) ||
                 (memcmp(sec + 0x52, "FAT", 3) == 0);
    if (!isFAT) {
        if (sec[510] != 0x55 || sec[511] != 0xAA) {
            LOG_ERROR("[Stealth] No valid MBR/VBR boot signature");
            if (cardState == 3) scrSendCmd7(0);
            SDMMC.intmask.val = savedIntMask;
            scrCleanupAndReleaseMux();
            return "";
        }
        vbrSector = readLE32(sec + 0x1C6);
        LOG_INFOF("[Stealth] MBR: Partition 1 at sector %u", vbrSector);

        if (!scrReadSector(vbrSector, sec)) {
            LOG_ERROR("[Stealth] Failed to read VBR");
            scrSendCmd12();
            if (cardState == 3) scrSendCmd7(0);
            SDMMC.intmask.val = savedIntMask;
            scrCleanupAndReleaseMux();
            return "";
        }
    }

    // Parse BPB
    uint16_t bytesPerSector  = readLE16(sec + 0x0B);
    uint8_t  sectorsPerClust = sec[0x0D];
    uint16_t reservedSectors = readLE16(sec + 0x0E);
    uint8_t  numFATs         = sec[0x10];
    uint16_t rootEntryCount  = readLE16(sec + 0x11);
    uint32_t fatSize         = readLE16(sec + 0x16);
    if (fatSize == 0) fatSize = readLE32(sec + 0x24);
    uint32_t rootCluster     = readLE32(sec + 0x2C);

    if (bytesPerSector != 512 || sectorsPerClust == 0) {
        LOG_ERRORF("[Stealth] Unsupported BPB: bps=%u spc=%u", bytesPerSector, sectorsPerClust);
        if (cardState == 3) scrSendCmd7(0);
        SDMMC.intmask.val = savedIntMask;
        scrCleanupAndReleaseMux();
        return "";
    }

    uint32_t fatStart    = vbrSector + reservedSectors;
    uint32_t dataStart   = fatStart + (numFATs * fatSize);
    uint32_t rootDirSectors = ((rootEntryCount * 32) + 511) / 512;
    uint32_t firstDataSector = dataStart + rootDirSectors;
    bool isFAT32 = (rootEntryCount == 0);

    auto clusterToSector = [&](uint32_t cluster) -> uint32_t {
        return firstDataSector + (cluster - 2) * sectorsPerClust;
    };

    // Scan root directory for "CONFIG  TXT"
    uint32_t configCluster = 0;
    uint32_t configSize = 0;
    bool found = false;

    uint32_t dirSector = isFAT32 ? clusterToSector(rootCluster) : dataStart;

    for (int s = 0; s < 16 && !found; s++) {
        if (!scrReadSector(dirSector + s, sec)) {
            scrSendCmd12();
            break;
        }
        for (int e = 0; e < 16 && !found; e++) {
            uint8_t* entry = sec + (e * 32);
            if (entry[0] == 0x00) { goto dir_done; }
            if (entry[0] == 0xE5) continue;
            if (entry[11] & 0x08) continue;
            if (entry[11] & 0x10) continue;

            if (memcmp(entry, "CONFIG  TXT", 11) == 0) {
                uint16_t clHi = readLE16(entry + 0x14);
                uint16_t clLo = readLE16(entry + 0x1A);
                configCluster = ((uint32_t)clHi << 16) | clLo;
                configSize = readLE32(entry + 0x1C);
                found = true;
                LOG_INFOF("[Stealth] Found config.txt: cluster=%u size=%u", configCluster, configSize);
            }
        }
    }
dir_done:

    if (!found || configCluster < 2 || configSize == 0) {
        LOG_WARN("[Stealth] config.txt not found in root directory");
        if (cardState == 3) scrSendCmd7(0);
        SDMMC.intmask.val = savedIntMask;
        scrCleanupAndReleaseMux();
        return "";
    }

    // ── Step 9: Read config.txt content (up to MAX_CONFIG_BYTES) ──
    uint32_t bytesToRead = configSize;
    if (bytesToRead > (uint32_t)MAX_CONFIG_BYTES) {
        LOG_WARNF("[Stealth] config.txt is %u bytes, truncating to %d", configSize, MAX_CONFIG_BYTES);
        bytesToRead = MAX_CONFIG_BYTES;
    }

    uint32_t sectorsToRead = (bytesToRead + 511) / 512;
    uint32_t fileSector = clusterToSector(configCluster);

    // Allocate buffer for the full file content
    // Using stack allocation for small files (typical config is <1KB)
    char fileBuf[MAX_CONFIG_BYTES + 1];
    memset(fileBuf, 0, sizeof(fileBuf));

    for (uint32_t i = 0; i < sectorsToRead; i++) {
        if (!scrReadSector(fileSector + i, sec)) {
            LOG_ERRORF("[Stealth] Failed to read config.txt sector %u (LBA %u)",
                       i, fileSector + i);
            scrSendCmd12();
            // Return whatever we've read so far if we got at least one sector
            if (i > 0) {
                fileBuf[i * 512] = '\0';
                break;
            }
            if (cardState == 3) scrSendCmd7(0);
            SDMMC.intmask.val = savedIntMask;
            scrCleanupAndReleaseMux();
            return "";
        }
        uint32_t copyLen = 512;
        if (i == sectorsToRead - 1) {
            // Last sector: only copy remaining bytes
            copyLen = bytesToRead - (i * 512);
        }
        memcpy(fileBuf + (i * 512), sec, copyLen);
    }
    fileBuf[bytesToRead] = '\0';

    String result(fileBuf);
    LOG_INFOF("[Stealth] config.txt read: %u bytes", bytesToRead);

    // ── Step 10: Restore card state and release MUX ──
    // Deselect card if it was in Standby when we found it
    if (cardState == 3) {
        scrSendCmd7(0);
    }

    SDMMC.intmask.val = savedIntMask;
    scrCleanupAndReleaseMux();

    LOG_INFO("[Stealth] === Stealth config.txt read complete ===");
    return result;
}

// ============================================================================
// restoreCardState — restore card to Standby after SD_MMC.end()
//
// After a regular upload cycle, SD_MMC.end() deinits the SDMMC host but does
// NOT send CMD0 — the card retains its state at RCA 0x1388.  We re-init the
// SDMMC host in stealth mode (no CMD0), verify the card, force 1-bit mode
// (AS10 native), and deselect (CMD7(0)) to put the card back in Standby.
// This is the state the AS10 CPAP expects when it resumes SD access.
//
// Caller MUST still hold the MUX on ESP side.  This function does NOT touch
// the MUX — SDCardManager::releaseControl() handles the MUX switch after.
// ============================================================================
bool StealthConfigReader::restoreCardState() {
    LOG_INFO("[StealthRestore] Attempting card state restoration...");

    // Step 1: Re-init SDMMC in stealth mode (no CMD0, no init clocks)
    if (!scrInitHardware()) {
        LOG_ERROR("[StealthRestore] Hardware re-init failed — card state NOT restored");
        return false;
    }

    // Step 2: Mask SDMMC ISR so bare-metal polling works
    uint32_t savedIntMask = SDMMC.intmask.val;
    SDMMC.intmask.val = 0;

    // Step 3: CMD13(0x1388) — verify card is still alive
    const uint16_t KNOWN_RCA = 0x1388;
    uint32_t r1 = 0;
    bool ok = scrSendCmd13(KNOWN_RCA, &r1);
    uint8_t cardState = (r1 >> 9) & 0x0F;

    if (!ok || cardState < 3 || r1 == 0) {
        LOG_WARNF("[StealthRestore] CMD13 failed (ok=%d state=%d r1=0x%08X) — cannot restore",
                  ok, cardState, r1);
        SDMMC.intmask.val = savedIntMask;
        scrDeinitHardware();
        return false;
    }

    LOG_INFOF("[StealthRestore] Card alive: state=%d(%s)", cardState, SCR_SD_STATE_NAME(cardState));

    // Step 4: Force 1-bit mode via ACMD6(0) — AS10 native bus width
    if (cardState == 4) {  // Transfer state — card is selected
        uint32_t resp55 = 0, resp6 = 0;
        uint32_t acmdFlags = (1u << 6) | (1u << 8) | (1u << 13) | (1u << 29);
        scrSendCmd(55, (uint32_t)KNOWN_RCA << 16, acmdFlags, &resp55, 50000);
        scrSendCmd(6, 0, acmdFlags, &resp6, 50000);
        scrSetHostBusWidth(1);
    }

    // Step 5: Deselect card → Standby state (CMD7 with RCA=0)
    if (cardState >= 4) {  // Transfer, Data, Rcv, Prg states — need deselect
        if (!scrSendCmd7(0)) {
            LOG_WARN("[StealthRestore] CMD7(0) deselect failed — card may not be in Standby");
        } else {
            LOG_INFO("[StealthRestore] Card deselected → Standby");
        }
    } else if (cardState == 3) {
        LOG_INFO("[StealthRestore] Card already in Standby — no deselect needed");
    }

    // Step 6: Verify final state
    r1 = 0;
    if (scrSendCmd13(KNOWN_RCA, &r1)) {
        uint8_t finalState = (r1 >> 9) & 0x0F;
        LOG_INFOF("[StealthRestore] Final card state: %d(%s)", finalState, SCR_SD_STATE_NAME(finalState));
    }

    // Step 7: Restore ISR mask and deinit hardware
    SDMMC.intmask.val = savedIntMask;
    scrDeinitHardware();

    LOG_INFO("[StealthRestore] Card state restoration complete");
    return true;
}
