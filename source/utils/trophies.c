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
#include <errno.h>
#include <stdatomic.h>
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
/* Set by CMake from whether extras/trophy/TROPHY.TRP was packaged. Defaults to 1 so
 * an out-of-tree build that never defines it keeps the old behaviour. */
#ifndef MCSM_HAVE_TROPHY_PACK
#define MCSM_HAVE_TROPHY_PACK 1
#endif

#ifndef MCSM_NP_COMM_ID
#error "MCSM_NP_COMM_ID must be defined by the build (see CMakeLists.txt)"
#endif
#if MCSM_HAVE_TROPHY_PACK
static char g_comm_id[16] = MCSM_NP_COMM_ID;

/* A DUMMY signature. Real titles carry a Sony-issued 160-byte NP communication
 * signature; homebrew cannot produce one, which is precisely the check NoTrpDrm
 * disables. The leading bytes are the standard header the system expects to parse
 * before the (now unverified) remainder. */
static char g_comm_sig[160] = { (char)0xb9, (char)0xdd, (char)0xe1, (char)0x3b, 0x01, 0x00 };

/* The sceNpTrophy context handle. Guarded with the two above because every use of
 * it -- create, unlock-state probe, setup dialog, worker unlock -- lives in the
 * pack-present path; a no-trophy build never opens a context at all. */
static int g_ctx = 0;
#endif /* MCSM_HAVE_TROPHY_PACK */

/* First-boot trophy setup, driven from gl_swap instead of blocking the boot.
 * g_setup_pending is read every present, so it is written last on completion.
 * Guarded: a no-trophy build never opens a dialog, so these would be unused. */
#if MCSM_HAVE_TROPHY_PACK
static atomic_int g_setup_pending = ATOMIC_VAR_INIT(0);
static char g_setup_marker[128];
static void remember_marker_path(const char *m) {
    size_t n = 0;
    while (n + 1 < sizeof(g_setup_marker) && m[n]) { g_setup_marker[n] = m[n]; n++; }
    g_setup_marker[n] = 0;
}
#endif

static int g_available = 0;
static McsmTrophyUnlockState g_unlocked;

/* Worker plumbing. There are only 128 legal trophy IDs, so a 128-entry ring can hold
 * every distinct outstanding request. g_queued_bits covers both queued and in-flight
 * IDs: repeated story callbacks are collapsed without claiming the trophy is already
 * earned. The bit moves to g_unlocked only after sceNpTrophyUnlockTrophy succeeds.
 *
 * The engine calls this from its script thread. It must never wait behind Sony's
 * blocking trophy API -- or even behind the worker's queue mutex. The normal path
 * uses a try-lock and the rare contended path records the ID in g_deferred_bits. The
 * worker folds those bits into the same bounded ring under its own lock. Thus mutex
 * contention cannot stall the game and cannot drop the achievement. */
#define TROPHY_QUEUE_CAP 128u
/* Upper bound on how long the worker defers unlocks for the first-boot setup
 * dialog. Long enough that a user reading the dialog is never cut short, short
 * enough that a dialog which never completes cannot silently disable trophies. */
#define TROPHY_SETUP_WAIT_MAX_MS (60u * 1000u)

/* ★ 2026-08-06 — THE ERROR THAT PROVES THE SET IS NOT REGISTERED.
 *
 * Device log (r147, 17-minute session):
 *     TROPHY: unlock-state probe says not present (rc=0x80551612 count=0)
 *     TROPHY: set already installed (ur0:.../MCSM00001_00 exists) — skipping setup
 *     TROPHY: unlock id=6 FAILED rc=0x80551612
 *
 * The comment above the directory check argues the probe is untrustworthy because
 * it returns this same code on a system where the set IS fully registered, so the
 * presence of `conf/<COMMID>_00` was made a second way to answer "already there".
 * On this device that reasoning inverts the truth: the folder exists, setup is
 * skipped, and then the ACTUAL UNLOCK fails with the very same code. A stale conf
 * folder with no matching registration in the system store is exactly the state
 * [[mcsm-trophy-reset-method]] describes after a failed trophy reset.
 *
 * A failed probe is ambiguous and a present folder is ambiguous, but a failed
 * UNLOCK is not: if the system refuses to record a trophy, the set is not usable,
 * whatever the filesystem says. So treat that specific failure as the trigger to
 * run setup once and retry, instead of failing silently for the whole session --
 * which is the "achievements do not pop at all" report. */
