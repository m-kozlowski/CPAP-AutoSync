# Implementation Plan: AP Setup UI Revamp (v2)

> **File:** `docs/archive/69-AP-SETUP-PLAN.md`
> **Branch:** `roadmap-to-4.0`
> **Goal:** Replace the current `src/web/setup.html` with a fully-revamped, opinionated configuration UI that:
> - Looks and feels like the existing dark-mode main dashboard (`web_ui.h`)
> - Shows only the settings 99% of users need, in a clear, guided way
> - Is friendly enough for a first-time user or someone who just formatted a blank SD card

---

## 1. Context: What Was Built and What Needs to Change

Two commits were added since `fbd10fad`:

| Commit | Summary |
|---|---|
| `9544480` | Added AP mode trigger in `main.cpp`, DNS captive portal, `WiFiManager` changes, `handleApiWifiScan`, `handleSetupPage` serving gzipped `setup.html` |
| `dc668af` | Added `setup_html_gz.h` to `.gitignore`, Python compression script `scripts/generate_html_gz.py` |

The AP infrastructure itself is solid. What needs replacing is `src/web/setup.html` — its style, UX, schema, and several functional bugs.

---

## 2. Issues to Resolve (Prioritised)

### 2.1 Visual Style — Must Match Main Dashboard

**Current problem:** `setup.html` uses a light-grey `#f5f5f5` background with plain white cards and generic `system-ui` font styling. This is completely inconsistent with the main dashboard, which has a dark `#0f1923` background, `#1b2838` cards, `#66c0f4` accents, animated badges, and the same brand header SVG.

