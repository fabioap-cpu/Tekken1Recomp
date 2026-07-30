#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "psx_netplay.h"

#include "memcard.h"
#include "savestate.h"
#include "sio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(PSX_HAS_RECOMP_NET)
#include "recomp_net/recomp_net.h"
#if defined(PSX_HAS_LOBBY_CLIENT)
#include "psx_lobby_client.h"
#endif
#endif

#ifndef PSX_MAX_PLAYERS
#define PSX_MAX_PLAYERS 2
#endif

/* Session pad count mirrored for release_pads (available without recomp-net). */
static int g_np_slot_count = 2;

/* Persists across shutdown so starvation dumps still see last session topology. */
static char g_np_diag_arch[24] = "off";
static int  g_np_diag_max_players = 0;
static int  g_np_diag_player_count = 0;
static int  g_np_diag_configured = 0;

int psx_netplay_diag_snapshot(char *arch_out, size_t arch_cap,
                              int *max_players_out, int *player_count_out)
{
    if (arch_out && arch_cap)
        snprintf(arch_out, arch_cap, "%s", g_np_diag_arch);
    if (max_players_out) *max_players_out = g_np_diag_max_players;
    if (player_count_out) *player_count_out = g_np_diag_player_count;
    return g_np_diag_configured;
}

void psx_netplay_config_defaults(PsxNetplayConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->local_slot = 0;
    cfg->slot_count = 2;
    cfg->player_count = 0;
    cfg->input_player = -1;
    cfg->input_delay = 2;
    cfg->force_input_relay = 0;
    cfg->force_turn = 0;
    cfg->transport = 0;
    cfg->session_id = 1;
    strncpy(cfg->bind_hostport, "0.0.0.0:7777", sizeof(cfg->bind_hostport) - 1);
    cfg->peer_hostport[0] = '\0';
}

static unsigned env_u(const char *name, unsigned def)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    return (unsigned)strtoul(v, NULL, 10);
}

void psx_netplay_apply_env(PsxNetplayConfig *cfg)
{
    const char *v;
    if (!cfg) return;
    v = getenv("PSX_NETPLAY");
    if (v && v[0] && v[0] != '0') cfg->enabled = 1;
    v = getenv("PSX_NET_SLOT");
    if (v && v[0]) cfg->local_slot = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_SLOTS");
    if (v && v[0]) cfg->slot_count = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_INPUT_PLAYER");
    if (v && v[0]) cfg->input_player = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_DELAY");
    if (v && v[0]) cfg->input_delay = (int)strtol(v, NULL, 10);
    cfg->session_id = env_u("PSX_NET_SESSION_ID", cfg->session_id);
    v = getenv("PSX_NET_BIND");
    if (v && v[0]) {
        strncpy(cfg->bind_hostport, v, sizeof(cfg->bind_hostport) - 1);
        cfg->bind_hostport[sizeof(cfg->bind_hostport) - 1] = '\0';
    }
    v = getenv("PSX_NET_PEER");
    if (v && v[0]) {
        strncpy(cfg->peer_hostport, v, sizeof(cfg->peer_hostport) - 1);
        cfg->peer_hostport[sizeof(cfg->peer_hostport) - 1] = '\0';
    }
    v = getenv("PSX_NET_TRANSPORT");
    if (v && v[0]) {
        if (strcmp(v, "ice") == 0 || strcmp(v, "ICE") == 0)
            cfg->transport = 1;
        else if (strcmp(v, "lan") == 0 || strcmp(v, "LAN") == 0)
            cfg->transport = 2;
    }
    v = getenv("PSX_NET_FORCE_TURN");
    if (v && v[0] && v[0] != '0')
        cfg->force_turn = 1;
}

void psx_netplay_normalize_pad(PsxNetPad *pad)
{
    const int dead = 24; /* ~SDL-ish center deadzone in 0..255 space */
    if (!pad) return;
    pad->connected = 1;
    if (pad->lx > (uint8_t)(0x80 - dead) && pad->lx < (uint8_t)(0x80 + dead)) pad->lx = 0x80;
    if (pad->ly > (uint8_t)(0x80 - dead) && pad->ly < (uint8_t)(0x80 + dead)) pad->ly = 0x80;
    if (pad->rx > (uint8_t)(0x80 - dead) && pad->rx < (uint8_t)(0x80 + dead)) pad->rx = 0x80;
    if (pad->ry > (uint8_t)(0x80 - dead) && pad->ry < (uint8_t)(0x80 + dead)) pad->ry = 0x80;
    if (!pad->analog) {
        pad->lx = pad->ly = pad->rx = pad->ry = 0x80;
    }
}

static void force_session_pads_connected(int slot_count)
{
    int i;
    if (slot_count < 2) slot_count = 2;
    if (slot_count > PSX_MAX_PLAYERS) slot_count = PSX_MAX_PLAYERS;
    if (slot_count >= 3)
        sio_set_multitap(1);
    else
        sio_set_multitap(0);
    for (i = 0; i < slot_count; ++i) {
        sio_connect_pad(i);
        /* Multitap taps are plain digital (sio clamps); lone port pad may be DS. */
        sio_set_pad_config_capable(i, sio_pad_on_multitap(i) ? 0 : 1);
    }
}

void psx_netplay_release_pads(void)
{
    int i;
    int n = g_np_slot_count;
    if (n < 2) n = 2;
    if (n > PSX_MAX_PLAYERS) n = PSX_MAX_PLAYERS;
    force_session_pads_connected(n);
    for (i = 0; i < n; ++i) {
        sio_set_pad_state_slot(i, 0xFFFFu);
        sio_set_pad_sticks(i, 0x80, 0x80, 0x80, 0x80);
        /* Tap slots stay digital; standalone port may request DualShock. */
        sio_request_pad_type(i, sio_pad_on_multitap(i) ? 0 : 1);
    }
}

#if !defined(PSX_HAS_RECOMP_NET)

int  psx_netplay_active(void) { return 0; }
int  psx_netplay_is_running(void) { return 0; }
const char *psx_netplay_transport_name(void) { return "none"; }
int  psx_netplay_ice_failed(void) { return 0; }
void psx_netplay_diag_tick(void) {}
int  psx_netplay_local_slot(void) { return -1; }
int  psx_netplay_input_player(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
int  psx_netplay_start(const PsxNetplayConfig *cfg)
{
    (void)cfg;
    return -1;
}
void psx_netplay_shutdown(void) {}
void psx_netplay_stage_local(const PsxNetPad *pad) { (void)pad; }
int  psx_netplay_needs_local_sample(void) { return 0; }
int  psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    (void)tick;
    (void)local_hash;
    (void)remote_hash;
    return 0;
}
int  psx_netplay_peer_disconnected(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return 0;
}
void psx_netplay_bind_guest_saves(void) {}
int  psx_netplay_is_host(void) { return 0; }
int  psx_netplay_request_save(int slot) { (void)slot; return 0; }
int  psx_netplay_request_load(int slot) { (void)slot; return 0; }
int  psx_netplay_in_load_barrier(void) { return 0; }
int  psx_netplay_consume_load_apply_failed(void) { return 0; }
void psx_netplay_pump(void) {}
int  psx_netplay_poll_admit(void) { return 1; }
void psx_netplay_finish_frame(void) {}
int  psx_netplay_remote_lead(void) { return 0; }
int  psx_netplay_input_delay(void) { return 2; }
int  psx_netplay_catchup_budget(void) { return 0; }
void psx_netplay_catchup_consume_frame(void) {}
void psx_netplay_wait_recv(int timeout_ms) { (void)timeout_ms; }
void psx_netplay_admit_wait_info(char *stall_out, size_t stall_cap,
                                 uint32_t *sim_tick_out, int *lead_out)
{
    if (stall_out && stall_cap) {
        stall_out[0] = '\0';
        if (stall_cap > 1)
            strncpy(stall_out, "off", stall_cap - 1);
    }
    if (sim_tick_out) *sim_tick_out = 0;
    if (lead_out) *lead_out = 0;
}

#else /* PSX_HAS_RECOMP_NET */

#define NP_SANDBOX_FALLBACK "saves/netplay"
#define NP_MC_BLOB_BYTES (4u + (size_t)MEMCARD_SIZE * 2u)
/* LOAD probe size==0 + this crc = post-load ready rendezvous (not SAVE coord). */
#define NP_LOAD_READY_CRC 0x4C4F4144u /* 'LOAD' */

typedef enum {
    NP_XFER_NONE = 0,
    NP_XFER_MC_PROBE,
    NP_XFER_MC_SEND,
    NP_XFER_SAVE_COORD,
    NP_XFER_SAVE_PROBE,
    NP_XFER_SAVE_SEND,
    NP_XFER_LOAD_PROBE,
    NP_XFER_LOAD_SEND,
    NP_XFER_LOAD_APPLYING, /* load staged; admit runs until savestate_poll fires */
    NP_XFER_LOAD_READY     /* local restore done; wait peer before lockstep */
} NpXferPhase;

typedef struct {
    RNetSession *session;
    PsxNetPad    staged;
    int          staged_valid;
    int          active;
    int          slot_count;
    int          local_slot;
    int          input_player; /* resolved host PlayerInput index */
    int          needs_advance;
    int          latched_for_tick; /* 1 if staged pad frozen for current sim_tick */
    uint32_t     latched_sim_tick;
    /* Guest sandbox: personal roots restored on shutdown. */
    int          guest_sandbox;
    char         personal_save_dir[512];
    char         personal_mc0[512];
    char         personal_mc1[512];
    uint32_t     bios_checksum;
    uint32_t     entry_pc;
    /* Host-owned save/memcard sync. */
    NpXferPhase  xfer;
    int          xfer_slot;
    int          mc_sync_done;
    int          mc_sync_sent;
    int          local_save_staged;
    int          local_save_acked;   /* guest: coord reply already sent */
    uint32_t     save_target_tick;   /* both peers save during this sim_tick */
    int          load_applied_local;
    int          load_ready_replied; /* READY exchanged; synced; stay LOAD_READY until admit */
    int          load_sync_done;     /* hard_resync+prime once at mutual ready */
    int          load_apply_failed;  /* sticky: staged apply rejected — soft-exit */
    /* Transport / ICE / diag (MotK online path). */
    int          use_ice;
    int          ice_has_turn;
    int          force_input_relay;
    int          is_host;
    int          input_delay;
    uint32_t     session_id;
    uint32_t     frames_finished;
    uint32_t     diag_session;
    unsigned     ice_stun_port;
    unsigned     ice_turn_port;
    char         ice_stun_host[128];
    char         ice_turn_host[128];
    char         ice_turn_user[192];
    char         ice_turn_pass[128];
    char         ice_bind_addr[64];
    char         bind_hostport[64];
    char         peer_hostport[64];
    char         match_mode[32];
    char         lobby_server[256];
    char         lobby_id[64];
} NetplayState;

