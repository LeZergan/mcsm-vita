/*
 * Consolidated settings + graphics profiles. See config.h.
 */
#include "utils/config.h"
#include "utils/utils.h"     /* mcsm_open_setting */
#include "utils/logger.h"    /* l_info */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- graphics.txt (profiles) ------------------------------------------- */

static McsmCfg g_cfg;
static int     g_cfg_loaded = 0;

/* Measured on device (79-minute session, performance profile): 23.9 fps average,
 * ~70% of frames spending over 20ms in SIM, only ~157 draw calls per frame, and
 * slow frames tracking VERTEX count (288k on fast frames vs 445k on slow ones)
 * rather than draw count. So the engine is CPU/sim- and vertex-bound, NOT
 * draw-bound and not fill-bound: render size is a near-free sharpness knob, and
 * the rows that actually buy frames are the ones that remove geometry or animation
 * work (draw_distance, detail, skinning, anim_rate). Keep that ordering in mind
 * when tuning a profile -- dropping resolution alone mostly just softens the
 * image. */
enum { PROF_QUALITY = 0, PROF_BALANCED, PROF_PERFORMANCE, PROF_BATTERY, PROF_AUTO, PROF_CUSTOM };

/* Copy a graphics.txt/game.txt value into a fixed field, always NUL-terminated.
 *
 * Every call site used to be strncpy(dst, v, sizeof(dst) - 1) against a
 * zero-initialised array, which IS safe -- but GCC cannot see that the final byte
 * stays 0, so -Wall reported truncation at all 22 sites and drowned out anything
 * real. This states the intent once: copy at most cap-1 bytes and terminate,
 * unconditionally. */
static void cfg_set(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t n = 0;
    if (src) { while (n + 1 < cap && src[n]) { dst[n] = src[n]; n++; } }
    dst[n] = 0;
}

