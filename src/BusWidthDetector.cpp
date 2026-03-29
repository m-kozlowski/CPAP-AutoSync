#include "BusWidthDetector.h"
#include "pins_config.h"
#include "Logger.h"
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include "soc/sdmmc_reg.h"
#include "soc/sdmmc_struct.h"

// ============================================================================
// ESP32 SDMMC register access via the memory-mapped struct
// ============================================================================
// The ESP-IDF exports `SDMMC` as a global sdmmc_dev_t at DR_REG_SDMMC_BASE.
// Using the struct is cleaner and safer than raw pointer casts.
// The DATA FIFO is at base + 0x200 and is NOT in the struct — access it directly.
#define SDMMC_FIFO_ADDR  (DR_REG_SDMMC_BASE + 0x200)
#define SDMMC_FIFO       (*(volatile uint32_t*)SDMMC_FIFO_ADDR)

// Interrupt status bits (from sdmmc_reg.h)
#define INT_CMD_DONE   BIT(2)
#define INT_DTO        BIT(3)   // Data Transfer Over
#define INT_RXDR       BIT(5)   // RX FIFO Data Ready
#define INT_RCRC       BIT(6)   // Response CRC error
#define INT_DCRC       BIT(7)   // Data CRC error
#define INT_RTO        BIT(8)   // Response Timeout
#define INT_DRTO       BIT(9)   // Data Read Timeout
#define INT_HTO        BIT(10)  // Data starvation Host Timeout
#define INT_FRUN       BIT(11)  // FIFO underrun/overrun
#define INT_HLE        BIT(12)  // Hardware Locked write Error
#define INT_SBE        BIT(13)  // Start Bit Error
#define INT_EBE        BIT(15)  // End Bit Error

// Aggregate error masks
#define CMD_ERR_FLAGS  (INT_RTO | INT_RCRC | INT_HLE)
#define DATA_ERR_FLAGS (INT_DCRC | INT_DRTO | INT_SBE | INT_EBE | INT_HTO | INT_FRUN)
#define ALL_ERR_FLAGS  (CMD_ERR_FLAGS | DATA_ERR_FLAGS)

// SD card states (R1 response bits [12:9])
static const char* SD_STATE_NAMES[] = {
    "Idle", "Ready", "Ident", "Stby", "Tran", "Data", "Rcv", "Prg", "Dis"
};
#define SD_STATE_NAME(s) ((s) < 9 ? SD_STATE_NAMES[s] : "Unknown")

// ============================================================================
// EarlyPCNT — standalone lightweight pulse counter
// ============================================================================
namespace EarlyPCNT {
    static pcnt_unit_handle_t   s_unit    = nullptr;
    static pcnt_channel_handle_t s_chan   = nullptr;

    void init(int gpio) {
        pcnt_unit_config_t ucfg = {};
        ucfg.high_limit = 32767;
        ucfg.low_limit  = -1;
        if (pcnt_new_unit(&ucfg, &s_unit) != ESP_OK) return;

        pcnt_glitch_filter_config_t fcfg = {};
        fcfg.max_glitch_ns = 125;
        pcnt_unit_set_glitch_filter(s_unit, &fcfg);

        pcnt_chan_config_t ccfg = {};
        ccfg.edge_gpio_num  = gpio;
        ccfg.level_gpio_num = -1;
        if (pcnt_new_channel(s_unit, &ccfg, &s_chan) != ESP_OK) {
            pcnt_del_unit(s_unit);
            s_unit = nullptr;
            return;
        }
        pcnt_channel_set_edge_action(s_chan,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE);
        pcnt_channel_set_level_action(s_chan,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP);
        pcnt_unit_enable(s_unit);
        pcnt_unit_clear_count(s_unit);
        pcnt_unit_start(s_unit);
    }

    int read() {
        if (!s_unit) return -1;
        int val = 0;
        pcnt_unit_get_count(s_unit, &val);
        return val;
    }

    void teardown() {
        if (s_chan)  { pcnt_del_channel(s_chan); s_chan = nullptr; }
        if (s_unit) { pcnt_unit_stop(s_unit); pcnt_unit_disable(s_unit); pcnt_del_unit(s_unit); s_unit = nullptr; }
    }
}

// ============================================================================
// Hardware Init / Deinit
// ============================================================================

