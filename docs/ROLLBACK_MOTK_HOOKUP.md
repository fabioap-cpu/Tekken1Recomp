# MotK rollback hookup checklist

Status: **plan** · branch `feat/rollback-netplay` · depends on `lib/recomp-net` @ `feat/rollback`

Today MotK / psxrecomp netplay is **delay-sync only**:
`stage_local → poll_admit (try_admit) → guest frame → finish_frame (advance)`.
Missing remote inputs **stall**. `INPUT_CONFIRM` hashes the **pad blob**, not
machine state. Rollback needs a second host path beside that loop.

recomp-net already provides: input contract, `RNetRbSession` episode FSM,
seal table, and `RNET_PKT_RB_*` wire. MotK must implement the host vtable,
rings, digests, invent, and transport mapping.

---

## 0. Ground rules (do not skip)

| Rule | Why |
|------|-----|
| Keep delay-sync `RNetSession` working behind a flag | Lobby / ICE / save-xfer stay useful; rollback opts in |
| Predicted rows promote only via `hash_confirm` (or host protect) | Library invariant; NULL `hash_confirm_promote` = always rewind |
| Digests must be bit-identical across peers for the same sealed inputs | Otherwise every invent becomes an episode storm |
| Snapshots must restore at the exact sim tick requested | Ring load is the only rewind mechanism |
| Single-thread ownership of session + rings | Same as delay-sync |

Env gate suggestion: `PSX_NET_MODE=delay|rollback` (default `delay`).

---

## 1. In-memory snapshot ring (host)

**Gap:** `boot_state_save` writes a path; load-from-buffer exists
(`boot_state_load_buffer`) but there is **no** `boot_state_save_buffer`.
User `.pst` slots are ~1.3–3.6 MB and go through `savestate_poll` + disk — too
slow / wrong API for per-frame GGPO.

### Tasks

- [x] Add `boot_state_save_buffer` (or serialize-to-malloc) mirroring load-buffer
- [x] New module e.g. `runtime/src/netplay_snap_ring.c`:
  - depth `N` (≥ `RNET_RB_SEAL_MAX_SPAN` = 128; start with 64 for soak)
  - keyed by `sim_tick`
  - `ring_save(tick)`, `ring_load(tick)`, `ring_has(tick)`, drop oldest
- [ ] Capture at a **safe** boundary (same constraints as `savestate_poll`:
      block leader, `in_exception == 0`) — next: wire from netplay frame loop
- [ ] On load: call existing frontend restage (`psx_frontend_on_savestate_loaded`
      / VRAM path) so present does not show a half-restored frame
- [ ] Memory budget: `N × ~1.5MB` → plan ~100MB; optional strip CDROM/MDEC
      sections later if MotK match path allows a thinner snap
- [x] Standalone ring bookkeeping test: `runtime/tests/test_netplay_snap_ring.c`

**Vtable:** `save_state` / `load_state` → ring only (never disk slots).

---

## 2. Master state digest + frame-commit watermark

**Gap:** delay-sync `INPUT_CONFIRM` is pad-checksum agreement, not state
agreement. Rollback `hash_confirm_through(tick)` needs a **state** watermark.

### Tasks

- [x] Define MotK master digest (start minimal, expand when soaks demand):
  - `netplay_master_digest`: CRC32 of CPU GPRs/PC/hi/lo + COP0 SR/Cause/EPC,
    cycle count, IRQ, timers, full RAM, dirty bitmap (VRAM/SPU/CD omitted)
- [x] After each committed sim tick, compute `digest[tick]` into a small ring
      (`netplay_hash_confirm` + `psx_netplay_finish_frame`)
- [x] Exchange `RNET_PKT_RB_FRAME_COMMIT` (opcode 24) via
      `rnet_session_send_rb_frame_commit` / `take_rb_frame_commit`
- [x] Advance local `resolved_through` only when digests match through `T`
- [x] Implement `psx_netplay_hash_confirm_through(tick)`
- [x] Wire `RNetInputContractHostGates.hash_confirm_promote` to that helper;
      leave other gates NULL initially (step 3 invent/contract)
- [x] Unit test: `runtime/tests/test_netplay_hash_confirm.c`

---

## 3. Input history + invent / prediction

**Gap:** `psx_netplay_poll_admit` waits for remote authority; no invent path.

### Tasks

- [x] Keep per-slot input history ring: tick → `RNetRbFrame`
      (`netplay_input_hist`, depth 128)
- [x] Local path: stage human pad as today (`PsxNetPad` → frame), mark
      `is_predicted = 0`
- [x] Remote path when authority missing for tick `T`:
  - invent = **hold-last** (neutral if no prior row)
  - mark `is_predicted = 1`
  - **do not stall** the guest (`np_try_admit_rollback`)
- [x] When remote INPUT arrives for `T`:
  - build published vs wire frames
  - `rnet_input_contract_stick_replace_decide` + `hash_confirm_promote`
  - promote → replace history row, no episode
  - rewind → set `pending_rewind_*` (episode begin is step 5)
- [ ] `get_input_row` vtable reads this history (local seal + self-seal fallback)
- [x] Map `PsxNetPad` ↔ `RNetRbFrame` once; SIO publish from history rows
      in rollback (`np_publish_hist_sio`)
- [x] Unit test: `runtime/tests/test_netplay_input_hist.c`
- [x] Env: `PSX_NET_MODE=delay|rollback` → `psx_netplay_rollback_mode()`

---

## 4. Live frame loop (replace admit barrier when rollback on)

Current (`main.cpp` `netplay_barrier_admit` / vblank):

```
stage_local → poll_admit (block) → run guest → finish_frame
```

Rollback live loop:

```
pump transport
stage local for T
invent remotes for T if needed
publish SIO from resolved+predicted rows
save_state(T) into ring          # after guest completes T, or at admit edge
advance_sim / finish guest tick T
state_digest(T); send FRAME_COMMIT
on late remote: contract → maybe begin_episode
```

### Tasks

- [x] Branch `poll_admit` on `PSX_NET_MODE=rollback` (gameplay invent path)
- [x] Never call delay-sync `try_admit` wait for missing remotes in rollback mode
- [x] Still use `RNetSession` for pad tip transport + ICE
      (`prepare_local_tip` / `peek_*_input`)
- [x] Keep load-barrier / save-xfer / soft-exit paths on delay-sync semantics
      for lobby rematch
- [ ] `finish_frame` snap-ring save at safe boundary (step 1 leftover + step 4)

---

## 5. Episode path (resim)

Library-owned FSM; host drives phases (see `docs/rollback.md`):

```
begin_episode → seal_inputs → exchange RB_SEAL_ROWS
→ AwaitingBaseline: load_state(load_tick), send RB_BASELINE digests
→ Replay: advance_sim(t) for t in [load..target] reading sealed rows
→ Verify: compare digests / RB_POST → on_post_match | on_post_diverge
```

### Tasks

- [ ] Create `RNetRbSession` beside (or instead of) delay admit when mode=rollback
- [ ] Fill `RNetRollbackVTable` completely
- [ ] On rewind decision: choose `load_tick` = oldest ring tick ≤ mismatch
      (clamp to ring depth; if too deep → abort / hard resync)
- [ ] Implement seal export/apply over `RNET_PKT_RB_SEAL_ROWS`
- [ ] During `rnet_rb_is_resimulating()`: mute audio present / skip wall pacer /
      do not push predicted SIO from live humans
- [ ] After commit: discard ring entries before `resolved_through`; resume live

---

## 6. Wire / transport mapping

| Opcode | Use |
|--------|-----|
| 20 `RB_SYNC` | Correction tuple |
| 21 `RB_SEAL_ROWS` | Peer sealed inputs |
| 22 `RB_BASELINE` | Post-load digests |
| 23 `RB_POST` | Post-replay digests + match |
| 24 `RB_FRAME_COMMIT` | Master-hash watermark |
| 25 `RB_RESOLVED` | Shared frontier advertise |

### Tasks

- [ ] Encode/decode helpers already in `rnet_protocol.*` — call from
      `psx_netplay` ingress next to existing INPUT handling
- [ ] Delay-sync peers must ignore opcodes 20–25 (library claim); verify MotK
      build only emits them when both sides negotiated rollback
- [ ] Negotiation: lobby match_caps or session hello flag `rollback=1`

---

## 7. Determinism prerequisites (MotK-specific)

Rollback only works if the same sealed pads produce the same digest.

### Tasks

- [ ] Confirm netplay clears mods (`commit_netplay` / `mod_runtime_clear_for_netplay`)
- [ ] Same BIOS stem + disc identity on both peers (existing verify)
- [ ] Audit non-deterministic host clocks in sim path (wall-time RNG, unsorted
      iteration) — fix or exclude from digest
- [ ] FMV / depth24: either pause invent (force delay) during FMV, or prove
      digests stay stable — MotK FMV was historically fragile under catch-up
- [ ] Multitap / N-slot: history + seal tables sized to `slot_count`

---

## 8. recomp-ui (branch `feat/rollback-netplay`)

Minimal for first soak; UI is not the blocker.

### Tasks

- [ ] Optional Host Lobby checkbox / advanced: “Rollback (experimental)”
- [ ] Plumb into `RecompLauncherCNetplayLaunch` or match_caps so
      `psx_netplay_start` sees the mode
- [ ] HUD/diag: show `resolved_through`, episode phase, invent count, ring
      occupancy (stderr or ImGui overlay)

---

## 9. Suggested implementation order

1. **`boot_state_save_buffer` + snap ring** — unit-test save/load tick roundtrip offline  
2. **Master digest + FRAME_COMMIT** — two peers, no invent yet; watermark advances in lockstep  
3. **Invent + input contract** — allow missing remotes; measure promote vs rewind  
4. **Episode resim** — force a stick mismatch; verify ring load + sealed replay  
5. **Lobby flag + UI** — MotK dual-instance soak  
6. **Thin snaps / FMV policy** — performance and movie stability  

---

## 10. File touch map (expected)

| Area | Files |
|------|--------|
| Snap ring | `boot_state.{c,h}`, new `netplay_snap_ring.{c,h}`, `savestate` only if sharing helpers |
| Digests / RB host | `psx_netplay.c`, `netplay_hash_confirm.*`, `netplay_state_digest.*` |
| Invent / contract | `netplay_input_hist.*`, `psx_netplay.c` (`np_try_admit_rollback`) |
| Frame loop | `main.cpp` (`netplay_barrier_admit`, vblank `finish_frame`) |
| Wire | `psx_netplay.c` ingress + `lib/recomp-net` peek/tip + FRAME_COMMIT |
| Caps / UI | MotK lobby callbacks, `recomp-ui` launch struct |

---

## 11. Done when

- [ ] Two MotK instances with `PSX_NET_MODE=rollback` complete a match without
      admit-stall on one-frame remote loss
- [ ] Forced remote stick correction triggers one episode and both digests match
      post-commit
- [ ] `hash_confirm` invent path shows promotes in diag without opening episodes
      when master hashes still agree
- [ ] Delay-sync path (`PSX_NET_MODE=delay`) unchanged for lobby rematch / save xfer

Reference: `lib/recomp-net/docs/rollback.md`,
`include/recomp_net/rollback.h`, `include/recomp_net/input_contract.h`,
`tests/rollback_episode_test.c`.
