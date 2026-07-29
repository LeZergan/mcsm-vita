/*
 * A REAL FMOD output plugin for PS Vita (sceAudioOut).
 *
 * This is the mechanism FMOD actually provides for adding a platform backend --
 * the same one console ports use -- and it replaces the ~900-line impersonation
 * of Android's OpenSL ES in opensl_audio.c.
 *
 * HOW THE OLD PATH WORKED, AND WHY THIS IS DIFFERENT
 * Previously the loader forced FMOD to its ANDROID OpenSL backend and then faked
 * the entire OpenSL ES object model so that backend would hand us PCM. FMOD
 * believed it was on Android; we caught the buffers on the way out and wrote
 * them to sceAudioOut inline, from FMOD's own mixer thread, with a helper thread
 * spinning to pump the callback. Nothing about that was FMOD's intended flow.
 *
 * Here FMOD is told the truth: a new output backend exists. Registered with
 * polling = 1, FMOD runs its mixer on its own thread and asks US where the
 * hardware play cursor is (getposition), then locks a span of the ring buffer,
 * writes into it, and unlocks. That is the standard ring-buffer output contract,
 * identical in shape to FMOD's DirectSound/console backends.
 *
 * The consequences are the point:
 *   - No fake OpenSL objects, vtables or interface whitelist.
 *   - No feed thread. FMOD paces itself off the real play cursor, which is what
 *     its mixer was designed around, instead of us guessing with a sleep.
 *   - Enqueue is no longer synchronous. FMOD writes into the ring and returns;
 *     draining to hardware happens on our thread, so the mixer never blocks on
 *     sceAudioOutOutput.
 *   - Buffer occupancy is real, not a permanent "queue is empty" lie.
 *
 * ABI
 * FMOD_OUTPUT_DESCRIPTION is version sensitive. FMOD_System_GetVersion on the
 * live system returned 0x00010608 = FMOD Studio 1.06.08, and the struct below is
 * the FMOD Studio 1.x low-level layout for that generation: 14 members, polling
 * model with lock/unlock/getposition. The 1.10+ redesign (mixer callbacks,
 * different FMOD_OUTPUT_STATE) does NOT apply here.
 *
 * ROLLOUT
 * Opt-in. fmod_output.txt must say "plugin"; anything else keeps the existing
 * OpenSL path untouched. Registration failure falls back rather than crashing,
 * so a wrong guess costs a log line and silence-free normal audio, not a boot
 * loop. Once this is proven on device, opensl_audio.c can be deleted.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "../utils/logger.h"

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
} g_out;

static int drain_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    /* sceAudioOutOutput BLOCKS until the grain has been consumed, so it is the
     * clock. No sleeping, no polling, no guessing -- the hardware sets the pace
     * and play_cursor advances in lockstep with what was actually heard, which
     * is exactly what getposition must report for FMOD to write ahead correctly. */
    while (g_out.running) {
        if (!g_out.started || g_out.port < 0 || !g_out.ring) {
            sceKernelDelayThread(5000);
            continue;
        }
        unsigned pos = g_out.play_cursor % g_out.frames;
        /* Never straddle the wrap in a single write; the tail is picked up next
         * iteration, which keeps the pointer arithmetic trivially correct. */
        unsigned chunk = g_out.grain;
        if (pos + chunk > g_out.frames) chunk = g_out.frames - pos;
        int rc = sceAudioOutOutput(g_out.port, g_out.ring + (size_t)pos * g_out.channels);
        if (rc < 0) { sceKernelDelayThread(2000); continue; }
        g_out.play_cursor += chunk;
    }
    return 0;
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
    (void)s; (void)driver; (void)flags; (void)extra;

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
    g_out.frames   = grain * nbuf;
    g_out.ring     = (int16_t *)calloc((size_t)g_out.frames * chans, sizeof(int16_t));
    g_out.play_cursor = 0;

    if (!g_out.ring || g_out.port < 0) {
        l_error("FMODOUT: init FAILED ring=%p port=%d", (void *)g_out.ring, g_out.port);
        free(g_out.ring); g_out.ring = NULL;
        return FMOD_ERR_INTERNAL_T;
    }

    if (outputrate)          *outputrate          = rate;
    if (speakermode)         *speakermode         = 3;
    if (speakermodechannels) *speakermodechannels = chans;
    if (format)              *format              = FMOD_SOUND_FORMAT_PCM16_T;

    g_out.running = 1;
    g_out.thread = sceKernelCreateThread("mcsm_fmod_out", drain_thread, 0x40, 0x4000, 0, 0, NULL);
    if (g_out.thread >= 0) sceKernelStartThread(g_out.thread, 0, NULL);

    l_info("FMODOUT: init rate=%d ch=%d grain=%u nbuf=%u ring=%u frames port=%d",
           rate, chans, grain, nbuf, g_out.frames, g_out.port);
    return FMOD_OK_T;
}

