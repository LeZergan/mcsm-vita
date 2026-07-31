/*
 * FMOD output plugin for PS Vita (sceAudioOut). This IS the audio path -- the
 * OpenSL ES impersonation it replaced was deleted on 2026-07-29.
 *
 * MODEL: callback-based, polling = 0. FMOD does NOT run a pump on this platform
 * (registered with polling = 1 first, and device counters showed getposition/
 * lock/unlock called exactly zero times while we pushed empty buffers). So our
 * drain thread pulls: readfrommixer runs FMOD's entire mixer INLINE on our
 * thread -- DSP graph, resampling, codec decode -- and we hand the result to
 * sceAudioOutOutput, whose blocking is the clock. Two consequences that are easy
 * to forget and expensive to rediscover: the thread needs a real stack (256KB,
 * not the 16KB it started with), and its priority must not starve the sim.
 *
 * ABI: FMOD Studio 1.06.08, confirmed at runtime by FMOD_System_GetVersion
 * returning 0x00010608. FMOD_OUTPUT_DESCRIPTION below is the 1.x low-level
 * layout for that generation -- 14 members. The 1.10+ redesign does not apply.
 *
 * out_lock / out_unlock / out_getposition are retained because the descriptor
 * layout is fixed, but with polling = 0 FMOD never calls them; their counters
 * are a canary that would catch a silent revert to polling.
 *
 * FAILURE: there is no fallback. If registerOutput or setOutputByPlugin fails
 * the session has no audio, so both paths log that plainly rather than naming a
 * path that no longer exists.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "../utils/logger.h"
#include "../utils/audio_gain.h"

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/mcsm/"
#endif

/* ---- FMOD Studio 1.06 low-level output ABI (see header comment) ---- */
typedef int FMOD_RESULT_T;
#define FMOD_OK_T            0
#define FMOD_ERR_INTERNAL_T  36

typedef struct FMOD_OUTPUT_STATE_T {
    void *plugindata;
    FMOD_RESULT_T (*readfrommixer)(struct FMOD_OUTPUT_STATE_T *, void *, unsigned int);
} FMOD_OUTPUT_STATE_T;

typedef struct {
    const char   *name;
    unsigned int  version;
    int           polling;
    FMOD_RESULT_T (*getnumdrivers)(FMOD_OUTPUT_STATE_T *, int *);
    FMOD_RESULT_T (*getdriverinfo)(FMOD_OUTPUT_STATE_T *, int, char *, short *, int, void *, int *, int *, int *);
    FMOD_RESULT_T (*init)(FMOD_OUTPUT_STATE_T *, int, unsigned int, int *, int *, int *, int *, int, int, void *);
    FMOD_RESULT_T (*start)(FMOD_OUTPUT_STATE_T *);
    FMOD_RESULT_T (*stop)(FMOD_OUTPUT_STATE_T *);
    FMOD_RESULT_T (*close)(FMOD_OUTPUT_STATE_T *);
    FMOD_RESULT_T (*update)(FMOD_OUTPUT_STATE_T *);
    FMOD_RESULT_T (*gethandle)(FMOD_OUTPUT_STATE_T *, void **);
    FMOD_RESULT_T (*getposition)(FMOD_OUTPUT_STATE_T *, unsigned int *);
    FMOD_RESULT_T (*lock)(FMOD_OUTPUT_STATE_T *, unsigned int, unsigned int, void **, void **, unsigned int *, unsigned int *);
    FMOD_RESULT_T (*unlock)(FMOD_OUTPUT_STATE_T *, void *, void *, unsigned int, unsigned int);
} FMOD_OUTPUT_DESCRIPTION_T;

#define FMOD_SOUND_FORMAT_PCM16_T 2

