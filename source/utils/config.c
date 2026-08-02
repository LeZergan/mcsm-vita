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
     * shown on 60Hz displays. No built-in preset currently uses 24, but a manual
     * override remains paced correctly.
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
        /* Sharpest preset. It targets 30 FPS but deliberately spends more CPU on
         * image quality, so demanding frames may miss an individual vblank. The
         * presenter must never turn those misses into a persistent 20 FPS lock. */
        c->render_w = 800; c->render_h = 452; c->fps_cap = 30; c->vsync = 1;
        /* Shadows remain off because they resubmit the scene. Full detail, outlines,
         * and the higher render resolution are the visible quality tradeoff. */
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
        c->outlines = 1;   c->shadows = 0;    c->draw_distance = 4000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 900;   c->render_quality = -1; c->gpu_tier = 0;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    case PROF_PERFORMANCE:
        /* Fastest preset. A 30 FPS cap matches the two-vblank cadence this workload
         * can realistically reach. CPU-side cuts use the weakest GPU identity,
         * shorter distance, coarser detail, and no outline/shadow submissions.
         * Full animation and skinning stay enabled because reducing either caused
         * visible facial or attachment defects without fixing the frame bottleneck. */
        c->render_w = 640; c->render_h = 362; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 0;   c->shadows = 0;    c->draw_distance = 2500; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 600;   c->render_quality = -1; c->gpu_tier = 0;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    case PROF_BATTERY:
        /* Longest-play-time preset: lower render cost, weakest GPU identity, coarser
         * geometry, and adaptive ARM clock. It still requests 30 FPS and keeps full
         * animation/skinning correctness; power savings come from simpler work. */
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
         * (720x408, dist 4000). Every other enumerator has its own case, so nothing
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
        c->outlines = 1;   c->shadows = 0;    c->draw_distance = 4000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 900;   c->render_quality = -1; c->gpu_tier = 0;
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
    char aarate[16] = "";

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
            else if (!strcmp(k, "advanced_anim_rate"))     cfg_set(aarate, sizeof(aarate), v);
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
     * block from leaking into the active one. Raw custom_* overrides still come
     * last so an advanced user can replace any grouped choice explicitly. */
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
            if (aarate[0]) cfg_set(arate, sizeof(arate), aarate);
            if (adet[0])   cfg_set(det,   sizeof(det),   adet);
            if (agpu[0])   cfg_set(gpu_name, sizeof(gpu_name), agpu);
        }

        /* Raw overrides intentionally come last. */
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
