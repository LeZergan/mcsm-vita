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
         * Native res, every effect on, real GPU reported so the engine configures
         * itself at full quality -- then locked to 24, delivered as a true 3:2:3:2
         * pulldown by the fractional present timeline (see the note above). At
         * 41.67ms per frame the engine (~30-45ms sim) makes the deadline, which is
         * what "does not move" means. Steady 24 reads far better than a 25-30 that
         * lurches, and it is the cinematic cadence this game was authored for. */
        c->render_w = 960; c->render_h = 544; c->fps_cap = 24; c->vsync = 1;
        /* draw_distance is NOT unlimited here, even though this is the pretty
         * profile. Device data: the GPU idles around 40% while frames take 55ms,
         * i.e. the CPU is the entire limit -- so pixels and shadows are nearly
         * free, and culling distant objects is what actually buys stability.
         * Unlimited distance made the CPU walk every far object for almost no
         * visible gain, and sim work measured p50 49ms / p90 71ms against the
         * 50ms budget -- half the frames missed 20fps before drawing anything. */
        c->outlines = 1;   c->shadows = 1;    c->draw_distance = 6000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000; c->render_quality = -1; c->gpu_tier = 1;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    case PROF_BALANCED:    /* "default" -- good visuals AND good performance */
        /* Middle GPU identity so the engine trims its heaviest effects but keeps the
         * look, outlines kept (they carry the art style), shadows off (biggest cheap
         * win), 800x452 stays sharp for text. Targets 30. */
        c->render_w = 720; c->render_h = 408; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 1;   c->shadows = 0;    c->draw_distance = 5000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000; c->render_quality = -1; c->gpu_tier = 1;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    case PROF_PERFORMANCE:
        /* Frames over everything -- but "frames" means DISPLAYED frames, which on a
         * 60Hz vsynced panel is a ladder, not a dial: only 60/30/20/15 exist.
         *
         * ★ CAP STAYS 60. It was briefly lowered to 30 on 2026-07-30 on the theory
         * that 60 "manufactures judder". That was wrong, and the arithmetic says so:
         * capping lower never makes a frame arrive sooner, it only ever delays one.
         * Same workload (sim p50 26ms / p90 42ms) through both caps:
         *     work 12ms   cap60 -> present 16.7ms = 60fps
         *                 cap30 -> HELD to 33.3ms = 30fps   <-- pure loss
         *     work 26ms   cap60 -> next vblank 33.3ms = 30fps
         *                 cap30 ->           33.3ms = 30fps   <-- identical
         *     work 42ms   cap60 -> next vblank 50ms   = 20fps
         *                 cap30 ->           50ms   = 20fps   <-- identical
         * 30 is never faster on a heavy frame and strictly slower on a light one.
         * The 60->30->20 spread is the WORKLOAD's variance, not something the cap
         * creates; a lower cap only hides it by discarding the good frames as well.
         * This profile exists for maximum frames, so it takes the spread -- balanced
         * and quality are where a flat cadence is the goal.
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
        c->render_w = 576; c->render_h = 326; c->fps_cap = 60; c->vsync = 1;
        c->outlines = 0;   c->shadows = 0;    c->draw_distance = 3000; c->skinning_full = 1;
        c->anim_rate = 2;   c->detail = 700;  c->render_quality = -1; c->gpu_tier = 0;
        c->shader_opt = 2; c->clock_adaptive = 0; c->clock_mhz = 444; break;

    case PROF_BATTERY:
        /* Longest play time. Power on this console tracks CPU clock and pixels, so:
         * downclock when idle, half native resolution (480x272 is an exact 1/2 of
         * 960x544, so it upscales cleanly instead of resampling), weakest GPU
         * identity, coarser geometry, half-rate animation, and 20fps -- a lower
         * locked rate is less work per second AND a clean 3-vblank lock. */
        c->render_w = 480; c->render_h = 272; c->fps_cap = 20; c->vsync = 1;
        c->outlines = 0;   c->shadows = 0;    c->draw_distance = 3000; c->skinning_full = 0;
        c->anim_rate = 2;   c->detail = 700;  c->render_quality = -1; c->gpu_tier = 0;
        c->shader_opt = 2; c->clock_adaptive = 1; c->clock_mhz = 444; break;

    case PROF_AUTO:
        /* Engine-managed: every knob the ENGINE decides is left at its pass-through
         * value so the loader overrides nothing, and the game runs whatever it chose
         * for the GPU we reported. Resolution/fps_cap/vsync/clock stay ours (the
         * engine has no concept of the Vita render scale or power), and skinning
         * stays full because reduced is a correctness defect. Use it to A/B the
         * engine own judgement against the tuned profiles. */
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
         * what custom actually does. The player-facing doc has been corrected too. */
        c->render_w = 720; c->render_h = 408; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 1;   c->shadows = 0;    c->draw_distance = 5000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000; c->render_quality = -1; c->gpu_tier = 1;
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
    char upsc[16] = "", gpu_name[32] = "", dist[16] = "", skin[16] = "", clk[16] = "", arate[16] = "", det[16] = "", rq[16] = "", gt[16] = "", sopt[16] = "", mapb[16] = "", bflip[16] = "", pvita[16] = "", fmodch[16] = "", spmin[16] = "", spmax[16] = "", trtest[16] = "";

    FILE *f = mcsm_open_setting("graphics.txt", "r");
    if (f) {
        char line[160], k[24], v[64];
        while (fgets(line, sizeof(line), f)) {
            if (!split_kv(line, k, sizeof(k), v, sizeof(v))) continue;
            if      (!strcmp(k, "profile"))       cfg_set(prof, sizeof(prof), v);
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
        if (gpu_name[0]) cfg_set(g_cfg.gpu_name, sizeof(g_cfg.gpu_name), gpu_name);
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