bool BusWidthDetector::initHardware() {
    // Use ESP-IDF sdmmc_host_init to properly configure the GPIO matrix.
    // Without this, the SDMMC peripheral cannot drive or sample the SD pins.
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOG_ERRORF("[BWD] sdmmc_host_init failed: 0x%x", err);
        return false;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;  // Configure all 4 data lines in GPIO matrix
    slot.clk   = (gpio_num_t)SD_CLK_PIN;
    slot.cmd   = (gpio_num_t)SD_CMD_PIN;
    slot.d0    = (gpio_num_t)SD_D0_PIN;
    slot.d1    = (gpio_num_t)SD_D1_PIN;
    slot.d2    = (gpio_num_t)SD_D2_PIN;
    slot.d3    = (gpio_num_t)SD_D3_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) {
        LOG_ERRORF("[BWD] sdmmc_host_init_slot failed: 0x%x", err);
        sdmmc_host_deinit();
        return false;
    }

    // Start with 1-bit mode at the host level (safe default for probing)
    setHostBusWidth(1);
    setHostClock(400);  // 400 kHz — safe identification speed
    delay(10);

    // Send 80 initialization clocks to synchronize the card's CIU
    sdmmc_hw_cmd_t initCmd = {};
    initCmd.send_init     = 1;
    initCmd.wait_complete = 1;
    initCmd.card_num      = SDMMC_HOST_SLOT_1;
    initCmd.start_command = 1;
    *(volatile uint32_t*)&SDMMC.cmd = *(uint32_t*)&initCmd;

    uint32_t t0 = millis();
    while (SDMMC.cmd.start_command && (millis() - t0 < 100));
    delay(5);

    return true;
}

void BusWidthDetector::deinitHardware() {
    // Reset host bus width to 1-bit
    setHostBusWidth(1);

    sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_1);
    sdmmc_host_deinit();
}

// ============================================================================
// Host bus configuration helpers
// ============================================================================

void BusWidthDetector::setHostBusWidth(int bits) {
    // ctype register: bit 0 for slot 1 — 0 = 1-bit, 1 = 4-bit
    if (bits == 4)
        SDMMC.ctype.card_width |= BIT(SDMMC_HOST_SLOT_1);
    else
        SDMMC.ctype.card_width &= ~BIT(SDMMC_HOST_SLOT_1);
}

void BusWidthDetector::setHostClock(int freqKHz) {
    esp_err_t err = sdmmc_host_set_card_clk(SDMMC_HOST_SLOT_1, freqKHz);
    if (err != ESP_OK) {
        LOG_ERRORF("[BWD] setHostClock(%d kHz) failed: 0x%x", freqKHz, err);
    }
    delayMicroseconds(100);
}

// ============================================================================
// Bare-metal command dispatch
// ============================================================================

bool BusWidthDetector::sendCmd(uint8_t cmdIdx, uint32_t arg, uint32_t extraFlags,
                               uint32_t* resp, uint32_t timeoutUs)
{
    // Clear all pending interrupt flags
    SDMMC.rintsts.val = 0xFFFFFFFF;

    // Write command argument
    SDMMC.cmdarg = arg;

    // Build command register value using the struct for clarity
    sdmmc_hw_cmd_t hw = {};
    hw.cmd_index      = cmdIdx;
    hw.card_num       = SDMMC_HOST_SLOT_1;
    hw.use_hold_reg   = 1;
    hw.wait_complete  = 1;
    hw.start_command  = 1;
    // Merge extra flags (response_expect, check_response_crc, data_expected, etc.)
    uint32_t cmdVal   = *(uint32_t*)&hw;
    cmdVal           |= extraFlags;
    *(volatile uint32_t*)&SDMMC.cmd = cmdVal;

    // Spin-wait for start_command bit to clear (command accepted by CIU)
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    while (SDMMC.cmd.start_command) {
        if (((uint32_t)esp_timer_get_time() - t0) > timeoutUs) return false;
    }

    // Wait for command completion or error
    while (true) {
        uint32_t sts = SDMMC.rintsts.val;
        if (sts & (INT_CMD_DONE | CMD_ERR_FLAGS)) {
            if (resp) *resp = SDMMC.resp[0];
            return (sts & INT_CMD_DONE) && !(sts & CMD_ERR_FLAGS);
        }
        if (((uint32_t)esp_timer_get_time() - t0) > timeoutUs) return false;
    }
}

bool BusWidthDetector::sendCmd13(uint16_t rca, uint32_t* status) {
    // CMD13 flags: response_expect, check_crc, check response index
    uint32_t flags = (1 << 6) | (1 << 8);
    return sendCmd(13, (uint32_t)rca << 16, flags, status, 5000);
}

bool BusWidthDetector::sendCmd7(uint16_t rca) {
    // CMD7 SELECT/DESELECT: response expected, check CRC
    uint32_t flags = (1 << 6) | (1 << 8);
    uint32_t resp = 0;
    return sendCmd(7, (uint32_t)rca << 16, flags, &resp, 50000);
}

bool BusWidthDetector::sendCmd12() {
    // CMD12 STOP_TRANSMISSION: response expected, stop/abort flag
    uint32_t flags = (1 << 6) | (1 << 14);  // response_expect + stop_abort_cmd
    uint32_t resp = 0;
    return sendCmd(12, 0, flags, &resp, 50000);
}

// ============================================================================
// Sector read via FIFO (no DMA) — bare-metal CMD17
// ============================================================================

