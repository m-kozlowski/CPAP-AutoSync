# 75 — SMB Improvements: Speed, Memory, Fragmentation

**Status:** **Phases A, B, C implemented (items 1–4 complete).** Phase D and items 5–7 remain deferred / optional.
**Trigger:** [Issue #85](https://github.com/ilyakruchinin/CPAP-AutoSync/issues/85) — "SMB upload throughput significantly slower than stable — per-folder TCP reconnect."
**Context:** v4.1-alpha8 is the reporter's firmware; the repo head has since moved on (e.g. `SMB_PRESERVE_TIMESTAMPS` is now opt-in, defaulting to `false`). The core observations about the per-folder reconnect pattern, however, still stood on current `main` and are now addressed on the `4.1-improve-progress-bar` branch.

---

## 1. Executive Summary

The reporter of issue #85 correctly identified **one real design flaw**: the SMB uploader tears the share connection (and the entire `smb2_context`) down at the end of every DATALOG folder and rebuilds it for the next folder. Some of the surrounding claims, however, are either imprecise or conflated with v4.1-alpha8 regressions that have already been fixed.

The biggest wins available, in descending order of **impact vs. implementation cost**:

| # | Change | Expected speed impact | Implementation cost | Memory impact | Status |
|---|---|---|---|---|---|
| 1 | **Persistent SMB session across the entire SMB phase** (remove per-folder `end()`) | 2×–10× on small folders, modest on large folders | Trivial (delete two lines, add session-scoped `begin()`) | Neutral / slightly positive (no re-alloc/free cycling of `smb2_context`) | **Completed (Phase A)** |
| 2 | **Use `smb2_get_max_write_size()` and raise per-write chunk to 32–64 KB when heap allows** | 2×–4× throughput | Low (buffer sizing already adaptive) | Higher peak usage, but single allocation, so zero fragmentation impact | **Completed (Phase B)** |
| 3 | **Pipeline N outstanding writes via SMB2 credits** | 3×–8× on a healthy LAN | Medium (refactor `smb2_write_ev` into multi-PDU pattern) | +N × write-buffer size; must stay in a pre-allocated arena | **Completed (Phase C, depth 2)** |
| 4 | **`TCP_NODELAY` on the libsmb2 socket** | ~10–30% on small PDUs | Trivial (setsockopt after connect) | None | **Completed (Phase A)** |
| 5 | **libsmb2 PDU pool allocator** (TlsArena-style hook for `smb2_pdu` / iovec buffers) | Minimal direct speed gain; removes fragmentation tail-risk | Medium (custom allocator, ~200 LOC) | Reserved static memory, but contiguous and recyclable | Deferred (Phase D, optional) |
| 6 | SMB2 compound open+write+close for small files | ~30% on sub-4 KB files | High — significant libsmb2 surface area | None | Deferred |
| 7 | Dual-core / parallel-folder upload | Theoretical 2× — **not recommended** | Very high; large attack surface on heap, lwIP, SD | Large | Rejected |

**Delivered:** items 1, 2, 3, 4 shipped in v4.1-beta1 on the `4.1-improve-progress-bar` branch. Items 5–7 remain deferred per the original plan.

---

## 2. Review of Issue #85

### 2.1 Claims that are correct

- **Per-folder reconnect is real.** `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp:1303-1304` explicitly calls `smbUploader->end()` at the bottom of `uploadDatalogFolderSmb()`. `end()` → `disconnect()` → `smb2_disconnect_share_async` + `smb2_destroy_context` (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:435-447`). The next folder calls `begin()` again, which runs a fresh TCP connect → NEGPROT → SESSION_SETUP → TREE_CONNECT. This is a real, measurable overhead (~1–2 s per folder on a typical home LAN).
- **The overhead accumulates with backlog size.** For 43 folders, at 1–2 s/reconnect, that's 45–90 s of pure reconnect cost. Not 3 hours, but real.

### 2.2 Claims that are imprecise or incorrect

- **"~3+ hours for 43 folders vs. minutes on stable."** This delta is far too large to be explained by reconnects alone (see math above). Three much larger factors in v4.1-alpha8 that the repo head has already addressed or partially addressed:
  1. `SMB_PRESERVE_TIMESTAMPS` was effectively always-on in that build. Every file paid an extra SMB2 `SET_INFO` round-trip (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:908-932`). On a bad link that's 5–50 ms per file; on a few hundred files, seconds to minutes. **Already fixed on `main`:** `SMB_PRESERVE_TIMESTAMPS=false` by default as of last week (see `@/opt/projects/personal/CPAP_data_uploader/release/RELEASE_NOTES_v4.1-beta1.md:54`).
  2. The pre-alpha8 "low-memory shortcut" in `createDirectory` that falsely skipped mkdir — fixed, but the *replacement* path (real stat+mkdir) *does* add 1–2 SMB ops per file if the parent folder isn't cached. `lastVerifiedParentDir` caching (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:668-686`) mitigates this but is reset on every reconnect. Item 1 above (persistent session) fixes the cache churn too.
  3. The small 8 KB write buffer combined with synchronous, serial writes (see §4) means the LAN pipe is ~empty most of the time.
- **"Root files also each get their own connection attempt."** The code doesn't do this. `uploadSingleFileSmb()` checks `isConnected()` and only calls `begin()` if not connected (`@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp:1351`). The only ways to see repeated "Connecting to ..." at that stage are (a) a transport error path calling `disconnect()` (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:737`, `:971`) between each root file, or (b) the redacted log conflated connect attempts with TCP keepalive probes / retries. The far more likely explanation on v4.1-alpha8 is (a): the `SET_INFO` timestamp call frequently returned transport-style errors on some NASes, tripping the recoverable-error path and forcing a disconnect after each file. That, again, is a v4.1-alpha8 artefact already addressed.
- **"Within each folder, files stream correctly over a single connection."** True, but "stream correctly" is overstating it — the writes are strictly serial 8 KB PDUs, with `SMB_WRITE_TCP_DRAIN_BYTES=16384` → `delay(10)` drains every 16 KB (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:883-887`). On a healthy gigabit LAN this caps effective throughput at ~1.5–2 MB/s regardless of NAS speed.

### 2.3 The real, underlying problem

Even after the v4.1-alpha8 regressions are accounted for, **SMB is still structurally slower than the cloud HTTPS backend**, which is counter-intuitive given:

- SMB is on a local LAN (sub-millisecond RTT) vs. cloud RTT typically 20–100 ms.
- SMB has no TLS handshake/record overhead per file.

The reason is that `SMBUploader::upload()` (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:773-898`) does strict synchronous stop-and-wait writes: read 8 KB from SD, `smb2_write_ev` (full send→poll→receive→callback round-trip), then next 8 KB. The HTTPS backend streams chunked HTTP bodies with the TCP send buffer doing the pipelining for free; SMB doesn't get that because *each* SMB write is its own request/response at the application layer.

Items 2 and 3 (bigger chunk, pipelined writes) directly target this.

---

## 3. Current Architecture Snapshot

### 3.1 Lifecycle (as of current `main`)

```
FileUploader::loop() [Phase 2: SMB]
  └─ smbUploader->allocateBuffer(8KB/4KB/2KB/1KB based on ESP.getMaxAllocHeap())
  └─ uploadMandatoryFilesSmb()          ← first begin() inside upload loop
  │    └─ uploadSingleFileSmb() × N    (keeps connection open between root files)
  └─ (does NOT end() between mandatory and DATALOG)
  └─ for each fresh folder:
  │    └─ uploadDatalogFolderSmb()
  │         ├─ (begin() if not connected — usually already is)
  │         ├─ for each file in folder: smbUploader->upload(...)
  │         └─ ***smbUploader->end()***  ← FULL teardown every folder
  └─ for each old folder: same thing
  └─ smbUploader->end() (if still connected)
  └─ smbUploader->freeBuffer()
```

The end-of-folder `end()` originates from a comment explicitly saying "Per-folder disconnect (not per-file — avoids socket exhaustion)". Under *v1* of the uploader that made sense because the library was leaking something. With the current code path (and current libsmb2), a single persistent session for the whole SMB phase is safe and correct.

### 3.2 Per-write critical path (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:792-816`)

For each 8 KB chunk:

1. `smb2_write_async` → allocates a fresh `smb2_pdu` (calloc, ~1 KB), builds iovectors, queues on `outqueue`.
2. `smb2_run_event_loop` → `poll(fd, 1s)` → `smb2_service` → `writev()` to TCP stack.
3. Waits for server reply: `poll` → `smb2_service` → reassembles, decodes, dispatches callback.
4. `cb.is_finished = 1` → loop exits → `smb2_free_pdu` (free).

Per-file extras (`smb2_open`, `smb2_close`, optional `smb2_set_basic_info`) each add another full allocate→queue→send→recv→dispatch→free cycle.

### 3.3 Buffer ownership and heap footprint

- **Upload buffer** (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:461-480`): single `malloc(size)` at the start of the SMB phase, `free()` at the end. Size chosen by `ESP.getMaxAllocHeap()` — 8 KB / 4 KB / 2 KB / 1 KB. Only one allocation, so not a fragmentation source.
- **`smb2_context`** (`@/opt/projects/personal/CPAP_data_uploader/components/libsmb2/lib/init.c:278`): a single `calloc(1, sizeof(struct smb2_context))`. Rough size (from `@/opt/projects/personal/CPAP_data_uploader/components/libsmb2/include/libsmb2-private.h:56-282`): `SMB2_MAX_VECTORS=256` × ~12 B iovec slots → ~3 KB just for the incoming io-vectors, plus error_string (256), header buffer, session keys, etc. Call it **~4–5 KB** on ESP32. Allocated on `connect()`, freed on `disconnect()`. Each folder → one full alloc+free cycle → one of the main fragmentation sources.
- **Per-PDU**: `smb2_pdu` struct (header, 64-byte `hdr[]`, ~16 bytes other fields, plus `smb2_io_vectors` for out). Roughly **400–800 B per PDU**, allocated on every SMB op and freed on completion. Plus `calloc(SMB2_WRITE_REQUEST_SIZE & ~1, 1)` ≈ 48 B for the write header (`@/opt/projects/personal/CPAP_data_uploader/components/libsmb2/lib/smb2-cmd-write.c:68`).
- **mbedTLS arena** (`@/opt/projects/personal/CPAP_data_uploader/src/TlsArena.cpp`): 2 × 17 KB in `.bss`. Already outside the heap.

The main heap-fragmentation risk on SMB comes from **churning many small calloc/free pairs** (one `smb2_context` per folder, dozens of PDUs per folder). Not "big block impossible to allocate" — we have the TLS arena for that — but **free-list fragmentation of the small-block pool**, which is exactly what `ESP.getMaxAllocHeap()` drops show. Every "low memory" fallback in `SMBUploader.cpp` is really a symptom, not a root cause.

---

## 4. Why SMB Is Slower Than the Cloud HTTPS Backend

A quantitative sketch of where time goes on a typical 1 MB CPAP `.edf` file, 100 Mbit LAN, ~1 ms RTT, SMB2 signing on:

| Cost | SMB (current, 8 KB chunks) | HTTPS (existing cloud uploader) |
|---|---|---|
| Connection setup (amortized over a file) | 1500 ms ÷ 7 files/folder = ~215 ms/file | 0 ms (connection stays open for the whole cloud phase) |
| Per-chunk app-layer round-trip | 128 chunks × (1 ms RTT + overhead) ≈ 130 ms | 1 TLS record per 16 KB ≈ 65 writes → no per-write wait (kernel send buffer absorbs) |
| `TCP_NODELAY` status | Not set → Nagle coalescing on tiny PDU fragments can add ~40 ms per file | Already effectively disabled by HTTP's larger chunked-body writes |
| Drain delays | `delay(10)` every 16 KB → 64 × 10 = 640 ms wasted per file | None |
| Signing CPU | ~2 ms/file | n/a |
| TLS CPU | 0 | ~50 ms/file |
| **Effective throughput** | **~1–2 MB/s** | **2–4 MB/s** |

`SMB_WRITE_TCP_DRAIN_BYTES` (the 10 ms per-16 KB delay) is a band-aid from a heap-pressure era; with a persistent session and a right-sized buffer, the lwIP send queue shouldn't fill in the first place.

The structural delta: **HTTPS lets TCP be the flow-controller**; SMB is artificially serialising at the app layer. Items 2 and 3 are about letting SMB do the same.

---

## 5. Recommendations

### 5.1 Item 1 — Persistent SMB session for the whole SMB phase *(do first)*

**What:** Call `smbUploader->begin()` once at the start of the Phase 2 SMB block in `FileUploader::loop()` (after `allocateBuffer` succeeds), and `smbUploader->end()` exactly once, at the very end of the Phase 2 block. Delete the `if (smbUploader->isConnected()) smbUploader->end();` at `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp:1304` and the inner `begin()` at `:1269-1274` (replace with a `return false` if not connected — which now would indicate a bug).

**Why it's safe:**

- libsmb2 has no per-folder state limit. The `smb2_context` is file-agnostic; files are identified by their own `smb2fh*` which is opened/closed per file.
- The cache `lastVerifiedParentDir` now survives folder boundaries, eliminating redundant mkdir/stat calls on the `DATALOG/` root between folders.
- On transport errors we already force `disconnect()` in two places (`@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:737,971`) and reconnect on the next file — that self-healing path is unchanged.

**Why the "avoid socket exhaustion" comment is obsolete:**

- We only ever have one context at a time.
- libsmb2 doesn't leak file descriptors across folders.
- The original symptom (heap pressure) is addressed by Item 5, not by tearing down the session.

**Expected gains:**

- Saves one `smb2_init_context` + NEGPROT + SESSION_SETUP + TREE_CONNECT (4 round-trips, ~5 PDUs allocated and freed, ~1.5 s) per folder. On a 43-folder backlog that's roughly **60 seconds and ~200 PDU alloc/free pairs** eliminated.
- Heap behaviour: monotonically better. The `smb2_context` occupies ~4–5 KB *continuously* instead of cycling in/out, which *reduces* fragmentation risk by removing the repeated alloc/free pattern.

**Risk:** If a NAS force-closes idle connections, we now notice that on the *next* operation instead of getting a fresh one per folder. Already handled by the recoverable-error/reconnect path. Optional mitigation: set `TCP keepalive` on the socket.

### 5.2 Item 2 — Right-size the write chunk

**What:**

- After `connect()`, read `smb2_get_max_write_size(smb2)` (typically 1 MB on Samba/Windows, 64 KB on old servers).
- Pick `uploadBufferSize = min(serverMax, policyMax, heapMax)` where:
  - `serverMax` = server-advertised `max_write_size`.
  - `policyMax` = 32 KB (reasonable upper bound: a single write then fits in one 64 KB TCP window).
  - `heapMax` = derived from `ESP.getMaxAllocHeap()` — current curve is already fine, just extend the top band: `>60 KB` free → 32 KB, `>40 KB` → 16 KB, etc.
- Drop `SMB_WRITE_TCP_DRAIN_BYTES` / `delay(10)` once buffer ≥ 16 KB — with a persistent session and bigger chunks, lwIP's own flow control handles drain.

**Expected gains:** 2×–4× throughput. A 32 KB write is *one* application round-trip instead of four 8 KB ones.

**Memory impact:** +24 KB peak during SMB phase. Compensated by Item 1 (no `smb2_context` churn) and by only allocating when the heap headroom supports it.

**Risk:** Older NAS devices may not support multi-credit writes (`supports_multi_credit == 0`); libsmb2 already caps those at 64 KB (`@/opt/projects/personal/CPAP_data_uploader/components/libsmb2/lib/smb2-cmd-write.c:79`), so we can't break anything by asking for more.

### 5.3 Item 3 — Pipeline multiple outstanding writes *(the real speed win)*

**What:**

- Refactor `SMBUploader::upload()` to keep **N write PDUs in flight** concurrently.
- Concretely, replace the `while (localFile.available())` serial loop with:
  1. Maintain a queue of up to `PIPELINE_DEPTH` (start with 4) outstanding `smb2_async_cb_data` slots.
  2. Fill them by calling `smb2_write_async` repeatedly (each with its own `cb_data`) until the window is full or EOF. libsmb2 queues them on `outqueue` and sends them as credits/socket allow.
  3. Run the event loop until *any* slot finishes, free its `cb_data`, advance the file offset bookkeeping, enqueue the next write.
  4. On any error, abort the window (the in-flight writes need to fail cleanly).

- Each credit serves one 64 KB write. libsmb2 supports up to `MAX_CREDITS=1024` (`@/opt/projects/personal/CPAP_data_uploader/components/libsmb2/include/libsmb2-private.h:132`); we'd use ≤ 8.

**Why it's the biggest speed win:** SMB2 was designed for this. With 4 × 32 KB outstanding, a 1 MB file transfers in ~8 application round-trips instead of 128, and the NAS's disk and the network can overlap.

**Why it's medium-cost:**

- Needs a small state machine (ordered write-offset tracker; libsmb2 replies can arrive out of order).
- Error handling gets trickier: if write #2 fails, we have to decide what to do about #3 and #4 that are already in flight.
- The shared event-loop pattern (`smb2_run_event_loop`) already handles multiple PDUs in flight, it just currently waits for a specific `cb->is_finished`. We'd poll all slots instead. Maybe 100–200 LOC, not more.

**Memory impact:** `PIPELINE_DEPTH × (writeBuffer + PDU overhead)`. For depth=4 × 32 KB, that's 128 KB of buffer. **This is the critical constraint.** It's only feasible if we allocate the pipeline buffer ring *once, statically* (or from an arena — see Item 5), not via per-slot `malloc`.

**Alternative low-memory variant:** depth=2 × 16 KB = 32 KB. Still ~2× the current throughput; fits comfortably in existing headroom.

### 5.4 Item 4 — `TCP_NODELAY` on the libsmb2 socket

**What:** After `smb2_connect_share_async` succeeds, call:

```c
int fd = smb2_get_fd(smb2);
int one = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
```

**Why:** libsmb2 already does one `writev()` per PDU with all its iovectors batched, so Nagle's algorithm can only hurt — it delays small PDUs (like `smb2_close`, `smb2_set_info`) for 40 ms waiting for more data that isn't coming. On small-file workloads the cumulative effect is measurable.

**Cost:** Two lines of code, no memory, no fragility.

### 5.5 Item 5 — libsmb2 pool/arena allocator *(the real memory win)*

**What:** Route libsmb2's internal `calloc/free` through a pool allocator, mirroring the pattern already used for mbedTLS in `TlsArena.cpp`. Two realistic approaches:

**Approach 5A: Hook the two high-frequency sites only.** libsmb2 doesn't expose a `mbedtls_platform_set_calloc_free`-style callback (unlike mbedTLS). We would:
- Add a compile-time `--with-custom-allocators` / header macro in libsmb2 defining `smb2_pdu_alloc`, `smb2_pdu_free`, `smb2_iovec_buf_alloc`, `smb2_iovec_buf_free`.
- Implement them as a small-block slab allocator over a static `uint8_t pool[POOL_SIZE] __attribute__((aligned))` in `.bss`.
- Big buffers (write data, read response bodies) pass through to normal heap — they're a single allocation per PDU and already fine.

**Approach 5B: Leave libsmb2 alone, pre-warm the heap.** A much cheaper option: at boot, immediately after `tlsArenaInit()` but before WiFi, do a single dummy `smb2_init_context()` + `smb2_destroy_context()` + pre-allocate the upload buffer slot in `.bss`. This doesn't reduce allocator churn *during* upload but it reserves the small-block pool early, before fragmentation. **Much lower complexity; modest benefit.**

**Approach 5C (recommended compromise):** A middle ground — define a **single static arena** for `smb2_pdu` slots (say, 16 × 1 KB slab in `.bss` = 16 KB). Add a tiny `smb2_pdu` cache via a small patch to `@/opt/projects/personal/CPAP_data_uploader/components/libsmb2/lib/pdu.c`'s `smb2_allocate_pdu` and `smb2_free_pdu`. That's where >50% of heap-churn pressure actually lives; leaving iovectors/buffers on the general heap is fine because they're variable-sized.

**Expected gains:** negligible raw speed, but **removes the heap-fragmentation tail that already forces `allocateBuffer()` to fall back to 4 KB / 2 KB / 1 KB** under pressure. That fall-back is itself a ~2× throughput loss in mixed SMB+Cloud mode, so the effective speed gain is real once combined with Item 2.

**Priority:** Third — attempt only if post–Item 1+2+3 profiling still shows `ESP.getMaxAllocHeap()` falling below the 30 KB band that governs the adaptive buffer.

### 5.6 Item 6 — Compound open+write+close for small files *(defer)*

SMB2 compound requests let a client chain multiple commands in one PDU (`@/opt/projects/personal/CPAP_data_uploader/components/libsmb2/include/smb2/libsmb2.h:652-658`). For 1 KB files (quite common in SETTINGS/), we could send create+write+close as one PDU → ~1/3 the round-trip cost.

Libsmb2's compound API is usable but sharp-edged, and the gain is only visible for files small enough that the round-trip dominates the transfer. Most CPAP files are 10–500 KB where the gain is lost in the noise. **Defer unless profiling shows meaningful time in small files.**

### 5.7 Item 7 — Parallel uploads on the second core *(do not do)*

Tempting, but:
- Both cores share the same lwIP stack; there's no true parallelism at the network layer.
- SD card is single-threaded in this firmware (only Core 1 holds control).
- The heap is shared; two backends allocating simultaneously is exactly the fragmentation scenario we've been fighting.
- Current design already does **backend phasing** (Cloud first, then SMB) specifically because the heap can't support both TLS and libsmb2 active at the same time.

There's no believable path to making this speed anything up without a redesign of the heap layout *and* the SD-access model. **Not recommended.**

### 5.8 Items explicitly not pursued

- **Multiple SMB sessions to multiple shares.** Pointless; we only have one backend.
- **SMB3 encryption disabled.** Already disabled (we only set signing, not sealing — `@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp:386-388`). SMB2 signing adds ~2 ms/file of HMAC-SHA256; fine.
- **Switching to a different SMB library.** `libsmb2` is the only viable C-only Arduino-compatible client. Ruled out in the original library selection (`@/opt/projects/personal/CPAP_data_uploader/include/SMBUploader.h:17-29`).
- **Streaming reads from SD in parallel with SMB writes.** SD is `FILE_READ` sequential today; adding a pre-read thread on Core 0 is Item 7's cousin and has the same problems.
- **Keep SMB session alive across upload *sessions*.** Would break WiFi power-cycling (`@/opt/projects/personal/CPAP_data_uploader/src/NetworkRecovery.cpp:23` explicitly blocks WiFi cycle while SMB is active), COOLDOWN-phase assumptions, and multi-tenant NAS connection-count quotas. Not worth it.

---

## 6. Interaction with Existing Subsystems

- **`TlsArena`:** Untouched. Remains the large-block arena for mbedTLS.
- **Phased upload (Cloud → SMB):** Unchanged. Cloud still tears down before SMB; SMB still runs with a clean heap.
- **`g_smbConnectionActive` guard on WiFi cycling** (`@/opt/projects/personal/CPAP_data_uploader/src/NetworkRecovery.cpp:23`): becomes more important with a session lasting the whole phase. Consider tightening the WiFi-cycle cooldown so a cycle can't be initiated while a pipeline is mid-flight (or, more simply: always flush the pipeline before disconnect).
- **Progress bar (archive #74):** Unaffected — progress is folder-scoped via `UploadStateManager`, not connection-scoped.
- **`SMB_PRESERVE_TIMESTAMPS`:** Unaffected. The `SET_INFO` call remains per-file; pipelining it into the same window as the writes is a micro-optimisation for later.
- **Reduced-retries mode outside scheduled hours:** Unaffected; the per-file retry counter is separate from the session lifetime.

---

## 7. Suggested Implementation Order

A deliberately conservative phasing that keeps each release testable in isolation.

**Phase A — delivered in v4.1-beta1:**
1. Item 1 — persistent session for the whole SMB phase. `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp` now calls `smbUploader->begin()` once at the top of Phase 2 and `smbUploader->end()` exactly once after both the mandatory-files and DATALOG-folder loops complete; the per-folder `end()` at the bottom of `uploadDatalogFolderSmb()` was removed.
2. Item 4 — `TCP_NODELAY`. Set immediately after `smb2_connect_share_ev` succeeds in `@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp` `connect()`.
3. Removed `SMB_WRITE_TCP_DRAIN_BYTES` / `delay(10)` drain path from the serial write loop.

Validation: a 20-folder backlog session drops by ~30–60 s vs. the previous build; per-folder logs show `Connected successfully` exactly once per SMB phase.

**Phase B — delivered in v4.1-beta1:**
4. Item 2 — query `smb2_get_max_write_size()` after `begin()` via new `SMBUploader::getNegotiatedMaxWriteSize()`; grow the adaptive-buffer top band to 32 KB (clamped by `min(policyMax=32KB, serverMax, heapBand)`).

**Phase C — delivered in v4.1-beta1 at depth 2:**
5. Item 3 — pipelined writes, depth 2 by default when heap permits (`currentMa > 80 KB` and server reports `max_write_size ≥ 64 KB`). Implementation uses `smb2_pwrite_async` with explicit offsets so queued writes address the correct file position without racing on `fh->offset`. A new `smb2_run_multi_slot_event_loop()` helper reaps any completed in-flight slot; the upload buffer is a single contiguous `depth * slotSize` allocation carved into slots. Falls back to serial on allocation failure. Depth 4 remains available (capped by `MAX_PIPELINE_DEPTH = 8`) but is gated off by the heap heuristic until a wider rollout validates it.

**Phase D — deferred:**
6. Item 5C — pool allocator for `smb2_pdu`. Pursue only if profiling on real deployments shows `ESP.getMaxAllocHeap()` regressions after Phases A–C.

**Validation matrix run before landing:** dual-mode smart upload, old-folder backfill, 4 KB low-heap fallback, 43-folder backlog timing — all passing. Build verified with `pio run -e pico32-ota`.

---

## 8. Appendix — What would *break* any of this

- If a deployed NAS advertises `max_write_size < 8192`: extremely rare (Samba default is 1 MB, Windows is 1 MB or 8 MB). Item 2 already clamps to `min(server, policy)`, so we'd just keep 8 KB.
- If libsmb2 is upgraded: Items 1 and 2 are library-version-agnostic. Item 3 relies on `smb2_write_async` / `smb2_queue_pdu` / `smb2_service`, which are stable API. Item 5C patches `@/opt/projects/personal/CPAP_data_uploader/components/libsmb2/lib/pdu.c` and would need re-applying on upgrade; keep it as a single isolated patch file.
- If the NAS enforces an idle-connection timeout (many do, ~5 min): with a persistent session across a 10-minute window we may see occasional transport errors mid-session. Already handled by the existing `recoverWiFiAfterSmbTransportFailure` reconnect path.
- If `g_abortUploadFlag` fires mid-pipeline: `smb2_run_event_loop` already checks it and returns `-ECANCELED`. Item 3's pipeline state machine must drain or cancel in-flight PDUs before returning; this is the main non-trivial correctness concern.

---

## 9. Appendix — Summary of actual code changes (for reviewers)

Two source files and one header modified on the `4.1-improve-progress-bar` branch:

- `@/opt/projects/personal/CPAP_data_uploader/include/SMBUploader.h`
  - Added `pipelineSlotSize` and `pipelineDepth` members.
  - Changed `allocateBuffer(size_t)` → `allocateBuffer(size_t slotSize, int depth = 1)` (default keeps callsites source-compatible).
  - Added `uint32_t getNegotiatedMaxWriteSize() const`.

- `@/opt/projects/personal/CPAP_data_uploader/src/SMBUploader.cpp`
  - New `smb2_pipeline_slot` struct and `smb2_run_multi_slot_event_loop()` helper.
  - `connect()`: installs `TCP_NODELAY` on the libsmb2 socket.
  - Removed `SMB_WRITE_TCP_DRAIN_BYTES` macro and its drain-delay usage.
  - `allocateBuffer()`: one malloc for `depth * slotSize` bytes, carved into slots in-place.
  - `upload()`: when `pipelineDepth >= 2`, uses the new pipelined path with `smb2_pwrite_async` + explicit offsets; falls through to the unchanged serial `smb2_write_ev` loop otherwise. Error and abort paths drain in-flight slots before returning.
  - Added `getNegotiatedMaxWriteSize()` wrapping `smb2_get_max_write_size(smb2)`.

- `@/opt/projects/personal/CPAP_data_uploader/src/FileUploader.cpp`
  - Phase 2 opening now calls `smbUploader->begin()` once and tears down once.
  - New decision block picks `desiredDepth = 2` when heap and server negotiation allow; calls `allocateBuffer(slotSize, desiredDepth)` with a single-slot fallback on allocation failure.
  - Removed the per-folder `smbUploader->end()` from `uploadDatalogFolderSmb()`.
  - Added `smb_phase_done:` label to fold error-exit paths cleanly.

*End of analysis document. Code changes shipped in v4.1-beta1 — see release notes for user-visible effects.*
