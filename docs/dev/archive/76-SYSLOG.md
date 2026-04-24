# 76 — Remote Syslog Support (UDP)

> Feasibility analysis for [Issue #44](https://github.com/amanuense/CPAP_data_uploader/issues/44)
> Date: 2026-04-24

---

## 1. Goal

Allow advanced users to stream device logs to a remote syslog server over UDP
for persistent collection, webhook triggers, and off-device analysis — without
touching the SD card or consuming meaningful heap.

---

## 2. Current Logging Architecture

All log calls flow through a single choke-point: `Logger::log(const char*)` in
`Logger.cpp:117`. Every macro (`LOG`, `LOGF`, `LOG_INFO`, `LOG_WARN`,
`LOG_ERROR`, and their `*F` variants) ultimately calls this method.

The current flow inside `Logger::log()`:

```
1. getTimestamp()           → prepend [HH:MM:SS]
2. (optional) append [res fh= ma= fd=] suffix if g_debugMode
3. writeToSerial(finalMsg)  → Serial.write()
4. writeToBuffer(finalMsg)  → 8 KB static circular buffer (BSS, not heap)
```

The buffer is periodically flushed to LittleFS (`syslog.0..3.txt`, 4 × 32 KB
rotation) by `dumpSavedLogsPeriodic()` called from the main loop every ~10s.

**Key observations:**

- `log()` already produces the final, formatted, timestamped string before
  dispatching it. Adding a third output after step 4 is trivial.
- The `logf()` method formats into a 256-byte stack buffer, then calls `log()`.
  This 256-byte limit is the natural ceiling for any single syslog message.
- `LOG_DEBUG` / `LOG_DEBUGF` are **compiled out** in production builds (unless
  `ENABLE_VERBOSE_LOGGING` is set in `platformio.ini`). They cannot be sent via
  syslog because they don't exist in the binary.
- The runtime `g_debugMode` flag (set from `DEBUG=true` in config.txt) only
  controls the `[res fh= ma= fd=]` suffix and some verbose file-uploader
  output. It does **not** unlock `LOG_DEBUG` calls.

---

## 3. Feasibility Analysis

### 3.1 Protocol Choice — UDP (RFC 3164)

| Factor | Assessment |
|---|---|
| **Complexity** | Minimal. ESP32 Arduino core ships `WiFiUDP`. No new dependency. |
| **Connection state** | None. UDP is fire-and-forget — no handshake, no keepalive. |
| **Reliability** | Best-effort. Acceptable: syslog over UDP on a LAN is standard practice. Packet loss on a healthy home network is negligible. |
| **TLS** | Not needed for LAN syslog. Avoids mbedTLS overhead entirely. |
| **Alternative (TCP/TLS)** | Rejected. TCP syslog (RFC 5425) adds connection management, reconnection logic, TLS memory (~40 KB), and blocking I/O complexity that is disproportionate for a logging feature. |
| **Alternative (MQTT)** | Rejected. Requires a client library, TCP connection, keepalives, and topic management. Over-engineered for pure log forwarding. |

**Verdict: UDP syslog (RFC 3164) is the right choice.** It is the simplest
possible network output — one `sendto()` per log line.

### 3.2 Memory & Heap Impact

| Resource | Cost | Notes |
|---|---|---|
| **WiFiUDP object** | ~16 bytes (stack/BSS) | Stateless for send-only use. No `begin()` needed for outbound-only UDP on ESP32 — just `beginPacket()` / `write()` / `endPacket()`. |
| **Format buffer** | 0 bytes additional | Reuse the already-formatted `finalMsg` from `log()`. The syslog PRI header (`<134>`) is only 5 bytes — prepend it in a small stack buffer or directly in the `beginPacket` call. |
| **DNS resolution** | 0 bytes | Config requires an IP address, not a hostname. No DNS overhead. |
| **lwIP TX buffers** | ~1.5 KB per in-flight packet | These are short-lived pbufs from the lwIP pool (already allocated at boot for WiFi). A single log line at ~100–200 bytes is well within one pbuf. Fire-and-forget: freed after TX. |
| **Heap fragmentation** | **Zero** | No heap allocation at any point. Everything is stack or static. |
| **Flash (code size)** | ~500–800 bytes | `WiFiUDP` is already linked into the binary (it's part of the WiFi core). The new code is just a `sendto` wrapper. |

**Total incremental RAM: ~16 bytes static + ~300 bytes stack (transient).**
**Total incremental heap: 0 bytes.**

### 3.3 Partition Size Impact

**None.** The code addition is trivially small (~500–800 bytes of flash).
Current `app0` partition is 1.75 MB (`0x1C0000`). No partition change needed.

### 3.4 Performance Impact

- `endPacket()` on ESP32 for a ~200 byte UDP datagram takes **< 100 µs** on a
  healthy WiFi connection. This is negligible next to the `Serial.write()` call
  that already happens in `writeToSerial()`.
- If WiFi is down or the lwIP TX queue is full, `endPacket()` returns 0
  (failure) immediately — no blocking, no retry. The log line is simply not sent
  over the network. Serial + buffer output are unaffected.
- During upload sessions (Core 0), log calls come from the upload task. The UDP
  `sendto()` uses the lwIP stack which is also on Core 0 — no cross-core
  contention. The main loop (Core 1) already handles its own log calls
  independently via the mutex.

### 3.5 Boot Timing

Config is loaded **before** WiFi connects (line ~695 in `main.cpp`). WiFi
connects at line ~746. This means:

- Syslog config keys (`SYSLOG_HOST`, `SYSLOG_PORT`) are available immediately
  after config load.
- The UDP sender cannot transmit until WiFi is connected.
- **Pre-WiFi boot logs will NOT be sent via syslog.** This is fine — they are
  captured in the circular buffer and flushed to LittleFS. The syslog stream
  begins once WiFi is up, which covers the operational lifecycle that users
  actually care about.
- No special "queue and replay" mechanism is needed. Keep it simple.

---

## 4. Positive Impacts

1. **Crash/brownout survival**: Logs reach the syslog server in real-time. Even
   if the device browns out mid-sentence, every log line up to that point is
   already on the server. The 8 KB circular buffer and LittleFS rotation are
   complementary but have a delay window; UDP has none.
2. **Webhook enablement**: Users can configure their syslog server (rsyslog,
   syslog-ng, Graylog, Papertrail) to trigger webhooks on specific patterns
   (e.g., `[FSM] Upload session complete`, `[ERROR]` lines). This is the
   feature users are explicitly asking for.
3. **Multi-device fleet monitoring**: Users with multiple CPAP units can
   aggregate all device logs into one dashboard without polling each device's
   `/api/logs` endpoint.
4. **Zero operational overhead**: Unlike `/api/logs` polling (which requires an
   always-on client and is lossy due to the 8 KB buffer), syslog is push-based
   and the server handles persistence.

## 5. Negative Impacts / Risks

1. **UDP is unreliable**: On a congested WiFi network, some log lines may be
   lost. This is inherent to UDP syslog and is universally accepted. Mitigation:
   the local circular buffer + LittleFS rotation remain the source of truth.
2. **WiFi-down gaps**: No syslog output while WiFi is disconnected. Same
   mitigation as above.
3. **Sensitive data exposure**: Log lines may contain WiFi SSIDs, file paths, or
   IP addresses. These are sent in plaintext on the LAN. Acceptable for a local
   network; users who enable this feature are advanced and understand the
   trade-off. Passwords/secrets are never logged.
4. **Minor latency on log calls**: Each `log()` call gains ~100 µs for the UDP
   send. With ~50–100 log lines per upload cycle, total added latency is
   ~5–10 ms per session. Imperceptible.

---

## 6. Recommendation: What to Send

### Option A: Mirror standard output (RECOMMENDED)

Send exactly what `log()` currently sends to Serial and the circular buffer.
This means all `LOG`, `LOGF`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` (and their
`*F` variants) are forwarded. `LOG_DEBUG` is excluded because it is compiled
out in production.

**Why this is the right choice:**

- **Zero code complexity**: No filtering logic. Every call to `log()` simply
  gains a third output destination. One `if` check (is syslog configured?) and
  one `sendto()`.
- **Matches user expectations**: The log stream they see in Serial Monitor and
  on the web dashboard is exactly what arrives at their syslog server.
- **Debug-gated logs are already excluded**: Lines like the Stealth mode logs
  (gated behind `g_debugMode`) and the `MINIMIZE_REBOOTS` log (also gated) will
  only appear in syslog when `DEBUG=true` is set — exactly the same filtering
  as all other outputs.
- **No new log level abstraction needed**: We don't need to invent a
  `SYSLOG_LEVEL` config key with `INFO`/`WARN`/`ERROR` filtering. The existing
  `LOG_*` macros already provide the right level of detail for operational
  monitoring.

### Option B: All-inclusive with DEBUG (NOT recommended)

Would require enabling `ENABLE_VERBOSE_LOGGING` at compile time, which bloats
the binary with hundreds of debug `logf()` calls that are normally dead code.
This defeats the purpose of compile-time stripping and adds flash overhead for
all users, not just syslog users.

### Severity Mapping

Map the existing `[INFO]`, `[WARN]`, `[ERROR]` prefixes to RFC 3164 severity
levels for proper syslog integration:

| Firmware prefix | Syslog severity | Value |
|---|---|---|
| `[INFO]` | Informational | 6 |
| `[WARN]` | Warning | 4 |
| `[ERROR]` | Error | 3 |

Use facility `local0` (16) by default. This gives PRI values:

- INFO: `<134>` (16×8 + 6)
- WARN: `<132>` (16×8 + 4)
- ERROR: `<131>` (16×8 + 3)

---

## 7. Config Format

```ini
# Remote syslog (optional, advanced)
SYSLOG_HOST=192.168.1.100
SYSLOG_PORT=514
```

### Design decisions:

| Key | Default | Notes |
|---|---|---|
| `SYSLOG_HOST` | *(empty)* | Must be an IPv4 address. Feature is disabled when empty. No hostname resolution — keeps it simple and avoids DNS dependency. |
| `SYSLOG_PORT` | `514` | Standard syslog port. Optional — only needs to be set if using a non-standard port. |

### What NOT to add:

- **`SYSLOG_FACILITY`**: Hardcode `local0` (16). No user has ever needed to
  change this. If they do, it's a one-line code change.
- **`SYSLOG_LEVEL`**: Not needed. We send everything that `log()` processes.
  The syslog server can filter by severity on its end.
- **`SYSLOG_PROTOCOL`**: Only UDP. No TCP/TLS option. Keep scope minimal.
- **`SYSLOG_HOSTNAME`**: Use the device's configured hostname from
  `config.getHostname()`. No separate key needed.

**Total new config keys: 2** (`SYSLOG_HOST`, `SYSLOG_PORT`). Minimal user
complexity.

---

## 8. Implementation Approach

### 8.1 Overview

The implementation touches exactly **3 files**:

1. **`Config.h` / `Config.cpp`** — Parse `SYSLOG_HOST` and `SYSLOG_PORT`.
2. **`Logger.h` / `Logger.cpp`** — Add a `writeToSyslog()` method and a static
   `WiFiUDP` instance. Call it from `log()`.
3. **`main.cpp`** — Pass the parsed config to Logger after WiFi is connected.

No new classes. No new files. No new dependencies.

### 8.2 Logger Changes

```
Logger.h:
  + static member: WiFiUDP  s_syslogUdp;  // BSS, not heap
  + member:        IPAddress syslogHost;
  + member:        uint16_t  syslogPort;
  + member:        bool      syslogEnabled;
  + method:        void enableSyslog(IPAddress host, uint16_t port);
  + method:        void writeToSyslog(const char* data, size_t len);
```

```
Logger::log() — after writeToBuffer():
  + if (syslogEnabled) writeToSyslog(finalMsg, len);
```

```
Logger::writeToSyslog(const char* data, size_t len):
  // Determine severity from prefix
  uint8_t severity = 6;  // default: INFO
  if (data[0]=='[' && len > 6) {
    if (memcmp(data, "[WARN]",  6)==0) severity = 4;
    if (memcmp(data, "[ERROR]", 7)==0) severity = 3;
  }
  // Skip the timestamp prefix that log() already added — syslog has its own
  // Actually: keep it. The receiver expects a MSG field and our timestamp
  // is useful context. RFC 3164 is lenient about MSG format.

  // Build PRI + hostname TAG prefix on stack
  char hdr[48];
  int hdrLen = snprintf(hdr, sizeof(hdr), "<%d>%s cpap: ",
                        (16 * 8) + severity,
                        hostname);  // "cpap" or config hostname

  // Fire-and-forget UDP send
  s_syslogUdp.beginPacket(syslogHost, syslogPort);
  s_syslogUdp.write((const uint8_t*)hdr, hdrLen);
  s_syslogUdp.write((const uint8_t*)data, len);
  s_syslogUdp.endPacket();
  // No error handling. If it fails, it fails silently. The local buffer
  // and LittleFS are the durable stores.
```

### 8.3 Config Changes

```cpp
// Config.h
String syslogHost;       // empty = disabled
uint16_t syslogPort = 514;

// Config.cpp — in parseConfigLine():
} else if (key == "SYSLOG_HOST") {
    syslogHost = value;
} else if (key == "SYSLOG_PORT") {
    syslogPort = value.toInt();
}
```

### 8.4 main.cpp Changes

After WiFi is connected (~line 780), enable syslog on the Logger:

```cpp
if (!config.getSyslogHost().isEmpty()) {
    IPAddress syslogIp;
    if (syslogIp.fromString(config.getSyslogHost())) {
        Logger::getInstance().enableSyslog(syslogIp, config.getSyslogPort());
        LOGF("[Syslog] UDP syslog enabled → %s:%d",
             config.getSyslogHost().c_str(), config.getSyslogPort());
    } else {
        LOG_WARNF("[Syslog] Invalid SYSLOG_HOST: %s (must be an IP address)",
                  config.getSyslogHost().c_str());
    }
}
```

### 8.5 Thread Safety

`Logger::log()` already serialises all output behind a FreeRTOS mutex.
The `writeToSyslog()` call happens inside the same critical section as
`writeToBuffer()` — or more precisely, after the buffer write but still within
the same `log()` call. Since `log()` is always called from one core at a time
(mutex-protected), and `WiFiUDP::endPacket()` is internally safe for lwIP, no
additional locking is needed.

**However**, to avoid holding the mutex during the (tiny but non-zero) network
I/O, the cleaner approach is:

1. Copy the final message into a small stack buffer (~256 bytes, matching
   `logf()`'s existing limit).
2. Release the mutex.
3. Call `writeToSyslog()` outside the critical section.

This prevents any theoretical stall where a slow `endPacket()` blocks a
concurrent `log()` call on the other core.

### 8.6 WiFi-Down Behavior

- `WiFiUDP::endPacket()` returns 0 when WiFi is disconnected.
- No retry. No queue. The log line is simply not sent remotely.
- `syslogEnabled` can be left `true` even when WiFi drops — the cost of a
  failed `endPacket()` is negligible (<10 µs). No need for a WiFi-state check.

---

## 9. What This Does NOT Include (Out of Scope)

| Feature | Reason |
|---|---|
| TCP syslog (RFC 5425) | Disproportionate complexity for a logging feature |
| TLS encryption | 40+ KB memory overhead; syslog over LAN is standard practice |
| Message queueing / replay | Adds heap allocation, ring buffer management, and complexity. The local buffer + LittleFS already serve this role |
| Hostname resolution | Adds DNS dependency and potential blocking. IP-only keeps it deterministic |
| Configurable facility | Hardcode `local0`. No user has asked for this |
| Configurable log level filter | Not needed. Send everything; filter on the server |
| Boot log replay | Pre-WiFi logs are already captured in LittleFS. Not worth the complexity |

---

## 10. Summary

| Dimension | Assessment |
|---|---|
| **Feasibility** | ✅ Trivial. ~100 lines of new code across 3 existing files. |
| **Heap impact** | ✅ Zero. All allocations are stack or static BSS. |
| **Flash impact** | ✅ Negligible (~500-800 bytes). No partition change. |
| **Fragmentation risk** | ✅ None. No dynamic memory involved. |
| **Performance impact** | ✅ ~100 µs per log call. ~5-10 ms total per upload session. |
| **Complexity** | ✅ Minimal. No new classes, files, or dependencies. |
| **Config burden** | ✅ 2 keys: `SYSLOG_HOST` + optional `SYSLOG_PORT`. |
| **What to send** | All standard log output (same as Serial + web UI). No compile-time debug. |
| **Risk** | ⚠️ Low. UDP loss on LAN is rare. Local storage is unaffected. |

**Recommendation: Proceed.** This is a high-value, low-risk, low-complexity
feature that directly addresses user demand for webhooks and persistent remote
logging.