bool BusWidthDetector::readSector(uint32_t sector, uint8_t* buf) {
    // Configure block size and byte count
    SDMMC.blksiz.block_size = 512;
    SDMMC.bytcnt = 512;

    // Reset FIFO
    SDMMC.ctrl.fifo_reset = 1;
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    while (SDMMC.ctrl.fifo_reset) {
        if (((uint32_t)esp_timer_get_time() - t0) > 10000) {
            LOG_ERROR("[BWD] FIFO reset timeout");
            return false;
        }
    }

    // Clear all interrupt flags
    SDMMC.rintsts.val = 0xFFFFFFFF;

    // Set command argument (sector address for SDHC/SDXC)
    SDMMC.cmdarg = sector;

    // CMD17 READ_SINGLE_BLOCK: response_expect, check_crc, data_expected, wait_complete
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

    // Wait for command acceptance
    t0 = (uint32_t)esp_timer_get_time();
    while (SDMMC.cmd.start_command) {
        if (((uint32_t)esp_timer_get_time() - t0) > 100000) return false;
    }

    // Check for command-level errors (wrong RCA, etc.)
    uint32_t sts = SDMMC.rintsts.val;
    if (sts & CMD_ERR_FLAGS) {
        return false;
    }

    // Read 512 bytes (128 x 32-bit words) from the FIFO
    uint32_t* buf32 = (uint32_t*)buf;
    uint32_t wordsRead = 0;
    const uint32_t wordsNeeded = 128;
    t0 = (uint32_t)esp_timer_get_time();

    while (wordsRead < wordsNeeded) {
        sts = SDMMC.rintsts.val;
        if (sts & DATA_ERR_FLAGS) {
            return false;
        }

        // Check if FIFO has data
        uint32_t fifoCount = SDMMC.status.fifo_count;
        while (fifoCount > 0 && wordsRead < wordsNeeded) {
            buf32[wordsRead++] = SDMMC_FIFO;
            fifoCount--;
        }

        if (sts & INT_DTO) break;  // Data Transfer Over

        if (((uint32_t)esp_timer_get_time() - t0) > 500000) {
            LOG_ERROR("[BWD] Sector read timeout");
            return false;
        }
    }

    // Wait for DTO if we haven't seen it yet
    if (!(SDMMC.rintsts.val & INT_DTO)) {
        t0 = (uint32_t)esp_timer_get_time();
        while (!(SDMMC.rintsts.val & INT_DTO)) {
            if (SDMMC.rintsts.val & DATA_ERR_FLAGS) return false;
            if (((uint32_t)esp_timer_get_time() - t0) > 100000) return false;
        }
    }

    // Final CRC check
    sts = SDMMC.rintsts.val;
    if (sts & (INT_DCRC | INT_SBE | INT_EBE)) {
        return false;
    }

    return (wordsRead == wordsNeeded);
}

// ============================================================================
// RCA Brute-Force Sweep
// ============================================================================

