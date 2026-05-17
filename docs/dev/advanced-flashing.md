# Advanced Flashing & Troubleshooting

## Finding Your Serial Port

**Note:** Connect the SD WIFI PRO to the development board with switches set to:
- Switch 1: OFF
- Switch 2: ON

**Easiest method:** Open the web flasher, click **Connect**, then plug in the board. The newly appeared port is usually the correct one.

### Windows
1. Open Device Manager (Win+X, then select Device Manager)
2. Expand "Ports (COM & LPT)"
3. Look for entries such as:
    - `USB Serial (COM5)`
    - `USB-SERIAL CH340 (COMx)`
    - `Silicon Labs CP210x USB to UART Bridge (COMx)`
4. Note the COM port number (for example `COM3`, `COM4`, `COM5`)

**Tip:** If you use the included Windows upload script, running it without arguments will also show COM port instructions.

### macOS
```bash
ls /dev/cu.*
```
Look for `/dev/cu.usbserial-*`, `/dev/cu.SLAB_USBtoUART`, or another newly appeared USB serial device

### Linux
```bash
ls /dev/ttyUSB* /dev/ttyACM*
```
Usually `/dev/ttyUSB0` or `/dev/ttyACM0`

---

## Fallback: Upload Scripts and Manual Firmware Upload

The browser-based web flasher is the preferred method for most users.
If it is not available on your system, use the included upload scripts or `esptool` directly:

### Windows
```cmd
REM Install esptool
pip install esptool

REM OTA firmware
python -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 firmware-ota.bin

REM Standard firmware
python -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 firmware-standard.bin
```

### macOS/Linux
```bash
# Install esptool
pip install esptool

# OTA firmware
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 firmware-ota.bin

# Standard firmware
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 firmware-standard.bin
```

**Note:** The firmware files are complete images (bootloader + partitions + app) and must be flashed at address `0x0`.

---

## Getting More Information (Serial Monitor & Logs)

**View Serial Monitor Output**
```bash
# Windows (using PlatformIO)
pio device monitor

# Linux/Mac
screen /dev/ttyUSB0 115200
# or
sudo pio device monitor
```

**View Logs via Web Interface**
```
http://<device-ip>/logs
```

**Check Upload State**
- Look for `/littlefs/.upload_state.v2.smb`/`.cloud` and their `.log` files in internal LittleFS
- Contains upload history, retry counts, and incremental journal updates for each backend

---

## Package Contents

- `firmware-ota.bin` - Complete firmware for initial flashing (1.3MB)
- `firmware-ota-upgrade.bin` - App-only binary for web OTA updates (1.2MB)
- `upload-ota.bat` - Windows fallback upload script
- `upload.sh` - macOS/Linux fallback upload script
- `requirements.txt` - Python dependencies (esptool)
- `config.txt.example.simple` - Minimal configuration (bare essentials)
- `config.txt.example.smb` - SMB/network share configuration
- `config.txt.example.sleephq` - SleepHQ cloud configuration
- `config.txt.example.both` - Dual upload (SMB + SleepHQ)
- `README.md` - This file

---

