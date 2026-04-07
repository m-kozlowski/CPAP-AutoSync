# CPAP AutoSync v3.6i-beta1 — Changelog

> **Note:** v3.6i-beta1 is a focused hardening and UX polish release for the AP Setup Wizard, dashboard timezone display, and WiFi boot robustness. No changes to upload logic, SD card handling, or cloud backend behaviour.

## What's New in v3.6i-beta1

### Timezone — Robust IANA Name Persistence (fixes setup page reverting timezone)

A long-standing bug caused the Setup Wizard to revert your saved timezone to a similar-but-wrong city (e.g. `Australia/Melbourne` → `Australia/Sydney`) every time you reopened `/setup`. This happened because multiple cities share the same POSIX TZ string and the page was doing a reverse-lookup by POSIX value rather than by name.

**Fix:** The Setup Wizard now saves a new `TZ_NAME` key to `config.txt` alongside the existing `TZ_STRING`. On reload, the dropdown is pre-selected by IANA name — no ambiguous reverse-lookup. The firmware ignores `TZ_NAME` and continues to use only `TZ_STRING` at runtime.

### Timezone — Better Timezone Data Source

The Setup Wizard previously fetched timezone data from a CSV file that required fragile regex parsing. It now fetches `zones.json` from the same source — a native JSON object that is parsed directly with no regex. This is both simpler and more reliable.

- **Online:** Latest `zones.json` fetched at page load
- **Offline / AP mode (first setup):** Build-time embedded copy (`BUILTIN_TZ`) used as fallback — the timezone dropdown works even with no internet connection
- **If both fail:** Plain POSIX text input shown with a link to the format reference

### Dashboard Timezone Display — Human-Readable UTC Offset

The dashboard **Timezone** field previously showed a raw POSIX string (e.g. `AEST-10AEDT,M10.1.0,M4.1.0/3`), which is not user-friendly.

**Fix:** The offset is now computed server-side using `localtime_r` vs `gmtime_r` on the ESP32's already-synced clock. This gives the **live UTC offset including active Daylight Saving Time** — no lookup table required.

Display examples:
- `UTC+10:00` (AEST, standard time)
- `UTC+11:00 (DST active)` (AEDT, summer time)
- `UTC+5:30` (IST, fixed offset)
- Falls back to `GMT_OFFSET_HOURS` if no `TZ_STRING` is set

### Setup Wizard UX — SleepHQ Help Text Repositioned (with ℹ️)

The SleepHQ API credentials instructions now appear **above** the Client ID and Client Secret fields (not below them). This means users see how to get their credentials **before** they try to fill in the form, not after. The instruction panel uses an ℹ️ info banner style consistent with other contextual help in the UI.

Instructions read:
> ℹ️ To get your API credentials: go to **sleephq.com/account**, click **Account Settings** in the lower-left side, navigate to **API Keys**, then click **Add API Key**.

### AP Mode — Cold-Boot Gate (`g_apModeAllowed`)

AP mode can now only start on a **genuine cold-boot** (power-on or external hard reset). It is permanently blocked for:

| Reset cause | AP mode |
|---|---|
| Power-on (`ESP_RST_POWERON`) | ✅ Allowed |
| External reset (`ESP_RST_EXT`) | ✅ Allowed |
| Unknown (`ESP_RST_UNKNOWN`) | ✅ Allowed (conservative) |
| Software reboot (`ESP_RST_SW`) | ❌ Blocked |
| Panic / crash (`ESP_RST_PANIC`) | ❌ Blocked |
| Watchdog (`ESP_RST_WDT` / `*_TASK_WDT` / `*_INT_WDT`) | ❌ Blocked |
| Brownout recovery (`ESP_RST_BROWNOUT`) | ❌ Blocked |

**Why this matters:** Previously, if you saved wrong WiFi credentials via the Setup Wizard, the device would soft-reboot, fail to connect, and start an unexpected AP. If your phone had auto-reconnected to your home network, you'd never notice the CPAP-AutoSync AP was sitting there and couldn't figure out why the device hadn't come online. Now:

- **Correct credentials:** Device connects, all good.
- **Wrong credentials:** Device logs the failure and continues without AP mode. **To re-enter setup**: power-cycle the device.

A log line `[AP] Reset reason N → AP mode ALLOWED/BLOCKED` is emitted on every boot for diagnostics.

### AP Mode — WiFi 2-Attempt Retry

The single `connectStation()` call is now replaced with two attempts separated by a 3-second pause. This gives slow or busy routers (especially mesh systems during channel switching) a second chance before the device gives up and starts AP mode.

- Attempt 1 → if failed, wait 3s → Attempt 2 → if still failed → AP (cold-boot only)

### Setup Wizard — Wrong Credentials Warning

After clicking **Save & Restart**, the success banner now includes an explicit power-cycle warning:

> ✅ Configuration saved! Rebooting now…
> If WiFi credentials are correct, the device will connect to your network.
> ⚠️ If the device cannot connect (e.g. wrong password), power-cycle it to re-enter setup mode.

This is consistent with the design of the existing Config Edit lock notice in the main dashboard.

---

### New Config Key

| Key | Default | Description |
|---|---|---|
| `TZ_NAME` | *(empty)* | IANA timezone name (e.g. `Australia/Melbourne`). Written automatically by the Setup Wizard. Used only by the web UI to restore the timezone dropdown — the firmware ignores it. Do not set manually; it is managed by the wizard. |

### Upgrade Notes

- **v3.5i → v3.6i-beta1:** OTA upgrade supported
- **No manual config.txt changes required.** The new `TZ_NAME` key is written automatically next time you open `/setup` and save. Until then, the existing `TZ_STRING` continues to work exactly as before.
- **If your timezone was being reset to the wrong city** (Sydney vs Melbourne etc.): open `/setup`, confirm the correct timezone is shown, and click **Save & Restart**. The `TZ_NAME` key will be written and the issue will not recur.

---

**Full Specification:** See `docs/archive/69-AP-SETUP-PLAN.md` §9 for the detailed implementation notes.

---

## How to Flash (Web Flasher)

**For users upgrading from v3.5i:**

1. Open the web interface at `http://cpap.local` (or `http://<device-ip>/`)
2. Go to the **OTA** tab
3. Upload `firmware-ota-upgrade.bin`
4. Wait for the device to restart (~30 seconds)

**For users doing a full USB flash (new installation or recovery):**

1. **Prepare the hardware:**
   - Connect the SD WIFI PRO to the development board with switches set to:
     - Switch 1: OFF
     - Switch 2: ON

2. **Use a desktop Chromium-based browser** (Chrome, Edge, or Opera — Firefox/Safari not supported)

3. **Steps:**
   - Extract the release ZIP to a folder on your computer
   - Open https://esptool.spacehuhn.com/ in Chrome, Edge, or Opera
   - Connect the ESP32 board by USB
   - Click **Connect** and choose the serial port
   - Delete any existing rows, click **Add**, set address to **`0x0`**
   - Select **`firmware-ota.bin`**
   - **Click Erase first** (mandatory for clean state)
   - Click **Program**

**CRITICAL:** After a full USB flash, update `config.txt` on your SD card with your actual WiFi credentials before inserting the ESP32 into your CPAP machine.