/* ---- ring buffer shared between FMOD's mixer thread and our drain thread ---- */
static struct {
    int16_t     *ring;          /* interleaved PCM16                      */
    unsigned     frames;        /* ring capacity in FRAMES                */
    unsigned     grain;         /* frames handed to sceAudioOut per write */
    int          channels;
    int          rate;
    int          port;
    volatile unsigned play_cursor;   /* frames consumed by hardware  */
    volatile int      running;
    volatile int      started;
    SceUID       thread;
    /* Diagnostics: the chain is FMOD mixer -> lock -> unlock -> our ring ->
     * drain thread -> sceAudioOutOutput. Silence means it breaks somewhere in
     * there; counting each stage says exactly where instead of guessing. */
    volatile unsigned n_lock, n_unlock, n_getpos, n_out, n_outfail;
    volatile int      last_out_rc;
    volatile unsigned nz_frames;   /* non-silent frames seen leaving the ring */
    FMOD_OUTPUT_STATE_T *state;    /* needed to call readfrommixer            */
    volatile unsigned n_read, n_readfail;
    volatile int      thread_exited;
    volatile int      in_mixer;      /* 1 while inside readfrommixer, lock not held */
    unsigned          ring_capacity; /* frames the allocation can hold          */
    SceUID            lock;          /* serialises mixer access vs teardown     */
} g_out;

static int drain_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    /* sceAudioOutOutput BLOCKS until the grain has been consumed, so it is the
     * clock. No sleeping, no polling, no guessing -- the hardware sets the pace
     * and play_cursor advances in lockstep with what was actually heard, which
     * is exactly what getposition must report for FMOD to write ahead correctly. */
    while (g_out.running) {
        /* TEARDOWN RACE (2026-07-29). Previously this snapshotted g_out.state,
         * passed the NULL check, and THEN called into the mixer. FMOD tears its
         * output down and rebuilds it whenever audio is reconfigured -- which is
         * exactly what happens on a load and on entering a menu -- so stop/close
         * could land in that window and FMOD would free the state while we were
         * inside readfrommixer holding the old pointer. Checking the pointer and
         * using it are two separate operations, and nothing made them atomic.
         *
         * Take the lock around BOTH. close() takes the same lock before it clears
         * the state, so a teardown either happens entirely before this iteration
         * or waits for it to finish -- never in the middle of it. The blocking
         * sceAudioOutOutput stays OUTSIDE the lock so teardown is never stuck
         * behind a full buffer of hardware latency. */
        /* Claim the mixer under the lock, then RELEASE it before calling in.
         * Holding it across readfrommixer was a lock-order inversion: this thread
         * held g_out.lock and then took FMOD's internal mixer lock, while FMOD
         * called out_stop/out_close holding that same internal lock and blocked on
         * g_out.lock with an infinite timeout. The in_mixer flag preserves the
         * teardown guarantee without the inversion -- close clears the state under
         * the lock, then waits for in_mixer to drain. */
        if (g_out.lock >= 0) sceKernelLockMutex(g_out.lock, 1, NULL);
        FMOD_OUTPUT_STATE_T *st = g_out.state;
        if (!g_out.started || g_out.port < 0 || !g_out.ring || !st) {
            if (g_out.lock >= 0) sceKernelUnlockMutex(g_out.lock, 1);
            sceKernelDelayThread(5000);
            continue;
        }
        g_out.in_mixer = 1;
        if (g_out.lock >= 0) sceKernelUnlockMutex(g_out.lock, 1);
        /* CALLBACK-BASED OUTPUT (2026-07-29). polling=1 was wrong for this build:
         * device counters showed getposition/lock/unlock called ZERO times while
         * our drain thread happily pushed 264 empty buffers. FMOD never started a
         * pumping thread.
         *
         * That matches how the Android backend actually works, and the old code
         * proves it -- opensl_audio.c needed a feed thread precisely because
         * OpenSL is callback-driven there: the OUTPUT drives FMOD, and FMOD's
         * mixer runs on whatever thread the output hands it. FMOD does not own a
         * pump on this platform, so waiting to be filled waits forever.
         *
         * So pull instead. readfrommixer runs the mixer inline and hands back
         * exactly the frames we ask for, on our thread, immediately before we
         * give them to the hardware. Same relationship as the old feed thread but
         * through FMOD's real API -- and still no fake OpenSL, no guessed sleep
         * (sceAudioOutOutput blocking is the clock), and no cursor negotiation. */
        unsigned chunk = g_out.grain;
        int16_t *src = g_out.ring;
        int pulled = 0;
        if (st->readfrommixer) {
            int rrc = st->readfrommixer(st, g_out.ring, chunk);
            if (rrc == FMOD_OK_T) { g_out.n_read++; pulled = 1; }
            else { g_out.n_readfail++; if (g_out.n_readfail <= 3u) l_warn("FMODOUT: readfrommixer rc=%d", rrc); }
        }
        /* Clearing in_mixer is what makes close()'s drain wait mean anything. A
         * previous edit left a second sceKernelUnlockMutex here instead -- an
         * unlock of a mutex this thread had already released at the top, issued
         * ~47x/second, and the flag was consequently never cleared, so the drain
         * wait always burned its full 400ms and proved nothing. */
        g_out.in_mixer = 0;
        if (!pulled) { sceKernelDelayThread(2000); continue; }
        /* Is the mixer actually giving us signal, or are we faithfully playing
         * silence? Cheap sample of the grain we are about to hand to hardware. */
        for (unsigned q = 0; q < chunk * (unsigned)g_out.channels; q += 64) {
            if (src[q]) { g_out.nz_frames++; break; }
        }
        /* MASTER GAIN. The deleted OpenSL path fed sceAudioOut through
         * audio_output_i16_frames(), which applies audio_gain_q8() -- a 1.25x
         * default plus a 50-200%% override from audio_gain.txt. Writing the ring
         * straight to the port bypassed that, so every build since was ~2dB
         * quieter than the one testers signed off on and the gain file did
         * nothing. sceAudioOutSetVolume is pinned at 0dB, so this is the only
         * volume control the loader has. */
        {
            /* Only ever gain a grain we actually just pulled. Applying it to a
             * stale buffer re-multiplies the same samples every iteration --
             * 1.25^N -- and saturates to a full-scale square wave within about a
             * second. The `continue` above makes that unreachable now; this is
             * belt-and-braces because the failure mode is loud enough to hurt. */
            const int g = pulled ? audio_gain_q8() : 256;
            if (g != 256) {
                const unsigned n = chunk * (unsigned)g_out.channels;
                for (unsigned i = 0; i < n; i++) src[i] = audio_apply_gain_i16(src[i], g);
            }
        }
        int rc = sceAudioOutOutput(g_out.port, (void *)src);
        g_out.last_out_rc = rc;
        if (rc < 0) { g_out.n_outfail++; sceKernelDelayThread(2000); continue; }
        g_out.n_out++;
        g_out.play_cursor += chunk;
    }
    g_out.thread_exited = 1;
    return 0;
}