static NetplayState g_np;

static FILE *g_diag_file;
static uint32_t g_diag_file_session;
static int g_diag_summary_written;
static uint32_t g_diag_last_write_ms;
static int g_diag_mkdir_done;

static void np_sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

static uint32_t np_mono_ms(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint32_t)((uint64_t)ts.tv_sec * 1000ull +
                          (uint64_t)ts.tv_nsec / 1000000ull);
#endif
#if defined(_WIN32)
    return (uint32_t)GetTickCount64();
#else
    return (uint32_t)((uint64_t)time(NULL) * 1000ull);
#endif
}

static void np_enter_load_ready(int slot);
static void np_commit_load_sync(void);
static void np_begin_load_apply(int slot);
static void np_starv_reset(void);
static void np_maybe_stage_target_save(void);

static int np_file_crc(const uint8_t *data, size_t size, uint32_t *crc_out)
{
    if (!data || size == 0 || !crc_out) return 0;
    *crc_out = rnet_checksum(data, size);
    return 1;
}

static int np_slot_crc(int slot, uint32_t *size_out, uint32_t *crc_out)
{
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t crc;
    if (!savestate_read_slot(slot, &data, &size) || !data) return 0;
    if (!np_file_crc(data, size, &crc)) {
        free(data);
        return 0;
    }
    if (size_out) *size_out = (uint32_t)size;
    if (crc_out) *crc_out = crc;
    free(data);
    return 1;
}

static int np_build_mc_blob(uint8_t *out, size_t cap, size_t *out_size)
{
    uint8_t *p;
    if (!out || cap < NP_MC_BLOB_BYTES || !out_size) return -1;
    memset(out, 0, NP_MC_BLOB_BYTES);
    p = out;
    p[0] = memcard_is_present(0) ? 1u : 0u;
    p[1] = memcard_is_present(1) ? 1u : 0u;
    if (p[0] && memcard_export_raw(0, p + 4) != 0) return -1;
    if (p[1] && memcard_export_raw(1, p + 4 + MEMCARD_SIZE) != 0) return -1;
    *out_size = NP_MC_BLOB_BYTES;
    return 0;
}

static int np_apply_mc_blob(const uint8_t *data, size_t size)
{
    if (!data || size < NP_MC_BLOB_BYTES) return -1;
    if (data[0]) {
        if (memcard_import_raw(0, data + 4) != 0) return -1;
    }
    if (data[1]) {
        if (memcard_import_raw(1, data + 4 + MEMCARD_SIZE) != 0) return -1;
    }
    return 0;
}

static int np_mc_blob_crc(uint32_t *size_out, uint32_t *crc_out)
{
    uint8_t *blob = (uint8_t *)malloc(NP_MC_BLOB_BYTES);
    size_t sz = 0;
    uint32_t crc;
    if (!blob) return 0;
    if (np_build_mc_blob(blob, NP_MC_BLOB_BYTES, &sz) != 0) {
        free(blob);
        return 0;
    }
    if (!np_file_crc(blob, sz, &crc)) {
        free(blob);
        return 0;
    }
    if (size_out) *size_out = (uint32_t)sz;
    if (crc_out) *crc_out = crc;
    free(blob);
    return 1;
}

static int np_xfer_busy(void)
{
    if (!g_np.session) return 0;
    if (g_np.xfer != NP_XFER_NONE) return 1;
    return rnet_session_state_busy(g_np.session) ||
           rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL);
}

static void np_enter_guest_sandbox(void)
{
    const char *dir = savestate_dir();
    const char *p0 = NULL;
    const char *p1 = NULL;
    uint32_t bios = 0, entry = 0;
    char sandbox[560];

    savestate_get_integrity(&bios, &entry);
    g_np.bios_checksum = bios;
    g_np.entry_pc = entry;
    if (dir && dir[0])
        strncpy(g_np.personal_save_dir, dir, sizeof(g_np.personal_save_dir) - 1);
    (void)memcard_debug_info(0, &p0, NULL, NULL, NULL);
    (void)memcard_debug_info(1, &p1, NULL, NULL, NULL);
    if (p0) strncpy(g_np.personal_mc0, p0, sizeof(g_np.personal_mc0) - 1);
    if (p1) strncpy(g_np.personal_mc1, p1, sizeof(g_np.personal_mc1) - 1);

    /* Prefer <memcard_dir>/netplay (absolute, next to the binary) so CWD does
     * not matter. Relative "saves/netplay" only as a last-resort fallback. */
    if (g_np.personal_save_dir[0]) {
        size_t n = strlen(g_np.personal_save_dir);
        while (n > 0 && (g_np.personal_save_dir[n - 1] == '/' ||
                         g_np.personal_save_dir[n - 1] == '\\')) {
            g_np.personal_save_dir[--n] = '\0';
        }
        snprintf(sandbox, sizeof(sandbox), "%s/netplay", g_np.personal_save_dir);
    } else {
        snprintf(sandbox, sizeof(sandbox), "%s", NP_SANDBOX_FALLBACK);
    }

    savestate_configure(sandbox, bios, entry);
    (void)memcard_rebind_dir(sandbox);
    g_np.guest_sandbox = 1;
    printf("psxrecomp: netplay guest sandbox -> %s\n", sandbox);
    fflush(stdout);
}

static void np_leave_guest_sandbox(void)
{
    if (!g_np.guest_sandbox) return;
    memcard_flush_all();
    (void)memcard_rebind_paths(
        g_np.personal_mc0[0] ? g_np.personal_mc0 : NULL,
        g_np.personal_mc1[0] ? g_np.personal_mc1 : NULL);
    (void)memcard_reload_bound();
    if (g_np.personal_save_dir[0])
        savestate_configure(g_np.personal_save_dir, g_np.bios_checksum, g_np.entry_pc);
    g_np.guest_sandbox = 0;
}