#define TROPHY_ERR_NOT_REGISTERED 0x80551612u
/* One automatic recovery per launch. If setup runs and the unlock still fails,
 * something else is wrong (no NoTrpDrm, corrupt store) and retrying forever would
 * just reopen the dialog on a loop. */
#define TROPHY_SETUP_RECOVERY_MAX 1u
static SceUID g_req_sema = -1;
static SceUID g_queue_mutex = -1;
static uint8_t g_queue[TROPHY_QUEUE_CAP];
static unsigned g_queue_head = 0;
static unsigned g_queue_tail = 0;
static unsigned g_queue_count = 0;
static atomic_uint_least32_t g_queued_bits[4];
static atomic_uint_least32_t g_deferred_bits[4];

/* Set by the worker when an unlock proves the set is not registered; consumed on
 * the GL thread by mcsm_trophies_setup_poll(), which is where a common dialog can
 * safely be opened and pumped. */
static atomic_int g_setup_request = ATOMIC_VAR_INIT(0);
static unsigned g_setup_recoveries = 0;
static char g_setup_marker_path[128];

/* The name -> id map lives in game.txt now; see mcsm_game_trophy_id_for(). */

/* ---- worker ------------------------------------------------------------------- */

#if MCSM_HAVE_TROPHY_PACK
static void trophy_queue_refill_locked(void) {
    for (unsigned word = 0; word < 4u; word++) {
        uint32_t bits = (uint32_t)atomic_exchange_explicit(
            &g_deferred_bits[word], 0u, memory_order_acquire);
        while (bits != 0u) {
            const unsigned bit = (unsigned)__builtin_ctz(bits);
            bits &= bits - 1u;

            /* This cannot fill: queued + deferred + the one in flight contains at
             * most one entry for each of the 128 valid IDs. Keep the guard anyway so
             * a future range change fails recoverably instead of corrupting memory. */
            if (g_queue_count >= TROPHY_QUEUE_CAP) {
                atomic_fetch_or_explicit(&g_deferred_bits[word],
                                         bits | (1u << bit),
                                         memory_order_release);
                l_warn("TROPHY: internal queue full while refilling (id=%u)",
                       word * 32u + bit);
                return;
            }
            g_queue[g_queue_tail] = (uint8_t)(word * 32u + bit);
            g_queue_tail = (g_queue_tail + 1u) % TROPHY_QUEUE_CAP;
            g_queue_count++;
        }
    }
}

