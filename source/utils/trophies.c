/*
 * PS Vita trophy backend. See trophies.h for the contract.
 *
 * ★ WHY THE API IS DECLARED BY HAND: VitaSDK ships libSceNpTrophy_stub.a but NO
 * header for it (vita-headers has only psp2/np/common.h), so the prototypes below are
 * written out here. They are not guesses -- they match the calling convention used by
 * shipped Vita soloader ports that link the same stub. Getting these wrong would
 * corrupt the stack, so they are kept together, commented, and changed only with
 * evidence.
 *
 * ★ WHY UNLOCKS RUN ON THEIR OWN THREAD: sceNpTrophyUnlockTrophy writes the trophy
 * file and raises the system notification, and it BLOCKS while doing so. The engine
 * calls UnlockAchievement from its script thread mid-scene; blocking there would stall
 * the sim for the duration of a disk write. So the hook only records an ID and signals
 * a semaphore, and a dedicated low-priority thread performs the actual unlock.
 */
#include "utils/trophies.h"
#include "utils/logger.h"
#include "utils/utils.h"      /* mcsm_open_setting */
#include "utils/config.h"     /* trophy_test */

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/sysmodule.h>
#include <psp2/common_dialog.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* vitaGL's swap, forward-declared: including <vitaGL.h> here collides with the
 * EGL/GLES headers this tree already pulls in.
 * ☠ The signature must match vitaGL.h EXACTLY -- `void vglSwapBuffers(GLboolean)`.
 * An earlier version here said `int vglSwapBuffers(int)`; a mismatched declaration is
 * undefined behaviour even when it happens to work on ARM, and it is the kind of thing
 * that only misbehaves once something else changes. GLboolean is unsigned char, so
 * that is spelled out rather than pulling in the GL headers. */
extern void vglSwapBuffers(unsigned char has_commondialog);

/* ---- the undocumented SceNpTrophy surface ------------------------------------- */
typedef struct {
    int sdkVersion;
    SceCommonDialogParam commonParam;
    int context;
    int options;
    uint8_t reserved[128];
} McsmTrophySetupParam;

typedef struct { uint32_t bits[4]; } McsmTrophyUnlockState;   /* 128 trophies max */

int  sceNpTrophyInit(void *unk);
int  sceNpTrophyCreateContext(int *context, const char *commId, const char *commSign, uint64_t options);
int  sceNpTrophyCreateHandle(int *handle);
int  sceNpTrophyDestroyHandle(int handle);
int  sceNpTrophyUnlockTrophy(int ctx, int handle, int id, int *platinum_id);
int  sceNpTrophyGetTrophyUnlockState(int ctx, int handle, McsmTrophyUnlockState *state, uint32_t *count);
int  sceNpTrophySetupDialogInit(McsmTrophySetupParam *param);
SceCommonDialogStatus sceNpTrophySetupDialogGetStatus(void);
int  sceNpTrophySetupDialogTerm(void);

/* ---- state -------------------------------------------------------------------- */

/* The NP communication ID -- NOT the title id. It comes from MCSM_NP_COMM_ID, the
 * single CMake variable that also names the sce_sys/trophy/<id>_00/ folder the pack is
 * packaged into, so the loader can no longer ask for an id the package does not
 * contain. That pairing used to be maintained by hand and drifted: packaged as
 * MCSM00002_00, requested as MCSM00001, declared as MCSM00001_00 inside the pack --
 * which silently disabled trophies on every fresh install.
 *
 * The third place, <npcommid> inside TROPHY.TRP, is a data file (tools/trophies.def)
 * and still has to be kept in step by hand when the pack is rebuilt;
 * `tools/trp_tool.py setcommid` rewrites it in an existing pack.
 *
 * Overridable at runtime from ux0:data/mcsm/settings/trophy_commid.txt so a
 * differently-built pack can be used without a rebuild. */
#ifndef MCSM_NP_COMM_ID
#error "MCSM_NP_COMM_ID must be defined by the build (see CMakeLists.txt)"
#endif
static char g_comm_id[16] = MCSM_NP_COMM_ID;