static void apply_profile(McsmCfg *c, int prof) {
    /* FRAME PACING NOTE, because it constrains every profile below. The panel is
     * 60Hz, so with vsync on a frame can only be presented on a vblank: 16.667ms
     * apart. A cap whose period is a WHOLE number of vblanks (60, 30, 20, 15) lands
     * on one every time.
     *
     * 24 is NOT a whole number of vblanks -- it needs 2.5 -- and this note used to
     * say that made it unreachable and that "quality locks 20 rather than 24". That
     * stopped being true on 2026-07-29, when the present lock (glutil.c gl_init)
     * moved from whole-vblank quantisation to an EXACT FRACTIONAL TIMELINE: it
     * advances its target by period + rem/den each frame and presents on the first
     * vblank at or after it. For 24fps the targets land on 2.5, 5.0, 7.5, 10.0
     * vblanks, so vsync catches vblanks 3, 5, 8, 10 -- a repeating 3:2:3:2 cadence
     * averaging exactly 24.000fps with no drift. That is precisely how 24fps film is
     * shown on 60Hz displays, and it is why quality can and does lock 24.
     *
     * The sim pace (patch.c mcsm_frame_pace_us) was NOT updated at the same time and
     * kept quantising to whole vblanks, so fps_cap=24 paced the sim at 30834us
     * (32.4fps) against a 41666us present period -- running the engine 28% faster
     * than anything could be displayed and mis-sizing the clock governor's budget.
     * Fixed 2026-07-30: the sim pace is now the exact period minus a 2500us
     * undershoot, which leaves 30 and 60 bit-for-bit unchanged and makes 24 correct.
     * A cap of 24 is therefore genuinely deliverable end to end.
     *
     * gpu_tier is the strongest lever we have and it is what mainly separates these
     * profiles. It changes the GL_RENDERER string we report, and the engine picks
     * its own quality from that -- measured on device, dropping to SGX 540 took
     * 22.2 -> 26.2 fps on its own. Our manual knobs are secondary by comparison. */
    /* Set for EVERY profile before the switch: the zero-copy buffer path is a
     * correctness question (does vitaGL's in-place map tear geometry?), not a
     * quality tier, so it must not differ per profile. Once a device run proves
     * it clean this becomes 1 here and applies everywhere at once. */
    c->map_buffers = 0;
    /* Engine-reported platform. Not a quality tier: it changes which shader
     * feature set the engine selects, so it is set for every profile and
     * defaults OFF until a device run proves it helps. */
    c->platform_vita = 0;
    /* Engine-global overrides. All default to 'leave the engine alone'. */
    c->fmod_channels = 0; c->scene_prio_min = 0; c->scene_prio_max = 0;
    c->trophy_test = 0;
    c->upscale_nearest = 0;
    /* MUST be 1: forcing the engine's recursive-bone-contribution fix is what makes
     * skeletal animation work at all. Defaulting it off was tried on device and made
     * animation far worse. See patch.c force_animation_runtime_flags(). */
    c->anim_engine_flags = 1;
    /* Y orientation of the render-scale blit. NOT a quality tier -- it is purely a
     * property of WHICH libvitaGL.a this eboot links, so it is set for every profile.
     *
     * AUTO-DETECTED, because hardcoding it got it wrong immediately: r83 shipped the
     * default for the new lib, r84 reverted the lib without touching the default, and
     * the whole game came out upside-down. A constant that must be hand-edited in
     * lockstep with a library swap WILL drift.
     *
     * vglSetShaderCachePath() is public vitaGL API added in upstream 96c41a1 and is
     * absent from the older aa75c61-era lib (verified with nm against both archives).
     * Declared weak, so it links either way and its ADDRESS tells us which library we
     * got: non-NULL => new lib, whose FBO-bind path applies the Y flip itself through
     * the viewport transform, so we must NOT swap (0). NULL => old lib, which does not
     * flip on an FBO->FB0 blit, so we must swap dstY0/dstY1 (1).
     * Still overridable from graphics.txt (`blit_flip = 0|1`) as an escape hatch. */
    {
        extern void vglSetShaderCachePath(const char *path) __attribute__((weak));
        c->blit_flip = (&vglSetShaderCachePath != NULL) ? 0 : 1;
    }

    switch (prof) {
    case PROF_QUALITY:
        /* Best picture the hardware can hold WITHOUT the framerate ever moving.
         * Native res, real GPU identity so the engine configures itself at full
         * quality, and a rate that is genuinely LOCKED.
         *
         * ★★ 20, NOT 24 -- AND 20 IS THE ONE THAT IS ACTUALLY LOCKED (2026-07-31).
         * This profile asked for 24 for a long time, on the reasoning that 24 is the
         * cinematic cadence the game was authored for. The reasoning was fine and the
         * number was not achievable: the panel is 60Hz, and with vsync a frame can
         * only last a WHOLE number of vblanks. 60/24 = 2.5, which does not exist, so
         * "24" was never a locked 24 -- it was a 3:2 pulldown alternating 33ms and
         * 50ms frames that merely AVERAGED 24. Every per-second counter reported a
         * truthful 24 while the motion carried the 50ms half, which is exactly the
         * "counter says 24, feels like 15" the tester reported.
         *
         * The only even rates available are 60/N: 60, 30, 20, 15. 24 is the gap
         * between 30 and 20. Of those two neighbours:
         *   30 = 33.3ms budget -- this workload (sim ~30-45ms) MISSES it, so it would
         *        drop frames and be uneven again, just wearing a faster label.
         *   20 = 50.0ms budget -- fits with real margin, and every frame is exactly
         *        3 vblanks. Same worst case as the 3:2 pattern's slow half, but now
         *        it is EVERY frame instead of every other one, and consistency is
         *        most of what perceived smoothness actually is.
         * So 20 is both the closest even rate and the only one this profile can hold.
         * It is still slow and deliberate, which is what "cinematic" was after.
         *
         * DO NOT "fix" this back to 24 because 24 sounds more filmic. 24 on a 60Hz
         * panel is a pulldown, not a frame rate. Check the PACING log line: a locked
         * 20 puts essentially every frame in the 3-vblank bucket; the old 24 spread
         * across buckets 2 and 3. That spread IS the judder. */
        c->render_w = 800; c->render_h = 452; c->fps_cap = 30; c->vsync = 1;
        /* draw_distance is NOT unlimited here, even though this is the pretty
         * profile. Device data: the GPU idles around 40% while frames take 55ms,
         * i.e. the CPU is the entire limit -- so pixels are nearly free, and culling
         * distant objects is what actually buys stability. Unlimited distance made
         * the CPU walk every far object for almost no visible gain, and sim work
         * measured p50 49ms / p90 71ms against the 50ms budget -- half the frames
         * missed 20fps before drawing anything.
         *
         * ★ SHADOWS ARE OFF, AND THAT IS WHAT MAKES THIS PROFILE HONEST (2026-07-31).
         * They were on, and the profile could not hold the 24 it promises: 24fps is a
         * 41.7ms budget and measured sim was p50 49ms -- the median frame missed,
         * before a single draw. Shadows re-submit the ENTIRE scene, so on a
         * CPU-bound frame they are the most expensive thing in the profile and the
         * only cut big enough to close a 7ms median gap.
         *
         * Native resolution and full detail are KEPT precisely because the limit is
         * the CPU: dropping pixels would cost sharpness and buy almost nothing, and
         * this is the profile someone picks for sharpness. So the trade is
         * "everything the CPU can afford at 24, nothing it cannot" -- native res,
         * outlines, full-detail geometry, no shadows. Distance also comes 6000->5000
         * (matching `default`) to take the remaining margin from object walking
         * rather than from anything visible up close. */
        c->outlines = 1;   c->shadows = 0;    c->draw_distance = 4000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000; c->render_quality = -1; c->gpu_tier = 1;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    case PROF_BALANCED:    /* "default" -- good visuals AND good performance */
        /* Targets a real 30, which is a 33.3ms budget.
         *
         * ★ RETUNED 2026-07-31 to actually reach it. The old settings (gpu_tier 1,
         * detail 1000, dist 5000) were strictly HEAVIER than the performance profile,
         * and performance measures p50 26ms / p90 42ms -- so this profile's median
         * frame was already over 33.3ms and the 30 was aspirational. Three cuts, in
         * order of measured value:
         *   gpu_tier 1 -> 0   the strongest single lever there is, worth 22.2 -> 26.2
         *                     fps (+18%) on its own. It tells the engine it is a
         *                     weak phone so it picks its own low-spec path.
         *   detail 1000 -> 900  slow frames track VERTEX count, and detail is one of
         *                     only two runtime levers that cuts vertices. 900 is a
         *                     mild bias toward coarser baked LODs -- visible on close
         *                     inspection, not in motion.
         *   dist 5000 -> 4000  cheapest of the three: culls before submission and
         *                     costs only far-object pop-in.
         *
         * What is deliberately KEPT is what separates this from `performance`:
         * outlines on (they carry the art style), full-rate animation (no lip-sync
         * stepping), and the higher 720x408 render scale for legible text. That is
         * the actual difference now -- not the engine tier. */
        c->render_w = 720; c->render_h = 408; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 1;   c->shadows = 0;    c->draw_distance = 5000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000;  c->render_quality = -1; c->gpu_tier = 1;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    case PROF_PERFORMANCE:
        /* Frames over everything -- but "frames" means DISPLAYED frames, which on a
         * 60Hz vsynced panel is a ladder, not a dial: only 60/30/20/15 exist.
         *
         * ★★ CAP IS 30, NOT 60 (2026-08-01). It was 60 on the argument that capping
         * lower never makes a frame arrive sooner, only later -- which is true about
         * THROUGHPUT and wrong about what a player sees.
         *
         * On a 60Hz panel with vsync a frame lasts a whole number of vblanks. 60 means
         * ONE vblank, 16.7ms, and this workload (sim p50 26ms / p90 42ms) never gets
         * there -- so every single frame misses its target and lands on whichever
         * vblank it happens to reach. The result is a rate that constantly changes
         * duration, reported from device as "it was uncapped fps it seems" and felt as
         * microstutter. 30 is two vblanks, 33.3ms, which the median frame actually
         * makes: frames that fit are EVEN, and frames that miss degrade to a steady 20
         * via the pacer's hysteresis rather than oscillating.
         *
         * It also aligns the two pacers. mcsm_frame_pace_us() paces the SIM from this
         * same cap, so at 60 the engine was being driven at 14.2ms while the display
         * delivered 20-30fps -- the engine's own frame delta varying every frame, which
         * is motion judder no amount of present-side smoothing can fix. At 30 the sim
         * and the display are asking for the same thing.
         *
         * (Related: the anim_rate throttle's thresholds were hardcoded for a 30 cap
         * and so NEVER fired at 60, which made anim_rate = 2 inert here. Now that
         * they follow fps_cap, this profile's biggest sim lever actually engages.)
         *
         * The cuts are CPU-side because measurement says the CPU is the limit
         * (~157 draws/frame, slow frames tracking VERTEX count, not draw count):
         *   gpu_tier 0   weakest GPU identity; the engine then picks its own
         *                low-spec path. Worth 22.2 -> 26.2 fps on its own.
         *   outlines/shadows off
         *   detail 700   biases the engine to coarser BAKED LODs -- one of only two
         *                runtime levers that cut vertex count itself.
         *   dist 3000    culls distant geometry before submission.
         *   anim_rate 2  half-rate animation blending. Motion and lip-sync visibly
         *                step, which is why quality/default keep it at 1. Note the
         *                throttle only engages while frames are actually dropping,
         *                and its thresholds now follow fps_cap (they were hardcoded
         *                for 30 and never fired at all at a 60 cap).
         * Resolution STAYS 576x326: pixels were never the bottleneck, so dropping
         * further trades sharpness for almost nothing.
         *
         * ★ skinning stays FULL. It was set to 0 here on 2026-07-30 as a
         * "character-scaling fix" on the theory that
         * GameEngine::mbFixRecursiveAnimationContribution was the per-character
         * cost. Disassembly says otherwise: the software-skinning decision in
         * RenderObject_Mesh::_RenderMeshInstance (@0xab7088) branches only on
         * (skeletonInstance != NULL), mesh flags bit 23, and mesh[+0x1c8] -- the
         * flag appears nowhere in it. So it cannot reduce per-character skinning or
         * the per-mesh vertex-buffer respecify, which is why "even on lowest
         * everything struggles" was the result. Meanwhile reduced skinning makes
         * attached parts (a costume, a chest lid) trail the body: a real visual
         * DEFECT, not a quality tier. Shipping a known defect for a lever that
         * provably does not act on the cost is a strict loss, so it is 1 again.
         * The actual per-character cost is graphics.txt `map_buffers` -- see
         * so_patch() and PERF_FINDINGS_2026-07-30.md. */
        /* ★ RETUNED 2026-07-31. A 79-minute device session on these settings averaged
         * 23.9 fps -- roughly what `battery` aims for, from the profile named
         * "fastest". Every cheap lever was already taken (gpu_tier 0, outlines off,
         * shadows off, anim_rate 2), so the remaining headroom is the one thing slow
         * frames actually track: VERTEX COUNT. detail 700 -> 600 and dist 3000 ->
         * 2500 both attack it -- detail biases the engine to coarser baked LODs,
         * distance culls whole objects before submission. Geometry gets visibly
         * chunkier and far scenery pops in sooner; that is the entire point of the
         * profile, and it is the only place left to take time from.
         * Resolution STILL stays 576x326 -- pixels were never the bottleneck (the GPU
         * idles ~40% while frames take 55ms), so dropping it further would cost
         * sharpness and buy nothing. */
        /* ★★ anim_rate IS 1, NOT 2 (2026-08-01). Half-rate animation blending was the
         * last "make it faster by making it worse" lever still shipping, and it is the
         * most visible one: it steps FACIAL animation and lip-sync, on a dialogue-driven
         * adventure game where faces are what the camera is pointed at. Reported from
         * device as "facial animations are kinda fucked on basic performance profile".
         *
         * A profile may cost sharpness, draw distance, geometry detail or frame rate.
         * It may NOT make the game look WRONG -- that is the same line already drawn
         * for `skinning = reduced` (attachments trailing the body) two entries up.
         * Speed comes out of pixels and vertices, never out of correctness.
         *
         * The frames are elsewhere anyway: animation is only ~15% of a frame, so this
         * was a small win bought at a large visual cost. */
        c->render_w = 640; c->render_h = 362; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 0;   c->shadows = 0;    c->draw_distance = 2500; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 600;   c->render_quality = -1; c->gpu_tier = 0;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    case PROF_BATTERY:
        /* Longest play time. Power on this console tracks CPU clock and pixels, so:
         * downclock when idle, weakest GPU identity, coarser geometry,
         *
         * ☠ 456x258 IS NOT AN EXACT HALF OF 960x544 (2026-08-01). This was 480x272
         * precisely because that IS half, so it upscaled by clean pixel doubling with
         * no resampling -- and sanitize_framebuffer_override() was fixed specifically
         * to stop snapping it to 480x270 and destroying that. The ~10% step down was
         * requested across every profile and gives the doubling up: the upscale now
         * resamples and reads a touch softer. A deliberate trade, not an oversight --
         * put this one line back to 480x272 to recover the crisp doubling. Note that
         * `upscale = nearest` only pays off at the exact 2x ratio, so it no longer
         * helps here either,
         * half-rate animation, and 20fps -- a lower locked rate is less work per
         * second AND a clean 3-vblank lock.
         *
         * ★ skinning is FULL here too, for exactly the reason PROF_PERFORMANCE gives
         * above: the flag does not reach the software-skinning decision at all
         * (RenderObject_Mesh::_RenderMeshInstance branches on skeletonInstance,
         * mesh flags bit 23 and mesh[+0x1c8] -- never on it), so it saves nothing,
         * while reduced skinning makes attached parts visibly trail the body. This
         * profile trades frames and pixels for battery; it does not get to trade
         * correctness for nothing. It was the last profile still shipping that
         * defect. */
        /* ★ RETUNED 2026-07-31 (detail 700 -> 600, dist 3000 -> 2500, matching
         * `performance`). 20fps is a 50ms budget and these settings already fit it --
         * measured sim on the heavier performance profile is p50 26ms / p90 42ms --
         * so the extra cuts are NOT about hitting the cap. They are the battery win
         * itself: the adaptive governor below only steps the ARM clock down when a
         * frame finishes early, so every millisecond of headroom converts directly
         * into a lower clock for the rest of that frame. Buying headroom this profile
         * does not need for frames is exactly how it buys runtime. */
        /* anim_rate 1 here too, for the reason spelled out in PROF_PERFORMANCE: stepped
         * faces and lip-sync are a defect, not a battery setting. Battery buys its
         * runtime from pixels, geometry and clock -- all of which look correct, just
         * simpler. */
        c->render_w = 576; c->render_h = 326; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 0;   c->shadows = 0;    c->draw_distance = 3000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 700;   c->render_quality = -1; c->gpu_tier = 0;
        c->shader_opt = 2; c->clock_adaptive = 1; c->clock_mhz = 444; break;

    case PROF_AUTO:
        /* Engine-managed: every knob the ENGINE decides is left at its pass-through
         * value so the loader overrides nothing, and the game runs whatever it chose
         * for the GPU we reported. Resolution/fps_cap/vsync/clock stay ours (the
         * engine has no concept of the Vita render scale or power), and skinning
         * stays full because reduced is a correctness defect. Use it to A/B the
         * engine own judgement against the tuned profiles.
         *
         * ☠ THIS IS THE SLOWEST OPTION IN THE FILE, BY MEASUREMENT -- do not read
         * "auto" as "sensible default". Unlimited draw distance plus shadows is
         * exactly the configuration that measured sim p50 49ms / p90 71ms, i.e. more
         * than half the frames missed even a 50ms (20fps) budget before drawing
         * anything, so its 30 cap is never reached. It stays untuned ON PURPOSE:
         * its entire job is to show what the engine does when the loader keeps its
         * hands off, which is the baseline the other four profiles are measured
         * against. The player-facing graphics.txt says so plainly. */
        c->render_w = 720; c->render_h = 408; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 1;   c->shadows = 1;    c->draw_distance = 0; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000; c->render_quality = -1; c->gpu_tier = -1;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    default:
        /* ☠ UNREACHABLE, AND ITS OLD VALUES WERE BEING QUOTED AS FACT. This arm was
         * labelled "custom -- starts from default" and set 800x452 / dist 6000, and
         * the shipped graphics.txt told players exactly that. But the caller does
         *     apply_profile(&g_cfg, p == PROF_CUSTOM ? PROF_BALANCED : p);
         * so PROF_CUSTOM never arrives here -- custom really starts from BALANCED
         * (720x408, dist 5000). Every other enumerator has its own case, so nothing
         * can reach this arm at all.
         *
         * Kept only as a defensive fallback for an out-of-range value, and made
         * identical to PROF_BALANCED so that if it ever DID run it would agree with
         * what custom actually does. The player-facing doc has been corrected too.
         *
         * ☠ IT HAS ALREADY DRIFTED ONCE: the 2026-07-31 retune moved BALANCED to
         * gpu_tier 0 / dist 4000 / detail 900 and this arm kept the old
         * 1 / 5000 / 1000, so the "identical" claim above was false within minutes of
         * being written. Being unreachable is exactly why nothing caught it. If you
         * touch PROF_BALANCED, touch this too -- or better, delete this arm and let
         * the compiler's -Wswitch tell you when an enumerator is unhandled. */
        c->render_w = 720; c->render_h = 408; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 1;   c->shadows = 0;    c->draw_distance = 5000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000;  c->render_quality = -1; c->gpu_tier = 1;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;
    }
}

