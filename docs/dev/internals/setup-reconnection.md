# Setup Wizard Reconnection Logic

## Overview

When you save configuration changes on the `/setup` page, the device performs a soft reboot. The setup wizard includes a "Saving & Rebooting" modal that monitors the network for the device to come back online.

## Reconnection Strategy

The reconnection assistant uses different strategies depending on the mode:

### Regular WiFi Mode (Connected Network)
Uses a **two-phase polling approach** to handle various network scenarios, including VPN configurations where mDNS may not be available.

### AP Mode (SoftAP / Initial Setup)
Uses **hostname-based polling only** for the full 2-minute duration. No IP-based fallback is attempted.

### Phase 1: Hostname-Based Polling (Attempts 1–10)

**Duration**: ~30 seconds  
**Polling Target**: `http://cpap.local/api/status`  
**Interval**: 3 seconds between attempts  
**Timeout per attempt**: 10 seconds

The wizard first attempts to reach the device using its configured hostname (default: `cpap.local`). This works in most home networks where mDNS is available.

**Why 10 attempts?**
- Testing shows mDNS typically responds within 2–3 attempts (6–9 seconds)
- 10 attempts (30 seconds) provides a comfortable margin for slower networks or busy routers
- If mDNS is available, the device will be found in this phase

### Phase 2: IP-Based Polling (Attempts 11–40) — Regular WiFi Mode Only

**Duration**: ~90 seconds  
**Polling Target**: `http://<current-ip>/api/status` (extracted from browser URL)  
**Interval**: 3 seconds between attempts  
**Timeout per attempt**: 10 seconds  
**Availability**: Regular WiFi mode only (not in AP/SoftAP mode)

If hostname-based polling fails after 10 attempts, the wizard automatically switches to IP-based polling using the IP address of the current browser session. This phase is **only active in regular WiFi mode** and provides a fallback for VPN scenarios where mDNS is unavailable.

**Why automatic IP fallback?**
- **VPN Scenarios**: Users accessing the device via VPN may not have mDNS available, but direct IP access still works
- **No Manual Input**: The IP is automatically extracted from the browser's current URL (e.g., `192.168.1.42`), eliminating the need for manual entry
- **Seamless Transition**: The switch happens automatically without user intervention
- **Transparent Logging**: The debug log shows when the switch occurs and which IP is being used

### Total Timeout

- **Total attempts**: 40 (across both phases)
- **Total duration**: ~120 seconds (2 minutes)
- **Fallback appears**: After 10 attempts (30 seconds), automatic IP-based polling begins

## Debug Log

The "Saving & Rebooting" modal includes a live debug log showing:
- When polling starts
- Each polling attempt (hostname or IP)
- Timeouts and warnings
- When the phase switches to IP-based polling
- Final success or failure

Example log output:
```
[20:31:04] Assistant started inside Regular mode
[20:31:04] Current origin: http://192.168.1.42
[20:31:04] IP fallback available: 192.168.1.42
[20:31:12] Wait period over, starting network polls...
[20:31:15] Polling: http://cpap.local/api/status
[20:31:18] Warning: Timeout
[20:31:21] Polling: http://cpap.local/api/status
...
[20:31:45] Switching to IP-based polling: 192.168.1.42
[20:31:48] Polling: http://192.168.1.42/api/status
[20:31:50] Success! Received heartbeat from http://192.168.1.42/api/status
[20:31:51] Redirecting to http://192.168.1.42/
```

## Configuration

The reconnection parameters are defined in `src/web/setup.html`:

```javascript
var RC_MAX_ATTEMPTS = 40;    // Total polling attempts
var RC_POLL_INTERVAL = 3000; // 3 seconds between attempts
var RC_FALLBACK_AFTER = 10;  // Switch to IP after 10 attempts
```

## Failure Handling

If the device cannot be reached after 2 minutes (40 attempts), the modal displays:
- **Regular WiFi Mode**: An error message indicating both the hostname and IP were tried
- **AP Mode**: An error message indicating only the hostname was tried
- Instructions to eject and reinsert the SD card
- Option to re-enter setup mode

## AP Mode Behavior

In Access Point (SoftAP) mode during initial setup, the reconnection logic is simplified:
1. Attempts to reach `cpap.local` for the full 2 minutes (40 attempts)
2. **Does NOT use IP-based fallback** — only hostname-based polling
3. If the device cannot be reached after 2 minutes, an error message is displayed

This preserves the original behavior in AP mode and avoids unnecessary complexity during initial device setup.
