# AP Setup Page Review & Recommendations

> **File:** `docs/archive/70-AP-SETUP-REVIEW.md`
> **Date:** 2026-04-12
> **Related:** GitHub Issue #63, `docs/archive/68-AP-SETUP-MODE.md`, `docs/archive/69-AP-SETUP-PLAN.md`

---

## 1. Context

The `/setup` page serves dual duty:
- **SoftAP mode** (first-time setup, WiFi failure recovery) — the *only* accessible page
- **Regular WebUI** — accessed via "⚙ Open Setup Wizard" button on the Config tab

GitHub Issue #63 reports that after "Save & Restart" the page stays on "saving configuration" and never redirects back to the main dashboard. This is a symptom of the broader problems outlined below.

---

## 2. Issue A: Navigation — Back Button in Regular WebUI Mode

### Problem

When `/setup` is accessed from the regular WebUI (Config tab → "⚙ Open Setup Wizard"), there is no way to navigate back. The user must manually edit the URL to return to `/`. In SoftAP mode, no back button should exist since `/setup` is the entire UI.

### Current State

- `g_apSetupMode` is a server-side global (`main.cpp:98`) — `true` only when the ESP is running as a SoftAP
- The `/api/status` endpoint does **not** currently expose `g_apSetupMode`
- The `/setup` page has no client-side logic to detect which mode it's in
- The page has two "Save & Restart" buttons (top and bottom) — the top one is redundant per user request

### Detection Options

| Method | Pros | Cons |
|---|---|---|
| **Check `window.location.hostname === '192.168.4.1'`** | Zero network overhead, instant, no server changes | Hardcoded IP assumption; could break if ESP AP IP is ever changed |
| **Query `/api/status` for `ap_setup` field** | Authoritative (server ground truth), future-proof | Extra HTTP request on page load; requires server-side change to expose `g_apSetupMode` in status JSON |
| **Check if `/` returns the main SPA** | No server changes | Slow (extra request), fragile (depends on response content) |
| **Server injects a flag into the HTML** | Zero client overhead, always correct | Requires either a separate HTML variant or dynamic injection into the gzipped response |

### Recommendation: `/api/status` field only

1. Add `ap_setup: true/false` to the `/api/status` JSON response. The `/setup` page JS fetches `/api/status` on load and reads the `ap_setup` field.

2. This is authoritative (server ground truth), future-proof, and avoids hardcoding IP assumptions.

3. The setup page already fetches `/api/config-raw` on load; adding one more lightweight fetch for `/api/status` is negligible overhead.

**Why not IP-based detection?** Checking `window.location.hostname === '192.168.4.1'` is fragile — the AP IP could be configured differently, and relying on a hardcoded IP is a maintenance liability. The server already knows its mode; let it tell the client.

**Why not server-injected HTML?** The setup page is served as a pre-gzipped PROGMEM blob. Injecting a dynamic flag would require either decompressing/recompressing on every request (expensive on ESP32) or maintaining two separate HTML blobs. Neither is worth it for a single boolean.

### Implementation

- If **not SoftAP mode** (regular WebUI): show a "← Back to Dashboard" link/button at the top of the page, styled as a secondary button (`class="btn bs"`), linking to `/`
- If **SoftAP mode**: no back button, no link to `/`
- Remove the top "Save & Restart" button (`#btn-save-top`) — keep only the bottom one

---

## 3. Issue B: Post-Save Feedback & WiFi Verification

### Problem

After "Save & Restart" in SoftAP mode:
1. The ESP reboots and tries to connect to the user's WiFi
2. The user's phone/laptop disconnects from the SoftAP and reconnects to their normal WiFi
3. The page currently shows a static message and never redirects — the user is left guessing

### What the Existing Plans Propose

`68-AP-SETUP-MODE.md` §6 explicitly recommends **against** `WIFI_AP_STA` simultaneous mode (correct — channel hopping kills the AP). It proposes:
1. User clicks Save → config posted → ESP reboots
2. JS shows "Rebooting and connecting to WiFi..."
3. If connection fails, the ESP falls back to AP mode automatically

`69-AP-SETUP-PLAN.md` §5/Q5 notes the current "Save & Restart" triggers POST then `/soft-reboot`, and says "no change needed" — but this is wrong, because the user gets no feedback about whether the new credentials worked.

### Analysis of the WiFi Verification Problem

The fundamental challenge: **the browser loses its connection to the ESP the moment the SoftAP goes down**. There is no way for the browser to poll the ESP during the transition. The verification must happen *after* the user's device reconnects to the normal WiFi network.

### Recommended Approach: "Reconnection Assistant" Modal

After "Save & Restart" is clicked in SoftAP mode, show a **full-screen modal overlay** (matching the existing `cfg-warn-modal` style: blurred background, centered card) with a multi-phase workflow:

```
┌─────────────────────────────────────────────┐
│  ⚙ Saving & Rebooting...                    │
│                                             │
│  Phase 1: Configuration saved ✅            │
│  Phase 2: Device is rebooting... ⏳         │
│                                             │
│  Your device is now connecting to:          │
│  📶 MyWiFiNetwork                           │
│                                             │
│  Your phone will disconnect from the        │
│  CPAP-AutoSync network shortly.             │
│                                             │
│  ─────────────────────────────────          │
│  Once your phone reconnects to your         │
│  normal WiFi, the page will automatically   │
│  try to reach the device.                   │
│                                             │
│  Attempting to reconnect... (1/10)          │
│  ████████░░░░░░  15s                        │
│                                             │
│  ℹ️ If connection fails after 60s, the       │
│  device may have fallen back to AP mode.    │
│  Power-cycle it to re-enter setup.          │
└─────────────────────────────────────────────┘
```

**Phase 1 (0–3s):** POST config → show "Configuration saved ✅"
**Phase 2 (3–5s):** Trigger `/soft-reboot` → show "Device is rebooting..."
**Phase 3 (5s+):** Begin reconnection attempts

### Reconnection Logic (JS)

```js
// After soft-reboot is triggered:
// 1. Wait ~8s for ESP to reboot + connect to WiFi
// 2. Then start polling http://<target-ip>/api/status
//    (we don't know the new IP yet — see below)
// 3. On success: redirect to http://<new-ip>/
// 4. On timeout (60s): show "connection failed" with instructions
```

**The IP problem:** In SoftAP mode, the browser is at `192.168.4.1`. After reboot, the ESP gets a DHCP address on the user's WiFi (e.g., `192.168.1.42`). The browser doesn't know this address.

**Solutions to the IP problem:**

| Approach | Feasibility |
|---|---|
| **mDNS (`<hostname>.local`)** | Best option. After the user's phone reconnects to WiFi, `<hostname>.local` resolves. JS can poll `http://<hostname>.local/api/status`. Works on most home networks. The hostname is read from the config (see below). |
| **User enters the IP manually** | Fallback. Show a text input: "Enter the device's new IP address (check your router)". Then poll that IP. |
| **ESP writes IP to a cloud service** | Overkill. Requires internet, adds complexity. |

**Hostname for mDNS:** The JS must use the configured `HOSTNAME` from `config.txt`, not a hardcoded value. The logic is:
1. Read `HOSTNAME` from the parsed config lines (already available in `rawLines`)
2. If `HOSTNAME` is set and non-empty → use that value for mDNS
3. If `HOSTNAME` is absent or empty → use the firmware default `cpap`
4. Reconnection target: `http://<hostname>.local/api/status`

### Recommended Reconnection Flow

1. After save + reboot trigger, show the modal with phases
2. Phase 3 begins polling `http://<hostname>.local/api/status` every 3 seconds (hostname from config, default `cpap`)
3. If `<hostname>.local` resolves and responds → redirect to `http://<hostname>.local/` (main dashboard)
4. If 30s pass with no response → show IP input field as fallback: "Enter the device IP (find it on your router's DHCP client list)"
5. If 60s total → show failure message with power-cycle instructions
6. **Important:** The JS must handle the fact that `fetch()` to `cpap.local` will fail while the phone is still on the SoftAP. This is expected. The polling should silently retry without alarming the user.

### For Regular WebUI Mode

In regular WebUI mode, the save flow is simpler because the browser stays on the same network:
1. POST config → show "Configuration saved ✅"
2. Trigger `/soft-reboot`
3. The existing `reboot-overlay` pattern from the main dashboard already handles this (polls `/api/status`, shows "RECONNECTING" badge, auto-recovers)
4. After recovery, redirect to `/` (dashboard)

This is essentially what the main dashboard's `saveAndReboot()` already does (`web_ui.h:861-863`). The `/setup` page should adopt the same pattern.

---

## 4. Issue C: UI Consistency & Config Editor Unification

### Problem 1: Duplicate Save Buttons

The `/setup` page currently has two "Save & Restart" buttons — one at the top (`#btn-save-top`) and one at the bottom (`#btn-save`). The user wants only the bottom one, matching the rest of the UI's pattern (action buttons at section bottom).

### Problem 2: No Modal Confirmation

The main WebUI's Config tab uses a modal dialog workflow:
1. Click "✎ Edit" → `cfg-warn-modal` appears (blurred background, warning about SD card access)
2. User confirms → lock acquired → textarea becomes editable
3. Click "Save & Reboot" → saves, shows "rebooting… redirecting in 10s"

The `/setup` page has no such confirmation. Clicking "Save & Restart" immediately saves and reboots with no undo. This is a UX regression compared to the main Config editor.

### Problem 3: Two Places to Edit config.txt

Currently there are two config editing surfaces:
1. **Config tab** → raw textarea editor (with lock/modal workflow)
2. **/setup page** → structured form with sections, radio buttons, scan, etc.

Having two editors for the same file is confusing and error-prone. The `/setup` page is strictly superior in UX (guided, structured, validated). The raw textarea should be a fallback only.

### Recommendation: Unify Under /setup