/* Stop the drain thread and make its state unreachable. Shared by out_close() and
 * out_init()'s double-init guard so there is exactly ONE teardown sequence and the
 * bounds below cannot drift apart again.
 *
 * Every wait here is bounded on purpose. FMOD calls out_close/out_init from its own
 * thread while holding its internal mixer lock, and the drain thread may be inside
 * readfrommixer waiting for that same lock -- so an untimed join is a guaranteed
 * deadlock, not a theoretical one. A stranded thread is recoverable; a hung game is
 * not, so a wait that expires is reported and leaked rather than waited out. */
static void drain_thread_stop(void) {
    /* Clearing these under the lock guarantees no readfrommixer is in flight and
     * none can start: any iteration already inside holds the lock and finishes
     * first. Without this, FMOD could free the state we were mid-call on. */
    if (g_out.lock >= 0) sceKernelLockMutex(g_out.lock, 1, NULL);
    g_out.started = 0;
    g_out.running = 0;
    g_out.state   = NULL;
    if (g_out.lock >= 0) sceKernelUnlockMutex(g_out.lock, 1);

    /* State is now unreachable to NEW iterations; drain any call already inside.
     * readfrommixer runs FMOD's entire mixer inline -- DSP graph traversal,
     * resampling, codec decode -- so on a scene load with the ARM at its floor a
     * single call can run for a long time, and FMOD frees the state as soon as we
     * return. 2s makes this a real barrier; exceeding it is reported, because at
     * that point we are knowingly returning while a reader is live. */
    {
        int waited = 0;
        for (; waited < 1000 && g_out.in_mixer; waited++) sceKernelDelayThread(2000);
        if (g_out.in_mixer) {
            l_error("FMODOUT: readfrommixer STILL in flight after %dms — FMOD is about "
                    "to free state we are inside; expect a fault if this recurs", waited * 2);
        }
    }

    if (g_out.thread >= 0) {
        /* The thread can be inside a blocking sceAudioOutOutput of up to one grain,
         * so let the flag settle first and then make the join the real barrier. */
        for (int i = 0; i < 200 && !g_out.thread_exited; i++) sceKernelDelayThread(2000);
        SceUInt to = 3000000;
        if (sceKernelWaitThreadEnd(g_out.thread, NULL, &to) >= 0) {
            sceKernelDeleteThread(g_out.thread);
        } else {
            l_error("FMODOUT: drain thread would not join — leaking uid 0x%08X rather than hanging",
                    (unsigned)g_out.thread);
        }
        g_out.thread = -1;
    }
}