static void np_apply_ready_state(void)
{
    rnet_u8 op = 0, slot = 0;
    const void *data = NULL;
    size_t size = 0;

    if (!rnet_session_state_take_ready(g_np.session, &op, &slot, &data, &size))
        return;
    if (!data || size == 0) {
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        if (g_np.local_slot != 0 && np_apply_mc_blob((const uint8_t *)data, size) != 0) {
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        g_np.mc_sync_done = 1;
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        g_np.needs_advance = 0;
        g_np.latched_for_tick = 0;
        return;
    }

    if (op == RNET_STATE_OP_SAVE) {
        if (g_np.local_slot != 0) {
            if (!savestate_write_slot((int)slot, data, size)) {
                printf("psxrecomp: netplay guest save slot=%u — write failed\n", (unsigned)slot);
                fflush(stdout);
                rnet_session_state_finish(g_np.session, 0);
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            /* Post-transfer hash verify against wire CRC. */
            {
                uint32_t got_sz = 0, got_crc = 0;
                if (!np_slot_crc((int)slot, &got_sz, &got_crc) ||
                    got_sz != (uint32_t)size ||
                    got_crc != rnet_checksum((const rnet_u8 *)data, size)) {
                    printf("psxrecomp: netplay guest save slot=%u — post-CRC mismatch\n",
                           (unsigned)slot);
                    fflush(stdout);
                    rnet_session_state_finish(g_np.session, 0);
                    g_np.xfer = NP_XFER_NONE;
                    return;
                }
            }
            printf("psxrecomp: netplay guest save slot=%u — synced (%zu bytes)\n",
                   (unsigned)slot, size);
            fflush(stdout);
        } else {
            printf("psxrecomp: netplay save slot=%u — transfer complete\n", (unsigned)slot);
            fflush(stdout);
        }
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        return;
    }

    /* LOAD transfer (hash miss): guest stages the wire blob in memory (no disk
     * dependency — relative sandbox/CWD issues used to fail write_slot here).
     * Both peers request apply so host cannot restore before guest has bytes. */
    if (g_np.local_slot != 0) {
        if (!savestate_request_load_blob_protocol(data, size)) {
            printf("psxrecomp: netplay guest load slot=%u — blob stage failed "
                   "(%zu bytes, sandbox='%s')\n",
                   (unsigned)slot, size, savestate_dir());
            fflush(stdout);
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        /* Best-effort mirror to sandbox for hash-probe hits on rematch. */
        if (!savestate_write_slot((int)slot, data, size)) {
            printf("psxrecomp: netplay guest load slot=%u — sandbox mirror "
                   "failed (in-memory apply continues)\n",
                   (unsigned)slot);
            fflush(stdout);
        }
    } else {
        (void)savestate_request_load_protocol((int)slot);
    }
    rnet_session_state_finish(g_np.session, 0);
    np_begin_load_apply((int)slot);
    printf("psxrecomp: netplay load slot=%u — applying after transfer…\n", (unsigned)slot);
    fflush(stdout);
}

static void np_guest_handle_probe(void)
{
    rnet_u8 op = 0, slot = 0;
    rnet_u32 size = 0, crc = 0;
    int match = 0;

    if (g_np.local_slot == 0) return;
    if (!rnet_session_state_probe_pending(g_np.session, &op, &slot, &size, &crc))
        return;

    /* Post-load ready rendezvous (must be before SAVE size==0 coord). */
    if (op == RNET_STATE_OP_LOAD && size == 0 && crc == NP_LOAD_READY_CRC) {
        if (g_np.xfer == NP_XFER_LOAD_APPLYING) {
            if (savestate_pending())
                return; /* still need guest cycles to apply */
            if (savestate_take_load_completed()) {
                np_enter_load_ready((int)slot);
            } else if (!g_np.load_applied_local) {
                return; /* staged but not yet applied */
            }
        }
        if (g_np.xfer != NP_XFER_LOAD_READY && !g_np.load_applied_local) {
            return;
        }
        /* ACK host ready, commit resync+prime once, then stay in LOAD_READY
         * until try_admit succeeds (host must probe_finish first). */
        if (rnet_session_state_probe_reply(g_np.session, 1) != 0)
            return;
        np_commit_load_sync();
        if (!g_np.load_ready_replied) {
            g_np.load_ready_replied = 1;
            printf("psxrecomp: netplay load slot=%u — ready acked, waiting lockstep…\n",
                   (unsigned)slot);
            fflush(stdout);
        }
        return;
    }

    if (size == 0) {
        /* SAVE coord: crc carries the shared target sim_tick. Both peers
         * stage the write when sim reaches that tick (see
         * np_maybe_stage_target_save) so CRCs match and skip transfer. */
        if (g_np.xfer != NP_XFER_SAVE_COORD) {
            g_np.xfer = NP_XFER_SAVE_COORD;
            g_np.xfer_slot = (int)slot;
            g_np.save_target_tick = crc;
            g_np.local_save_staged = 0;
            g_np.local_save_acked = 0;
            printf("psxrecomp: netplay guest save slot=%u — armed target "
                   "sim=%u\n",
                   (unsigned)slot, (unsigned)crc);
            fflush(stdout);
        }
        if (savestate_pending()) return;
        if (!g_np.local_save_staged || !savestate_slot_exists((int)slot)) return;
        if (!g_np.local_save_acked) {
            g_np.local_save_acked = 1;
            (void)rnet_session_state_probe_reply(g_np.session, 1);
            printf("psxrecomp: netplay guest save slot=%u — local write done "
                   "@ target sim (frozen until hash probe)\n",
                   (unsigned)slot);
            fflush(stdout);
        }
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        uint32_t local_sz = 0, local_crc = 0;
        match = np_mc_blob_crc(&local_sz, &local_crc) && local_sz == size && local_crc == crc;
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (match) g_np.mc_sync_done = 1;
        return;
    }

    {
        uint32_t local_sz = 0, local_crc = 0;
        char reason[192];
        match = np_slot_crc((int)slot, &local_sz, &local_crc) && local_sz == size &&
                local_crc == crc;
        /* CRC match of a stale .pst (wrong codegen) is not loadable — ask the
         * host to transfer. Host also refuses probe start if its own slot is
         * stale, so this mainly covers guest-sandbox drift. */
        if (match && op == RNET_STATE_OP_LOAD &&
            !savestate_slot_compatible((int)slot, reason, sizeof(reason))) {
            printf("psxrecomp: netplay guest load slot=%u — hash matched but "
                   "unloadable (%s); requesting transfer\n",
                   (unsigned)slot, reason[0] ? reason : "incompatible");
            fflush(stdout);
            match = 0;
        }
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (op == RNET_STATE_OP_SAVE) {
            if (match) {
                g_np.xfer = NP_XFER_NONE;
                printf("psxrecomp: netplay guest save slot=%u — hashes match, "
                       "skip transfer\n",
                       (unsigned)slot);
                fflush(stdout);
            } else {
                /* Host will chunk the authoritative .pst — stay parked. */
                g_np.xfer = NP_XFER_SAVE_SEND;
                g_np.xfer_slot = (int)slot;
            }
        } else if (op == RNET_STATE_OP_LOAD) {
            if (match) {
                if (g_np.xfer != NP_XFER_LOAD_APPLYING &&
                    g_np.xfer != NP_XFER_LOAD_READY) {
                    (void)savestate_request_load_protocol((int)slot);
                    np_begin_load_apply((int)slot);
                    printf("psxrecomp: netplay guest load slot=%u — hashes match, "
                           "applying…\n",
                           (unsigned)slot);
                    fflush(stdout);
                }
            } else {
                /* Must mark LOAD_SEND or guest keeps the 20s admit timeout and
                 * BYEs the host mid-TURN transfer. */
                g_np.xfer = NP_XFER_LOAD_SEND;
                g_np.xfer_slot = (int)slot;
                printf("psxrecomp: netplay guest load slot=%u — hash miss, "
                       "waiting for transfer…\n",
                       (unsigned)slot);
                fflush(stdout);
            }
        }
    }
}

static void np_host_drive_xfer(void)
{
    int match = 0;
    uint32_t size = 0, crc = 0;
    uint8_t *buf = NULL;
    size_t n = 0;

    if (g_np.local_slot != 0 || !g_np.session) return;

    switch (g_np.xfer) {
    case NP_XFER_MC_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            g_np.mc_sync_done = 1;
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        {
            uint8_t *blob = (uint8_t *)malloc(NP_MC_BLOB_BYTES);
            size_t sz = 0;
            if (!blob || np_build_mc_blob(blob, NP_MC_BLOB_BYTES, &sz) != 0 ||
                rnet_session_state_begin(g_np.session, RNET_STATE_OP_SRAM, 0, blob, sz) != 0) {
                free(blob);
                g_np.mc_sync_done = 1;
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            free(blob);
            g_np.xfer = NP_XFER_MC_SEND;
        }
        return;

    case NP_XFER_SAVE_COORD:
        /* Host + guest both stage at save_target_tick; wait for local write
         * and guest ACK before hashing. */
        if (savestate_pending()) return;
        if (!g_np.local_save_staged || !savestate_slot_exists(g_np.xfer_slot))
            return;
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (!match) {
            /* Guest failed to save — still ship host blob. */
        }
        if (!np_slot_crc(g_np.xfer_slot, &size, &crc) ||
            rnet_session_state_probe(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)g_np.xfer_slot, size,
                                     crc) != 0) {
            printf("psxrecomp: netplay save slot=%d — hash probe failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        printf("psxrecomp: netplay save slot=%d — hash probe (%u bytes)\n", g_np.xfer_slot,
               (unsigned)size);
        fflush(stdout);
        g_np.xfer = NP_XFER_SAVE_PROBE;
        return;

    case NP_XFER_SAVE_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            printf("psxrecomp: netplay save slot=%d — hashes match, skip transfer\n",
                   g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        if (!savestate_read_slot(g_np.xfer_slot, &buf, &n) || !buf ||
            rnet_session_state_begin(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)g_np.xfer_slot, buf,
                                     n) != 0) {
            free(buf);
            printf("psxrecomp: netplay save slot=%d — transfer begin failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        printf("psxrecomp: netplay save slot=%d — transferring %zu bytes to guest\n",
               g_np.xfer_slot, n);
        fflush(stdout);
        free(buf);
        g_np.xfer = NP_XFER_SAVE_SEND;
        return;

    case NP_XFER_LOAD_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            (void)savestate_request_load_protocol(g_np.xfer_slot);
            np_begin_load_apply(g_np.xfer_slot);
            printf("psxrecomp: netplay load slot=%d — hashes match, applying…\n",
                   g_np.xfer_slot);
            fflush(stdout);
            return;
        }
        if (!savestate_read_slot(g_np.xfer_slot, &buf, &n) || !buf) {
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        /* Do not stage savestate_request_load here — host would apply during
         * SEND, enter LOAD_READY, and suppress INPUT before the guest can
         * admit frames for its own savestate_poll (deadlock). Both peers
         * stage in np_apply_ready_state when the transfer completes. */
        g_np.load_applied_local = 0;
        g_np.load_sync_done = 0;
        if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)g_np.xfer_slot, buf,
                                     n) != 0) {
            free(buf);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        free(buf);
        printf("psxrecomp: netplay load slot=%d — transferring %zu bytes\n", g_np.xfer_slot, n);
        fflush(stdout);
        g_np.xfer = NP_XFER_LOAD_SEND;
        return;

    case NP_XFER_MC_SEND:
    case NP_XFER_SAVE_SEND:
    case NP_XFER_LOAD_SEND:
        /* apply_ready runs first and clears take_ready (LOAD → LOAD_APPLYING). */
        if (rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL)) {
            if (g_np.xfer == NP_XFER_MC_SEND)
                g_np.mc_sync_done = 1;
            rnet_session_state_finish(g_np.session, 0);
            if (g_np.xfer == NP_XFER_LOAD_SEND) {
                np_begin_load_apply(g_np.xfer_slot);
            } else {
                g_np.xfer = NP_XFER_NONE;
            }
        }
        return;

    case NP_XFER_LOAD_READY:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        /* Mutual ready: drop probe stall first, then resync+prime. Stay in
         * LOAD_READY until try_admit (do not drop the app barrier early). */
        rnet_session_state_probe_finish(g_np.session);
        np_commit_load_sync();
        g_np.load_ready_replied = 1;
        printf("psxrecomp: netplay load slot=%d — mutual ready, waiting lockstep…\n",
               g_np.xfer_slot);
        fflush(stdout);
        return;

    default:
        return;
    }
}

static void np_prime_after_hard_resync(void)
{
    uint8_t bytes[PSX_NETPLAY_PAD_BYTES];
    PsxNetPad pad;

    /* Prime delay prefix with the current local hold (not forced neutral) so the
     * first D play frames continue what the player is already pressing. Each
     * peer only primes its own slot — lockstep stays valid. Tip latency for
     * *changes* remains D; we just avoid a post-load dead zone of released pads. */
    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0xFFFFu;
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
    pad.analog = 1;
    pad.connected = 1;
    if (g_np.staged_valid)
        pad = g_np.staged;
    pad.connected = 1;
    psx_netplay_normalize_pad(&pad);

    bytes[0] = (uint8_t)(pad.buttons & 0xFFu);
    bytes[1] = (uint8_t)((pad.buttons >> 8) & 0xFFu);
    bytes[2] = pad.lx;
    bytes[3] = pad.ly;
    bytes[4] = pad.rx;
    bytes[5] = pad.ry;
    bytes[6] = pad.analog ? 1u : 0u;
    bytes[7] = 1u;
    rnet_session_prime_delay_inputs(g_np.session, bytes, (rnet_u16)PSX_NETPLAY_PAD_BYTES);

    /* Keep staged matching the prime so the first tip sample is not a sudden
     * release while [0..D) still holds the live pad. */
    g_np.staged = pad;
    g_np.staged_valid = 1;
}

/* Stage restore. Keep INPUT flowing so try_admit can still run guest cycles
 * for savestate_poll — suppress only at mutual ready (np_commit_load_sync).
 * Ready probe must also leave INPUT unstalled (recomp-net size==0 LOAD). */
static void np_begin_load_apply(int slot)
{
    /* Transfer admit failures (state_xfer) often latch starvation; lead can sit
     * at D-1 after ICE xfer and would block the only frame savestate_poll needs. */
    np_starv_reset();
    g_np.xfer = NP_XFER_LOAD_APPLYING;
    g_np.load_applied_local = 0;
    g_np.load_sync_done = 0;
    g_np.load_ready_replied = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    g_np.xfer_slot = slot;
}

/* Once per load, at mutual ready (guest READY ACK / host take_reply). */
static void np_commit_load_sync(void)
{
    if (g_np.load_sync_done || !g_np.session)
        return;
    /* Suppress empty tips only for the hard_resync→prime window. */
    rnet_session_set_input_send_suppress(g_np.session, 1);
    rnet_session_hard_resync(g_np.session);
    np_prime_after_hard_resync(); /* clears suppress + emits fresh tip */
    g_np.load_sync_done = 1;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    /* staged_valid left set by prime — tip must match delay-prefix hold. */
}

static void np_enter_load_ready(int slot)
{
    /* Do not hard_resync/prime here — the later-applying peer would clear the
     * earlier peer's tip and stall resume. Sync runs at mutual ready.
     * Do not suppress INPUT here either: the first peer to finish apply must
     * keep sending pads so the other can still admit frames for savestate_poll. */
    g_np.load_applied_local = 1;
    g_np.load_ready_replied = 0;
    g_np.load_sync_done = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    g_np.xfer = NP_XFER_LOAD_READY;
    g_np.xfer_slot = slot;
}

/* After both peers stage a load: run until restore completes, then rendezvous.
 * hard_resync+prime happens once at mutual ready (not at apply). */
static void np_drive_load_barrier(void)
{
    if (g_np.xfer != NP_XFER_LOAD_APPLYING)
        return;
    if (savestate_pending())
        return;
    if (savestate_take_load_failed()) {
        /* Stale/mismatched .pst: do not sit in load_apply_done forever. */
        printf("psxrecomp: netplay load slot=%d — apply failed "
               "(incompatible or missing .pst) — aborting barrier\n",
               g_np.xfer_slot);
        fflush(stdout);
        if (g_np.session)
            rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        g_np.load_applied_local = 0;
        g_np.load_ready_replied = 0;
        g_np.load_sync_done = 0;
        g_np.load_apply_failed = 1;
        if (g_np.session)
            rnet_session_set_input_send_suppress(g_np.session, 0);
        return;
    }
    if (!g_np.load_applied_local && !savestate_take_load_completed())
        return;

    np_enter_load_ready(g_np.xfer_slot);

    if (g_np.local_slot == 0) {
        if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)g_np.xfer_slot, 0,
                                     NP_LOAD_READY_CRC) != 0) {
            printf("psxrecomp: netplay load slot=%d — ready probe failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            g_np.load_applied_local = 0;
            if (g_np.session)
                rnet_session_set_input_send_suppress(g_np.session, 0);
            return;
        }
        printf("psxrecomp: netplay load slot=%d — applied, waiting for guest…\n",
               g_np.xfer_slot);
        fflush(stdout);
    } else {
        printf("psxrecomp: netplay guest load slot=%d — applied, waiting for host…\n",
               g_np.xfer_slot);
        fflush(stdout);
    }
}