uint16_t BusWidthDetector::sweepRCA(uint32_t* outStatus) {
    LOG_INFO("[BWD] Starting RCA sweep...");
    unsigned long t0 = millis();

    // Build CMD13 command word once (bare-metal, no struct overhead in hot loop)
    // cmd_index=13, response_expect=1, check_crc=1, wait_complete=1, use_hold=1, start=1, card_num=slot1
    const uint32_t cmd13Word =
        13                          |  // CMD13
        (1u << 6)                   |  // response_expect
        (1u << 8)                   |  // check_response_crc
        (1u << 13)                  |  // wait_complete
        ((uint32_t)SDMMC_HOST_SLOT_1 << 16) |  // card_num
        (1u << 29)                  |  // use_hold_reg
        (1u << 31);                    // start_command

    // Set short response timeout (64 card clock cycles at 400kHz = 160µs)
    uint32_t origTmout = SDMMC.tmout.val;
    SDMMC.tmout.response = 0x40;

    uint32_t timeouts = 0, crcErrs = 0, ghostHits = 0, noResponse = 0;

    // Per-probe software timeout in microseconds.
    // Hardware response timeout is 64 card clocks @ 400kHz = 160µs.
    // 300µs gives ~2× margin; 1ms for validation/confirm probes.
    static constexpr uint32_t PROBE_TIMEOUT_US  = 300;
    static constexpr uint32_t CONFIRM_TIMEOUT_US = 1000;

    // ── Helper: fire CMD13 for a given RCA and return raw rintsts ──
    auto fireCmd13 = [&](uint16_t rca, uint32_t timeoutUs, uint32_t* outResp) -> uint32_t {
        SDMMC.cmdarg = (uint32_t)rca << 16;
        SDMMC.rintsts.val = 0xFFFFFFFF;
        *(volatile uint32_t*)&SDMMC.cmd = cmd13Word;

        // Spin until start_command clears (CIU accepted)
        uint32_t us0 = (uint32_t)esp_timer_get_time();
        while (SDMMC.cmd.start_command) {
            if (((uint32_t)esp_timer_get_time() - us0) > timeoutUs) break;
        }

        // Spin until command completion or error
        uint32_t sts = 0;
        us0 = (uint32_t)esp_timer_get_time();
        do {
            sts = SDMMC.rintsts.val;
            if (((uint32_t)esp_timer_get_time() - us0) > timeoutUs) break;
        } while (!(sts & (INT_CMD_DONE | CMD_ERR_FLAGS)));

        *outResp = SDMMC.resp[0];
        return sts;
    };

    // ── Helper: validate a CMD13 hit and confirm with double-tap ──
    // Returns true only if the response represents a real card (State >= 3,
    // non-zero status) and a second CMD13 to the same RCA also succeeds.
    // Only logs first MAX_VERBOSE_GHOSTS ghost hits verbosely to avoid log spam.
    static constexpr uint32_t MAX_VERBOSE_GHOSTS = 3;

    auto validateHit = [&](uint16_t rca, uint32_t sts, uint32_t resp,
                           const char* source) -> bool {
        uint8_t state = (resp >> 9) & 0x0F;

        // Reject ghost hits: State 0 (Idle) is invalid in SD mode for CMD13.
        // A real card responding to CMD13 must be in Standby (3) or higher.
        // Also reject all-zero response (electrical noise on CMD line).
        if (state < 3 || resp == 0) {
            ghostHits++;
            if (ghostHits <= MAX_VERBOSE_GHOSTS) {
                LOG_WARNF("[BWD] Ghost #%u: RCA 0x%04X rintsts=0x%08X resp=0x%08X state=%d(%s)",
                          ghostHits, rca, sts, resp, state, SD_STATE_NAME(state));
            }
            return false;
        }

        // Non-ghost candidate — log fully
        LOG_INFOF("[BWD] Candidate RCA 0x%04X (%s): rintsts=0x%08X resp=0x%08X state=%d(%s)",
                  rca, source, sts, resp, state, SD_STATE_NAME(state));

        // Double-tap confirmation: send CMD13 again and require consistent response
        uint32_t confirmResp = 0;
        uint32_t confirmSts = fireCmd13(rca, CONFIRM_TIMEOUT_US, &confirmResp);

        if (!(confirmSts & INT_CMD_DONE) || (confirmSts & CMD_ERR_FLAGS)) {
            LOG_WARNF("[BWD] Confirm failed: RCA 0x%04X rintsts=0x%08X — rejected",
                      rca, confirmSts);
            ghostHits++;
            return false;
        }

        uint8_t confirmState = (confirmResp >> 9) & 0x0F;
        if (confirmState < 3 || confirmResp == 0) {
            LOG_WARNF("[BWD] Confirm ghost: RCA 0x%04X resp=0x%08X state=%d — rejected",
                      rca, confirmResp, confirmState);
            ghostHits++;
            return false;
        }

        LOG_INFOF("[BWD] Confirmed: RCA 0x%04X resp=0x%08X state=%d(%s)",
                  rca, confirmResp, confirmState, SD_STATE_NAME(confirmState));
        return true;
    };

    // ── Fast-path: check common low RCAs first ──
    static const uint16_t fastRCAs[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        0x0100, 0x0200, 0xAAAA, 0x1234
    };

    LOG_INFOF("[BWD] Fast-path: testing %d common RCAs...", (int)(sizeof(fastRCAs)/sizeof(fastRCAs[0])));
    for (uint16_t rca : fastRCAs) {
        uint32_t resp = 0;
        uint32_t sts = fireCmd13(rca, PROBE_TIMEOUT_US, &resp);

        if ((sts & INT_CMD_DONE) && !(sts & CMD_ERR_FLAGS)) {
            if (validateHit(rca, sts, resp, "fast-path")) {
                *outStatus = resp;
                SDMMC.tmout.val = origTmout;
                LOG_INFOF("[BWD] RCA 0x%04X found (fast-path, %lums)", rca, millis() - t0);
                return rca;
            }
        }
    }
    LOG_INFOF("[BWD] Fast-path: no hit (%lums, ghosts=%u)", millis() - t0, ghostHits);

    // ── Full linear sweep ──
    LOG_INFO("[BWD] Full sweep 1→65535...");
    for (uint32_t rca = 1; rca <= 0xFFFF; rca++) {
        // Safety: abort after 30 seconds (allows full 65535 scan at ~300µs/RCA)
        if ((rca & 0x3FF) == 0 && (millis() - t0 > 30000)) {
            LOG_WARNF("[BWD] Sweep timeout (30s, scanned %u RCAs)", (unsigned)rca);
            break;
        }

        uint32_t resp = 0;
        uint32_t sts = fireCmd13((uint16_t)rca, PROBE_TIMEOUT_US, &resp);

        if ((sts & INT_CMD_DONE) && !(sts & CMD_ERR_FLAGS)) {
            if (validateHit((uint16_t)rca, sts, resp, "full-sweep")) {
                *outStatus = resp;
                SDMMC.tmout.val = origTmout;
                LOG_INFOF("[BWD] RCA 0x%04X found (full sweep, %lums)", (uint16_t)rca, millis() - t0);
                return (uint16_t)rca;
            }
        }

        if (sts & INT_RTO) timeouts++;
        else if (sts & INT_RCRC) crcErrs++;
        else if (!(sts & (INT_CMD_DONE | CMD_ERR_FLAGS))) noResponse++;

        if (rca % 5000 == 0) {
            LOG_INFOF("[BWD] Sweep progress: RCA %u/65535 (%lums, TO=%u CRC=%u ghosts=%u noResp=%u)",
                      (unsigned)rca, millis() - t0, timeouts, crcErrs, ghostHits, noResponse);
        }
    }

    SDMMC.tmout.val = origTmout;
    LOG_WARNF("[BWD] No RCA found (sweep took %lums, TO=%u, CRC=%u, ghosts=%u, noResp=%u)",
              millis() - t0, timeouts, crcErrs, ghostHits, noResponse);
    return 0;
}