static int trophy_worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        if (sceKernelWaitSema(g_req_sema, 1, NULL) < 0) break;

        /* A newly-installed set is not usable until its common dialog completes,
         * so hold early story callbacks until it does.
         *
         * ★ BOUNDED. This wait used to be unconditional (`while (pending) sleep`).
         * g_setup_pending is only ever cleared by mcsm_trophies_setup_poll(), which
         * runs from gl_swap and only clears once the dialog reports a non-RUNNING
         * status. If the dialog never reaches that state -- it opened but was never
         * completed, or presents stopped -- the flag stays set and this loop parks
         * the worker for the rest of the session, so NOTHING ever unlocks. That is
         * the "achievements do not pop at all" report. Give the dialog a generous
         * window, then proceed: an unlock attempted too early merely fails, gets
         * its queued bit cleared, and is retried by the next story callback. */
        {
            unsigned waited_ms = 0;
            while (atomic_load_explicit(&g_setup_pending, memory_order_acquire)) {
                if (waited_ms >= TROPHY_SETUP_WAIT_MAX_MS) {
                    l_warn("TROPHY: setup dialog still pending after %ums; "
                           "proceeding with queued unlocks", waited_ms);
                    break;
                }
                sceKernelDelayThread(50u * 1000u);
                waited_ms += 50u;
            }
        }

        int id = -1;
        const int lock_rc = sceKernelLockMutex(g_queue_mutex, 1, NULL);
        if (lock_rc < 0) {
            /* The semaphore token still represents a live request. Put the token
             * back after a short worker-only delay rather than stranding that ID. */
            l_warn("TROPHY: queue lock failed rc=0x%08X; retrying request",
                   (unsigned)lock_rc);
            sceKernelDelayThread(10u * 1000u);
            sceKernelSignalSema(g_req_sema, 1);
            continue;
        }
        trophy_queue_refill_locked();
        if (g_queue_count != 0u) {
            id = (int)g_queue[g_queue_head];
            g_queue_head = (g_queue_head + 1u) % TROPHY_QUEUE_CAP;
            g_queue_count--;
        }
        sceKernelUnlockMutex(g_queue_mutex, 1);
        if (id < 0) {
            /* A wake with no item is harmless (for example, recovery after a rare
             * signal overflow); never manufacture a trophy ID from it. */
            l_warn("TROPHY: request wake had no queue item");
            continue;
        }

        int handle = 0;
        int platinum = -1;
        if (sceNpTrophyCreateHandle(&handle) >= 0) {
            const int rc = sceNpTrophyUnlockTrophy(g_ctx, handle, id, &platinum);
            if (rc < 0) {
                l_warn("TROPHY: unlock id=%d FAILED rc=0x%08X", id, (unsigned)rc);
                /* ☠ OPT-IN (2026-08-06). This recovery is MINE and it fires EVERY
                 * session, because the unlock-state probe fails on this device every
                 * boot -- so every playthrough now opens a trophy common dialog
                 * mid-gameplay and then refreshes g_unlocked from the system. Neither
                 * happened before I added it. Achievements are reported as having
                 * worked before and as broken after, so the default is off and the
                 * old behaviour is restored. `trophy_recovery = on` in graphics.txt
                 * re-enables it (it does genuinely register an unregistered set --
                 * device-confirmed -- it is just not worth the side effects unless
                 * trophies are actually dead). */
                if (mcsm_cfg()->trophy_recovery &&
                    (unsigned)rc == TROPHY_ERR_NOT_REGISTERED &&
                    g_setup_recoveries < TROPHY_SETUP_RECOVERY_MAX) {
                    /* The set is not actually registered, whatever the conf folder
                     * said at init. Ask the GL thread to run setup, and put this id
                     * back so it is retried once the dialog completes -- otherwise
                     * the story beat that earned it never fires again this session. */
                    g_setup_recoveries++;
                    atomic_fetch_or_explicit(&g_deferred_bits[id >> 5],
                                             1u << (id & 31), memory_order_release);
                    atomic_store_explicit(&g_setup_request, 1, memory_order_release);
                    l_warn("TROPHY: set is not registered — requesting setup dialog, "
                           "id=%d requeued for retry", id);
                    sceNpTrophyDestroyHandle(handle);
                    /* Wait for the GL thread to actually pick the request up before
                     * re-arming, or the retry would be dequeued and fail again while
                     * g_setup_pending is still 0 and the one recovery is spent. */
                    for (unsigned waited = 0;
                         atomic_load_explicit(&g_setup_request, memory_order_acquire) &&
                         waited < 5000u;
                         waited += 50u) {
                        sceKernelDelayThread(50u * 1000u);
                    }
                    sceKernelSignalSema(g_req_sema, 1);
                    continue;   /* keep the queued bit: the retry is already pending */
                }
            }
            else {
                __atomic_fetch_or(&g_unlocked.bits[id >> 5],
                                  1u << (id & 31), __ATOMIC_RELEASE);
                if (platinum >= 0 && platinum < 128)
                    __atomic_fetch_or(&g_unlocked.bits[platinum >> 5],
                                      1u << (platinum & 31), __ATOMIC_RELEASE);
                l_info("TROPHY: unlocked id=%d%s", id,
                       platinum >= 0 ? " (platinum condition met)" : "");
            }
            sceNpTrophyDestroyHandle(handle);
        } else {
            l_warn("TROPHY: could not create handle for id=%d", id);
        }

        /* Success is now recorded in g_unlocked. On failure, clearing only the
         * queued marker intentionally permits the next engine callback to retry. */
        atomic_fetch_and_explicit(&g_queued_bits[id >> 5],
                                  ~(1u << (id & 31)), memory_order_release);
    }
    return 0;
}
#endif /* MCSM_HAVE_TROPHY_PACK -- the worker is only ever started by the
        * pack-present init path, so it would be an unused-function warning
        * in a no-trophy build. */