static void np_maybe_start_mc_sync(void)
{
    uint32_t size = 0, crc = 0;
    if (g_np.local_slot != 0 || g_np.mc_sync_sent || g_np.mc_sync_done)
        return;
    if (!rnet_session_is_running(g_np.session)) return;
    if (np_xfer_busy()) return;
    if (!np_mc_blob_crc(&size, &crc)) {
        g_np.mc_sync_done = 1;
        return;
    }
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_SRAM, 0, size, crc) != 0) {
        g_np.mc_sync_done = 1;
        return;
    }
    g_np.mc_sync_sent = 1;
    g_np.xfer = NP_XFER_MC_PROBE;
}

static void encode_pad(const PsxNetPad *pad, RNetInputSample *out, rnet_u32 tick)
{
    PsxNetPad n = *pad;
    psx_netplay_normalize_pad(&n);
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->size = PSX_NETPLAY_PAD_BYTES;
    out->bytes[0] = (rnet_u8)(n.buttons & 0xFFu);
    out->bytes[1] = (rnet_u8)((n.buttons >> 8) & 0xFFu);
    out->bytes[2] = n.lx;
    out->bytes[3] = n.ly;
    out->bytes[4] = n.rx;
    out->bytes[5] = n.ry;
    out->bytes[6] = n.analog ? 1u : 0u;
    out->bytes[7] = 1u;
    out->valid = 1;
}

static void decode_pad(const RNetInputSample *in, PsxNetPad *pad)
{
    memset(pad, 0, sizeof(*pad));
    pad->buttons = 0xFFFFu;
    pad->lx = pad->ly = pad->rx = pad->ry = 0x80u;
    pad->analog = 1;
    pad->connected = 1;
    if (!in || !in->valid || in->size < PSX_NETPLAY_PAD_BYTES) return;
    pad->buttons = (uint16_t)in->bytes[0] | ((uint16_t)in->bytes[1] << 8);
    pad->lx = in->bytes[2];
    pad->ly = in->bytes[3];
    pad->rx = in->bytes[4];
    pad->ry = in->bytes[5];
    pad->analog = in->bytes[6] ? 1u : 0u;
    pad->connected = 1;
    psx_netplay_normalize_pad(pad);
}

static void apply_pad_slot(int slot, const PsxNetPad *pad)
{
    if (slot < 0 || slot >= g_np.slot_count || slot >= PSX_MAX_PLAYERS || !pad) return;
    const int on_tap = sio_pad_on_multitap(slot);
    sio_set_pad_connected(slot, 1);
    sio_set_pad_config_capable(slot, on_tap ? 0 : 1);
    sio_set_pad_state_slot(slot, pad->buttons);
    if (on_tap)
        sio_set_pad_sticks(slot, 0x80, 0x80, 0x80, 0x80);
    else
        sio_set_pad_sticks(slot, pad->lx, pad->ly, pad->rx, pad->ry);
    sio_request_pad_type(slot, (!on_tap && pad->analog) ? 1 : 0);
}

static void host_sample_local(rnet_u32 tick, RNetInputSample *out, void *ctx)
{
    NetplayState *st = (NetplayState *)ctx;
    PsxNetPad pad;
    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0xFFFFu;
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
    pad.analog = 1;
    pad.connected = 1;
    if (st->staged_valid) pad = st->staged;
    pad.connected = 1;
    encode_pad(&pad, out, tick);
}

static void host_publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *ctx)
{
    int i;
    int n;
    (void)tick;
    (void)ctx;
    if (!by_slot || slots <= 0) return;
    n = g_np.slot_count;
    if (n > slots) n = slots;
    if (n > PSX_MAX_PLAYERS) n = PSX_MAX_PLAYERS;
    force_session_pads_connected(n);
    for (i = 0; i < n; ++i) {
        PsxNetPad pad;
        decode_pad(&by_slot[i], &pad);
        apply_pad_slot(i, &pad);
    }
}

int psx_netplay_active(void)
{
    return g_np.active && g_np.session != NULL;
}

int psx_netplay_is_running(void)
{
    return psx_netplay_active() && rnet_session_is_running(g_np.session);
}

const char *psx_netplay_transport_name(void)
{
    if (!psx_netplay_active()) return "none";
    return g_np.use_ice ? "ice" : "lan";
}

int psx_netplay_ice_failed(void)
{
#if defined(RNET_ENABLE_ICE)
    if (!psx_netplay_active() || !g_np.use_ice)
        return 0;
    return rnet_session_ice_state(g_np.session) == RNET_ICE_STATE_FAILED;
#else
    return 0;
#endif
}

int psx_netplay_local_slot(void)
{
    return psx_netplay_active() ? g_np.local_slot : -1;
}

int psx_netplay_input_player(void)
{
    return psx_netplay_active() ? g_np.input_player : 0;
}

uint32_t psx_netplay_sim_tick(void)
{
    if (!psx_netplay_active()) return 0;
    return rnet_session_sim_tick(g_np.session);
}

void psx_netplay_stage_local(const PsxNetPad *pad)
{
    if (!pad) {
        g_np.staged_valid = 0;
        return;
    }
    /* Once running, freeze the first sample for the current sim tick so
     * re-admits / barrier retries cannot change the INPUT_CONFIRM hash. */
    if (psx_netplay_active() && rnet_session_is_running(g_np.session)) {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        if (g_np.latched_for_tick && g_np.latched_sim_tick == t)
            return;
        g_np.staged = *pad;
        psx_netplay_normalize_pad(&g_np.staged);
        g_np.staged_valid = 1;
        g_np.latched_for_tick = 1;
        g_np.latched_sim_tick = t;
        return;
    }
    /* Linking: keep refreshing released/local pads until START. */
    g_np.staged = *pad;
    psx_netplay_normalize_pad(&g_np.staged);
    g_np.staged_valid = 1;
}

int psx_netplay_needs_local_sample(void)
{
    if (!psx_netplay_active()) return 0;
    if (!rnet_session_is_running(g_np.session)) return 1; /* linking */
    {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        return !(g_np.latched_for_tick && g_np.latched_sim_tick == t);
    }
}

int psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    if (!psx_netplay_active()) return 0;
    return rnet_session_input_desync(g_np.session, tick, local_hash, remote_hash);
}

int psx_netplay_peer_disconnected(uint32_t timeout_ms)
{
    if (!psx_netplay_active()) return 0;
    /* timeout_ms == 0: BYE / peer_gone only (no silence timeout). Used during
     * load barriers where INPUT is suppressed for seconds. */
    return rnet_session_peer_disconnected(g_np.session, (rnet_u64)timeout_ms);
}

static void np_diag_capture(const PsxNetplayConfig *cfg, int slots)
{
    const char *arch = "p2p";
    int players;
    if (!cfg) return;
    if (cfg->force_input_relay)
        arch = "server_relay";
    else if (slots >= 3)
        arch = "host_relay";
    players = cfg->player_count > 0 ? cfg->player_count : slots;
    if (players < 1) players = slots;
    snprintf(g_np_diag_arch, sizeof(g_np_diag_arch), "%s", arch);
    g_np_diag_max_players = slots;
    g_np_diag_player_count = players;
    g_np_diag_configured = 1;
}

#if defined(__linux__)
static int peer_is_loopback(const char *peer_hostport)
{
    if (!peer_hostport || !peer_hostport[0]) return 0;
    if (strncmp(peer_hostport, "127.", 4) == 0) return 1;
    if (strncmp(peer_hostport, "localhost:", 10) == 0) return 1;
    if (strncmp(peer_hostport, "::1:", 4) == 0) return 1;
    if (strcmp(peer_hostport, "::1") == 0) return 1;
    return 0;
}

/* Same-machine MotK FMV: lockstep syncs both peers' MDEC peaks; pinning each
 * slot to a disjoint CPU half cut headless FMV ~40 → ~45 in A/B. */
static void pin_localhost_peer_cpus(int local_slot)
{
    long ncpu;
    cpu_set_t set;
    int i, lo, hi;

    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 4) return;
    CPU_ZERO(&set);
    if (local_slot <= 0) {
        lo = 0;
        hi = (int)(ncpu / 2);
    } else {
        lo = (int)(ncpu / 2);
        hi = (int)ncpu;
    }
    for (i = lo; i < hi; i++)
        CPU_SET(i, &set);
    (void)sched_setaffinity(0, sizeof(set), &set);
}
#endif

#if defined(PSX_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
static void host_on_signal(const RNetSignal *msg, void *ctx)
{
    (void)ctx;
    if (!msg) return;
    (void)psx_lobby_send_signal((int)msg->type, (int)msg->flag, msg->text);
}