static FMOD_RESULT_T out_getnumdrivers(FMOD_OUTPUT_STATE_T *s, int *n) { (void)s; if (n) *n = 1; return FMOD_OK_T; }

static FMOD_RESULT_T out_getdriverinfo(FMOD_OUTPUT_STATE_T *s, int id, char *name, short *nameW,
                                       int namelen, void *guid, int *rate, int *mode, int *modech) {
    (void)s; (void)id; (void)nameW; (void)guid;
    if (name && namelen > 0) { strncpy(name, "PS Vita sceAudioOut", (size_t)namelen - 1); name[namelen - 1] = 0; }
    if (rate)   *rate   = 48000;
    if (mode)   *mode   = 3;  /* FMOD_SPEAKERMODE_STEREO */
    if (modech) *modech = 2;
    return FMOD_OK_T;
}

static FMOD_RESULT_T out_init(FMOD_OUTPUT_STATE_T *s, int driver, unsigned int flags, int *outputrate,
                              int *speakermode, int *speakermodechannels, int *format,
                              int dspbufferlength, int dspnumbuffers, void *extra) {
    (void)driver; (void)flags; (void)extra;

    if (g_out.lock < 0) g_out.lock = sceKernelCreateMutex("mcsm_fmod_out", 0, 0, NULL);

    /* DOUBLE-INIT GUARD. FMOD may init again without an intervening close. The
     * old code would then start a SECOND drain thread while the first was live:
     * two threads pulling the same mixer into the same ring and pushing the same
     * audio port. Tear any existing one down first -- through the SAME bounded
     * teardown close() uses. This path used to do its own thing and ended in
     * sceKernelWaitThreadEnd(..., NULL) -- an INFINITE join, the exact deadlock
     * close() was rewritten to avoid: FMOD calls us holding its mixer lock while
     * the drain thread is inside readfrommixer waiting for that same lock. */
    if (g_out.thread >= 0) {
        l_warn("FMODOUT: init with a live drain thread — tearing the old one down first");
        drain_thread_stop();
    }

    int rate = (outputrate && *outputrate > 0) ? *outputrate : 48000;
    int chans = 2;

    /* Honour FMOD's requested DSP buffering: the ring is exactly the span FMOD
     * expects to write ahead into, so its lock offsets map 1:1 onto our frames. */
    unsigned grain  = (dspbufferlength > 0) ? (unsigned)dspbufferlength : 1024u;
    unsigned nbuf   = (dspnumbuffers   > 0) ? (unsigned)dspnumbuffers   : 4u;
    if (grain < 256u)  grain = 256u;
    if (grain > 8192u) grain = 8192u;
    if (nbuf  < 2u)    nbuf  = 2u;

    extern int audio_open_port(int sample_rate, int channels, int desired_frames);
    int actual_grain = audio_open_port(rate, chans, (int)grain);
    if (actual_grain > 0) grain = (unsigned)actual_grain;

    extern int mcsm_audio_port_id(void);
    g_out.port     = mcsm_audio_port_id();
    g_out.grain    = grain;
    g_out.channels = chans;
    g_out.rate     = rate;
    g_out.state    = s;
    /* Allocate once and REUSE across FMOD's close/init cycles. FMOD reconfigures
     * its output at scene transitions -- the stop/close pair seen at boot -- and
     * the previous version freed the ring in close() while the drain thread was
     * still reading it. That use-after-free is what killed the game after the
     * menu loaded: the counters simply stopped mid-session with no close ever
     * logged, because the ring had been freed out from under a live reader.
     * Growing only when a later init asks for more is safe here, since the thread
     * is confirmed stopped before this point. */
    /* Size is only published AFTER the allocation succeeds. Committing frames up
     * front meant a failed realloc left the OLD smaller block paired with the NEW
     * larger count -- and the memset below then wrote past the end of it, with the
     * !g_out.ring guard passing because realloc correctly preserves the old
     * pointer on failure. Compute into locals, publish on success only. */
    unsigned want = grain * nbuf;
    if (nbuf > 16u) { nbuf = 16u; want = grain * nbuf; }   /* clamp: was unbounded above */
    if (!g_out.ring || g_out.ring_capacity < want) {
        int16_t *nring = (int16_t *)realloc(g_out.ring, (size_t)want * chans * sizeof(int16_t));
        if (!nring) {
            l_error("FMODOUT: ring realloc %u frames FAILED — keeping %u", want, g_out.ring_capacity);
            if (!g_out.ring) return FMOD_ERR_INTERNAL_T;
            want = g_out.ring_capacity;                     /* stay within what we own */
        } else {
            g_out.ring = nring; g_out.ring_capacity = want;
        }
    }
    g_out.frames = want;
    /* grain drives every read/write in the drain loop, so it must never exceed
     * what we actually own. It was published above before the allocation, so a
     * failed realloc previously left a large grain against a small ring -- FMOD
     * writing grain frames into a buffer sized for fewer. Clamp it here. */
    if (g_out.grain > g_out.frames) {
        l_warn("FMODOUT: grain %u > ring %u after failed grow — clamping", g_out.grain, g_out.frames);
        g_out.grain = g_out.frames;
    }
    memset(g_out.ring, 0, (size_t)g_out.frames * chans * sizeof(int16_t));
    g_out.play_cursor = 0;

    if (!g_out.ring || g_out.port < 0) {
        l_error("FMODOUT: init FAILED ring=%p port=%d", (void *)g_out.ring, g_out.port);
        return FMOD_ERR_INTERNAL_T;
    }

    if (outputrate)          *outputrate          = rate;
    if (speakermode)         *speakermode         = 3;
    if (speakermodechannels) *speakermodechannels = chans;
    if (format)              *format              = FMOD_SOUND_FORMAT_PCM16_T;

    g_out.running = 1;
    g_out.thread_exited = 0;
    /* PRIORITY: 0x70, not 0x40. 0x40 is SCE_KERNEL_HIGHEST_PRIORITY_USER and was
     * harmless while this thread only blocked in sceAudioOutOutput. It is not
     * harmless now: readfrommixer runs FMOD's whole mixer inline here, so at 0x40
     * that DSP/decode work preempts the sim and render threads (both at the 0xA0
     * pthread default) ~47 times a second on a title that is sim-bound. 0x70
     * still outranks them for latency without owning the CPU outright.
     *
     * STACK SIZE IS LOAD-BEARING (2026-07-29). readfrommixer runs FMOD's ENTIRE
     * mixer inline on this thread -- DSP graph traversal, resampling, codec
     * decode -- which is a deep call chain, not a memcpy. This thread was created
     * with 0x4000 (16KB) and the game began dying shortly after audio started,
     * always somewhere unrelated: rendering carried on, the audio counters went
     * quiet, and ~20s later it fell over in the DLC/network path. That is the
     * signature of a stack overflow corrupting whatever follows it, not of a bug
     * where it appears to happen.
     *
     * The old OpenSL feed thread never hit this because it was a pthread, and
     * pthr.c hands those a 128KB default -- 8x what this had. FMOD was simply
     * being given room it no longer had. 256KB restores headroom over the old
     * path rather than merely matching it, since the mixer depth here is the same
     * code that used to run on the pthread. */
    g_out.thread = sceKernelCreateThread("mcsm_fmod_out", drain_thread, 0x70, 0x40000, 0, 0, NULL);
    int start_rc = (g_out.thread >= 0) ? sceKernelStartThread(g_out.thread, 0, NULL) : -1;
    l_info("FMODOUT: drain thread uid=0x%08X start=0x%08X", (unsigned)g_out.thread, (unsigned)start_rc);
    /* NOTHING pulls the mixer without this thread, so a failure here is total silence.
     * Reporting FMOD_OK anyway (the old behaviour) left FMOD believing it had a working
     * output and the log claiming a successful init -- a silent no-audio session with
     * no evidence of why. Fail the init instead, and undo the state so a later attempt
     * starts clean. */
    if (g_out.thread < 0 || start_rc < 0) {
        l_error("FMODOUT: drain thread would not start (uid=0x%08X start=0x%08X) — "
                "no audio; failing init rather than reporting a working output",
                (unsigned)g_out.thread, (unsigned)start_rc);
        if (g_out.thread >= 0) { sceKernelDeleteThread(g_out.thread); g_out.thread = -1; }
        g_out.running = 0;
        g_out.state = NULL;
        return FMOD_ERR_INTERNAL_T;
    }

    l_info("FMODOUT: init rate=%d ch=%d grain=%u nbuf=%u ring=%u frames port=%d",
           rate, chans, grain, nbuf, g_out.frames, g_out.port);
    return FMOD_OK_T;
}