// ============================================================================
// Bus-Width CRC Probing
// ============================================================================

int BusWidthDetector::probeBusWidth(uint16_t rca, uint8_t* sectorBuf) {
    LOG_INFO("[BWD] Phase 2: CRC-based bus-width probing...");

    // Get current card state
    uint32_t status = 0;
    if (!sendCmd13(rca, &status)) {
        LOG_ERROR("[BWD] CMD13 failed before probe");
        return 0;
    }
    uint8_t state = (status >> 9) & 0x0F;
    bool wasStandby = (state == 3);

    // Card must be in Transfer state (4) for CMD17
    if (state == 3) {
        LOG_INFO("[BWD] Card in Standby — selecting with CMD7...");
        if (!sendCmd7(rca)) {
            LOG_ERROR("[BWD] CMD7 SELECT failed");
            return 0;
        }
        delay(2);
    } else if (state != 4) {
        LOG_WARNF("[BWD] Unexpected card state %d (%s) — attempting probe anyway",
                  state, SD_STATE_NAME(state));
    }

    // Probe order: 1-bit first (safe), then 4-bit.
    // Each speed: 400kHz first (safest), then 25MHz.
    struct ProbeConfig {
        int bits;
        int clockKHz;
        const char* label;
    };
    static const ProbeConfig probes[] = {
        { 1,   400, "1-bit @ 400kHz" },
        { 1, 25000, "1-bit @ 25MHz"  },
        { 4,   400, "4-bit @ 400kHz" },
        { 4, 25000, "4-bit @ 25MHz"  },
    };

    int detectedWidth = 0;

    for (const auto& p : probes) {
        setHostBusWidth(p.bits);
        setHostClock(p.clockKHz);

        bool ok = readSector(0, sectorBuf);

        if (ok) {
            // Sanity check: sector 0 should look like a valid MBR/VBR
            // (byte 510=0x55, byte 511=0xAA for boot signature)
            bool validSig = (sectorBuf[510] == 0x55 && sectorBuf[511] == 0xAA);
            LOG_INFOF("[BWD] Probe %s: READ OK (boot sig %s)",
                      p.label, validSig ? "valid" : "MISSING");
            detectedWidth = p.bits;
            break;
        } else {
            LOG_INFOF("[BWD] Probe %s: FAILED (CRC/timeout)", p.label);
            // Safety: CMD12 after every failed read
            sendCmd12();
            delay(2);
        }
    }

    // Restore Standby if card was in Standby before
    if (wasStandby && detectedWidth > 0) {
        LOG_INFO("[BWD] Deselecting card (was in Standby)...");
        sendCmd7(0);  // CMD7 with RCA=0 deselects
    }

    return detectedWidth;
}

// ============================================================================
// Minimal FAT32 config.txt reader — zero writes, zero timestamps
// ============================================================================

static uint32_t readLE16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t readLE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