/* A DUMMY signature. Real titles carry a Sony-issued 160-byte NP communication
 * signature; homebrew cannot produce one, which is precisely the check NoTrpDrm
 * disables. The leading bytes are the standard header the system expects to parse
 * before the (now unverified) remainder. */
static char g_comm_sig[160] = { (char)0xb9, (char)0xdd, (char)0xe1, (char)0x3b, 0x01, 0x00 };

static int g_ctx = 0;
static int g_available = 0;
static McsmTrophyUnlockState g_unlocked;

/* Worker plumbing. g_req is "a trophy is waiting", g_done is "the worker is idle"
 * (starts signalled) so a caller never overwrites a pending ID. */
static SceUID g_req_sema = -1, g_done_sema = -1;
static volatile int g_pending_id = -1;

/* name -> id map, loaded once from ux0:data/mcsm/trophies.txt */
#define TROPHY_MAP_MAX 128
#define TROPHY_NAME_MAX 64
typedef struct { char name[TROPHY_NAME_MAX]; int id; } TrophyMapEntry;
static TrophyMapEntry g_map[TROPHY_MAP_MAX];
static int g_map_count = 0;
static int g_map_loaded = 0;

/* ---- worker ------------------------------------------------------------------- */

static int trophy_worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        if (sceKernelWaitSema(g_req_sema, 1, NULL) < 0) break;
        const int id = g_pending_id;
        int handle = 0;
        int platinum = -1;
        if (sceNpTrophyCreateHandle(&handle) >= 0) {
            const int rc = sceNpTrophyUnlockTrophy(g_ctx, handle, id, &platinum);
            if (rc < 0) l_warn("TROPHY: unlock id=%d FAILED rc=0x%08X", id, (unsigned)rc);
            else        l_info("TROPHY: unlocked id=%d%s", id,
                               platinum >= 0 ? " (platinum condition met)" : "");
            sceNpTrophyDestroyHandle(handle);
        } else {
            l_warn("TROPHY: could not create handle for id=%d", id);
        }
        /* Signal AFTER the blocking work so the next request cannot race this one. */
        sceKernelSignalSema(g_done_sema, 1);
    }
    return 0;
}

/* ---- name -> id map ----------------------------------------------------------- */

/* trophies.txt format, one per line, '#' comments allowed:
 *     <achievement name> = <trophy id>
 * The engine's achievement names live in packed .ttarch2 Lua and cannot be read
 * offline, so this is deliberately a runtime file: the log names every unmapped
 * achievement it sees, and those lines can be pasted straight in. */
static void trophy_map_load(void) {
    if (g_map_loaded) return;
    g_map_loaded = 1;

    FILE *f = mcsm_open_setting("trophies.txt", "r");
    if (!f) {
        l_info("TROPHY: no trophies.txt — achievement names will be logged unmapped");
        return;
    }
    char line[160];
    while (g_map_count < TROPHY_MAP_MAX && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        /* right-trim the key */
        char *end = eq - 1;
        while (end >= p && (*end == ' ' || *end == '\t')) *end-- = 0;
        const int id = atoi(eq + 1);
        if (id < 0 || !*p) continue;
        size_t n = strlen(p);
        if (n >= TROPHY_NAME_MAX) n = TROPHY_NAME_MAX - 1;
        memcpy(g_map[g_map_count].name, p, n);
        g_map[g_map_count].name[n] = 0;
        g_map[g_map_count].id = id;
        g_map_count++;
    }
    fclose(f);
    l_info("TROPHY: loaded %d name->id mappings from trophies.txt", g_map_count);
}

/* ---- public ------------------------------------------------------------------- */

int mcsm_trophies_available(void) { return g_available; }