/* ---- name -> id map ----------------------------------------------------------- */

/* The achievement-name -> trophy-id map moved into game.txt (repeated
 * `trophy_map = <name>:<id>` lines) so the loader ships exactly two settings
 * files. config.c owns the parsing; mcsm_game_trophy_id_for() is the lookup. */

/* ---- public ------------------------------------------------------------------- */

int mcsm_trophies_available(void) { return g_available; }

/* Called from gl_swap every present. Returns 1 while the first-boot setup dialog is
 * up, so the presenter knows to composite it (has_commondialog) -- the same contract
 * the IME uses. Finishes the registration the moment the system reports it done:
 * writes the once-only marker and re-reads the unlock bitmap so trophies earned in
 * past sessions do not all re-notify. */
/* Side-effect free: the presenter asks this once per frame purely to decide the
 * has_commondialog flag. Kept separate from the poll so asking cannot advance state. */
int mcsm_trophies_setup_active(void) {
#if !MCSM_HAVE_TROPHY_PACK
    return 0;
#else
    return atomic_load_explicit(&g_setup_pending, memory_order_acquire);
#endif
}

#if MCSM_HAVE_TROPHY_PACK
/* Open the registration dialog. Must run on the GL thread: a common dialog only
 * advances while frames are presented, and gl_swap is what pumps it. */
static int trophy_open_setup_dialog(const char *why) {
    McsmTrophySetupParam setup;
    sceClibMemset(&setup, 0, sizeof(setup));
    _sceCommonDialogSetMagicNumber(&setup.commonParam);
    setup.sdkVersion = PSP2_SDK_VERSION;
    setup.options = 0;
    setup.context = g_ctx;
    const int drc = sceNpTrophySetupDialogInit(&setup);
    if (drc >= 0) {
        if (g_setup_marker_path[0]) remember_marker_path(g_setup_marker_path);
        atomic_store_explicit(&g_setup_pending, 1, memory_order_release);
        l_info("TROPHY: setup dialog opened (%s), running alongside the game", why);
        return 1;
    }
    l_warn("TROPHY: setup dialog would not open (rc=0x%08X, %s) — trophies stay "
           "unavailable this session; next boot retries", (unsigned)drc, why);
    return 0;
}
#endif /* MCSM_HAVE_TROPHY_PACK */

int mcsm_trophies_setup_poll(void) {
#if !MCSM_HAVE_TROPHY_PACK
    return 0;
#else
    /* Recovery path: the worker proved the set is unregistered by having an unlock
     * rejected. Open the dialog here, where it can actually be pumped. */
    if (atomic_exchange_explicit(&g_setup_request, 0, memory_order_acq_rel)) {
        if (!atomic_load_explicit(&g_setup_pending, memory_order_acquire) &&
            trophy_open_setup_dialog("unlock reported set not registered")) {
            /* Report "dialog up" for THIS frame instead of falling through to
             * GetStatus below. A dialog queried in the same call that created it
             * need not have reached RUNNING yet, and a non-RUNNING answer here
             * would Term it immediately and record a registration that never
             * happened. The next present polls it normally. */
            return 1;
        }
    }

    if (!atomic_load_explicit(&g_setup_pending, memory_order_acquire)) return 0;
    const SceCommonDialogStatus st = sceNpTrophySetupDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_RUNNING) return 1;

    sceNpTrophySetupDialogTerm();

    FILE *mf = g_setup_marker[0] ? fopen(g_setup_marker, "wb") : NULL;
    if (mf) {
        fputs("trophy setup dialog completed for this comm id\n", mf);
        fclose(mf);
        l_info("TROPHY: setup finished — set registered (record: %s). Later boots skip "
               "this on their own, because the system then reports it present.",
               g_setup_marker);
    } else {
        l_warn("TROPHY: setup finished but could not write %s (errno=%d) — the dialog "
               "will appear again next boot", g_setup_marker, errno);
    }

    /* Registration is what fills the unlock bitmap in. Query into a private object,
     * then publish whole words atomically: story callbacks can safely inspect the old
     * snapshot while the common-dialog thread is refreshing it. */
    { int h = 0; uint32_t cnt = 0; McsmTrophyUnlockState refreshed;
      sceClibMemset(&refreshed, 0, sizeof(refreshed));
      if (sceNpTrophyCreateHandle(&h) >= 0) {
          sceNpTrophyGetTrophyUnlockState(g_ctx, h, &refreshed, &cnt);
          sceNpTrophyDestroyHandle(h);
      }
      for (unsigned i = 0; i < 4u; i++)
          __atomic_store_n(&g_unlocked.bits[i], refreshed.bits[i], __ATOMIC_RELEASE);
      l_info("TROPHY: registered — %u trophies now known to the system", cnt); }

    /* Release the worker only after the bitmap refresh above is complete. */
    atomic_store_explicit(&g_setup_pending, 0, memory_order_release);
    return 0;