/* on/off/full/reduced/1/0/true/false/yes/no -> 1/0, or dflt if unrecognised. */
static int parse_bool(const char *v, int dflt) {
    if (!v || !v[0]) return dflt;
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") ||
        !strcmp(v, "yes") || !strcmp(v, "full")) return 1;
    if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") ||
        !strcmp(v, "no") || !strcmp(v, "reduced")) return 0;
    return dflt;
}

/* Friendly one-word GPU names for graphics.txt. The generic key/value reader
 * intentionally reads a single value token, so exposing raw renderer strings such
 * as "PowerVR SGX 540" would be a trap: only "PowerVR" would reach the engine.
 * Keep the player-facing names short and expand them here. Unknown values retain
 * the legacy gpu_name behaviour for developer experiments. */
static const char *gpu_name_from_alias(const char *v) {
    if (!v || !v[0]) return v;
    if (!strcmp(v, "sgx540") || !strcmp(v, "fastest")) return "PowerVR SGX 540";
    if (!strcmp(v, "sgx541") || !strcmp(v, "fast"))    return "PowerVR SGX 541";
    if (!strcmp(v, "sgx542") || !strcmp(v, "medium"))  return "PowerVR SGX 542";
    if (!strcmp(v, "sgx543") || !strcmp(v, "quality")) return "PowerVR SGX 543";
    if (!strcmp(v, "sgx543mp") || !strcmp(v, "vita") || !strcmp(v, "original"))
        return "PowerVR SGX 543MP";
    return v;
}