int mcsm_trophies_init(void) {
    /* An override lets a pack built with a different comm id be used as-is. */
    { FILE *f = mcsm_open_setting("trophy_commid.txt", "r");
      if (f) { char b[16] = {0};
               if (fgets(b, sizeof(b), f)) {
                   size_t n = strcspn(b, "\r\n"); b[n] = 0;
                   if (b[0]) { strncpy(g_comm_id, b, sizeof(g_comm_id) - 1);
                               g_comm_id[sizeof(g_comm_id) - 1] = 0; } }
               fclose(f); } }

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY) < 0) {
        l_warn("TROPHY: NP_TROPHY sysmodule failed to load — trophies disabled");
        return -1;
    }
    sceNpTrophyInit(NULL);

    const int rc = sceNpTrophyCreateContext(&g_ctx, g_comm_id, g_comm_sig, 0);
    if (rc < 0) {
        /* By far the most likely cause, and worth saying plainly rather than leaving
         * a bare error code: without NoTrpDrm the signature check rejects the dummy
         * signature above, and without the pack there is nothing to open. */
        l_warn("TROPHY: CreateContext(commId=%s) failed rc=0x%08X — trophies OFF. "
               "Expected causes: NoTrpDrm not installed, or no "
               "sce_sys/trophy/%s_00/TROPHY.TRP in the app folder.",
               g_comm_id, (unsigned)rc, g_comm_id);
        return rc;
    }

    /* ★ REGISTER ONCE, NOT EVERY LAUNCH (2026-07-31).
     *
     * sceNpTrophySetupDialogInit is the REGISTRATION step -- it is what writes the
     * set into the system's trophy store. This used to run unconditionally on every
     * boot, which is wrong twice over: it re-does an operation the system has
     * already recorded (and a trophy registration is exactly the state this port has
     * already seen get poisoned and become unrepairable in place), and, because a
     * common dialog only advances while frames are presented, it PRESENTS at least
     * one frame -- which threw away the boot picture and left a black screen for the
     * ~20 seconds of loading that follow.
     *
     * "Already registered?" is answered by asking for the unlock state rather than
     * by testing an error constant: if the set is registered the call succeeds and
     * reports how many trophies it knows about, and if it is not, it fails. That
     * needs no assumption about which of several plausible SCE_NP_TROPHY_ERROR_*
     * values this firmware returns -- an assumption there would silently either
     * re-register forever or never register at all. */
    sceClibMemset(&g_unlocked, 0, sizeof(g_unlocked));
    uint32_t trophy_count = 0;
    int registered = 0;
    { int h = 0;
      if (sceNpTrophyCreateHandle(&h) >= 0) {
          const int urc = sceNpTrophyGetTrophyUnlockState(g_ctx, h, &g_unlocked, &trophy_count);
          sceNpTrophyDestroyHandle(h);
          registered = (urc >= 0 && trophy_count > 0);
          if (!registered)
              l_info("TROPHY: set not registered yet (rc=0x%08X count=%u) — running setup once",
                     (unsigned)urc, trophy_count);
      } }

    if (!registered) {
        /* One-time system setup dialog. It is a common dialog, so it only advances
         * while something presents frames -- hence this must run on the GL thread. */
        McsmTrophySetupParam setup;
        sceClibMemset(&setup, 0, sizeof(setup));
        _sceCommonDialogSetMagicNumber(&setup.commonParam);
        setup.sdkVersion = PSP2_SDK_VERSION;
        setup.options = 0;
        setup.context = g_ctx;
        if (sceNpTrophySetupDialogInit(&setup) >= 0) {
            SceCommonDialogStatus st = SCE_COMMON_DIALOG_STATUS_RUNNING;
            /* Bounded: if the dialog never reports FINISHED we must not hang the boot. */
            for (int guard = 0; guard < 3600 && st == SCE_COMMON_DIALOG_STATUS_RUNNING; guard++) {
                st = sceNpTrophySetupDialogGetStatus();
                vglSwapBuffers(1 /* has_commondialog: let vitaGL composite the dialog */);
            }
            if (st == SCE_COMMON_DIALOG_STATUS_RUNNING)
                l_warn("TROPHY: setup dialog did not finish in 3600 frames — continuing anyway");
            sceNpTrophySetupDialogTerm();
        }
        /* Re-read: registration is what fills this in, and the bitmap must reflect
         * trophies earned in past sessions or they would all re-notify. */
        { int h = 0;
          sceClibMemset(&g_unlocked, 0, sizeof(g_unlocked));
          if (sceNpTrophyCreateHandle(&h) >= 0) {
              sceNpTrophyGetTrophyUnlockState(g_ctx, h, &g_unlocked, &trophy_count);
              sceNpTrophyDestroyHandle(h);
          } }
    }
    l_info("TROPHY: context ready (commId=%s, %u trophies in pack, %s)",
           g_comm_id, trophy_count,
           registered ? "already registered" : "registered this launch");

    g_done_sema = sceKernelCreateSema("mcsm_trp_done", 0, 1, 1, NULL);
    g_req_sema  = sceKernelCreateSema("mcsm_trp_req",  0, 0, 1, NULL);
    if (g_done_sema < 0 || g_req_sema < 0) {
        l_warn("TROPHY: semaphore creation failed — trophies disabled");
        return -1;
    }
    /* Low priority: this thread does one blocking call at a time and must never compete
     * with the sim or render threads. 64KB stack rather than the 16KB first written
     * here -- sceNpTrophyUnlockTrophy writes the trophy file and drives the system
     * notification, and its stack use is not documented, so this matches the size a
     * shipped Vita port uses instead of guessing low and risking an overflow inside
     * a Sony library. */
    SceUID thd = sceKernelCreateThread("mcsm_trophy", &trophy_worker, 0x10000100, 0x10000, 0, 0, NULL);
    if (thd < 0) {
        l_warn("TROPHY: worker thread creation failed — trophies disabled");
        return -1;
    }
    sceKernelStartThread(thd, 0, NULL);

    trophy_map_load();
    g_available = 1;

    /* PIPELINE SELF-TEST. Telltale achievements are driven by story-progress Lua, so
     * without this the only way to find out whether ANY of this works is to play to a
     * beat. This fires the identical path -- worker thread, blocking
     * sceNpTrophyUnlockTrophy, system notification -- immediately after init.
     * Deliberately AFTER g_available is set, since mcsm_trophies_unlock() is a no-op
     * until then. Re-firing on later boots is harmless: the unlock-state bitmap seeded
     * above already holds the trophy, so it is skipped. */
    { const int t = mcsm_cfg()->trophy_test;
      if (t > 0) {
          l_info("TROPHY: SELF-TEST unlocking id=%d (graphics.txt trophy_test) — "
                 "a system notification here proves context, worker and unlock all work", t);
          mcsm_trophies_unlock((uint32_t)t);
      } }
    return 0;
}

