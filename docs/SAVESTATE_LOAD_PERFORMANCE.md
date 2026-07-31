# Savestate load performance — solo and netplay

Status: **shipped** (MotK / Bomberman runtime + vendored `recomp-net`).
Scope: host-side restore hitch after F-key / protocol load, and lockstep
correctness when both peers restore together.

This is not the CD “load window” turbo work in [`LOAD_TIME_ZERO.md`](LOAD_TIME_ZERO.md).
That doc is about *in-game* disc loads. This one is about **user savestate
restore** (`.pst` / `boot_state_*`) feeling instant and staying in sync under
delay-netplay.

---

## Symptoms that drove the work

| Mode | Symptom | Host FPS | Root class |
|------|---------|----------|------------|
| Solo | Picture frozen ~1–several seconds after load; worse on 2nd/3rd load of same slot | ~60 | Giant `psx_advance_cycles` / IRQ blackout / present latch |
| Netplay | Host `applied, waiting for guest…`; guest stuck at `applying after transfer…` then disconnect | N/A | INPUT / admit deadlock across apply ↔ ready |
| Netplay spam | Hang when mashing load while holding directions | N/A | Same tip-starvation class; tip runway emptied by `hard_resync` |

---

## Solo restore path

Call chain (HLE scheduler required):

```
F-key / debug → savestate_request_load
  → savestate_poll (block leader)
    → boot_state_load
    → psx_cycles_resync_after_restore
    → interrupts_resync_after_restore
    → cdrom_accelerate_after_savestate
    → psx_frontend_on_savestate_loaded
    → psx_scheduler_resume_at(pc)   /* longjmp; does not return */
```

### 1. Anchor host-only cycle deadlines (`psx_cycles_resync_after_restore`)

**Bug:** `gte_ts_done` / `muldiv_ts_done` and load-absorb fields live on
`CPUState` but are **not** in the savestate wire format. After a warm load,
`psx_cycle_count` rewinds to the snapshot while those deadlines still sit on
the pre-load live timeline. The next GTE / muldiv stall then advances
`(live_ts − restored_cycle)` in one shot — tens of millions of cycles, many
nested presents, `chk(e=0)`, sticky VBlank, no draw.

**Fix:** After restore, set `gte_ts_done` / `muldiv_ts_done` to the restored
`psx_cycle_count`, clear absorb/fudge state, and re-anchor device sync /
idle-skip latches.

- `runtime/src/psx_cycles.c` — `psx_cycles_resync_after_restore`
- Called from `savestate_poll` immediately after a successful `boot_state_load`

### 2. Clear absolute IRQ cooldowns (`interrupts_resync_after_restore`)

**Bug:** `post_exception_cooldown_until` is an absolute guest-cycle stamp.
Leaving it in the future after a clock rewind blocks every IRQ (including
VBlank) until the restored clock “catches up” — freeze for however long the
user played past the save, while host FPS stays ~60.

**Fix:** Zero the cooldown and related exception / VBlank phase bookkeeping.

### 3. Cap CD second-response debt (`cdrom_accelerate_after_savestate`)

Restored / imminent CD command delays (ReadTOC, Init, seeks) can freeze the
picture for ~1s after the restored frame presents. A short post-load boost
window clamps outstanding delays so the display recovers immediately.

### 4. Frontend present / audio re-anchor (`psx_frontend_on_savestate_loaded`)

After restore:

- Force present (`s_force_present_after_load`) so a disabled-display blank
  latch or smooth-60 duplicate does not hide the restored frame.
- Invalidate GL present-dirty early-out (`gl_renderer_invalidate_present`) —
  critical on **2nd+ load of the same slot**, where the framebuffer can match
  the last swap and skip `SwapWindow`.
- Reset frame pacer + FPS baseline (admit / hitch can leave deadlines in the past).
- Resync guest-cycle→audio sample budgeting.

### 5. Smaller / faster `.pst` I/O (boot_state v4 zlib)

Large sections may be zlib-compressed on save (`BS_SEC` pad bit0). Shrinks
disk and helps slow storage; older readers still accept uncompressed v3.

### 6. No post-load request cooldown

A former 12-frame (earlier 60-frame) “load ignored” debounce in
`request_load_inner` was removed. It was only key-repeat padding and could
break netplay: protocol path entered `LOAD_APPLYING` while the cooldown
silently refused to stage `s_load_pending`.

Overlapping loads are gated elsewhere:

- Solo: single `s_load_pending` slot (last request wins).
- Netplay: `np_xfer_busy()` until the barrier clears.

### Diagnostics