static void drain_lobby_signals(void)
{
    int type = 0, flag = 0;
    char text[2048];
    if (!g_np.session) return;
    while (psx_lobby_poll_signal(&type, &flag, text, sizeof(text))) {
        RNetSignal sig;
        memset(&sig, 0, sizeof(sig));
        /* Peers emit LOCAL_*; push_signal expects REMOTE_* for SDP/candidates. */
        if (type == (int)RNET_SIGNAL_LOCAL_SDP)
            type = (int)RNET_SIGNAL_REMOTE_SDP;
        else if (type == (int)RNET_SIGNAL_LOCAL_CANDIDATE)
            type = (int)RNET_SIGNAL_REMOTE_CANDIDATE;
        sig.type = (RNetSignalType)type;
        sig.flag = (rnet_u8)(flag & 0xFF);
        strncpy(sig.text, text, sizeof(sig.text) - 1);
        rnet_session_push_signal(g_np.session, &sig);
    }
}
#else
static void drain_lobby_signals(void) {}
#endif

static int resolve_use_ice(const PsxNetplayConfig *cfg)
{
    int in_motk_room = 0;

    if (cfg->transport == 2) return 0; /* force LAN */
#if defined(PSX_HAS_LOBBY_CLIENT)
    in_motk_room = psx_lobby_connected() && psx_lobby_in_lobby();
#endif

    /* Server UDP pad relay: dial relay_endpoint with LAN transport (not ICE).
     * MotK previously always preferred ICE and ignored the relay rewrite. */
    if (cfg->force_input_relay) {
        if (!cfg->peer_hostport || !cfg->peer_hostport[0]) {
            fprintf(stderr,
                    "psx_netplay: force_input_relay set but peer/relay "
                    "endpoint empty\n");
            return -1;
        }
        fprintf(stderr,
                "psx_netplay: server input relay — LAN transport to %s\n",
                cfg->peer_hostport);
        return 0;
    }

#if defined(RNET_ENABLE_ICE) && defined(PSX_HAS_LOBBY_CLIENT)
    if (cfg->transport == 1) {
        if (!in_motk_room) {
            fprintf(stderr,
                    "psx_netplay: ICE requested but MotK lobby not connected\n");
            return -1;
        }
        return 1;
    }
    /* Auto: hosted MotK room always uses ICE. Do not demote to LAN when the
     * lobby rewrites 0.0.0.0 binds to a private TCP peer IP (often wrong).
     * Direct IP / LAN file lobby (no MotK seat) stays on LAN UDP. */
    if (in_motk_room)
        return 1;
    return 0;
#else
    {
        int online_requested = cfg->transport == 1 ||
                               (cfg->transport == 0 && in_motk_room);
        if (online_requested) {
            fprintf(stderr,
                    "psx_netplay: hosted lobby requires ICE, but ICE is not "
                    "available in this build (configure with PSX_NET_ICE=ON / "
                    "RNET_ENABLE_ICE=ON)\n");
            return -1;
        }
    }
    return 0;
#endif
}


int psx_netplay_start(const PsxNetplayConfig *cfg)
{
    RNetConfig rcfg;
    RNetHostVTable host;
    int in_player;
    int slots;
    int local;
    int use_ice;

    if (!cfg || !cfg->enabled) return -1;
    if (g_np.session) psx_netplay_shutdown();

    slots = cfg->slot_count;
    if (slots < 2) slots = 2;
    if (slots > PSX_MAX_PLAYERS) slots = PSX_MAX_PLAYERS;
    if (slots > RNET_MAX_SLOTS) slots = RNET_MAX_SLOTS;

    local = cfg->local_slot;
    if (local < 0) local = 0;
    if (local >= slots) local = slots - 1;

    rnet_config_init_defaults(&rcfg);
    rcfg.slot_count = (rnet_u8)slots;
    rcfg.local_slot = (rnet_u8)local;
    /* Max delay 20 matches RNET_MAX_BUNDLE 21 (neutral prefix + tip). */
    rcfg.input_delay = (rnet_u8)(cfg->input_delay < 0 ? 0
                               : (cfg->input_delay > 20 ? 20 : cfg->input_delay));
    rcfg.session_id = cfg->session_id ? cfg->session_id : 1u;

    /* Host resolves auto (-1) before start; accept 0..PSX_MAX_PLAYERS-1. */
    in_player = cfg->input_player;
    if (in_player < 0 || in_player >= PSX_MAX_PLAYERS) in_player = 0;

    use_ice = resolve_use_ice(cfg);
    if (use_ice < 0)
        return -4;

    memset(&host, 0, sizeof(host));
    host.sample_local = host_sample_local;
    host.publish = host_publish;
    host.ctx = &g_np;
#if defined(PSX_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
    if (use_ice)
        host.on_signal = host_on_signal;
#endif

    g_np.session = rnet_session_create(&rcfg, &host);
    if (!g_np.session) return -2;

    if (use_ice) {
#if defined(RNET_ENABLE_ICE)
        RNetIceConfig ice;
        RNetIpv4Address addrs[8];
        int naddr;
        const char *env_turn_host = getenv("PSX_NET_TURN_HOST");
        const char *env_turn_user = getenv("PSX_NET_TURN_USER");
        const char *env_turn_pass = getenv("PSX_NET_TURN_PASS");
        const char *env_stun = getenv("PSX_NET_STUN_HOST");

        g_np.ice_has_turn = 0;
        g_np.ice_stun_host[0] = '\0';
        g_np.ice_turn_host[0] = '\0';
        g_np.ice_turn_user[0] = '\0';
        g_np.ice_turn_pass[0] = '\0';
        g_np.ice_bind_addr[0] = '\0';

        rnet_ice_config_init_defaults(&ice);
        ice.controlling = (rcfg.local_slot == 0) ? 1u : 0u;

        naddr = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
        if (naddr > 0 && addrs[0].address[0]) {
            snprintf(g_np.ice_bind_addr, sizeof(g_np.ice_bind_addr), "%s",
                     addrs[0].address);
            ice.bind_address = g_np.ice_bind_addr;
        }

#if defined(PSX_HAS_LOBBY_CLIENT)
        /* Prefer TURN prefetched at WS welcome; re-request and wait if stale. */
        if (psx_lobby_connected()) {
            int i;
            const PsxLobbyTurnCredentials *tc = psx_lobby_turn_credentials();
            if (!tc || !tc->valid) {
                (void)psx_lobby_request_turn_credentials();
                for (i = 0; i < 200; ++i) { /* up to ~2s */
                    tc = psx_lobby_turn_credentials();
                    if (tc && tc->valid)
                        break;
                    psx_lobby_pump();
                    np_sleep_ms(10);
                }
            }
        }
        {
            const PsxLobbyTurnCredentials *tc = psx_lobby_turn_credentials();
            if (tc && tc->valid) {
                if (tc->stun_host[0]) {
                    snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host),
                             "%s", tc->stun_host);
                    ice.stun_host = g_np.ice_stun_host;
                    ice.stun_port = (rnet_u16)(tc->stun_port > 0 ? tc->stun_port
                                                                  : 3478);
                }
                snprintf(g_np.ice_turn_host, sizeof(g_np.ice_turn_host), "%s",
                         tc->turn_host);
                snprintf(g_np.ice_turn_user, sizeof(g_np.ice_turn_user), "%s",
                         tc->username);
                snprintf(g_np.ice_turn_pass, sizeof(g_np.ice_turn_pass), "%s",
                         tc->password);
                ice.turn_host = g_np.ice_turn_host;
                ice.turn_user = g_np.ice_turn_user;
                ice.turn_pass = g_np.ice_turn_pass;
                ice.turn_port = (rnet_u16)(tc->turn_port > 0 ? tc->turn_port
                                                              : 3478);
                g_np.ice_has_turn = 1;
            }
        }
#endif
        if (env_stun && env_stun[0]) {
            snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host), "%s",
                     env_stun);
            ice.stun_host = g_np.ice_stun_host;
            ice.stun_port = (rnet_u16)env_u("PSX_NET_STUN_PORT", ice.stun_port
                                                                     ? ice.stun_port
                                                                     : 3478);
        }
        if (env_turn_host && env_turn_host[0] && env_turn_user &&
            env_turn_user[0] && env_turn_pass && env_turn_pass[0]) {
            snprintf(g_np.ice_turn_host, sizeof(g_np.ice_turn_host), "%s",
                     env_turn_host);
            snprintf(g_np.ice_turn_user, sizeof(g_np.ice_turn_user), "%s",
                     env_turn_user);
            snprintf(g_np.ice_turn_pass, sizeof(g_np.ice_turn_pass), "%s",
                     env_turn_pass);
            ice.turn_host = g_np.ice_turn_host;
            ice.turn_user = g_np.ice_turn_user;
            ice.turn_pass = g_np.ice_turn_pass;
            ice.turn_port = (rnet_u16)env_u("PSX_NET_TURN_PORT", 3478);
            g_np.ice_has_turn = 1;
        }

        if (!g_np.ice_stun_host[0] && ice.stun_host && ice.stun_host[0]) {
            snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host), "%s",
                     ice.stun_host);
        }
        g_np.ice_stun_port = ice.stun_port ? (unsigned)ice.stun_port : 19302u;
        g_np.ice_turn_port = ice.turn_port ? (unsigned)ice.turn_port : 0u;

        if (g_np.ice_has_turn) {
            fprintf(stderr,
                    "psx_netplay: ICE stun=%s:%u turn=%s:%u user=%s bind=%s\n",
                    ice.stun_host ? ice.stun_host : "(default)",
                    (unsigned)ice.stun_port,
                    ice.turn_host, (unsigned)ice.turn_port, ice.turn_user,
                    ice.bind_address ? ice.bind_address : "(any)");
        } else {
            const char *allow_stun = getenv("PSX_NET_ALLOW_STUN_ONLY");
            fprintf(stderr,
                    "psx_netplay: ICE STUN-only (no TURN) stun=%s:%u "
                    "bind=%s — online MotK requires Coturn "
                    "(lobby get_turn_credentials or PSX_NET_TURN_*); set "
                    "PSX_NET_ALLOW_STUN_ONLY=1 to override\n",
                    ice.stun_host ? ice.stun_host : "(default)",
                    (unsigned)ice.stun_port,
                    ice.bind_address ? ice.bind_address : "(any)");
            /* BattleShip-style: refuse WAN ICE without TURN (CGNAT hangs). */
            if (!allow_stun || !allow_stun[0] || allow_stun[0] == '0') {
                rnet_session_destroy(g_np.session);
                g_np.session = NULL;
                return -4;
            }
        }

        {
            /* Online default is Force TURN (match_caps / UI); env overrides. */
            int force_turn = cfg->force_turn ? 1 : 0;
            const char *ft = getenv("PSX_NET_FORCE_TURN");
            if (ft && ft[0] && ft[0] != '0')
                force_turn = 1;
            else if (ft && ft[0] == '0')
                force_turn = 0;
            if (force_turn && !g_np.ice_has_turn) {
                fprintf(stderr,
                        "psx_netplay: FORCE_TURN requires Coturn credentials "
                        "(lobby get_turn_credentials or PSX_NET_TURN_*)\n");
                rnet_session_destroy(g_np.session);
                g_np.session = NULL;
                return -4;
            }
            if (force_turn) {
                ice.force_relay = 1;
                fprintf(stderr,
                        "psx_netplay: FORCE_TURN — ICE will use relay-only "
                        "candidates (host match_caps / all peers)\n");
            }
        }

        if (rnet_session_start_ice(g_np.session, &ice) != 0) {
            fprintf(stderr,
                    "psx_netplay: start_ice failed; refusing unsafe LAN "
                    "fallback for an online lobby\n");
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            return -4;
        }