int mcsm_trophies_already_unlocked(uint32_t id) {
    if (id >= 128u) return 0;
    return (g_unlocked.bits[id >> 5] & (1u << (id & 31u))) != 0u;
}

void mcsm_trophies_unlock(uint32_t id) {
    if (!g_available) return;
    if (id >= 128u) { l_warn("TROPHY: id %u out of range", (unsigned)id); return; }
    /* Local bitmap check first: this is what keeps a story beat that re-fires from
     * queueing a redundant blocking unlock every time it plays. The bitmap is seeded
     * at init from the SYSTEM's store, so trophies earned in past sessions count. */
    if (mcsm_trophies_already_unlocked(id)) return;
    g_unlocked.bits[id >> 5] |= (1u << (id & 31u));

    /* Wait for the worker to be idle, then hand it the ID.
     * ☠ BOUNDED WAIT. This runs on the ENGINE'S SCRIPT THREAD, so an untimed wait
     * here would hang the whole game if the worker ever wedged inside a blocking
     * sceNpTrophyUnlockTrophy. Two seconds is far longer than a trophy write needs and
     * far shorter than a player would tolerate a freeze; on timeout we drop THIS
     * unlock rather than the game. The bit was already set above, so a dropped unlock
     * is not retried -- deliberately, because retrying is what would turn a wedged
     * worker into a permanent per-achievement stall. */
    SceUInt timeout_us = 2u * 1000u * 1000u;
    if (sceKernelWaitSema(g_done_sema, 1, &timeout_us) < 0) {
        l_warn("TROPHY: worker busy, dropped unlock id=%u (game not stalled)", (unsigned)id);
        return;
    }
    g_pending_id = (int)id;
    sceKernelSignalSema(g_req_sema, 1);
}

