/* savestate.c — user save states (Shift+F1-F12 save, F1-F12 load). See savestate.h.
 *
 * Wraps boot_state.c's full-machine serializer. Requests are staged by the SDL
 * key handler / debug server and executed by savestate_poll at a block-leader
 * boundary (in_exception == 0), where cpu->pc is a valid resume PC. A load
 * restores the full machine then unwinds to the scheduler and re-dispatches. */

#include "savestate.h"
#include "boot_state.h"
#include "cdrom.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_scheduler.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#endif

static double savestate_mono_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

static char     s_dir[512];
static uint32_t s_bios_checksum;
static uint32_t s_entry_pc;
static int      s_configured   = 0;
static int      s_save_pending = -1;   /* slot, or -1 */
static int      s_load_pending = -1;
static int      s_load_completed = 0;
static int      s_load_failed = 0;
static uint8_t *s_load_blob = NULL;   /* optional in-memory .pst for netplay */
static size_t   s_load_blob_len = 0;

extern int psx_hle_scheduler_enabled(void);

/* Create each path component (mkdir -p). Single-level mkdir fails for
 * "saves/netplay" when parent "saves" is missing. */
static void ensure_dir(const char* dir) {
    char tmp[512];
    size_t len;
    size_t i;
    if (!dir || !dir[0]) return;
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[--len] = '\0';
    }
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
#ifdef _WIN32
            /* Keep drive prefix "C:" intact — do not mkdir("C:"). */
            if (i == 2 && tmp[1] == ':')
                continue;
#endif
            tmp[i] = '\0';
#ifdef _WIN32
            (void)_mkdir(tmp);
#else
            (void)mkdir(tmp, 0755);
#endif
            tmp[i] = '/';
        }
    }
#ifdef _WIN32
    (void)_mkdir(tmp);
#else
    (void)mkdir(tmp, 0755);
#endif
}

static void clear_load_blob(void) {
    free(s_load_blob);
    s_load_blob = NULL;
    s_load_blob_len = 0;
}

void savestate_configure(const char* dir, uint32_t bios_checksum, uint32_t entry_pc) {
    if (dir && dir[0]) {
        strncpy(s_dir, dir, sizeof(s_dir) - 1);
        s_dir[sizeof(s_dir) - 1] = '\0';
        ensure_dir(s_dir);
    } else {
        s_dir[0] = '\0';
    }
    s_bios_checksum = bios_checksum;
    s_entry_pc      = entry_pc;
    s_configured    = 1;
}

const char* savestate_dir(void) {
    return s_dir;
}

void savestate_get_integrity(uint32_t* bios_checksum, uint32_t* entry_pc) {
    if (bios_checksum) *bios_checksum = s_bios_checksum;
    if (entry_pc) *entry_pc = s_entry_pc;
}

int savestate_slot_path(int slot, char* out, size_t cap) {
    if (!s_configured || !out || cap == 0) return 0;
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    /* Keyed by entry_pc so slots from different games in a shared dir never
     * collide; boot_state_load also rejects a mismatched entry_pc internally. */
    snprintf(out, cap, "%s%sstate_%08X_slot%02d.pst",
             s_dir, (s_dir[0] ? "/" : ""), (unsigned)s_entry_pc, slot);
    return 1;
}

int savestate_slot_exists(int slot) {
    char path[600];
    FILE* f;
    long sz;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    fclose(f);
    return sz > 0;
}

int savestate_slot_compatible(int slot, char* reason, size_t reason_cap) {
    uint8_t* data = NULL;
    size_t size = 0;
    int ok;
    if (reason && reason_cap)
        reason[0] = '\0';
    if (!s_configured) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "not_configured");
        return 0;
    }
    if (!savestate_read_slot(slot, &data, &size) || !data) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "missing");
        return 0;
    }
    ok = boot_state_check_buffer(data, size, s_bios_checksum, s_entry_pc,
                                 reason, reason_cap);
    free(data);
    return ok;
}