static FMOD_RESULT_T out_start(FMOD_OUTPUT_STATE_T *s) { (void)s; g_out.started = 1; l_info("FMODOUT: start"); return FMOD_OK_T; }
static FMOD_RESULT_T out_stop (FMOD_OUTPUT_STATE_T *s) { (void)s; g_out.started = 0; l_info("FMODOUT: stop");  return FMOD_OK_T; }
static FMOD_RESULT_T out_update(FMOD_OUTPUT_STATE_T *s) { (void)s; return FMOD_OK_T; }
static FMOD_RESULT_T out_gethandle(FMOD_OUTPUT_STATE_T *s, void **h) { (void)s; if (h) *h = (void *)(intptr_t)g_out.port; return FMOD_OK_T; }

static FMOD_RESULT_T out_close(FMOD_OUTPUT_STATE_T *s) {
    (void)s;
    g_out.started = 0;
    g_out.running = 0;
    if (g_out.thread >= 0) { sceKernelWaitThreadEnd(g_out.thread, NULL, NULL); sceKernelDeleteThread(g_out.thread); g_out.thread = -1; }
    free(g_out.ring); g_out.ring = NULL;
    l_info("FMODOUT: close");
    return FMOD_OK_T;
}

/* The hardware play cursor, in frames, wrapped to the ring. FMOD subtracts this
 * from its own write cursor to decide how much it may safely produce. */
static FMOD_RESULT_T out_getposition(FMOD_OUTPUT_STATE_T *s, unsigned int *pcm) {
    (void)s;
    if (pcm) *pcm = g_out.frames ? (g_out.play_cursor % g_out.frames) : 0u;
    return FMOD_OK_T;
}

static FMOD_RESULT_T out_lock(FMOD_OUTPUT_STATE_T *s, unsigned int offset, unsigned int length,
                              void **ptr1, void **ptr2, unsigned int *len1, unsigned int *len2) {
    (void)s;
    if (!g_out.ring || !g_out.frames) return FMOD_ERR_INTERNAL_T;
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
    (void)s; (void)p1; (void)p2; (void)l1; (void)l2;
    return FMOD_OK_T;
}

static FMOD_OUTPUT_DESCRIPTION_T g_desc = {
    "PS Vita sceAudioOut (MCSM loader)",
    0x00010608u,          /* built against FMOD Studio 1.06.08, as reported by the live system */
    1,                    /* polling: FMOD runs its own mixer thread and paces off getposition */
    out_getnumdrivers, out_getdriverinfo, out_init, out_start, out_stop, out_close,
    out_update, out_gethandle, out_getposition, out_lock, out_unlock,
};

int mcsm_fmod_output_wanted(void) {
    FILE *fp = fopen(DATA_PATH "fmod_output.txt", "r");
    if (!fp) return 0;
    char buf[64] = {0};
    char *l = fgets(buf, sizeof(buf), fp);
    fclose(fp);
    return (l && (strstr(l, "plugin") || strstr(l, "PLUGIN"))) ? 1 : 0;
}

/* Returns 1 if the engine is now driven by this backend, 0 to keep the old path. */
int mcsm_fmod_output_install(void *low_level_system,
                             int (*reg)(void *, const void *, unsigned int *),
                             int (*setbyplugin)(void *, unsigned int)) {
    if (!low_level_system || !reg || !setbyplugin) {
        l_warn("FMODOUT: cannot install (system=%p reg=%p set=%p)", low_level_system, (void *)reg, (void *)setbyplugin);
        return 0;
    }
    g_out.port = -1;
    g_out.thread = -1;

    unsigned int handle = 0;
    int rc = reg(low_level_system, &g_desc, &handle);
    if (rc != FMOD_OK_T) {
        l_warn("FMODOUT: registerOutput FAILED rc=%d — keeping the OpenSL path", rc);
        return 0;
    }
    int rc2 = setbyplugin(low_level_system, handle);
    if (rc2 != FMOD_OK_T) {
        l_warn("FMODOUT: setOutputByPlugin FAILED rc=%d handle=%u — keeping the OpenSL path", rc2, handle);
        return 0;
    }
    l_info("FMODOUT: INSTALLED — engine now on a real FMOD output plugin (handle=%u), "
           "no OpenSL impersonation, no feed thread", handle);
    return 1;
}