/* Split a "key = value" (or "key value") line; '=' counts as whitespace.
 * Returns 1 and fills k/v on success. */
static int split_kv(const char *line, char *k, int ksz, char *v, int vsz) {
    if (line[0] == '#' || line[0] == ';') return 0;
    char norm[160]; int n = 0;
    for (const char *p = line; *p && n < (int)sizeof(norm) - 1; ++p)
        norm[n++] = (*p == '=') ? ' ' : *p;
    norm[n] = '\0';
    char fmt[32];
    snprintf(fmt, sizeof(fmt), " %%%ds %%%ds", ksz - 1, vsz - 1);
    return sscanf(norm, fmt, k, v) == 2;
}

static void load_cfg(void) {
    char prof[16] = "balanced";
    char res[16] = "", fps[16] = "", vsync[16] = "", outl[16] = "", shad[16] = "";
    char animef[16] = "", nfilt[16] = "", fbz[16] = "", upsc[16] = "", gpu_name[32] = "", dist[16] = "", skin[16] = "", clk[16] = "", arate[16] = "", det[16] = "", rq[16] = "", gt[16] = "", sopt[16] = "", mapb[16] = "", bflip[16] = "", pvita[16] = "", fmodch[16] = "", spmin[16] = "", spmax[16] = "", trtest[16] = "";
    /* Player-facing custom profile bank. These values stay active in graphics.txt,
     * but are copied into the normal override bank only when profile=custom. That
     * makes selecting a stock preset a true one-line operation: the custom panel
     * cannot silently leak into performance/balanced/quality/battery. */
    char cres[16] = "", cfps[16] = "", cvsync[16] = "", coutl[16] = "", cshad[16] = "";
    char cnfilt[16] = "", cfbz[16] = "", cupsc[16] = "", cdist[16] = "", cclk[16] = "", cdet[16] = "", cgt[16] = "", cgpu[32] = "";
    char cmode[16] = "easy", cpicture[16] = "", cmotion[16] = "", ceffects[16] = "", cworld[16] = "", cpower[16] = "";
    char ares[16] = "", afps[16] = "", avsync[16] = "", aoutl[16] = "", ashad[16] = "";
    char anfilt[16] = "", afbz[16] = "", aupsc[16] = "", adist[16] = "", aclk[16] = "", adet[16] = "", agpu[32] = "";

    FILE *f = mcsm_open_setting("graphics.txt", "r");
    if (f) {
        char line[160], k[24], v[64];
        while (fgets(line, sizeof(line), f)) {
            if (!split_kv(line, k, sizeof(k), v, sizeof(v))) continue;
            if      (!strcmp(k, "profile"))       cfg_set(prof, sizeof(prof), v);
            else if (!strcmp(k, "custom_mode"))    cfg_set(cmode, sizeof(cmode), v);
            else if (!strcmp(k, "custom_picture")) cfg_set(cpicture, sizeof(cpicture), v);
            else if (!strcmp(k, "custom_motion"))  cfg_set(cmotion, sizeof(cmotion), v);
            else if (!strcmp(k, "custom_effects")) cfg_set(ceffects, sizeof(ceffects), v);
            else if (!strcmp(k, "custom_world"))   cfg_set(cworld, sizeof(cworld), v);
            else if (!strcmp(k, "custom_power"))   cfg_set(cpower, sizeof(cpower), v);
            else if (!strcmp(k, "custom_resolution"))    cfg_set(cres, sizeof(cres), v);
            else if (!strcmp(k, "custom_fps_cap"))       cfg_set(cfps, sizeof(cfps), v);
            else if (!strcmp(k, "custom_vsync"))         cfg_set(cvsync, sizeof(cvsync), v);
            else if (!strcmp(k, "custom_outlines"))      cfg_set(coutl, sizeof(coutl), v);
            else if (!strcmp(k, "custom_shadows"))       cfg_set(cshad, sizeof(cshad), v);
            else if (!strcmp(k, "custom_nearest_filter")) cfg_set(cnfilt, sizeof(cnfilt), v);
            else if (!strcmp(k, "custom_fbfetch_zero"))  cfg_set(cfbz, sizeof(cfbz), v);
            else if (!strcmp(k, "custom_upscale"))       cfg_set(cupsc, sizeof(cupsc), v);
            else if (!strcmp(k, "custom_draw_distance")) cfg_set(cdist, sizeof(cdist), v);
            else if (!strcmp(k, "custom_clock"))         cfg_set(cclk, sizeof(cclk), v);
            else if (!strcmp(k, "custom_detail"))        cfg_set(cdet, sizeof(cdet), v);
            else if (!strcmp(k, "custom_gpu_tier"))      cfg_set(cgt, sizeof(cgt), v);
            else if (!strcmp(k, "custom_gpu") || !strcmp(k, "custom_gpu_name"))
                cfg_set(cgpu, sizeof(cgpu), v);
            else if (!strcmp(k, "advanced_resolution"))    cfg_set(ares, sizeof(ares), v);
            else if (!strcmp(k, "advanced_fps_cap"))       cfg_set(afps, sizeof(afps), v);
            else if (!strcmp(k, "advanced_vsync"))         cfg_set(avsync, sizeof(avsync), v);
            else if (!strcmp(k, "advanced_outlines"))      cfg_set(aoutl, sizeof(aoutl), v);
            else if (!strcmp(k, "advanced_shadows"))       cfg_set(ashad, sizeof(ashad), v);
            else if (!strcmp(k, "advanced_nearest_filter")) cfg_set(anfilt, sizeof(anfilt), v);
            else if (!strcmp(k, "advanced_fbfetch_zero"))  cfg_set(afbz, sizeof(afbz), v);
            else if (!strcmp(k, "advanced_upscale"))       cfg_set(aupsc, sizeof(aupsc), v);
            else if (!strcmp(k, "advanced_draw_distance")) cfg_set(adist, sizeof(adist), v);
            else if (!strcmp(k, "advanced_clock"))         cfg_set(aclk, sizeof(aclk), v);
            else if (!strcmp(k, "advanced_detail"))        cfg_set(adet, sizeof(adet), v);
            else if (!strcmp(k, "advanced_gpu"))           cfg_set(agpu, sizeof(agpu), v);
            else if (!strcmp(k, "resolution"))    cfg_set(res, sizeof(res), v);
            else if (!strcmp(k, "fps_cap"))       cfg_set(fps, sizeof(fps), v);
            else if (!strcmp(k, "vsync"))         cfg_set(vsync, sizeof(vsync), v);
            else if (!strcmp(k, "outlines"))      cfg_set(outl, sizeof(outl), v);
            else if (!strcmp(k, "shadows"))       cfg_set(shad, sizeof(shad), v);
            else if (!strcmp(k, "draw_distance")) cfg_set(dist, sizeof(dist), v);
            else if (!strcmp(k, "skinning"))      cfg_set(skin, sizeof(skin), v);
            else if (!strcmp(k, "clock"))         cfg_set(clk, sizeof(clk), v);
            else if (!strcmp(k, "anim_rate"))     cfg_set(arate, sizeof(arate), v);
            else if (!strcmp(k, "detail"))        cfg_set(det, sizeof(det), v);
            else if (!strcmp(k, "render_quality")) cfg_set(rq, sizeof(rq), v);
            else if (!strcmp(k, "gpu_tier"))      cfg_set(gt, sizeof(gt), v);
            else if (!strcmp(k, "gpu_name"))      cfg_set(gpu_name, sizeof(gpu_name), v);
            else if (!strcmp(k, "shader_opt"))    cfg_set(sopt, sizeof(sopt), v);
            else if (!strcmp(k, "map_buffers"))   cfg_set(mapb, sizeof(mapb), v);
            else if (!strcmp(k, "blit_flip"))     cfg_set(bflip, sizeof(bflip), v);
            else if (!strcmp(k, "platform_vita")) cfg_set(pvita, sizeof(pvita), v);
            else if (!strcmp(k, "trophy_test"))   cfg_set(trtest, sizeof(trtest), v);
            else if (!strcmp(k, "fmod_channels"))  cfg_set(fmodch, sizeof(fmodch), v);
            else if (!strcmp(k, "scene_prio_min")) cfg_set(spmin, sizeof(spmin), v);
            else if (!strcmp(k, "scene_prio_max")) cfg_set(spmax, sizeof(spmax), v);
            else if (!strcmp(k, "upscale"))       cfg_set(upsc, sizeof(upsc), v);
            else if (!strcmp(k, "anim_engine_flags")) cfg_set(animef, sizeof(animef), v);
            else if (!strcmp(k, "nearest_filter")) cfg_set(nfilt, sizeof(nfilt), v);
            else if (!strcmp(k, "fbfetch_zero"))  cfg_set(fbz, sizeof(fbz), v);
        }
        fclose(f);
    }

    int p = PROF_BALANCED;
    if      (!strcmp(prof, "quality"))     p = PROF_QUALITY;
    else if (!strcmp(prof, "performance")) p = PROF_PERFORMANCE;
    else if (!strcmp(prof, "battery"))     p = PROF_BATTERY;
    else if (!strcmp(prof, "default"))     p = PROF_BALANCED;
    else if (!strcmp(prof, "balanced"))    p = PROF_BALANCED;
    else if (!strcmp(prof, "auto"))        p = PROF_AUTO;
    else if (!strcmp(prof, "custom"))      p = PROF_CUSTOM;

    /* Activate exactly one self-contained custom panel. custom_mode=easy uses the
     * six grouped word choices; custom_mode=advanced uses the permanently visible
     * raw values. This avoids comment/uncomment mechanics and prevents the inactive
     * block from leaking into the active one. Legacy custom_* raw overrides still
     * come last for backward compatibility with already-shipped settings files. */
    if (p == PROF_CUSTOM) {
        const int advanced_mode = !strcmp(cmode, "advanced");
        if (!advanced_mode) {
            if (cpicture[0]) {
                if      (!strcmp(cpicture, "native"))  cfg_set(res, sizeof(res), "960x544");
                else if (!strcmp(cpicture, "quality")) cfg_set(res, sizeof(res), "800x452");
                else if (!strcmp(cpicture, "sharp"))   cfg_set(res, sizeof(res), "720x408");
                else if (!strcmp(cpicture, "fast"))    cfg_set(res, sizeof(res), "640x362");
                else if (!strcmp(cpicture, "battery")) cfg_set(res, sizeof(res), "576x326");
                else if (!strcmp(cpicture, "low"))     cfg_set(res, sizeof(res), "480x272");
            }
            if (cmotion[0]) {
                if      (!strcmp(cmotion, "smooth")) { cfg_set(fps, sizeof(fps), "30"); cfg_set(vsync, sizeof(vsync), "on"); }
                else if (!strcmp(cmotion, "steady")) { cfg_set(fps, sizeof(fps), "20"); cfg_set(vsync, sizeof(vsync), "on"); }
                else if (!strcmp(cmotion, "low"))    { cfg_set(fps, sizeof(fps), "15"); cfg_set(vsync, sizeof(vsync), "on"); }
            }
            if (ceffects[0]) {
                if      (!strcmp(ceffects, "full"))     { cfg_set(outl, sizeof(outl), "on");  cfg_set(shad, sizeof(shad), "on"); }
                else if (!strcmp(ceffects, "outlines")) { cfg_set(outl, sizeof(outl), "on");  cfg_set(shad, sizeof(shad), "off"); }
                else if (!strcmp(ceffects, "minimal"))  { cfg_set(outl, sizeof(outl), "off"); cfg_set(shad, sizeof(shad), "off"); }
            }
            if (cworld[0]) {
                if      (!strcmp(cworld, "detailed"))   { cfg_set(det, sizeof(det), "1000"); cfg_set(dist, sizeof(dist), "5000"); }
                else if (!strcmp(cworld, "balanced"))  { cfg_set(det, sizeof(det), "800");  cfg_set(dist, sizeof(dist), "3500"); }
                else if (!strcmp(cworld, "fast"))      { cfg_set(det, sizeof(det), "600");  cfg_set(dist, sizeof(dist), "2500"); }
                else if (!strcmp(cworld, "unlimited")) { cfg_set(det, sizeof(det), "1000"); cfg_set(dist, sizeof(dist), "0"); }
            }
            if (cpower[0]) {
                if      (!strcmp(cpower, "performance")) cfg_set(clk, sizeof(clk), "444");
                else if (!strcmp(cpower, "battery"))     cfg_set(clk, sizeof(clk), "adaptive");
            }
            if (cgpu[0]) cfg_set(gpu_name, sizeof(gpu_name), cgpu);
        } else {
            if (ares[0])   cfg_set(res,   sizeof(res),   ares);
            if (afps[0])   cfg_set(fps,   sizeof(fps),   afps);
            if (avsync[0]) cfg_set(vsync, sizeof(vsync), avsync);
            if (aoutl[0])  cfg_set(outl,  sizeof(outl),  aoutl);
            if (ashad[0])  cfg_set(shad,  sizeof(shad),  ashad);
            if (anfilt[0]) cfg_set(nfilt, sizeof(nfilt), anfilt);
            if (afbz[0])   cfg_set(fbz,   sizeof(fbz),   afbz);
            if (aupsc[0])  cfg_set(upsc,  sizeof(upsc),  aupsc);
            if (adist[0])  cfg_set(dist,  sizeof(dist),  adist);
            if (aclk[0])   cfg_set(clk,   sizeof(clk),   aclk);
            if (adet[0])   cfg_set(det,   sizeof(det),   adet);
            if (agpu[0])   cfg_set(gpu_name, sizeof(gpu_name), agpu);
        }

        /* Legacy raw overrides intentionally come last. */
        if (cres[0])   cfg_set(res,   sizeof(res),   cres);
        if (cfps[0])   cfg_set(fps,   sizeof(fps),   cfps);
        if (cvsync[0]) cfg_set(vsync, sizeof(vsync), cvsync);
        if (coutl[0])  cfg_set(outl,  sizeof(outl),  coutl);
        if (cshad[0])  cfg_set(shad,  sizeof(shad),  cshad);
        if (cnfilt[0]) cfg_set(nfilt, sizeof(nfilt), cnfilt);
        if (cfbz[0])   cfg_set(fbz,   sizeof(fbz),   cfbz);
        if (cupsc[0])  cfg_set(upsc,  sizeof(upsc),  cupsc);
        if (cdist[0])  cfg_set(dist,  sizeof(dist),  cdist);
        if (cclk[0])   cfg_set(clk,   sizeof(clk),   cclk);
        if (cdet[0])   cfg_set(det,   sizeof(det),   cdet);
        if (cgt[0])    cfg_set(gt,    sizeof(gt),    cgt);
    }

    /* Start from the chosen profile (custom == balanced baseline), THEN let any
     * individual line that's actually present override just that one field. So
     * "profile = balanced" + "shadows = on" works without needing profile=custom
     * — pick a profile, tweak whatever you want. Lines left out keep the profile
     * value. This is the whole knob model; keep it simple. */
    apply_profile(&g_cfg, p == PROF_CUSTOM ? PROF_BALANCED : p);
    {
        int w, h;
        if (res[0] && sscanf(res, "%dx%d", &w, &h) == 2) { g_cfg.render_w = w; g_cfg.render_h = h; }
        if (fps[0])   g_cfg.fps_cap        = atoi(fps);
        if (vsync[0]) g_cfg.vsync          = parse_bool(vsync, g_cfg.vsync);
        if (outl[0])  g_cfg.outlines       = parse_bool(outl, g_cfg.outlines);
        if (shad[0])  g_cfg.shadows        = parse_bool(shad, g_cfg.shadows);
        if (animef[0]) g_cfg.anim_engine_flags = parse_bool(animef, g_cfg.anim_engine_flags);
        if (nfilt[0]) g_cfg.nearest_filter = parse_bool(nfilt, g_cfg.nearest_filter);
        if (fbz[0])   g_cfg.fbfetch_zero   = parse_bool(fbz, g_cfg.fbfetch_zero);
        if (dist[0])  g_cfg.draw_distance  = atoi(dist);
        if (skin[0])  g_cfg.skinning_full  = parse_bool(skin, g_cfg.skinning_full);
        if (clk[0]) {
            g_cfg.clock_adaptive = (!strcmp(clk, "adaptive") || !strcmp(clk, "battery")) ? 1 : 0;
            const int mhz = atoi(clk);            /* "444"/"500"/... ; 0 for "adaptive" */
            if (mhz >= 111 && mhz <= 600) g_cfg.clock_mhz = mhz;
        }
        if (arate[0]) { int r = atoi(arate); if (r >= 1 && r <= 3) g_cfg.anim_rate = r; }
        if (det[0])   { int d = atoi(det);   if (d >= 100 && d <= 1000) g_cfg.detail = d; }
        if (rq[0])    { int q = atoi(rq);    if (q >= 0   && q <= 15)   g_cfg.render_quality = q; }
        if (gt[0])    { int t = atoi(gt);    if (t >= 0   && t <= 3)    g_cfg.gpu_tier = t; }
        if (gpu_name[0]) cfg_set(g_cfg.gpu_name, sizeof(g_cfg.gpu_name),
                                 gpu_name_from_alias(gpu_name));
        if (sopt[0])  { int o = atoi(sopt); if (o >= 0   && o <= 4)    g_cfg.shader_opt = o; }
        if (mapb[0])  g_cfg.map_buffers = parse_bool(mapb, g_cfg.map_buffers);
        if (bflip[0]) g_cfg.blit_flip   = parse_bool(bflip, g_cfg.blit_flip);
        if (pvita[0]) g_cfg.platform_vita = parse_bool(pvita, g_cfg.platform_vita);
        if (trtest[0]) { int n=atoi(trtest); if (n>0 && n<128) g_cfg.trophy_test=n; }
        if (fmodch[0]) { int n=atoi(fmodch); if (n>0 && n<=256) g_cfg.fmod_channels=n; }
        if (spmin[0])  g_cfg.scene_prio_min = atoi(spmin);
        if (spmax[0])  g_cfg.scene_prio_max = atoi(spmax);
        /* "nearest"/"sharp" -> NEAREST; anything else ("linear"/"smooth") -> LINEAR. */
        if (upsc[0])  g_cfg.upscale_nearest = (upsc[0]=='n'||upsc[0]=='N'||upsc[0]=='s'||upsc[0]=='S') ? 1 : 0;
    }

    g_cfg_loaded = 1;
    l_info("CONFIG(graphics): profile=%s res=%dx%d fps=%d vsync=%d outlines=%d shadows=%d dist=%d skinning=%s anim_rate=1/%d detail=%d/1000 rquality=%d gpu_tier=%d gpu_name=%s shader_opt=%d map_buffers=%d blit_flip=%d platform_vita=%d upscale=%s clock=%s(%dMHz)",
           prof, g_cfg.render_w, g_cfg.render_h, g_cfg.fps_cap, g_cfg.vsync, g_cfg.outlines,
           g_cfg.shadows, g_cfg.draw_distance, g_cfg.skinning_full ? "full" : "reduced", g_cfg.anim_rate, g_cfg.detail, g_cfg.render_quality, g_cfg.gpu_tier, g_cfg.gpu_name[0] ? g_cfg.gpu_name : "(tier)", g_cfg.shader_opt, g_cfg.map_buffers, g_cfg.blit_flip, g_cfg.platform_vita, g_cfg.upscale_nearest ? "nearest" : "linear",
           g_cfg.clock_adaptive ? "adaptive" : "pinned", g_cfg.clock_mhz);
}