static FMOD_RESULT_T out_start(FMOD_OUTPUT_STATE_T *s) { (void)s; g_out.started = 1; l_info("FMODOUT: start"); return FMOD_OK_T; }
static FMOD_RESULT_T out_stop (FMOD_OUTPUT_STATE_T *s) {
    (void)s;
    /* Under the lock, so this cannot land mid-mixer-call. */
    if (g_out.lock >= 0) sceKernelLockMutex(g_out.lock, 1, NULL);
    g_out.started = 0;
    if (g_out.lock >= 0) sceKernelUnlockMutex(g_out.lock, 1);
    l_info("FMODOUT: stop");
    return FMOD_OK_T;
}
static FMOD_RESULT_T out_update(FMOD_OUTPUT_STATE_T *s) { (void)s; return FMOD_OK_T; }
static FMOD_RESULT_T out_gethandle(FMOD_OUTPUT_STATE_T *s, void **h) { (void)s; if (h) *h = (void *)(intptr_t)g_out.port; return FMOD_OK_T; }

static FMOD_RESULT_T out_close(FMOD_OUTPUT_STATE_T *s) {
    (void)s;
    l_info("FMODOUT: close ENTER (read=%u out=%u) — stopping drain thread",
           g_out.n_read, g_out.n_out);
    drain_thread_stop();
    /* Deliberately NOT freeing g_out.ring: it is reused by the next init. See the
     * note there -- freeing it here is what crashed the game after menu load. */
    l_info("FMODOUT: close DONE (buffer retained for reuse)");
    return FMOD_OK_T;
}