#else
        fprintf(stderr, "psx_netplay: ICE requested but not built\n");
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
        return -4;
#endif
    }

    if (!use_ice) {
        /* Host-as-relay: slot 0 with 3+ seats and no dial peer. */
#if PSX_MAX_PLAYERS >= 3
        const int peer_empty =
            !cfg->peer_hostport || !cfg->peer_hostport[0];
        const int use_hub = (local == 0 && slots >= 3 && peer_empty);
        const int rc = use_hub
            ? rnet_session_start_lan_hub(g_np.session, cfg->bind_hostport)
            : rnet_session_start_lan(g_np.session, cfg->bind_hostport,
                                    cfg->peer_hostport);
#else
        const int rc = rnet_session_start_lan(g_np.session, cfg->bind_hostport,
                                              cfg->peer_hostport);
#endif
        if (rc != 0) {
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            return -3;
        }
    }
    np_diag_capture(cfg, slots);
    g_np.active = 1;
    g_np.use_ice = use_ice ? 1 : 0;
    g_np.slot_count = (int)rcfg.slot_count;
    g_np_slot_count = g_np.slot_count;
    g_np.local_slot = (int)rcfg.local_slot;
    g_np.input_player = in_player;
    if (g_np.slot_count >= 3)
        sio_set_multitap(1);
    else
        sio_set_multitap(0);
    g_np.staged_valid = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.latched_sim_tick = 0;
    g_np.xfer = NP_XFER_NONE;
    g_np.xfer_slot = 0;
    g_np.mc_sync_done = 0;
    g_np.mc_sync_sent = 0;
    g_np.local_save_staged = 0;
    g_np.load_applied_local = 0;
    g_np.guest_sandbox = 0;
    g_np.force_input_relay = cfg->force_input_relay ? 1 : 0;
    g_np.input_delay = (int)rcfg.input_delay;
    g_np.session_id = rcfg.session_id;
    g_np.is_host = (g_np.local_slot == 0) ? 1 : 0;
    g_np.frames_finished = 0;
    g_np.diag_session++;
    g_diag_summary_written = 0;
    if (g_diag_file) {
        fclose(g_diag_file);
        g_diag_file = NULL;
    }
    g_diag_file_session = 0;
    g_diag_last_write_ms = 0;
    snprintf(g_np.bind_hostport, sizeof(g_np.bind_hostport), "%s",
             cfg->bind_hostport);
    snprintf(g_np.peer_hostport, sizeof(g_np.peer_hostport), "%s",
             cfg->peer_hostport);
    g_np.lobby_server[0] = '\0';
    g_np.lobby_id[0] = '\0';
#if defined(PSX_HAS_LOBBY_CLIENT)
    if (use_ice && psx_lobby_connected() && psx_lobby_in_lobby()) {
        const PsxLobbyJoinInfo *ji = psx_lobby_join_info();
        snprintf(g_np.match_mode, sizeof(g_np.match_mode), "hosted_lobby");
        snprintf(g_np.lobby_server, sizeof(g_np.lobby_server), "%s",
                 psx_lobby_default_url());
        if (ji && ji->lobby_id[0])
            snprintf(g_np.lobby_id, sizeof(g_np.lobby_id), "%s", ji->lobby_id);
        g_np.is_host = psx_lobby_is_host() ? 1 : 0;
    } else
#endif
    {
        snprintf(g_np.match_mode, sizeof(g_np.match_mode), "direct_ip");
    }

#if defined(__linux__)
    if (!use_ice && peer_is_loopback(cfg->peer_hostport))
        pin_localhost_peer_cpus(g_np.local_slot);
#endif

    psx_netplay_release_pads();
    fprintf(stderr,
            "psx_netplay: started transport=%s slot=%d input_player=%d session=%u "
            "delay=%u force_input_relay=%d force_turn=%d bind=%s peer=%s\n",
            use_ice ? "ice" : "lan", g_np.local_slot, g_np.input_player,
            (unsigned)rcfg.session_id, (unsigned)rcfg.input_delay,
            g_np.force_input_relay, cfg->force_turn ? 1 : 0, cfg->bind_hostport,
            use_ice ? "(ice)" : cfg->peer_hostport);
    return 0;
}

void psx_netplay_bind_guest_saves(void)
{
    if (!psx_netplay_active() || g_np.local_slot == 0 || g_np.guest_sandbox)
        return;
    np_enter_guest_sandbox();
}

/* Delay-sync starvation hold (lockstep-safe; mirrors snes_host_barrier_admit). */
#define PSX_STARVATION_ENTER_DEFAULT 4
#define PSX_STARVATION_EXIT_DEFAULT 3
#define PSX_STARVATION_EXIT_HR_LEAD_DEFAULT 0
#define PSX_STARVATION_GRACE_TICKS 60
/* Default 0: after starvation clears, resume ~1 sim/wall frame and let
 * remote_lead rebuild toward D instead of a turbo recovery burst.
 * Override: PSX_NET_STARVATION_RECOVERY_BURST / PSX_NET_CATCHUP_CAP. */
#define PSX_STARVATION_RECOVERY_BURST_DEFAULT 0
#define PSX_CATCHUP_CAP_DEFAULT 0

static struct {
    int latched;
    int enter_run;
    int exit_run;
    int recovery_amount;
    int latch_logged;
    int just_cleared;
} g_starv;

/* Defined below poll_admit; used by the starvation runway check. */
int psx_netplay_remote_lead(void);
int psx_netplay_input_delay(void);

static int np_starv_env_int(const char *name, int def)
{
    const char *v = getenv(name);
    long n;
    char *end;
    if (!v || !v[0])
        return def;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 0 || n > 64)
        return def;
    return (int)n;
}

static void np_starv_reset(void)
{
    memset(&g_starv, 0, sizeof(g_starv));
}

static int np_starv_runway_ok(void)
{
    int lead = psx_netplay_remote_lead();
    int delay = psx_netplay_input_delay();
    int hr_lead = np_starv_env_int("PSX_NET_STARVATION_EXIT_HR_LEAD",
                                   PSX_STARVATION_EXIT_HR_LEAD_DEFAULT);
    if (delay < 0)
        delay = 0;
    return lead >= delay + hr_lead;
}

void psx_netplay_shutdown(void)
{
    if (g_diag_file) {
        fclose(g_diag_file);
        g_diag_file = NULL;
    }
    g_diag_file_session = 0;
    g_diag_summary_written = 0;
    g_diag_last_write_ms = 0;
    if (g_np.session) {
        (void)rnet_session_send_bye(g_np.session);
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
    }
    np_leave_guest_sandbox();
    memset(&g_np, 0, sizeof(g_np));
    np_starv_reset();
}

int psx_netplay_is_host(void)
{
    return psx_netplay_active() && g_np.local_slot == 0;
}

int psx_netplay_request_save(int slot)
{
    uint32_t sim;
    uint32_t delay;
    uint32_t target;
    if (!psx_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0)
        return 1; /* guest: host-only; ignore */
    if (np_xfer_busy() || !g_np.mc_sync_done)
        return 1;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;

    /* Agree a future sim_tick so TURN/coord latency cannot make the host
     * write tick T while the guest still writes T+k (CRC miss → transfer).
     * crc field of size==0 probe carries the target tick. */
    sim = rnet_session_sim_tick(g_np.session);
    delay = (uint32_t)psx_netplay_input_delay();
    if (delay < 1u) delay = 1u;
    target = sim + delay + 2u;
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)slot, 0,
                                 target) != 0)
        return 1;
    g_np.xfer = NP_XFER_SAVE_COORD;
    g_np.xfer_slot = slot;
    g_np.save_target_tick = target;
    g_np.local_save_staged = 0;
    g_np.local_save_acked = 0;
    printf("psxrecomp: netplay save slot=%d — coordinating local writes "
           "(target sim=%u, now=%u)…\n",
           slot, (unsigned)target, (unsigned)sim);
    fflush(stdout);
    return 1;
}

int psx_netplay_request_load(int slot)
{
    uint32_t size = 0, crc = 0;
    char reason[192];
    if (!psx_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0)
        return 1;
    if (np_xfer_busy() || !g_np.mc_sync_done)
        return 1;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;
    if (!savestate_slot_compatible(slot, reason, sizeof(reason))) {
        printf("psxrecomp: netplay load slot=%d refused — %s "
               "(resave with this build: Shift+F%d)\n",
               slot, reason[0] ? reason : "incompatible", slot + 1);
        fflush(stdout);
        return 1;
    }
    if (!np_slot_crc(slot, &size, &crc))
        return 1;
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)slot, size, crc) != 0)
        return 1;
    g_np.xfer = NP_XFER_LOAD_PROBE;
    g_np.xfer_slot = slot;
    g_np.load_applied_local = 0;
    g_np.load_apply_failed = 0;
    printf("psxrecomp: netplay load slot=%d — hash probe (%u bytes)\n", slot, (unsigned)size);
    fflush(stdout);
    return 1;
}

int psx_netplay_in_load_barrier(void)
{
    if (!psx_netplay_active())
        return 0;
    /* Any save/load/memcard sync phase — TURN chunk xfers of ~1.4MB need the
     * 90s budget (20s admit stall was killing SAVE mid-transfer). */
    return (g_np.xfer != NP_XFER_NONE) ? 1 : 0;
}

int psx_netplay_consume_load_apply_failed(void)
{
    int v = g_np.load_apply_failed;
    g_np.load_apply_failed = 0;
    return v;
}


static int np_diag_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("PSX_NET_DIAG");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* Verbose delay-sync starvation latch/clear spam. Off by default — the latch
 * can toggle every few frames under jitter and floods stderr. Enable with
 * PSX_NET_DELAY_SYNC_DIAG=1 (alias: PSX_NET_STARVATION_DIAG=1). */
static int np_delay_sync_diag_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("PSX_NET_DELAY_SYNC_DIAG");
        if (!v || !v[0])
            v = getenv("PSX_NET_STARVATION_DIAG");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

static unsigned np_diag_interval_ms(void)
{
    static unsigned cached = 0;
    unsigned hz;
    if (cached)
        return cached;
    hz = env_u("PSX_NET_DIAG_HZ", 2);
    if (hz < 1) hz = 1;
    if (hz > 30) hz = 30;
    cached = 1000u / hz;
    if (cached < 1) cached = 1;
    return cached;
}