int savestate_read_slot(int slot, uint8_t** data_out, size_t* size_out) {
    char path[600];
    FILE* f;
    long sz;
    uint8_t* buf;
    if (!data_out || !size_out) return 0;
    *data_out = NULL;
    *size_out = 0;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz <= 0 || (size_t)sz > 8u * 1024u * 1024u) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *data_out = buf;
    *size_out = (size_t)sz;
    return 1;
}

int savestate_write_slot(int slot, const void* data, size_t size) {
    char path[600];
    FILE* f;
    size_t wrote;
    if (!data || size == 0) return 0;
    if (!savestate_slot_path(slot, path, sizeof(path))) {
        fprintf(stderr,
                "savestate: write_slot=%d failed (not configured / bad slot) "
                "dir='%s' configured=%d\n",
                slot, s_dir, s_configured);
        return 0;
    }
    ensure_dir(s_dir);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "savestate: write_slot fopen('%s') failed: %s\n",
                path, strerror(errno));
        return 0;
    }
    wrote = fwrite(data, 1, size, f);
    if (wrote != size) {
        fprintf(stderr,
                "savestate: write_slot fwrite('%s') %zu/%zu failed: %s\n",
                path, wrote, size, strerror(errno));
        fclose(f);
        remove(path);
        return 0;
    }
    if (fflush(f) != 0 || fclose(f) != 0) {
        fprintf(stderr, "savestate: write_slot flush/close('%s') failed: %s\n",
                path, strerror(errno));
        remove(path);
        return 0;
    }
    return 1;
}

/* User APIs during netplay: guests cannot initiate; host must use
 * psx_netplay_request_* so peers hash-probe and sync over STATE_*. */
static int netplay_user_blocked(void) {
    if (!psx_netplay_active()) return 0;
    if (!psx_netplay_is_host()) {
        fprintf(stderr, "savestate: netplay guest cannot save/load (host-only)\n");
        return 1;
    }
    fprintf(stderr,
            "savestate: during netplay use host Shift+F / F (synced path)\n");
    return 1;
}

static int request_save_inner(int slot) {
    if (!s_configured) { fprintf(stderr, "savestate: not configured\n"); return 0; }
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    s_save_pending = slot;
    return 1;
}

static int request_load_inner(int slot) {
    if (!s_configured) { fprintf(stderr, "savestate: not configured\n"); return 0; }
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    if (!psx_hle_scheduler_enabled()) {
        /* LLE (host-fiber) mode: the restore longjmp target lives on the
         * scheduler fiber; cross-fiber unwind is unsafe. HLE is the default. */
        fprintf(stderr, "savestate: load requires the HLE scheduler (default); "
                        "PSX_HLE_SCHEDULER=0 run cannot load states.\n");
        return 0;
    }
    s_load_failed = 0;
    s_load_completed = 0;
    s_load_pending = slot;
    return 1;
}

int savestate_request_save(int slot) {
    if (netplay_user_blocked()) return 0;
    return request_save_inner(slot);
}

int savestate_request_load(int slot) {
    if (netplay_user_blocked()) return 0;
    return request_load_inner(slot);
}

int savestate_request_save_protocol(int slot) {
    /* Follow-host sync: guests must write the host-authoritative .pst. */
    return request_save_inner(slot);
}

int savestate_request_load_protocol(int slot) {
    /* Follow-host sync: guests must apply the host-authoritative .pst. */
    clear_load_blob();
    return request_load_inner(slot);
}

int savestate_request_load_blob_protocol(const void* data, size_t size) {
    uint8_t* copy;
    if (!s_configured) {
        fprintf(stderr, "savestate: load_blob — not configured\n");
        return 0;
    }
    if (!data || size == 0 || size > 64u * 1024u * 1024u)
        return 0;
    if (!psx_hle_scheduler_enabled()) {
        fprintf(stderr, "savestate: load_blob requires the HLE scheduler\n");
        return 0;
    }
    copy = (uint8_t*)malloc(size);
    if (!copy) {
        fprintf(stderr, "savestate: load_blob malloc(%zu) failed\n", size);
        return 0;
    }
    memcpy(copy, data, size);
    clear_load_blob();
    s_load_blob = copy;
    s_load_blob_len = size;
    s_load_failed = 0;
    s_load_completed = 0;
    s_load_pending = 0; /* non-negative: poll will prefer the blob */
    return 1;
}