/* The hardware play cursor, in frames, wrapped to the ring. FMOD subtracts this
 * from its own write cursor to decide how much it may safely produce. */
static FMOD_RESULT_T out_getposition(FMOD_OUTPUT_STATE_T *s, unsigned int *pcm) {
    (void)s;
    g_out.n_getpos++;
    if (pcm) *pcm = g_out.frames ? (g_out.play_cursor % g_out.frames) : 0u;
    return FMOD_OK_T;
}

static FMOD_RESULT_T out_lock(FMOD_OUTPUT_STATE_T *s, unsigned int offset, unsigned int length,
                              void **ptr1, void **ptr2, unsigned int *len1, unsigned int *len2) {
    (void)s;
    if (!g_out.ring || !g_out.frames) return FMOD_ERR_INTERNAL_T;
    g_out.n_lock++;
    offset %= g_out.frames;
    if (length > g_out.frames) length = g_out.frames;
    unsigned first = length;
    if (offset + first > g_out.frames) first = g_out.frames - offset;   /* split at wrap */
    if (ptr1) *ptr1 = g_out.ring + (size_t)offset * g_out.channels;
    if (len1) *len1 = first;
    if (ptr2) *ptr2 = (length > first) ? g_out.ring : NULL;
    if (len2) *len2 = length - first;
    return FMOD_OK_T;
}