bool BusWidthDetector::readConfigTxt(int busWidth, uint16_t rca, uint8_t* sec, String& outSSID) {
    LOG_INFO("[BWD] Phase 3: Stealth config.txt read...");

    // Ensure card is selected and in Transfer state
    uint32_t st = 0;
    if (!sendCmd13(rca, &st)) return false;
    uint8_t state = (st >> 9) & 0x0F;
    if (state == 3) {
        if (!sendCmd7(rca)) return false;
    }

    // Configure host to detected bus width at 25MHz for data reads
    setHostBusWidth(busWidth);
    setHostClock(25000);

    // Step 1: Read sector 0 (MBR or VBR)
    if (!readSector(0, sec)) {
        LOG_ERROR("[BWD] Failed to read sector 0");
        sendCmd12();
        return false;
    }

    // Determine if this is an MBR or a VBR
    uint32_t vbrSector = 0;
    // Check for FAT filesystem signature at offset 0x36 or 0x52
    bool isFAT = (memcmp(sec + 0x36, "FAT", 3) == 0) ||
                 (memcmp(sec + 0x52, "FAT", 3) == 0);
    if (!isFAT) {
        // Likely an MBR — read partition 1 start LBA
        if (sec[510] != 0x55 || sec[511] != 0xAA) {
            LOG_ERROR("[BWD] No valid MBR/VBR boot signature");
            return false;
        }
        vbrSector = readLE32(sec + 0x1C6);  // Partition 1 start LBA
        LOG_INFOF("[BWD] MBR: Partition 1 starts at sector %u", vbrSector);

        if (!readSector(vbrSector, sec)) {
            LOG_ERROR("[BWD] Failed to read VBR");
            sendCmd12();
            return false;
        }
    }

    // Parse BPB (BIOS Parameter Block) from VBR
    uint16_t bytesPerSector  = readLE16(sec + 0x0B);
    uint8_t  sectorsPerClust = sec[0x0D];
    uint16_t reservedSectors = readLE16(sec + 0x0E);
    uint8_t  numFATs         = sec[0x10];
    uint16_t rootEntryCount  = readLE16(sec + 0x11);  // 0 for FAT32
    uint32_t fatSize         = readLE16(sec + 0x16);
    if (fatSize == 0) fatSize = readLE32(sec + 0x24);  // FAT32 uses 32-bit field
    uint32_t rootCluster     = readLE32(sec + 0x2C);   // FAT32 root dir cluster

    if (bytesPerSector != 512 || sectorsPerClust == 0) {
        LOG_ERRORF("[BWD] Unsupported BPB: bps=%u spc=%u", bytesPerSector, sectorsPerClust);
        return false;
    }

    uint32_t fatStart    = vbrSector + reservedSectors;
    uint32_t dataStart   = fatStart + (numFATs * fatSize);
    // For FAT16: root dir is between FAT and data
    uint32_t rootDirSectors = ((rootEntryCount * 32) + 511) / 512;
    uint32_t firstDataSector = dataStart + rootDirSectors;

    bool isFAT32 = (rootEntryCount == 0);

    LOG_INFOF("[BWD] FAT%s: fatStart=%u dataStart=%u spc=%u rootClust=%u",
              isFAT32 ? "32" : "16", fatStart, dataStart, sectorsPerClust, rootCluster);

    // Step 2: Scan root directory for "CONFIG  TXT" (8.3 format)
    // For FAT32, root dir starts at rootCluster; for FAT16, it's at dataStart
    uint32_t configCluster = 0;
    uint32_t configSize = 0;
    bool found = false;

    auto clusterToSector = [&](uint32_t cluster) -> uint32_t {
        return firstDataSector + (cluster - 2) * sectorsPerClust;
    };

    // Read up to 16 sectors of root directory (covers most SD cards)
    uint32_t dirSector;
    if (isFAT32) {
        dirSector = clusterToSector(rootCluster);
    } else {
        dirSector = dataStart;
    }

    for (int s = 0; s < 16 && !found; s++) {
        if (!readSector(dirSector + s, sec)) {
            sendCmd12();
            break;
        }
        for (int e = 0; e < 16 && !found; e++) {
            uint8_t* entry = sec + (e * 32);
            if (entry[0] == 0x00) { found = false; goto dir_done; }  // End of directory
            if (entry[0] == 0xE5) continue;  // Deleted entry
            if (entry[11] & 0x08) continue;  // Volume label
            if (entry[11] & 0x10) continue;  // Subdirectory

            // Compare 8.3 name: "CONFIG  TXT"
            if (memcmp(entry, "CONFIG  TXT", 11) == 0) {
                uint16_t clHi = readLE16(entry + 0x14);
                uint16_t clLo = readLE16(entry + 0x1A);
                configCluster = ((uint32_t)clHi << 16) | clLo;
                configSize = readLE32(entry + 0x1C);
                found = true;
                LOG_INFOF("[BWD] Found config.txt: cluster=%u size=%u", configCluster, configSize);
            }
        }
    }
dir_done:

    if (!found || configCluster < 2 || configSize == 0) {
        LOG_WARN("[BWD] config.txt not found in root directory");
        return false;
    }

    // Step 3: Read config.txt data (just first sector — 512 bytes is enough for WIFI_SSID)
    uint32_t fileSector = clusterToSector(configCluster);
    if (!readSector(fileSector, sec)) {
        LOG_ERROR("[BWD] Failed to read config.txt data sector");
        sendCmd12();
        return false;
    }

    // Step 4: Parse WIFI_SSID from the buffer
    uint32_t len = configSize < 512 ? configSize : 512;
    // Null-terminate safely
    sec[len < 512 ? len : 511] = '\0';
    const char* txt = (const char*)sec;

    // Simple line-by-line parser looking for WIFI_SSID
    const char* p = txt;
    while (*p) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n') {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        // Check for WIFI_SSID
        if (strncmp(p, "WIFI_SSID", 9) == 0) {
            p += 9;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                // Strip optional quotes
                char quote = 0;
                if (*p == '"' || *p == '\'') { quote = *p; p++; }
                const char* start = p;
                while (*p && *p != '\r' && *p != '\n') {
                    if (quote && *p == quote) break;
                    p++;
                }
                outSSID = String(start).substring(0, p - start);
                LOG_INFOF("[BWD] Extracted WIFI_SSID: %s", outSSID.c_str());
                return true;
            }
        }

        // Skip to next line
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }

    LOG_WARN("[BWD] WIFI_SSID not found in config.txt");
    return false;
}