int savestate_pending(void) {
    return (s_save_pending >= 0 || s_load_pending >= 0) ? 1 : 0;
}

int savestate_take_load_completed(void) {
    int v = s_load_completed;
    s_load_completed = 0;
    return v;
}

int savestate_take_load_failed(void) {
    int v = s_load_failed;
    s_load_failed = 0;
    return v;
}

void savestate_poll(CPUState* cpu, uint32_t resume_pc) {
    if (s_save_pending < 0 && s_load_pending < 0) return;   /* hot path: nothing staged */

    if (s_save_pending >= 0) {
        int slot = s_save_pending;
        s_save_pending = -1;
        char path[600];
        if (savestate_slot_path(slot, path, sizeof(path))) {
            /* Save the exact resume PC (cpu->pc is 0 mid-block; resume_pc is the
             * block leader the interrupt path would resume at). */
            CPUState snap = *cpu;
            snap.pc = resume_pc;
            int ok = boot_state_save(&snap, s_bios_checksum, s_entry_pc, path);
            fprintf(stderr, "savestate: %s slot %d @ pc=0x%08X -> %s\n",
                    ok ? "SAVED" : "SAVE FAILED", slot, (unsigned)resume_pc, path);
        }
    }

    if (s_load_pending >= 0) {
        int slot = s_load_pending;
        int loaded = 0;
        s_load_pending = -1;
        char path[600];
        const double t_load0 = savestate_mono_ms();
        double t_after_boot = t_load0;
        double t_after_frontend = t_load0;
        path[0] = '\0';
        if (s_load_blob && s_load_blob_len > 0) {
            const size_t blob_len = s_load_blob_len;
            loaded = boot_state_load_buffer(s_load_blob, blob_len,
                                            s_bios_checksum, s_entry_pc, cpu);
            clear_load_blob();
            if (!loaded) {
                fprintf(stderr,
                        "savestate: LOAD FAILED blob (%zu bytes, entry=%08X)\n",
                        blob_len, (unsigned)s_entry_pc);
                s_load_failed = 1;
            }
        } else if (savestate_slot_path(slot, path, sizeof(path))) {
            loaded = boot_state_load(path, s_bios_checksum, s_entry_pc, cpu);
            if (!loaded) {
                fprintf(stderr,
                        "savestate: LOAD FAILED slot %d %s\n",
                        slot, path);
                s_load_failed = 1;
            }
        } else {
            fprintf(stderr, "savestate: LOAD FAILED slot %d (no path)\n", slot);
            s_load_failed = 1;
        }
        if (loaded) {
            t_after_boot = savestate_mono_ms();
            psx_cycles_resync_after_restore(cpu);
            /* Drop absolute-cycle IRQ cooldowns / VBlank phase from the
             * pre-load host timeline (cycle rewind would otherwise blackout
             * VBlank delivery for however long the user played past the save). */
            interrupts_resync_after_restore();
            /* Collapse restored / imminent CD second-response debt (ReadTOC,
             * Init, seeks) so the picture does not freeze for ~1s after the
             * restored frame presents. */
            cdrom_accelerate_after_savestate();
            /* Netplay post-load barrier observes this before the longjmp. */
            s_load_completed = 1;
            /* Restage FBO/present latch so the restored frame is visible
             * immediately (avoids disabled-display blank latch + stale smooth). */
            psx_frontend_on_savestate_loaded();
            t_after_frontend = savestate_mono_ms();
            fprintf(stderr,
                    "savestate: LOADED slot %d -> resuming pc=0x%08X "
                    "(boot=%.1f frontend=%.1f poll_total=%.1f ms)%s\n",
                    slot, (unsigned)cpu->pc,
                    t_after_boot - t_load0,
                    t_after_frontend - t_after_boot,
                    t_after_frontend - t_load0,
                    path[0] ? "" : " [blob]");
            /* Unwind to the scheduler and re-dispatch the restored PC. Never
             * returns; abandons the suspended CPS frames on the current stack. */
            psx_scheduler_resume_at(cpu->pc);
        }
    }
}