**Plan:**
- Adopt the **exact same CSS design tokens** from `web_ui.h`: `--bg: #0f1923`, `--card: #1b2838`, `--border: #2a475e`, `--accent: #66c0f4`, `--text: #c7d5e0`, `--muted: #8f98a0`.
- Reuse the **same header SVG** (the animated CPAP AutoSync logo with WiFi arcs and spinning ring) verbatim from `web_ui.h`. This is a significant brand consistency win.
- Use the same `.card`, `.btn`, `.bp`, `.bs`, `.bd`, `.fg`, `.sm` CSS class conventions.
- Font: `-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif` (same as main SPA).
- Keep `max-width: 800px; margin: 0 auto; padding: 16px;` layout (slightly wider than current, same as main dashboard's `900px` but appropriate for a form).

**Implementation note:** The CSS does not need to be byte-for-byte identical to `web_ui.h` (since `web_ui.h` has many classes the setup page will never use), but it must visually match. A dedicated `<style>` block in `setup.html` is perfectly fine — no shared CSS file is needed.

---

### 2.2 WiFi Scan — Deduplicate SSIDs

**Current problem:** `handleApiWifiScan` returns every AP independently (including multiple BSSIDs of the same mesh network SSID). The current `setup.html` renders all of them, so "GGL" appears three times with different signal strengths.

**Plan — Server-side dedup (recommended):**

Modify `handleApiWifiScan` in `CpapWebServer.cpp` to deduplicate by SSID, keeping **only the strongest RSSI** for each unique SSID:

```cpp
// In handleApiWifiScan(), after scanning:
// Build a map: ssid -> strongest rssi, auth type
// Then emit only distinct SSIDs sorted by RSSI descending.
```

Implementation steps:
1. Allocate a small fixed-capacity dedup table on the stack (e.g., `struct { String ssid; int rssi; int auth; } dedup[32]`).
2. Iterate scan results; for each SSID, either insert if not seen or update if stronger RSSI.
3. Sort by RSSI descending before emitting JSON.
4. Emit only the deduped list.

**Why server-side:** Keeps client-side JS simpler. The ESP does the scan anyway; dedup costs negligible stack space.

**UI change:** Remove the dropdown entirely. Instead, use a **radio-button list** showing the deduplicated SSIDs with a lock icon (🔒/🔓) and RSSI. Tapping/clicking a radio immediately fills the SSID text field. This matches the look seen in the screenshots (the radio-list UI) but now with deduplicated entries. The explicit SSID text input remains below as the authoritative value and can be overridden manually.

Design:
```
[ Scan for Networks ]   <-- button

○  🔒 GGL              -59 dBm
○  🔒 Asus_Ax          -80 dBm
○  🔓 OpenNetwork      -85 dBm

WiFi Password
[ ••••••••                  ] 👁
```

---

### 2.3 Remove BSSID Lock Field

**Current problem:** The `WIFI_BSSID` field encourages locking to a single AP, which breaks mesh network roaming.

**Plan:** Remove `WIFI_BSSID` from the SCHEMA entirely. Any existing `WIFI_BSSID` key in `config.txt` must be **preserved as-is** via the "unknown keys → raw/advanced bucket" logic — the JS parser should not touch it. This way, advanced users who manually added `WIFI_BSSID` don't lose it when they save through the GUI.

> **Note:** Ensure `WIFI_BSSID` is not on the known-key list so it falls through to the "advanced/unrecognized" bucket.

---

### 2.4 Password Eye-Icon Toggle

**Current problem:** Passwords are plain `type="password"` with no way to verify what was typed. The "Leave as `******` to keep current password" hint is confusing.

**Plan:**

Every password field gets a `👁` toggle button injected next to it:

```html
<div class="pwd-wrap">
  <input type="password" id="field-WIFI_PASSWORD" ...>
  <button type="button" class="eye-btn" onclick="togglePwd(this)">👁</button>
</div>
```

CSS:
```css
.pwd-wrap { position: relative; display: flex; align-items: center; }
.pwd-wrap input { flex: 1; padding-right: 36px; }
.eye-btn { position: absolute; right: 6px; background: none; border: none;
           cursor: pointer; color: #8f98a0; font-size: 1.1em; padding: 0; }
.eye-btn:hover { color: #66c0f4; }
```

JS:
```js
function togglePwd(btn) {
  const inp = btn.previousElementSibling;
  inp.type = inp.type === 'password' ? 'text' : 'password';
  btn.textContent = inp.type === 'password' ? '👁' : '🫣';
}
```

**Password masking UX — key design decision:**

The confusing "Leave as `******` to keep current password" message must go. The new design:

1. When `buildConfigString()` runs, if a password field's `value === '******'` **OR is blank**, the original raw value is preserved from `rawLines` (no change to the config file for that key).
2. If the user types a new value, it replaces the old one.
3. The placeholder text reads: `Enter new password (leave blank to keep existing)`.
4. The input is initially **empty** (not pre-filled with `******`). This is clean and unambiguous.

This means the JS must **not** pre-populate password fields with `******`. Instead, it should leave them blank and use the "blank = keep" logic in `buildConfigString`.

> **Important note on key naming:** The actual `config.txt` key is `WIFI_PASSWORD` (not `WIFI_PASS`). The masking logic in `CpapWebServer.cpp` currently checks for `WIFI_PASS=` (a bug — it won't mask `WIFI_PASSWORD=`). This discrepancy needs to be fixed as part of this work. See §2.9 below.

---

### 2.5 Timezone Selector — Fix the Bug

**Current problem:**
- The CSV from GitHub is comma-delimited, but timezone names themselves can contain commas (e.g., `"America/Indiana/Indianapolis"`). The current `line.split(',')` breaks on the first comma, leaving the leading double-quote in `parts[0]`.
- The "Your local timezone" sub-label below the POSIX text field is confusing.
- The POSIX string leaks into a visible text field that the user doesn't need to understand.

**Root cause:** The zones.csv format is:
```
"America/New_York","EST5EDT,M3.2.0,M11.1.0"
```
Both columns are double-quoted. A naive `split(',')` yields `parts[0] = '"America/New_York"'` (with quotes) and `parts[1] = '"EST5EDT'` (split at the comma inside the POSIX string!).

**Plan — Correct CSV parsing:**

Use a minimal quoted-CSV parser:
```js
function parseZonesCsv(csv) {
  return csv.split('\n').map(line => {
    line = line.trim();
    if (!line) return null;
    // Match two double-quoted fields
    const m = line.match(/^"([^"]+)","([^"]+)"$/);
    if (!m) return null;
    return { name: m[1], posix: m[2] };
  }).filter(Boolean);
}
```

This correctly handles all rows including those with commas in the POSIX string.

**UI redesign — clean timezone selector:**

Remove the visible raw POSIX text field entirely from the user-facing UI. Show only:

```
Timezone
[ Australia/Melbourne ▾ ]      <-- searchable <select> populated from zones.csv
```

A hidden `<input type="hidden" id="field-TZ_STRING">` stores the selected POSIX value for `buildConfigString()`. The `<select>` shows **friendly names** (e.g., `Australia/Melbourne`), and selecting one updates the hidden field.

If the current TZ_STRING doesn't match any entry in the loaded list (custom/unknown), show "Custom (current value)" as a pre-selected option and show a secondary text input for manual override.

If `zones.csv` fails to load (no internet during AP mode), gracefully fall back to a plain text input with a placeholder like `e.g. AEST-10AEDT,M10.1.0,M4.1.0/3` and link to the POSIX TZ format doc — or just leave the field pre-filled from config.

**Loading state:** Show "Loading timezone list..." in the `<select>` while fetching. If the fetch fails, replace with a plain text input for the POSIX string.

**Note on AP mode / offline:** The ESP is in AP mode when the user first sets up. The user's device (phone/laptop) is connected to the CPAP-AutoSync AP and has no internet. The timezone CSV fetch from GitHub will fail. This is expected and must not block the UI. The graceful fallback is: show a plain text input pre-populated with the existing `TZ_STRING` value (or empty).

---

### 2.6 Upload Mode — SMART vs SCHEDULED

**Current problem:** The setup page shows raw `UPLOAD_START_HOUR` and `UPLOAD_END_HOUR` number inputs without context. SMART mode isn't selectable. INACTIVITY_SECONDS is visible when it shouldn't be.

**Plan — Mode-first UX:**

Replace the "Time & Schedule" section with a cleaner structure:

```
Upload Mode
( ) Smart — upload automatically when CPAP is idle
(•) Scheduled — upload only during a daily time window

  [only shown if Scheduled selected]
  Upload Window Start   [ 9  ] :00
  Upload Window End     [ 21 ] :00

Cooldown Between Uploads  [ 10 ] minutes
```

**SMART mode note:** Smart mode is always shown regardless of `g_pcntCapable`, since the user can choose it and the firmware will override on boot if the hardware doesn't support it (already documented in the dashboard). The setup UI shows a small info note: *"Smart mode requires 4-bit SD bus activity. On AirSense 10 machines, the firmware automatically switches to Scheduled mode."*

**Mapping to config keys:**
- Radio → `UPLOAD_MODE` value: `smart` or `scheduled`.
- Window fields → `UPLOAD_START_HOUR`, `UPLOAD_END_HOUR`.
- Cooldown → `COOLDOWN_MINUTES`.
- `INACTIVITY_SECONDS` → **hidden from UI, preserved in raw/advanced bucket** (never written unless already in config).

**`COOLDOWN_MINUTES` default note:** Show the default value (10) as the placeholder. Keep a small help text: *"Minutes to wait after completion before the next upload cycle."*

---

### 2.7 SleepHQ — Remove Team ID, Add Context

**Current problem:** Three SleepHQ fields are shown: Team ID, Client ID, Client Secret. Team ID is advanced.

**Plan:**

Show only:
```
☑ Upload to SleepHQ  [link: How to get credentials →]
  SleepHQ Client ID       [ _____ ]
  SleepHQ Client Secret   [ ••••• ] 👁
```

- `CLOUD_TEAM_ID` → moved to **Advanced/Raw config only**. Preserve any existing value.
- `CLOUD_CLIENT_ID` → maps to `CLOUD_CLIENT_ID` key.
- `CLOUD_CLIENT_SECRET` → maps to `CLOUD_CLIENT_SECRET` key (password field with eye toggle).
- The help link reads: *"Get your credentials from [SleepHQ → Settings → Developer](https://sleephq.com)"*.
- The "Enable Cloud Uploads" checkbox maps to `ENDPOINT_TYPE` including `CLOUD`.

**`ENDPOINT_TYPE` logic:** This is the trickiest mapping. The setup UI needs to reconcile `ENDPOINT_TYPE` (which can be `SMB`, `CLOUD`, or `SMB,CLOUD`) with two independent "Enable" checkboxes. The JS must:
1. Read `ENDPOINT_TYPE` from config.
2. Set `cloudEnabled = type.includes('CLOUD')` and `smbEnabled = type.includes('SMB')`.
3. On save, reconstruct `ENDPOINT_TYPE` from the two checkbox states: if both → `SMB,CLOUD`; cloud only → `CLOUD`; SMB only → `SMB`; neither → clear (or warn).

---

### 2.8 SMB — Clear, Split-Field UX

**Current problem:** The config uses `ENDPOINT = //hostname/share` format. The setup page splits this into `SMB_HOST` and `SMB_SHARE` fields that **don't exist as real config keys** — these are UI-only fields that must be recombined into `ENDPOINT` on save. Additionally, the "Target Folder Path" concept is confusing because the folder is *part of the share path* in the `ENDPOINT` key.

**Actual config.txt reality (audited from Config.cpp):**
- `ENDPOINT = //hostname/sharename` (the SMB UNC path)
- `ENDPOINT_TYPE = SMB` (or `SMB,CLOUD`)
- `ENDPOINT_USER = username`
- `ENDPOINT_PASSWORD = password`

There is **no separate folder concept** in the config — the `ENDPOINT` key is the full UNC path including the share root. The folder that files are copied *into* is determined by the uploader logic (it mirrors the SD card folder structure). `SMB_FOLDER` in the current setup.html is a fictitious key.

**Plan:**

```
☑ Upload to Network Share (SMB)

  Server Address (hostname or IP)    [ nas.local      ]
  Share Name                         [ cpap_backups   ]
  → Combined path: //nas.local/cpap_backups

  Username (optional)    [ username ]
  Password               [ ••••••   ] 👁
```

- Show the reconstructed `//host/share` path below the two fields as a live preview.
- On save, `ENDPOINT = //<host>/<share>`.
- On load, parse `ENDPOINT`: strip the leading `//`, split on the first `/` to get host and share.
- `ENDPOINT_USER` → Username field.
- `ENDPOINT_PASSWORD` → Password field (masked, blank = keep).
- **Remove "Target Folder Path" entirely** — it was a UI fiction.

---

### 2.9 Key Name Correctness Audit

A critical bug exists: `setup.html` uses **non-canonical config key names** that don't match what `Config.cpp` parses. This means values written by the setup page may be silently ignored by the firmware.

| setup.html key | Real config.txt key | Action |
|---|---|---|
| `WIFI_PASS` | `WIFI_PASSWORD` | **Fix in schema** |
| `SMB_PASS` | `ENDPOINT_PASSWORD` | **Fix in schema + masking** |
| `SMB_USER` | `ENDPOINT_USER` | **Fix in schema** |
| `SMB_HOST` | *(doesn't exist — part of `ENDPOINT`)* | **Remap to `ENDPOINT` via split/join** |
| `SMB_SHARE` | *(doesn't exist — part of `ENDPOINT`)* | **Remap to `ENDPOINT` via split/join** |
| `SMB_FOLDER` | *(doesn't exist)* | **Remove entirely** |
| `SLEEPHQ_TEAM` | `CLOUD_TEAM_ID` | **Move to Advanced** |
| `SLEEPHQ_CLIENT_ID` | `CLOUD_CLIENT_ID` | **Fix in schema** |
| `SLEEPHQ_CLIENT_SECRET` | `CLOUD_CLIENT_SECRET` | **OK already in masking logic** |
| `CLOUD_ENABLED` | *(doesn't exist — controlled by `ENDPOINT_TYPE`)* | **Remap; see §2.7** |
| `SMB_ENABLED` | *(doesn't exist — controlled by `ENDPOINT_TYPE`)* | **Remap; see §2.8** |
| `STEALTH_RESTORE` | `STEALTH_RESTORE` | **Remove from main UI → Advanced only** |
| `MINIMIZE_REBOOTS` | `MINIMIZE_REBOOTS` | **Remove from main UI → Advanced only** |
| `INACTIVITY_SECONDS` | `INACTIVITY_SECONDS` | **Remove from main UI → Advanced only** |

Also fix the masking in `CpapWebServer.cpp::handleApiConfigRawGet`:
- Line 1030 checks `WIFI_PASS=` but the actual key is `WIFI_PASSWORD=`. Fix to `WIFI_PASSWORD=`.
- Line 1031 checks `SMB_PASS=` but the actual key is `ENDPOINT_PASSWORD=`. Fix to `ENDPOINT_PASSWORD=`.
- The unmasking in `handleApiConfigRawPost` has the same mismatch (lines 1101–1108). Fix both.

---

### 2.10 System Settings — Simplify

**Remove from main UI:** `STEALTH_RESTORE`, `MINIMIZE_REBOOTS`, `INACTIVITY_SECONDS`. They all land in the "Advanced / Raw Config" bucket (preserved from existing config, editable there).

**Keep no "System Settings" section visible at all in the main form.** Advanced users have the raw textarea.

---

### 2.11 Advanced / Raw Config Section

Keep the collapsible "Advanced / Raw Config" section, but improve it:

1. The textarea is **read-only by default** and updates live as the user changes form values.
2. The textarea shows the full raw preview of the *entire* generated config payload (known + unknown).
3. It is collapsed by default (checkbox to reveal).
4. If a user insists on manual edits, they can check "Show raw config" and type directly.

**Recommended approach: Full preview (entire payload, collapsed by default).** It's the simplest and most predictable. The JS always applies form values first, then appends unknown/advanced keys, then writes the result.

---

## 3. Proposed New Section Layout

```
┌──────────────────────────────────────┐
│        [CPAP AutoSync SVG Logo]      │
│    Tap Settings. Save. Done.         │
└──────────────────────────────────────┘

[Status message area — success/error/info]

┌── WiFi ──────────────────────────────┐
│ SSID Text Field                      │
│ [Scan] → radio list of networks      │
│ Password  [••••••] 👁                │
└──────────────────────────────────────┘

┌── Timezone ──────────────────────────┐
│ [Australia/Melbourne ▾  ]            │
│ (offline fallback: text input)       │
└──────────────────────────────────────┘

┌── Upload Schedule ───────────────────┐
│ (•) Smart Mode                       │
│ ( ) Scheduled Mode                   │
│   [only if Scheduled:]               │
│   Window  09:00 to 21:00             │
│ Cooldown  [10] minutes               │
└──────────────────────────────────────┘

┌── SleepHQ ───────────────────────────┐
│ ☐ Upload to SleepHQ                  │
│   [if checked:]                      │
│   Client ID      [_______]           │
│   Client Secret  [•••••••] 👁        │
│   ℹ️ Get credentials → sleephq.com   │
└──────────────────────────────────────┘

┌── Network Share (SMB) ───────────────┐
│ ☐ Upload to Network Share            │
│   [if checked:]                      │
│   Server    [nas.local   ]           │
│   Share     [cpap_backup ]           │
│   → //nas.local/cpap_backup (preview)│
│   Username  [_______]                │
│   Password  [•••••••] 👁             │
└──────────────────────────────────────┘

┌── Advanced / Raw Config ─────────────┐
│ ☐ Show raw config (auto-generated)   │
│   [textarea, read-only preview]      │
└──────────────────────────────────────┘

           [Save & Restart]
```

---

## 4. Build Pipeline (No Minification — Keep Source Readable)

The existing approach from the initial implementation is retained:
- `src/web/setup.html` is the human-readable source (version-controlled, fully readable).
- `scripts/generate_html_gz.py` compresses it to `include/setup_html_gz.h` at build time.
- `include/setup_html_gz.h` is in `.gitignore` (auto-generated).
- PlatformIO `extra_scripts` runs the Python script before each build.

**No minification.** The HTML + JS + inline CSS is expected to be well under `~40 KB` plain-text, which compresses to `~15–20 KB` gzip. This is comfortably within what the ESP can serve in PROGMEM without any modification to the existing serving logic (which already sets `Content-Encoding: gzip`).

---

## 5. Files to Change

### `src/web/setup.html` — Full Rewrite

The entire file is replaced. New responsibilities:
- Dark-theme CSS matching `web_ui.h` visual language.
- Corrected SCHEMA with real config.txt key names.
- SSID radio-list UI (populated after scan, replaces the dropdown).
- Eye-icon password toggle on all password fields.
- ENDPOINT_TYPE ↔ checkbox reconciliation logic.
- ENDPOINT ↔ host/share split-field logic.
- Correct CSV parser for `zones.csv`.
- Hidden TZ field + searchable select.
- SMART/SCHEDULED radio selector.
- No BSSID, no Team ID, no STEALTH_RESTORE, no MINIMIZE_REBOOTS, no INACTIVITY_SECONDS in the main view.
- CLOUD_TEAM_ID, STEALTH_RESTORE, MINIMIZE_REBOOTS, INACTIVITY_SECONDS preserved in advanced bucket.
- Full raw-config preview in collapsible textarea.

### `src/CpapWebServer.cpp` — Password Masking Key-Name Fix

Fix the password masking/unmasking key names:

```cpp
// handleApiConfigRawGet — masking:
if (trimmed.startsWith("WIFI_PASSWORD=") && trimmed.length() > 14) prefix = "WIFI_PASSWORD=";
else if (trimmed.startsWith("ENDPOINT_PASSWORD=") && trimmed.length() > 18) prefix = "ENDPOINT_PASSWORD=";
else if (trimmed.startsWith("CLOUD_CLIENT_SECRET=") && trimmed.length() > 20) prefix = "CLOUD_CLIENT_SECRET=";

// handleApiConfigRawPost — unmasking:
if (trimmed == "WIFI_PASSWORD=******") {
    prefix = "WIFI_PASSWORD="; origVal = config->getWifiPassword();
} else if (trimmed == "ENDPOINT_PASSWORD=******") {
    prefix = "ENDPOINT_PASSWORD="; origVal = config->getEndpointPassword();
} else if (trimmed == "CLOUD_CLIENT_SECRET=******") {
    prefix = "CLOUD_CLIENT_SECRET="; origVal = config->getCloudClientSecret();
}
```

### `src/CpapWebServer.cpp` — SSID Deduplication in WiFi Scan

Modify `handleApiWifiScan()` to deduplicate by SSID (keep strongest RSSI), sort descending by RSSI, and emit JSON only for unique SSIDs. The `bssid` field is **not included** in the JSON response.

---

## 6. What Stays the Same

- AP mode triggering logic in `main.cpp` — no changes needed.
- DNS captive portal — no changes needed.
- `handleSetupPage()` serving gzipped content — no changes.
- `/api/config-raw` GET/POST endpoints — only the masking key-names fix (§5).
- `/api/wifi-scan` endpoint signature — just dedup logic added.
- `scripts/generate_html_gz.py` — no changes.
- `platformio.ini` build scripts — no changes.

---

## 7. Open Questions / Decisions Required

### Q1 — Upload mode for first-time setup (blank config)

When `config.txt` doesn't exist, the JS fetches `/api/config-raw` and gets an empty string. The UI should then show smart defaults. Recommended defaults:
- `UPLOAD_MODE: smart`
- `UPLOAD_START_HOUR: 9`, `UPLOAD_END_HOUR: 21`
- `COOLDOWN_MINUTES: 10`
- Both SleepHQ and SMB unchecked

**Do you agree with these defaults?**

### Q2 — SleepHQ credentials link

The UI will say "Get your credentials from SleepHQ → Settings → Developer". Should this be a clickable hyperlink to `https://sleephq.com/oauth/applications`? In AP mode, the phone is on the ESP's AP network and has no internet — but the link should still be shown (it just won't open). Clicking it could open in a new tab once they're back on WiFi.

**Confirm: include the link even if not clickable in AP mode.**

### Q3 — `HOSTNAME` field

This was not in the current setup.html. Should it be added to the main form (between WiFi section and Timezone), or kept in Advanced only?

**Recommendation:** Include it in the main form — it's useful for mDNS discovery and users who set up multiple devices. Simple text input with `cpap` as default/placeholder.

### Q4 — `NTP_SERVER` field

Custom NTP server is rare (99.9% of users leave it empty and use DHCP/pool.ntp.org). Recommend Advanced-only. **Confirm.**

### Q5 — Save flow in AP mode vs STA mode

Currently, "Save & Restart" triggers `/api/config-raw POST` then `/soft-reboot GET`. In AP mode, after reboot the device tries to connect to the newly configured WiFi. The UI shows "Rebooting — if WiFi credentials are correct, the device will connect to your network." This is the right behaviour. **No change needed here.**

---

## 8. Verification Plan

1. **Build check:** Ensure `scripts/generate_html_gz.py` produces a valid `setup_html_gz.h` file and the project compiles cleanly.
2. **Browser test (desktop):** Open `setup.html` directly in a browser (pointing `fetch` to a local mock or real device IP) to verify:
   - Dark theme renders correctly.
   - WiFi scan populates deduplicated radio list.
   - Timezone selector populates from zones.csv; correct POSIX stored in hidden field.
   - ENDPOINT is correctly split on load and recombined on save.
   - Unknown keys (e.g., `CLOUD_TEAM_ID`) are preserved in the raw output.
   - Passwords masked as `******` in GET response do not appear in the field; blank field on save correctly restores original.
3. **Device test — AP mode:** Flash device, remove config.txt, boot. Connect phone to CPAP-AutoSync AP. Confirm captive portal redirects to setup page. Verify all sections render on mobile. Fill in WiFi + SleepHQ credentials. Save. Confirm device reboots and connects.
4. **Device test — STA mode:** Navigate to `http://cpap.local/setup` or `http://<ip>/setup` while in normal operation. Confirm existing config loads, edits work, save triggers soft reboot.
5. **Masking regression:** Confirm `WIFI_PASSWORD`, `ENDPOINT_PASSWORD`, and `CLOUD_CLIENT_SECRET` are correctly masked in GET response and correctly restored on POST.