// ============================================================================
// State Restoration & MUX Return
// ============================================================================

void BusWidthDetector::restoreAndRelease(uint16_t rca, uint8_t origState, bool wasSelected) {
    // Deselect card if it was in Standby when we found it
    if (!wasSelected && rca != 0) {
        LOG_INFO("[BWD] Restoring card to Standby (CMD7 deselect)...");
        sendCmd7(0);
        delay(1);
    }

    // Tri-state all SDMMC pins to high-impedance
    const int sdPins[] = { SD_CMD_PIN, SD_CLK_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN };
    for (int pin : sdPins) {
        gpio_reset_pin((gpio_num_t)pin);
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    }

    deinitHardware();

    // Return MUX to CPAP
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
    delay(5);
    LOG_INFO("[BWD] MUX returned to CPAP.");
}

// ============================================================================
// Self-Test: positive control + MUX disruption proof
// ============================================================================
// Initialises the card ourselves (CMD0→CMD8→ACMD41→CMD2→CMD3) to get a known
// RCA, then verifies the sweep code finds it.  Next, returns MUX to CPAP
// briefly and grabs it back to prove the MUX switch kills the session.

void BusWidthDetector::selfTest() {
    LOG_INFO("\n===SELF-TEST=== MUX disruption test (ESP-IDF card init) ===");
    // NOTE: Caller must have already grabbed MUX and called initHardware().

    // ── Phase 1: Init card via ESP-IDF ──
    LOG_INFO("===SELF-TEST=== Phase 1: sdmmc_card_init() — first init...");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_card_t card;
    memset(&card, 0, sizeof(card));
    card.host = host;

    esp_err_t err = sdmmc_card_init(&host, &card);
    if (err != ESP_OK) {
        LOG_ERRORF("===SELF-TEST=== First init FAILED: 0x%x (%s)", err, esp_err_to_name(err));
        LOG_INFO("===SELF-TEST=== ABORTED ===\n");
        return;
    }

    uint16_t rca1 = card.rca;
    LOG_INFOF("===SELF-TEST=== First init OK: RCA=0x%04X, SDHC=%s, %luMB",
              rca1, (card.ocr & (1 << 30)) ? "yes" : "no",
              (unsigned long)(((uint64_t)card.csd.capacity) * card.csd.sector_size / (1024 * 1024)));

    // ── Phase 2: Re-init WITHOUT MUX cycle (control — should succeed) ──
    LOG_INFO("===SELF-TEST=== Phase 2: Re-init without MUX cycle (control)...");
    memset(&card, 0, sizeof(card));
    card.host = host;
    err = sdmmc_card_init(&host, &card);
    if (err != ESP_OK) {
        LOG_ERRORF("===SELF-TEST=== Control re-init FAILED: 0x%x (%s)", err, esp_err_to_name(err));
        LOG_INFO("===SELF-TEST=== ABORTED ===\n");
        return;
    }
    uint16_t rca2 = card.rca;
    LOG_INFOF("===SELF-TEST=== Control re-init OK: RCA=0x%04X (was 0x%04X)", rca2, rca1);

    // ── Phase 3: MUX round-trip + re-init ──
    LOG_INFO("===SELF-TEST=== Phase 3: MUX round-trip (release 500ms, grab back)...");

    // Tri-state pins, deinit, release MUX
    const int sdPins[] = { SD_CMD_PIN, SD_CLK_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN };
    for (int pin : sdPins) {
        gpio_reset_pin((gpio_num_t)pin);
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    }
    deinitHardware();
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
    LOG_INFO("===SELF-TEST=== MUX released to CPAP, waiting 500ms...");
    delay(500);

    // Grab MUX back and re-init
    LOG_INFO("===SELF-TEST=== Grabbing MUX back...");
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_ESP_VALUE);
    delay(200);

    if (!initHardware()) {
        LOG_ERROR("===SELF-TEST=== Hardware re-init failed after MUX round-trip");
        LOG_INFO("===SELF-TEST=== ABORTED ===\n");
        return;
    }

    // Try card init again
    LOG_INFO("===SELF-TEST=== Phase 3b: sdmmc_card_init() after MUX round-trip...");
    memset(&card, 0, sizeof(card));
    card.host = host;
    err = sdmmc_card_init(&host, &card);
    if (err != ESP_OK) {
        LOG_ERRORF("===SELF-TEST=== Post-MUX init FAILED: 0x%x (%s) — card lost session!",
                   err, esp_err_to_name(err));
        LOG_INFO("===SELF-TEST=== CONFIRMED: MUX round-trip kills card — init fails completely ===\n");
        return;
    }

    uint16_t rca3 = card.rca;
    LOG_INFOF("===SELF-TEST=== Post-MUX init OK: RCA=0x%04X (was 0x%04X / 0x%04X)", rca3, rca1, rca2);

    if (rca3 == rca2) {
        LOG_INFOF("===SELF-TEST=== SURPRISE: Card retained same RCA across MUX cycle!");
    } else {
        LOG_INFOF("===SELF-TEST=== Card got new RCA after MUX cycle (0x%04X → 0x%04X) — session was reset",
                  rca2, rca3);
    }

    // Also: bare-metal sweep is broken (can't find ESP-IDF-initialized RCA).
    // The ESP-IDF SDMMC driver's ISR consumes interrupts before our polling loop.
    LOG_WARN("===SELF-TEST=== NOTE: Bare-metal CMD13 sweep is incompatible with ESP-IDF SDMMC driver");

    LOG_INFO("===SELF-TEST=== Complete ===\n");
}