static void np_diag_escape(char *out, size_t out_len, const char *in)
{
    size_t oi = 0;
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;
    for (; *in && oi + 2 < out_len; ++in) {
        char c = *in;
        if (c == '"' || c == '\\') {
            if (oi + 3 >= out_len)
                break;
            out[oi++] = '\\';
            out[oi++] = c;
        } else if ((unsigned char)c < 0x20) {
            /* skip */
        } else {
            out[oi++] = c;
        }
    }
    out[oi] = '\0';
}

static const char *np_diag_ice_path(const RNetSessionStats *st)
{
    if (!g_np.use_ice)
        return "lan";
    if (!st)
        return "pending";
    if (st->ice_state == RNET_ICE_STATE_FAILED)
        return "failed";
    if (st->ice_path[0])
        return st->ice_path;
    if (st->ice_state == RNET_ICE_STATE_COMPLETED ||
        st->ice_state == RNET_ICE_STATE_CONNECTED)
        return "unknown";
    return "pending";
}

static const char *np_diag_ice_nat(const char *path)
{
    if (!g_np.use_ice)
        return "lan";
    if (!path || !path[0] || strcmp(path, "pending") == 0)
        return "pending";
    if (strcmp(path, "failed") == 0)
        return "failed";
    if (strcmp(path, "relay") == 0)
        return "turn";
    if (strcmp(path, "srflx") == 0 || strcmp(path, "prflx") == 0)
        return "stun";
    if (strcmp(path, "host") == 0)
        return "host";
    return "unknown";
}

static int np_diag_path_ready(const RNetSessionStats *st)
{
    if (!g_np.use_ice)
        return 1;
    if (!st)
        return 0;
    if (st->ice_state == RNET_ICE_STATE_FAILED)
        return 1;
    if (st->ice_path[0] && strcmp(st->ice_path, "pending") != 0 &&
        strcmp(st->ice_path, "unknown") != 0)
        return 1;
    if (st->ice_state == RNET_ICE_STATE_COMPLETED ||
        st->ice_state == RNET_ICE_STATE_CONNECTED)
        return 1;
    return 0;
}

static void np_diag_write_summary(FILE *f, const RNetSessionStats *st, uint32_t now)
{
    char server_esc[280];
    char lobby_esc[80];
    char bind_esc[80];
    char peer_esc[80];
    char stun_esc[140];
    char turn_esc[140];
    char ice_local_esc[120];
    char ice_remote_esc[120];
    const char *path = np_diag_ice_path(st);
    const char *nat = np_diag_ice_nat(path);
    const char *ice_state =
        st ? rnet_ice_state_name(st->ice_state) : "idle";

    np_diag_escape(server_esc, sizeof(server_esc), g_np.lobby_server);
    np_diag_escape(lobby_esc, sizeof(lobby_esc), g_np.lobby_id);
    np_diag_escape(bind_esc, sizeof(bind_esc), g_np.bind_hostport);
    np_diag_escape(peer_esc, sizeof(peer_esc), g_np.peer_hostport);
    np_diag_escape(stun_esc, sizeof(stun_esc), g_np.ice_stun_host);
    np_diag_escape(turn_esc, sizeof(turn_esc), g_np.ice_turn_host);
    np_diag_escape(ice_local_esc, sizeof(ice_local_esc),
                   st ? st->ice_local : "");
    np_diag_escape(ice_remote_esc, sizeof(ice_remote_esc),
                   st ? st->ice_remote : "");

    fprintf(f,
            "{\"type\":\"summary\",\"t_ms\":%u,\"match\":\"%s\","
            "\"lobby_server\":\"%s\",\"lobby_id\":\"%s\",\"is_host\":%d,"
            "\"slot\":%d,\"session_id\":%u,\"input_delay\":%d,"
            "\"force_input_relay\":%d,"
            "\"transport\":\"%s\",\"bind\":\"%s\",\"peer\":\"%s\","
            "\"turn_configured\":%d,\"stun_host\":\"%s\",\"stun_port\":%u,"
            "\"turn_host\":\"%s\",\"turn_port\":%u,\"ice_state\":\"%s\","
            "\"ice_path\":\"%s\",\"ice_nat\":\"%s\","
            "\"ice_local\":\"%s\",\"ice_remote\":\"%s\"}\n",
            (unsigned)now, g_np.match_mode[0] ? g_np.match_mode : "unknown",
            server_esc, lobby_esc, g_np.is_host, g_np.local_slot,
            (unsigned)g_np.session_id, g_np.input_delay, g_np.force_input_relay,
            g_np.use_ice ? "ice" : "lan", bind_esc, peer_esc,
            g_np.ice_has_turn ? 1 : 0, stun_esc, g_np.ice_stun_port, turn_esc,
            g_np.ice_turn_port, ice_state ? ice_state : "idle", path, nat,
            ice_local_esc, ice_remote_esc);
}

void psx_netplay_diag_tick(void)
{
    RNetSessionStats st;
    uint32_t now;
    const char *transport;
    const char *ice_state;
    const char *path;

    if (!np_diag_enabled() || !psx_netplay_active() || !g_np.session)
        return;

    rnet_session_get_stats(g_np.session, &st);

    if (!g_diag_summary_written && !np_diag_path_ready(&st))
        return;

    now = np_mono_ms();
    if (g_diag_last_write_ms &&
        (uint32_t)(now - g_diag_last_write_ms) < np_diag_interval_ms() &&
        g_diag_summary_written)
        return;
    g_diag_last_write_ms = now ? now : 1u;

    if (!g_diag_mkdir_done) {
        g_diag_mkdir_done = 1;
#ifdef _WIN32
        _mkdir("saves");
        _mkdir("saves\\netplay");
#else
        mkdir("saves", 0755);
        mkdir("saves/netplay", 0755);
#endif
    }

    if (!g_diag_file || g_diag_file_session != g_np.diag_session) {
        char pathbuf[64];
        if (g_diag_file) {
            fclose(g_diag_file);
            g_diag_file = NULL;
        }
        snprintf(pathbuf, sizeof(pathbuf), "saves/netplay/net_diag.jsonl");
        g_diag_file = fopen(pathbuf, "wb");
        if (!g_diag_file)
            return;
        setvbuf(g_diag_file, NULL, _IOLBF, 0);
        g_diag_file_session = g_np.diag_session;
        g_diag_summary_written = 0;
        fprintf(stderr, "psx_netplay: diag writing %s "
                        "(PSX_NET_DIAG_HZ interval %ums)\n",
                pathbuf, np_diag_interval_ms());
    }

    if (!g_diag_summary_written) {
        np_diag_write_summary(g_diag_file, &st, now);
        g_diag_summary_written = 1;
    }

    {
        char ice_local_esc[120];
        char ice_remote_esc[120];
        const char *stall = rnet_admit_stall_name(st.last_stall);
        int using_turn_path = (strcmp(np_diag_ice_path(&st), "relay") == 0) ? 1 : 0;

        transport = psx_netplay_transport_name();
        ice_state = rnet_ice_state_name(st.ice_state);
        path = np_diag_ice_path(&st);
        np_diag_escape(ice_local_esc, sizeof(ice_local_esc), st.ice_local);
        np_diag_escape(ice_remote_esc, sizeof(ice_remote_esc), st.ice_remote);

        fprintf(g_diag_file,
                "{\"t_ms\":%u,\"slot\":%d,\"transport\":\"%s\",\"ice_state\":\"%s\","
                "\"ice_path\":\"%s\",\"ice_nat\":\"%s\",\"turn\":%d,"
                "\"ice_local\":\"%s\",\"ice_remote\":\"%s\","
                "\"running\":%d,\"sim_tick\":%u,\"frames_finished\":%u,"
                "\"delay\":%u,\"stall\":\"%s\","
                "\"stall_ms\":%u,\"stall_max_ms\":%u,\"stall_streaks\":%u,"
                "\"consec_stalls\":%u,\"admit_ok\":%u,\"remote_lead\":%d,"
                "\"remote_wire\":%u,\"peer_rx_age_ms\":%llu,\"peer_gone\":%d,"
                "\"desync\":%d,\"desync_tick\":%u,\"state_busy\":%d,\"state_op\":%u,"
                "\"pkts_rx\":%u,\"input_sends\":%u}\n",
                (unsigned)now, g_np.local_slot, transport ? transport : "none",
                ice_state ? ice_state : "idle", path, np_diag_ice_nat(path),
                using_turn_path, ice_local_esc, ice_remote_esc, st.is_running,
                (unsigned)st.sim_tick, (unsigned)g_np.frames_finished,
                (unsigned)st.delay, stall ? stall : "unknown",
                (unsigned)st.last_admit_wait_ms, (unsigned)st.max_admit_wait_ms,
                (unsigned)st.stall_streaks, (unsigned)st.consecutive_stalls,
                (unsigned)st.admit_ok_count, st.remote_lead,
                (unsigned)st.highest_remote_wire,
                (unsigned long long)st.last_peer_rx_age_ms, st.peer_gone,
                st.input_desync, (unsigned)st.desync_tick, st.state_busy,
                (unsigned)st.state_op, (unsigned)st.packets_rx,
                (unsigned)st.input_bundle_sends);
    }
}

/* Stage the coord save once sim_tick reaches the agreed target. */
static void np_maybe_stage_target_save(void)
{
    uint32_t sim;
    if (g_np.xfer != NP_XFER_SAVE_COORD || g_np.local_save_staged)
        return;
    if (!g_np.session || !rnet_session_is_running(g_np.session))
        return;
    sim = rnet_session_sim_tick(g_np.session);
    if (sim < g_np.save_target_tick)
        return;
    if (!savestate_request_save_protocol(g_np.xfer_slot))
        return;
    g_np.local_save_staged = 1;
    printf("psxrecomp: netplay %s save slot=%d — staging @ sim=%u (target=%u)\n",
           g_np.local_slot == 0 ? "host" : "guest", g_np.xfer_slot,
           (unsigned)sim, (unsigned)g_np.save_target_tick);
    fflush(stdout);
}

static void np_pump_session(void)
{
#if defined(PSX_HAS_LOBBY_CLIENT)
    if (g_np.use_ice || psx_lobby_connected())
        psx_lobby_pump();
#endif
    drain_lobby_signals();
    rnet_session_pump(g_np.session);
    np_guest_handle_probe();
    np_maybe_stage_target_save();
    np_apply_ready_state();
    np_drive_load_barrier();
    np_host_drive_xfer();
    if (rnet_session_is_running(g_np.session))
        np_maybe_start_mc_sync();
}