#endif
}


int mcsm_trophies_init(void) {
#if !MCSM_HAVE_TROPHY_PACK
    /* ☠ NO PACK WAS SHIPPED -- DO NOTHING AT ALL. Not "try and fail gracefully":
     * sceNpTrophyCreateContext SUCCEEDS with no pack behind it, so the unlock-state
     * probe below reports 0 trophies, the code concludes the set is unregistered and
     * runs the SYSTEM SETUP DIALOG, which cannot complete without a pack. It then
     * burns the entire 3600-frame guard and leaves an NP error dialog on screen --
     * on every single boot. Observed on device 2026-07-31 with the pack removed from
     * an installed app:
     *     TROPHY: set not registered yet (rc=0x80551612 count=0) -- running setup once
     *     TROPHY: setup dialog did not finish in 3600 frames -- continuing anyway
     * Bailing here is what actually makes a no-trophy build quiet. */
    l_info("TROPHY: no trophy pack in this build — trophy support compiled out "
           "(no context, no setup dialog).");
    g_available = 0;
    return 0;
#else
    /* An override lets a pack built with a different comm id be used as-is.
     *
     * ☠ IT MUST ANNOUNCE ITSELF. This applied silently, and a stale
     * ux0:data/mcsm/settings/trophy_commid.txt holding "MCSM00002" survived on the
     * test console long after the build moved to MCSM00001 -- so every launch
     * shipped a pack at sce_sys/trophy/MCSM00001_00/ and then asked the system for
     * MCSM00002, which cannot ever match. That is the same packaged-vs-requested
     * drift CMakeLists.txt was written to prevent, reintroduced at RUNTIME by a
     * leftover file, and it was invisible precisely because nothing logged it.
     * A one-time override is a debugging tool; a permanent undetectable one is a
     * trap. If this line appears in a log and you did not put the file there,
     * DELETE THE FILE -- the compiled-in id is the correct one. */
    { const char *ov = mcsm_game()->trophy_commid;
      if (ov[0] && strcmp(ov, g_comm_id) != 0) {
          l_warn("TROPHY: ☠ comm id OVERRIDDEN by game.txt trophy_commid: "
                 "compiled-in '%s' -> '%s'. The packaged pack is "
                 "sce_sys/trophy/%s_00/TROPHY.TRP, so unless that matches the "
                 "override there will be NO trophies. Remove the line to use "
                 "the build's own id.", g_comm_id, ov, g_comm_id);
          strncpy(g_comm_id, ov, sizeof(g_comm_id) - 1);
          g_comm_id[sizeof(g_comm_id) - 1] = 0;
      } }

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
              l_info("TROPHY: unlock-state probe says not present (rc=0x%08X count=%u)",
                     (unsigned)urc, trophy_count);
      } }

    /* ★★ THE SYSTEM IS THE SOURCE OF TRUTH, CHECKED EVERY BOOT (2026-08-01).
     *
     * There WAS a marker file here that recorded "setup already completed once" and
     * suppressed the dialog on later boots. It was wrong: the marker lives in
     * ux0:data/mcsm/ and knows nothing about the trophy store, so after the trophies
     * were deleted from the system the marker still said "done" and registration never
     * happened again. Reported from device: deleted the trophies, second boot did not
     * re-initialise them.
     *
     * The marker only existed because setup used to BLOCK the boot for up to 3600
     * presents, which made re-running it every boot genuinely expensive. Setup is now
     * non-blocking (it is pumped from gl_swap), so the honest thing is also the cheap
     * thing: ask the system every boot, and register whenever it says the set is not
     * there. Present -> nothing happens. Absent for any reason, including the player
     * deleting it -> it comes back.
     *
     * The marker is still WRITTEN on completion, purely as a timestamped record for
     * the log. Nothing reads it to make a decision. */
    /* ★★★ THE PROBE IS NOT TRUSTWORTHY -- ASK THE FILESYSTEM (2026-08-01).
     *
     * sceNpTrophyGetTrophyUnlockState returns 0x80551612 on this system even when the
     * set is fully registered: device-verified with 51 rows in trophy_local.db and all
     * three trophy folders present. So it can NEVER answer "present", and registration
     * was being requested on every single boot. (Harmless -- the system just no-ops --
     * but it is the "trophies re-initialise every reboot" complaint, and it is why the
     * marker file got invented to paper over it.)
     *
     * Registration creates ur0:user/00/trophy/conf/<COMMID>_00 and deleting the
     * trophies removes it. That directory IS the state we are asking about, so testing
     * it directly is both more truthful than the API and correct in the case the marker
     * got wrong: delete the trophies and the folder is gone, so the next boot registers
     * again. Present -> skip. Absent -> register. Nothing cached, nothing to go stale.
     *
     * The API probe is still run first and still believed when it says YES -- it also
     * fills the unlock bitmap, which the folder check cannot. This only adds a second
     * way to say "already there". */
    char trophy_dir[160];
    sceClibSnprintf(trophy_dir, sizeof(trophy_dir),
                    "ur0:user/00/trophy/conf/%s_00", g_comm_id);
    if (!registered && is_dir(trophy_dir)) {
        registered = 1;
        l_info("TROPHY: set already installed (%s exists) — skipping setup", trophy_dir);
    }

    /* Published so the recovery path can open the dialog later with the same
     * marker path, without recomputing it off a stack buffer that is long gone. */
    sceClibSnprintf(g_setup_marker_path, sizeof(g_setup_marker_path),
                    DATA_PATH "trophy_registered_%s.txt", g_comm_id);

    if (!registered) {
        /* ★★ START THE DIALOG, DO NOT SIT ON IT (2026-08-01).
         *
         * This used to spin here until the dialog finished -- up to 3600 presents,
         * which at a 30fps cap is TWO MINUTES of a blocked GL thread, in the middle of
         * the ~30s asset load, on the one boot a new player ever has. A common dialog
         * only advances while frames are presented, and that was the excuse for pumping
         * it inline; but gl_swap already pumps a common dialog every frame for the IME,
         * so the correct place is there. Kick it off and return: the game keeps loading,
         * the dialog composites over it, and mcsm_trophies_setup_poll() finishes the job
         * when the system says it is done. No stall, and no arbitrary frame budget that
         * can expire mid-registration. */
        (void)trophy_open_setup_dialog("not registered at boot");
    }

    l_info("TROPHY: context ready (commId=%s, %u trophies in pack, %s)",
           g_comm_id, trophy_count,
           registered ? "already registered" : "registered this launch");

    g_queue_head = g_queue_tail = g_queue_count = 0u;
    for (unsigned i = 0; i < 4u; i++) {
        atomic_store_explicit(&g_queued_bits[i], 0u, memory_order_relaxed);
        atomic_store_explicit(&g_deferred_bits[i], 0u, memory_order_relaxed);
    }
    g_queue_mutex = sceKernelCreateMutex("mcsm_trp_queue", 0, 0, NULL);
    g_req_sema = sceKernelCreateSema("mcsm_trp_req", 0, 0,
                                     (int)TROPHY_QUEUE_CAP, NULL);
    if (g_queue_mutex < 0 || g_req_sema < 0) {
        l_warn("TROPHY: queue synchronisation creation failed — trophies disabled");
        if (g_req_sema >= 0) sceKernelDeleteSema(g_req_sema);
        if (g_queue_mutex >= 0) sceKernelDeleteMutex(g_queue_mutex);
        g_req_sema = g_queue_mutex = -1;
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
        sceKernelDeleteSema(g_req_sema);
        sceKernelDeleteMutex(g_queue_mutex);
        g_req_sema = g_queue_mutex = -1;
        return -1;
    }
    const int start_rc = sceKernelStartThread(thd, 0, NULL);
    if (start_rc < 0) {
        l_warn("TROPHY: worker thread start failed rc=0x%08X — trophies disabled",
               (unsigned)start_rc);
        sceKernelDeleteThread(thd);
        sceKernelDeleteSema(g_req_sema);
        sceKernelDeleteMutex(g_queue_mutex);
        g_req_sema = g_queue_mutex = -1;
        return start_rc;
    }

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
#endif /* MCSM_HAVE_TROPHY_PACK */
}

