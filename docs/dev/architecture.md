# Architecture and Internals

## How It Works

### First Boot
1. Device reads `config.txt` from SD card
2. Connects to WiFi network
3. Synchronizes time with internet (NTP)
4. Loads upload history from internal LittleFS state files (if present)

### Daily Upload Cycle
1. Waits for upload eligibility based on configured mode (`UPLOAD_MODE`)
   - Smart mode: shortly after therapy ends (activity + inactivity detection)
   - Scheduled mode: during configured upload window
2. **Pre-flight scan** checks for new/changed files (SD-only, no network)
3. Takes control of SD card (only when CPAP is idle)
4. **Phased dual-backend upload** (optimizes memory usage):
   - **Phase 1 — Cloud**: Upload to SleepHQ (on-demand TLS, highest heap for handshake)
   - **Phase 2 — SMB**: Upload to network share (TLS torn down, clean sockets for libsmb2)
   - Time budget auto-scales to 2× configured minutes when both backends enabled
5. Uploads new/changed files in priority order:
   - **SMB**: Root/SETTINGS files first, then DATALOG folders (newest first)
   - **Cloud**: DATALOG folders first (newest first), then Root/SETTINGS files (only if DATALOG files uploaded)
6. **SMB:** Automatically creates directories on remote share
   **Cloud:** Associates data with your SleepHQ account (OAuth only if needed)
7. Releases SD card after session or time budget exhausted
8. Enters COOLDOWN → LISTENING loop for the next cycle (elective reboots skipped by default; a heap safety valve forces a reboot if `max_alloc < 32 KB`)
9. Saves progress to separate internal state files for each backend (`/littlefs/.upload_state.v2.smb`/`.cloud` + journals)

### Smart File Tracking
- **DATALOG folders**: Tracks completion (all files uploaded = done)
- **Root/SETTINGS files**: Tracks checksums (only uploads if changed)
- Never uploads the same file twice

### SD Card Sharing
- **Passive Operation:** Only accesses the card when the CPAP machine is idle (no therapy recording)
- **Short Sessions:** Limits exclusive access time (default 5 minutes) to ensure CPAP can reclaim access if needed
- **Automatic Release:** Releases control immediately after session or if therapy starts

---

## File Structure

### On SD Card
```
/
├── config.txt               # Your configuration (you create this)
├── Identification.json      # ResMed 11 identification (if present)
├── Identification.crc       # Identification checksum (if present)
├── Identification.tgt       # ResMed 9/10 identification (if present)
├── STR.edf                  # Summary data (if present)
├── DATALOG/                 # Therapy data folders
│   ├── 20241114/           # Date-named folders (YYYYMMDD)
│   │   ├── file1.edf
│   │   └── file2.edf
│   └── 20241113/
└── SETTINGS/                # Settings folder
    ├── CurrentSettings.json
    └── CurrentSettings.crc
```

### Internal LittleFS (ESP32 flash)
```
/littlefs/.upload_state.v2.smb
/littlefs/.upload_state.v2.smb.log
/littlefs/.upload_state.v2.cloud
/littlefs/.upload_state.v2.cloud.log
/littlefs/.backend_summary.smb
/littlefs/.backend_summary.cloud
/littlefs/syslog.0.txt
/littlefs/syslog.1.txt
/littlefs/syslog.2.txt
/littlefs/syslog.3.txt
/littlefs/crash_log.txt
```

### On Network Share
Files are uploaded maintaining the same structure:
```
//server/share/
├── Identification.json
├── Identification.crc
├── Identification.tgt
├── STR.edf
├── DATALOG/
│   ├── 20241114/
│   │   ├── file1.edf
│   │   └── file2.edf
│   └── 20241113/
└── SETTINGS/
    ├── CurrentSettings.json
    └── CurrentSettings.crc
```

---

## Technical Note: Unified Stealth SD Card Access

The firmware implements a single, unified stealth mode approach for SD card access to ensure the CPAP machine is never disrupted:

1. **Capture**: Before `SD_MMC.begin()` is called (which sends CMD0), a stealth probe reads the card's RCA, current state, and bus width.
2. **Mount & Access**: The card is mounted and accessed normally.
3. **Restore**: After `SD_MMC.end()`, the card is restored to its exact pre-mount state (RCA, bus width, selected/deselected).

This ensures the CPAP machine resumes seamlessly without noticing the interruption.

*(Historical Note: Earlier versions used a custom FAT32 parser specifically for AS10 boot-time config reads. This has been retired in favor of the safer, unified capture/restore approach for all devices).*

---