`PSX_POST_LOAD_PROBE=1` arms a short post-load window that logs advance size,
IRQ check outcomes, dirty/idle/horizon, and host ms (`main.cpp` + cycle
attribution). Use this when a freeze returns; do **not** pause the runtime to
measure — extend the probe / rings instead.

---

## Netplay load path

Host-only initiate (`psx_netplay_request_load`). Guest follows via STATE_* on
the same UDP/relay path as inputs.

### High-level sequence

```
host: hash PROBE(op=LOAD, size, crc)
guest: REPLY match?
  yes → both stage savestate_request_load_protocol + LOAD_APPLYING
  no  → host STATE_BEGIN/CHUNK → guest writes sandbox
        → both stage load in np_apply_ready_state (transfer complete)
both: admit while savestate_pending (guest cycles → savestate_poll)
both: local LOADED → LOAD_READY
host: ready PROBE(op=LOAD, size=0, NP_LOAD_READY_CRC)
guest: ACK when applied
both: hard_resync + prime_delay_inputs (once)
both: stay in LOAD_READY until try_admit succeeds → resume lockstep
```

### Correctness / performance rules (do not regress)

1. **Stage apply only when both peers have the bytes**
   - Do **not** call `savestate_request_load_protocol` on the host at SEND
     begin. Host would restore during transfer, enter ready early, and starve
     the guest of tips needed for `savestate_poll`.
   - Hash-miss: both stage in `np_apply_ready_state` after transfer.
   - Hash-hit: both stage when the probe reply is handled.

2. **Keep INPUT flowing until mutual ready**
   - `LOAD_APPLYING` / enter `LOAD_READY`: do **not** set
     `input_send_suppress`.
   - Suppress only inside `np_commit_load_sync` for the
     `hard_resync` → `prime_delay_inputs` window (prime clears suppress).
   - App barrier (`psx_netplay_poll_admit`) freezes sim after apply /
     during ready; that is separate from INPUT emission.

3. **Ready probe must not stall INPUT (`recomp-net`)**
   - `rnet_session_state_probe` with `LOAD` + `total_size == 0` sets
     `state_stall_sim = 0` (same as SAVE coord).
   - Previously `state_stall_sim = 1` blocked `send_input_bundle` on the host
     as soon as the first peer finished apply — same deadlock as suppress,
     worse under spam because `hard_resync` leaves only ~D tip frames of
     runway.
   - Hash probes (`total_size != 0`) still stall until finish / transfer.

4. **`hard_resync` + prime once at mutual ready, not at apply**
   - Clearing rings / `sim_tick → 0` at apply time lets the later peer wipe
     the earlier peer’s tip and stall resume.
   - After mutual ready: clear local **and** remote rings, prime neutral
     delay prefix, wait for `try_admit` (fresh tip + INPUT_CONFIRM) before
     dropping `LOAD_READY`.

5. **Peer disconnect during barrier**
   - `psx_netplay_peer_disconnected(0)` while `psx_netplay_in_load_barrier()`
     so rx silence for a multi-second restore does not soft-exit to lobby.
     BYE / `peer_gone` still honored.

6. **Spam loads**
   - Host ignores new requests while `np_xfer_busy()`.
   - No savestate-layer frame cooldown (see solo §6).

### Where the code lives

| Piece | Location |
|-------|----------|
| App xfer / barrier | `runtime/src/psx_netplay.c` |
| Session stall / tip / hard_resync | `lib/recomp-net/src/session/rnet_session.c` |
| Protocol notes | `lib/recomp-net/docs/protocol.md` |
| Staging API | `savestate_request_load_protocol` (bypasses netplay user block) |

---

## Expected log lines (healthy netplay load)

```
netplay load slot=N — hash probe (…)
netplay load slot=N — hashes match, applying…   # or transferring / applying after transfer
savestate: LOADED slot N …
netplay load slot=N — applied, waiting for guest…   # host
netplay guest load slot=N — applied, waiting for host…  # or ready acked
netplay load slot=N — mutual ready, waiting lockstep…
netplay load slot=N — peer ready, resuming lockstep
```

Stuck on `waiting for guest` / guest never leaving `applying…` → tip
starvation (rules 1–3). Sticky `INPUT desync … stalled` → confirm/hash
disagreement after resume (inspect tip epoch / history collisions).

---

## Related docs

- [`LOAD_TIME_ZERO.md`](LOAD_TIME_ZERO.md) — in-game CD load wall-time (different problem).
- [`CYCLE_TIMING_ARCH.md`](CYCLE_TIMING_ARCH.md) — cycle / GTE stall model.
- `lib/recomp-net/docs/protocol.md` — STATE_PROBE / post-load ready rendezvous.
- `runtime/include/boot_state.h` — `.pst` section version / zlib flags.