/* ★ DERIVE THE TROPHY ID FROM THE NAME — no configuration required.
 *
 * This is not a guess. The game's own data settles it: the engine's Lua calls
 * AchievementManager_Unlock("achievement_NN"), and achievementmanager.lua resolves
 * that through achievement_ids.prop, which carries a per-platform id column for
 * PS3 / PS4 / Steam / Vita / XB1 / Xbox360. Extracting that prop from
 * MCSM_android-pvr_Project_all.ttarch2 shows the Vita column is a PERFECT 1:1 map:
 *     achievement_01 -> Vita "001",  achievement_02 -> "002",  ... all 32 records,
 *     zero exceptions.
 * Trophy 000 is the platinum (pid -1 in TROPCONF), which is why the numbering starts
 * at 1 and why it lines up with the 51-icon pack exactly.
 *
 * Which of the two forms actually reaches us depends on whether the Lua's platform
 * lookup resolves (the prop has no Android column, and this build reports as
 * Platform_Android), so BOTH are accepted: the key "achievement_07" and the resolved
 * id "007" both yield 7. Anything else falls through to the map file.
 * Returns -1 when no id can be derived. */
static int trophy_id_from_name(const char *name) {
    const char *p = name;
    /* Accept an optional "achievement_" prefix, then require the remainder to be all
     * digits -- refusing anything else keeps a stray name from unlocking a random
     * trophy, which would be worse than not unlocking at all. */
    const char *pfx = "achievement_";
    size_t plen = strlen(pfx);
    if (strncmp(p, pfx, plen) == 0) p += plen;
    if (!*p) return -1;
    int v = 0;
    for (const char *q = p; *q; q++) {
        if (*q < '0' || *q > '9') return -1;
        v = v * 10 + (*q - '0');
        if (v > 127) return -1;
    }
    return v;
}

/* Shared tail for both resolution paths. The engine re-fires achievement unlocks
 * whenever a story beat replays, so an id we already hold is dropped WITHOUT a log
 * line -- otherwise a replayed chapter writes the same "unlock" over and over and
 * buries whatever else the log was meant to show. Throttled confirmation only. */
static void trophy_unlock_resolved(const char *name, int id, const char *how) {
    if (mcsm_trophies_already_unlocked((uint32_t)id)) {
        static unsigned s_dup = 0;
        if (s_dup++ < 8u)
            l_info("TROPHY: achievement \"%s\" (id %d) is already held — ignoring "
                   "(further repeats are not logged)", name, id);
        return;
    }
    l_info("TROPHY: achievement \"%s\" -> trophy id %d (%s)", name, id, how);
    mcsm_trophies_unlock((uint32_t)id);
}

void mcsm_trophies_unlock_by_name(const char *name) {
    if (!name || !*name) return;
    trophy_map_load();
    /* An explicit mapping always wins, so a wrong derivation can be corrected on
     * device without a rebuild. */
    for (int i = 0; i < g_map_count; i++) {
        if (strcmp(g_map[i].name, name) == 0) {
            trophy_unlock_resolved(name, g_map[i].id, "mapped");
            return;
        }
    }
    const int derived = trophy_id_from_name(name);
    if (derived >= 0) {
        trophy_unlock_resolved(name, derived, "derived");
        return;
    }
    /* Only reached by a name shaped like nothing the game data contains. Throttled so
     * a repeating story beat cannot flood the log. */
    static unsigned s_unmapped = 0;
    if (s_unmapped++ < 64u)
        l_info("TROPHY: achievement \"%s\" is UNMAPPED — add a line \"%s = <id>\" to "
               "ux0:data/mcsm/trophies.txt", name, name);
}