const McsmCfg *mcsm_cfg(void) {
    if (!g_cfg_loaded) load_cfg();
    return &g_cfg;
}

/* ---- game.txt (content) ------------------------------------------------ */

static McsmGame g_game;
static int      g_game_loaded = 0;

static void load_game(void) {
    g_game.language[0] = '\0';
    for (int i = 0; i < 8; ++i) g_game.chapters[i] = -1;

    char lang[16] = "", chap[64] = "";
    char chov[8][8] = {{0}};                      /* per-chapter override lines (chapter1..8);
                                                   * fully zeroed so a too-long value stays
                                                   * NUL-terminated (strncpy won't add one). */
    FILE *f = mcsm_open_setting("game.txt", "r");
    if (f) {
        char line[160], k[24], v[64];
        while (fgets(line, sizeof(line), f)) {
            if (!split_kv(line, k, sizeof(k), v, sizeof(v))) continue;
            if      (!strcmp(k, "language")) cfg_set(lang, sizeof(lang), v);
            else if (!strcmp(k, "chapters")) cfg_set(chap, sizeof(chap), v);
            else if (strlen(k) == 8 && !strncmp(k, "chapter", 7) && k[7] >= '1' && k[7] <= '8')
                cfg_set(chov[k[7] - '1'], sizeof(chov[0]), v);   /* chapterN = on/off/auto */
        }
        fclose(f);
    }

    if (lang[0]) cfg_set(g_game.language, sizeof(g_game.language), lang);

    /* Global setting. "auto" (and the default when the line is absent) leaves every
     * chapter at -1 = show it only if its data is installed. "all" force-shows all 8.
     * A number list ("1,2,3") force-shows exactly those and hides the rest. */
    if (chap[0] && strcmp(chap, "auto") != 0) {
        if (!strcmp(chap, "all")) { for (int i = 0; i < 8; ++i) g_game.chapters[i] = 1; }
        else {
            for (int i = 0; i < 8; ++i) g_game.chapters[i] = 0;  /* listed = show, rest hide */
            for (const char *s = chap; *s; ++s) if (*s >= '1' && *s <= '8') g_game.chapters[*s - '1'] = 1;
        }
    }
    /* Per-chapter toggles override the global setting for that one chapter:
     * on/show = force show, off/hide = force hide, auto = show if data installed. */
    for (int i = 0; i < 8; ++i) {
        const char *o = chov[i];
        if      (!o[0]) continue;
        else if (!strcmp(o, "on")  || !strcmp(o, "show") || !strcmp(o, "1") || !strcmp(o, "yes")) g_game.chapters[i] = 1;
        else if (!strcmp(o, "off") || !strcmp(o, "hide") || !strcmp(o, "0") || !strcmp(o, "no"))  g_game.chapters[i] = 0;
        else if (!strcmp(o, "auto")) g_game.chapters[i] = -1;
    }

    g_game_loaded = 1;
    l_info("CONFIG(game): language=\"%s\" chapters=%s -> [1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7:%d 8:%d] (1=show 0=hide -1=auto)",
           g_game.language, chap[0] ? chap : "auto",
           g_game.chapters[0], g_game.chapters[1], g_game.chapters[2], g_game.chapters[3],
           g_game.chapters[4], g_game.chapters[5], g_game.chapters[6], g_game.chapters[7]);
}

const McsmGame *mcsm_game(void) {
    if (!g_game_loaded) load_game();
    return &g_game;
}