/* Nothing to copy: FMOD wrote straight into the ring, and the drain thread reads
 * it from there. Real hardware backends would commit here; we already own the
 * memory the mixer filled. */
static FMOD_RESULT_T out_unlock(FMOD_OUTPUT_STATE_T *s, void *p1, void *p2, unsigned int l1, unsigned int l2) {
    (void)s; (void)p1; (void)p2;
    g_out.n_unlock++;
    if (g_out.n_unlock <= 4u) l_info("FMODOUT: unlock #%u len1=%u len2=%u", g_out.n_unlock, l1, l2);
    return FMOD_OK_T;
}

void mcsm_fmod_output_report(void) {
    if (!g_out.ring && !g_out.n_read) return;   /* never initialised at all */
    l_info("FMODOUT: read=%u readfail=%u out=%u outfail=%u rc=0x%08X nonsilent=%u | lock=%u unlock=%u getpos=%u started=%d",
           g_out.n_read, g_out.n_readfail, g_out.n_out, g_out.n_outfail,
           (unsigned)g_out.last_out_rc, g_out.nz_frames,
           g_out.n_lock, g_out.n_unlock, g_out.n_getpos, g_out.started);
}

static FMOD_OUTPUT_DESCRIPTION_T g_desc = {
    "PS Vita sceAudioOut (MCSM loader)",
    0x00010608u,          /* built against FMOD Studio 1.06.08, as reported by the live system */
    0,                    /* callback-based: WE pull via readfrommixer. polling=1 was tried
                           * first and FMOD never called getposition/lock/unlock even once. */
    out_getnumdrivers, out_getdriverinfo, out_init, out_start, out_stop, out_close,
    out_update, out_gethandle, out_getposition, out_lock, out_unlock,
};


/* Returns 1 if the engine is now driven by this backend; 0 means no audio. */
int mcsm_fmod_output_install(void *low_level_system,
                             int (*reg)(void *, const void *, unsigned int *),
                             int (*setbyplugin)(void *, unsigned int)) {
    if (!low_level_system || !reg || !setbyplugin) {
        l_warn("FMODOUT: cannot install (system=%p reg=%p set=%p)", low_level_system, (void *)reg, (void *)setbyplugin);
        return 0;
    }
    /* Only initialise these ONCE. Blanking them unconditionally orphaned a live
     * drain thread on a second Studio::initialize: out_init's double-init guard
     * tests thread >= 0, so it saw nothing to tear down and started a SECOND
     * thread pulling the same mixer into the same ring; and lock = -1 leaked the
     * mutex while every `if (g_out.lock >= 0)` site silently became a no-op,
     * disabling the very teardown protection this file adds. */
    static int s_once = 0;
    if (!s_once) { s_once = 1; g_out.port = -1; g_out.thread = -1; g_out.lock = -1; }

    unsigned int handle = 0;
    int rc = reg(low_level_system, &g_desc, &handle);
    if (rc != FMOD_OK_T) {
        l_error("FMODOUT: registerOutput FAILED rc=%d — NO AUDIO this session (no fallback exists)", rc);
        return 0;
    }
    int rc2 = setbyplugin(low_level_system, handle);
    if (rc2 != FMOD_OK_T) {
        l_error("FMODOUT: setOutputByPlugin FAILED rc=%d handle=%u — NO AUDIO this session (no fallback exists)", rc2, handle);
        return 0;
    }
    l_info("FMODOUT: INSTALLED — engine now on a real FMOD output plugin (handle=%u), "
           "no OpenSL impersonation, no feed thread", handle);
    return 1;
}