void psx_netplay_pump(void)
{
    if (!psx_netplay_active())
        return;
    np_pump_session();
    psx_netplay_diag_tick();
}

static int np_try_admit_gameplay(void)
{
    rnet_u32 sim = rnet_session_sim_tick(g_np.session);
    if (rnet_session_try_admit(g_np.session, sim)) {
        g_np.needs_advance = 1;
        return 1;
    }
    force_session_pads_connected(g_np.slot_count);
    return 0;
}

int psx_netplay_poll_admit(void)
{
    rnet_u32 sim;
    int enter_need;
    int exit_need;

    if (!psx_netplay_active()) return 1;

    np_pump_session();

    if (!rnet_session_is_running(g_np.session)) {
        psx_netplay_release_pads();
        np_starv_reset();
        psx_netplay_diag_tick();
        return 0;
    }

    /* Both peers stall until initial memcard hash-agree / transfer finishes. */
    if (!g_np.mc_sync_done)
        return 0;

    /* Post-load: allow admit only while savestate_poll still needs guest
     * cycles. After restore (or during ready rendezvous) freeze the sim clock
     * so peers cannot drift before hard_resync + prime. */
    if (g_np.xfer == NP_XFER_LOAD_APPLYING && !savestate_pending())
        return 0;

    /* Staged load must run guest cycles — bypass starvation latch. ICE xfer
     * often leaves lead=D-1 and would otherwise block try_admit forever. */
    if (g_np.xfer == NP_XFER_LOAD_APPLYING && savestate_pending()) {
        if (g_np.needs_advance)
            return 1;
        return np_try_admit_gameplay();
    }

    /* Both peers: after mutual ready + sync, stay in LOAD_READY until try_admit
     * succeeds (fresh tip exchange + INPUT_CONFIRM). Dropping the barrier early
     * on the host let it spin on confirm with FPS/present already "live". */
    if (g_np.xfer == NP_XFER_LOAD_READY) {
        if (g_np.load_sync_done && g_np.load_ready_replied && !g_np.needs_advance) {
            sim = rnet_session_sim_tick(g_np.session);
            if (rnet_session_try_admit(g_np.session, sim)) {
                g_np.xfer = NP_XFER_NONE;
                g_np.load_applied_local = 0;
                g_np.load_ready_replied = 0;
                g_np.load_sync_done = 0;
                g_np.needs_advance = 1;
                printf("psxrecomp: netplay load slot=%d — peer ready, resuming lockstep\n",
                       g_np.xfer_slot);
                fflush(stdout);
                return 1;
            }
            force_session_pads_connected(g_np.slot_count);
        }
        return 0;
    }

    /* Already published this tick and waiting for finish_frame — do not
     * re-admit / re-sample (would desync the delay rings). */
    if (g_np.needs_advance) return 1;

    sim = rnet_session_sim_tick(g_np.session);
    enter_need = np_starv_env_int("PSX_NET_STARVATION_ENTER_FRAMES",
                                  PSX_STARVATION_ENTER_DEFAULT);
    exit_need = np_starv_env_int("PSX_NET_STARVATION_EXIT_FRAMES",
                                 PSX_STARVATION_EXIT_DEFAULT);

    /* SAVE coord: run admit until both reach save_target_tick and flush the
     * staged write. After the local .pst exists, freeze (host also freezes
     * unless the guest tip is behind and still needs catch-up admits). */
    if (g_np.xfer == NP_XFER_SAVE_COORD) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        np_maybe_stage_target_save();
        if (!g_np.local_save_staged || savestate_pending())
            return np_try_admit_gameplay();
        if (g_np.local_slot != 0)
            return 0; /* guest saved — wait for hash probe */
        /* Host saved: freeze for same-tick match. If guest is still behind
         * the target, keep admitting so it can catch up and write. */
        if (psx_netplay_remote_lead() < 0)
            return np_try_admit_gameplay();
        return 0;
    }

    if (g_np.xfer == NP_XFER_LOAD_PROBE || g_np.xfer == NP_XFER_LOAD_SEND ||
        g_np.xfer == NP_XFER_SAVE_PROBE || g_np.xfer == NP_XFER_SAVE_SEND ||
        g_np.xfer == NP_XFER_MC_PROBE || g_np.xfer == NP_XFER_MC_SEND) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        return np_try_admit_gameplay();
    }

    /* Startup grace: do not latch before the delay rings warm up. */
    if (sim < (rnet_u32)PSX_STARVATION_GRACE_TICKS) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        return np_try_admit_gameplay();
    }

    if (g_starv.latched) {
        /* Pump already ran; hold try_admit until remote tip refills. */
        if (np_starv_runway_ok()) {
            g_starv.exit_run++;
            if (g_starv.exit_run >= exit_need) {
                g_starv.latched = 0;
                g_starv.exit_run = 0;
                g_starv.latch_logged = 0;
                g_starv.just_cleared = 1;
            } else {
                return 0;
            }
        } else {
            g_starv.exit_run = 0;
            return 0;
        }
    }

    if (np_try_admit_gameplay()) {
        g_starv.enter_run = 0;
        if (g_starv.just_cleared) {
            int burst = np_starv_env_int("PSX_NET_STARVATION_RECOVERY_BURST",
                                         PSX_STARVATION_RECOVERY_BURST_DEFAULT);
            g_starv.just_cleared = 0;
            g_starv.recovery_amount = burst;
            if (np_delay_sync_diag_enabled()) {
                if (burst > 0) {
                    fprintf(stderr,
                            "psxrecomp: delay_sync_starvation cleared sim=%u lead=%d "
                            "D=%d — recovery burst %d\n",
                            (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                            psx_netplay_input_delay(), burst);
                } else {
                    fprintf(stderr,
                            "psxrecomp: delay_sync_starvation cleared sim=%u lead=%d "
                            "D=%d — resume 1:1 (rebuild input buffer)\n",
                            (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                            psx_netplay_input_delay());
                }
            }
        }
        return 1;
    }

    g_starv.just_cleared = 0;
    g_starv.enter_run++;
    if (g_starv.enter_run >= enter_need) {
        g_starv.latched = 1;
        g_starv.enter_run = 0;
        if (!g_starv.latch_logged) {
            if (np_delay_sync_diag_enabled()) {
                fprintf(stderr,
                        "psxrecomp: delay_sync_starvation latched sim=%u lead=%d "
                        "D=%d (enter=%d)\n",
                        (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                        psx_netplay_input_delay(), enter_need);
            }
            g_starv.latch_logged = 1;
        }
    }
    return 0;
}

void psx_netplay_finish_frame(void)
{
    if (!psx_netplay_active()) return;
    if (!g_np.needs_advance) return;
    rnet_session_advance(g_np.session);
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.frames_finished++;
}

int psx_netplay_remote_lead(void)
{
    RNetSessionStats st;
    if (!psx_netplay_active())
        return 0;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    return st.remote_lead;
}

int psx_netplay_input_delay(void)
{
    RNetSessionStats st;
    if (!psx_netplay_active())
        return 2;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    return st.delay > 0 ? (int)st.delay : 2;
}

int psx_netplay_catchup_budget(void)
{
    int lead;
    int delay;
    int extra;
    int budget;
    int cap;

    if (!psx_netplay_active())
        return 0;
    cap = np_starv_env_int("PSX_NET_CATCHUP_CAP", PSX_CATCHUP_CAP_DEFAULT);
    if (cap <= 0 && g_starv.recovery_amount <= 0)
        return 0;
    lead = psx_netplay_remote_lead();
    delay = psx_netplay_input_delay();
    if (delay < 0)
        delay = 0;
    /* Only spend surplus above D; keep the delay runway intact. */
    extra = lead - delay;
    if (extra < 0)
        extra = 0;
    budget = extra;
    if (g_starv.recovery_amount > budget)
        budget = g_starv.recovery_amount;
    if (budget > cap)
        budget = cap;
    return budget;
}

void psx_netplay_catchup_consume_frame(void)
{
    if (g_starv.recovery_amount > 0)
        g_starv.recovery_amount--;
}

void psx_netplay_wait_recv(int timeout_ms)
{
    if (!psx_netplay_active()) return;
    (void)rnet_session_wait_recv(g_np.session, timeout_ms);
}

void psx_netplay_admit_wait_info(char *stall_out, size_t stall_cap,
                                 uint32_t *sim_tick_out, int *lead_out)
{
    RNetSessionStats st;
    const char *name = "inactive";
    char phase[96];
    memset(&st, 0, sizeof(st));
    phase[0] = '\0';
    if (psx_netplay_active() && g_np.session) {
        rnet_session_get_stats(g_np.session, &st);
        name = rnet_admit_stall_name(st.last_stall);
        if (!name || !name[0])
            name = "unknown";
        /* LOAD_READY never calls try_admit, so last_stall stays "ok" — surface
         * the app barrier phase (+ transfer progress) instead. */
        switch (g_np.xfer) {
        case NP_XFER_SAVE_COORD:
            snprintf(phase, sizeof(phase), "save_coord");
            break;
        case NP_XFER_SAVE_PROBE:
            snprintf(phase, sizeof(phase), "save_probe");
            break;
        case NP_XFER_SAVE_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "save_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "save_xfer");
            break;
        case NP_XFER_MC_PROBE:
            snprintf(phase, sizeof(phase), "mc_probe");
            break;
        case NP_XFER_MC_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "mc_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "mc_xfer");
            break;
        case NP_XFER_LOAD_PROBE:
            snprintf(phase, sizeof(phase), "load_probe");
            break;
        case NP_XFER_LOAD_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "load_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "load_xfer");
            break;
        case NP_XFER_LOAD_APPLYING:
            if (savestate_pending()) {
                if (g_starv.latched)
                    snprintf(phase, sizeof(phase), "load_applying+starv_%s", name);
                else
                    snprintf(phase, sizeof(phase), "load_applying+%s", name);
            } else {
                snprintf(phase, sizeof(phase), "load_apply_done+%s", name);
            }
            break;
        case NP_XFER_LOAD_READY:
            if (g_np.load_ready_replied)
                snprintf(phase, sizeof(phase), "load_ready_admit+%s", name);
            else if (g_np.load_applied_local)
                snprintf(phase, sizeof(phase), "load_ready_wait_peer+%s", name);
            else
                snprintf(phase, sizeof(phase), "load_ready+%s", name);
            break;
        default:
            break;
        }
    }
    if (stall_out && stall_cap) {
        if (phase[0])
            snprintf(stall_out, stall_cap, "%s", phase);
        else {
            strncpy(stall_out, name, stall_cap - 1);
            stall_out[stall_cap - 1] = '\0';
        }
    }
    if (sim_tick_out)
        *sim_tick_out = st.sim_tick;
    if (lead_out)
        *lead_out = st.remote_lead;
}

#endif /* PSX_HAS_RECOMP_NET */