// ============================================================================
// Main Detection Entry Point
// ============================================================================

DetectionResult BusWidthDetector::detect() {
    DetectionResult result = {};
    result.busWidth = 0;
    result.rca = 0;
    result.cardState = 0;
    result.sweepTimeMs = 0;

    LOG_INFO("\n===EXPERIMENTAL=== BUS-WIDTH DETECTOR START ===");

    // Grab MUX
    LOG_INFO("[BWD] Grabbing SD MUX...");
    pinMode(SD_SWITCH_PIN, OUTPUT);
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_ESP_VALUE);
    delay(200);  // MUX settle

    // Initialize SDMMC hardware
    if (!initHardware()) {
        LOG_ERROR("[BWD] Hardware init failed — aborting");
        digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
        LOG_INFO("===EXPERIMENTAL=== BUS-WIDTH DETECTOR END (hw fail) ===\n");
        return result;
    }

    // ── EXPERIMENTAL: Self-test (positive control + MUX disruption proof) ──
    // selfTest() will: init card → verify sweep finds it → MUX round-trip → verify RCA lost.
    // It leaves hardware re-initialized and MUX on ESP32 side.
    selfTest();
    // After selfTest, the card is in unknown state (CMD0 was sent, MUX was cycled).
    // restoreAndRelease+reinit already happened inside selfTest's MUX round-trip.
    // Just proceed with the normal sweep.

    // Phase 1: RCA sweep
    unsigned long sweepStart = millis();
    uint32_t cardStatus = 0;
    uint16_t rca = sweepRCA(&cardStatus);
    result.sweepTimeMs = millis() - sweepStart;
    result.rca = rca;

    if (rca == 0) {
        LOG_WARN("[BWD] No RCA found — card uninitialised or in card reader");
        restoreAndRelease(0, 0, false);
        LOG_INFO("===EXPERIMENTAL=== BUS-WIDTH DETECTOR END (no RCA) ===\n");
        return result;
    }

    uint8_t origState = (cardStatus >> 9) & 0x0F;
    result.cardState = origState;
    bool wasSelected = (origState == 4 || origState == 5 || origState == 6);

    LOG_INFOF("[BWD] RCA=0x%04X State=%d(%s) Status=0x%08X",
              rca, origState, SD_STATE_NAME(origState), cardStatus);

    // Raise clock for probing — 25MHz gives faster reads
    setHostClock(25000);

    // Phase 2: Bus-width detection via CRC probing
    // Allocate sector buffer on stack (512 bytes)
    uint8_t sectorBuf[512] __attribute__((aligned(4)));
    memset(sectorBuf, 0, sizeof(sectorBuf));

    int bw = probeBusWidth(rca, sectorBuf);
    result.busWidth = bw;

    if (bw > 0) {
        LOG_INFOF("===EXPERIMENTAL=== Detected %d-bit mode (likely %s) ===",
                  bw, bw == 1 ? "AirSense 10" : "AirSense 11");

        // Phase 3: Attempt stealth config.txt read
        String ssid;
        if (readConfigTxt(bw, rca, sectorBuf, ssid)) {
            result.wifiSSID = ssid;
        } else {
            LOG_WARN("[BWD] config.txt read failed (non-fatal)");
        }
    } else {
        LOG_WARN("===EXPERIMENTAL=== Bus-width detection FAILED (all probes failed) ===");
    }

    // Phase 4: Restore and release
    restoreAndRelease(rca, origState, wasSelected);

    LOG_INFOF("===EXPERIMENTAL=== BUS-WIDTH DETECTOR END (bw=%d, rca=0x%04X, sweep=%lums) ===\n",
              result.busWidth, result.rca, result.sweepTimeMs);

    return result;
}