**Phase 1 (immediate):** Replace the Config tab's "✎ Edit" → raw textarea flow with a redirect to `/setup`. The Config tab becomes a landing page that says "Use the Setup Wizard to edit your configuration" with a prominent "⚙ Open Setup Wizard" button (which already exists). Remove the raw textarea editor from the main dashboard entirely.

**Phase 2 (follow-up):** Add an "Advanced / Raw Config" collapsible section *inside* `/setup` that serves the same purpose as the old raw editor. This already exists in the current `/setup` page (`#advanced-raw` textarea). Ensure it has the same lock/unlock workflow for safety.

### Save & Restart UX

For the "Save & Restart" button in `/setup`:

1. **Click** → show a confirmation modal (matching `cfg-warn-modal` style):
   ```
   ┌─────────────────────────────────────────────┐
   │  ⚠ Save & Restart                           │
   │                                             │
   │  This will save your configuration and      │
   │  reboot the device. All active uploads      │
   │  will be interrupted.                        │
   │                                             │
   │  After reboot, always eject and reinsert     │
   │  the SD card before powering on the CPAP.    │
   │                                             │
   │  [Cancel]              [Save & Restart]     │
   └─────────────────────────────────────────────┘
   ```

2. **Confirm** → execute save, then show the reconnection assistant modal (Issue B above)

3. **In SoftAP mode:** the modal includes the full reconnection workflow (phases, polling, fallback IP input)
4. **In regular WebUI mode:** the modal shows a simpler "rebooting… redirecting in 10s" with auto-redirect to `/` (same as current dashboard behavior)

### Button Placement

- Remove `#btn-save-top` entirely
- Keep only the bottom "Save & Restart" button
- Add a "← Back to Dashboard" secondary button (SoftAP: hidden; WebUI: visible) next to the save button at the bottom

---

## 5. Summary of Recommendations

| Issue | Recommendation | Priority |
|---|---|---|
| **A: Back button** | Detect SoftAP via `192.168.4.1` IP check + `/api/status.ap_setup` field. Show "← Back to Dashboard" only in regular WebUI mode. | High |
| **A: Top save button** | Remove `#btn-save-top`. Keep only bottom save button. | High |
| **B: Post-save feedback (SoftAP)** | Reconnection Assistant modal: phase indicators → mDNS poll (`cpap.local/api/status`) → IP fallback input → timeout with power-cycle instructions. | High |
| **B: Post-save feedback (WebUI)** | Reuse existing `reboot-overlay` pattern: poll `/api/status`, auto-redirect to `/` after recovery. | Medium |
| **C: Save confirmation** | Add modal dialog before save (matching `cfg-warn-modal` style). No immediate save without confirmation. | High |
| **C: Config editor unification** | Replace Config tab raw editor with redirect to `/setup`. Keep "Advanced / Raw Config" section inside `/setup`. | Medium |
| **C: Button placement** | Bottom-only save button + conditional back button. Match main dashboard UX patterns. | High |

---

## 6. Implementation Order

1. **Add `ap_setup` to `/api/status`** — one-line server change, enables all downstream detection
2. **Add SoftAP detection to `/setup` JS** — IP check + API status
3. **Remove `#btn-save-top`** — single HTML edit
4. **Add "← Back to Dashboard" button** — conditional on mode
5. **Add save confirmation modal** — matching existing modal style
6. **Add reconnection assistant modal** — the main UX improvement for SoftAP save flow
7. **Unify Config tab** — replace raw editor with redirect to `/setup` (can be deferred)

---

## 7. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| mDNS doesn't resolve on some networks | Fallback IP input field; instruct user to check router |
| Custom HOSTNAME not yet applied (just saved) | The `/setup` page reads HOSTNAME from the *parsed config lines* (client-side), which reflects what was just saved — even though the ESP hasn't rebooted yet. This is correct because the reboot will apply the saved config. |
| Phone stays on SoftAP too long | Show clear "Your phone should reconnect to [SSID] automatically" message |
| User navigates away during reconnection | The modal state is lost; on next visit to `/setup` or `/`, the dashboard's existing `reboot-overlay` handles it |
| Removing raw editor upsets power users | The "Advanced / Raw Config" section in `/setup` provides the same capability |
| `/api/status` unavailable on page load | Fall back to showing the page without back button (safe default: assume SoftAP if status fetch fails) |
| Regression in `/setup` page scaling/layout | No CSS changes to the existing layout; all new UI is in modal overlays which are position:fixed and don't affect flow |

---

## 8. What NOT to Do

- **Do not** use `WIFI_AP_STA` mode for verification (already rejected in `68-AP-SETUP-MODE.md` §6)
- **Do not** add a cloud/external service for IP notification (over-engineered)
- **Do not** change the existing page scaling/layout (user explicitly flagged this as a regression risk)
- **Do not** minify or restructure the existing `setup.html` CSS (it works correctly)
- **Do not** add SSE or WebSocket for reconnection status (HTTP polling is sufficient and simpler)