int mcsm_trophies_already_unlocked(uint32_t id) {
    if (id >= 128u) return 0;
    return (__atomic_load_n(&g_unlocked.bits[id >> 5], __ATOMIC_ACQUIRE) &
            (1u << (id & 31u))) != 0u;
}

void mcsm_trophies_unlock(uint32_t id) {
    if (!g_available) return;
    if (id >= 128u) { l_warn("TROPHY: id %u out of range", (unsigned)id); return; }
    /* Local bitmap check first: this is what keeps a story beat that re-fires from
     * queueing a redundant blocking unlock every time it plays. The bitmap is seeded
     * at init from the SYSTEM's store, so trophies earned in past sessions count. */
    if (mcsm_trophies_already_unlocked(id)) return;

    const unsigned word = id >> 5;
    const uint32_t mask = 1u << (id & 31u);
    if ((atomic_fetch_or_explicit(&g_queued_bits[word], mask,
                                  memory_order_acq_rel) & mask) != 0u)
        return;

    /* The worker may have completed between the first bitmap check and our queue-bit
     * reservation. Do not enqueue a redundant system call in that narrow window. */
    if (mcsm_trophies_already_unlocked(id)) {
        atomic_fetch_and_explicit(&g_queued_bits[word], ~mask, memory_order_release);
        return;
    }

    int in_ring = 0;
    if (sceKernelTryLockMutex(g_queue_mutex, 1) >= 0) {
        if (g_queue_count < TROPHY_QUEUE_CAP) {
            g_queue[g_queue_tail] = (uint8_t)id;
            g_queue_tail = (g_queue_tail + 1u) % TROPHY_QUEUE_CAP;
            g_queue_count++;
            in_ring = 1;
        }
        sceKernelUnlockMutex(g_queue_mutex, 1);
    }

    /* Queue contention never reaches the script thread. The worker transfers this
     * atomic fallback into the ring before dequeuing its next request. */
    if (!in_ring)
        atomic_fetch_or_explicit(&g_deferred_bits[word], mask, memory_order_release);

    const int signal_rc = sceKernelSignalSema(g_req_sema, 1);
    if (signal_rc < 0) {
        /* With one bit per valid ID and a 128-slot semaphore this cannot overflow.
         * Retain the request so another successful wake can recover it. */
        l_warn("TROPHY: could not wake worker for id=%u rc=0x%08X; request retained",
               (unsigned)id, (unsigned)signal_rc);
    }
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
    /* An explicit mapping always wins, so a wrong derivation can be corrected on
     * device without a rebuild. The map now lives in game.txt as repeated
     * `trophy_map = <name>:<id>` lines; it used to be its own trophies.txt. */
    const int mapped = mcsm_game_trophy_id_for(name);
    if (mapped >= 0) {
        trophy_unlock_resolved(name, mapped, "mapped");
        return;
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
        l_info("TROPHY: achievement \"%s\" is UNMAPPED — add a line "
               "\"trophy_map = %s:<id>\" to ux0:data/mcsm/settings/game.txt", name, name);
}
