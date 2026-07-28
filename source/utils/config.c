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
enum { PROF_QUALITY = 0, PROF_BALANCED, PROF_PERFORMANCE, PROF_BATTERY, PROF_CUSTOM };

static void apply_profile(McsmCfg *c, int prof) {
    switch (prof) {
    case PROF_QUALITY:     /* best looks, ~20-24fps in heavy scenes */
        c->render_w = 960; c->render_h = 544; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 1;   c->shadows = 1;    c->draw_distance = 0; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000; c->render_quality = -1; c->gpu_tier = -1;
        c->clock_adaptive = 0; c->clock_mhz = 444; break;
    case PROF_PERFORMANCE: /* steady 30, effects trimmed to hold it in heavy scenes */
        /* skinning stays FULL (2026-07-25): "reduced" turns off the engine's
         * recursive per-bone contribution, which is what propagates a parent
         * bone's transform to attached geometry in the SAME frame. Without it,
         * attached parts visibly TRAIL the body they are attached to (Reuben's
         * costume lagging as he moves, chest lids lagging as they open). That
         * reads as a bug, not a quality setting, so it is no longer part of the
         * performance preset -- only "battery" still trades it away. Anyone who
         * wants the CPU back can still set `skinning = reduced` explicitly, which
         * now overrides on top of any profile. */
        c->render_w = 720; c->render_h = 408; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 0;   c->shadows = 0;    c->draw_distance = 4000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000; c->render_quality = -1; c->gpu_tier = -1;
        c->clock_adaptive = 0; c->clock_mhz = 444; break;
    case PROF_BATTERY:     /* lowest power: the only profile that trades quality away */
        /* Was 640x360 + the same effect set as "performance", so the two profiles
         * were visually near-identical and battery looked no cheaper than it is.
         * 480x272 is exactly HALF of native 960x544, which both halves the fill
         * cost again and upscales cleanly (integer ratio) instead of resampling.
         * Shorter draw distance culls more geometry -- the engine is sim/vertex
         * bound, so that is where the real power saving is. */
        c->render_w = 480; c->render_h = 272; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 0;   c->shadows = 0;    c->draw_distance = 3000; c->skinning_full = 0;
        c->anim_rate = 2;   c->detail = 700;  c->render_quality = -1; c->gpu_tier = -1;
        c->clock_adaptive = 1; c->clock_mhz = 444; break;
    case PROF_BALANCED:    /* default */
    default:
        c->render_w = 800; c->render_h = 452; c->fps_cap = 30; c->vsync = 1;
        c->outlines = 1;   c->shadows = 0;    c->draw_distance = 6000; c->skinning_full = 1;
        c->anim_rate = 1;   c->detail = 1000; c->render_quality = -1; c->gpu_tier = -1;
        c->clock_adaptive = 0; c->clock_mhz = 444; break;
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
    char dist[16] = "", skin[16] = "", clk[16] = "", arate[16] = "", det[16] = "", rq[16] = "", gt[16] = "";

    FILE *f = mcsm_open_setting("graphics.txt", "r");
    if (f) {
        char line[160], k[24], v[64];
        while (fgets(line, sizeof(line), f)) {
            if (!split_kv(line, k, sizeof(k), v, sizeof(v))) continue;
            if      (!strcmp(k, "profile"))       strncpy(prof,  v, sizeof(prof)  - 1);
            else if (!strcmp(k, "resolution"))    strncpy(res,   v, sizeof(res)   - 1);
            else if (!strcmp(k, "fps_cap"))       strncpy(fps,   v, sizeof(fps)   - 1);
            else if (!strcmp(k, "vsync"))         strncpy(vsync, v, sizeof(vsync) - 1);
            else if (!strcmp(k, "outlines"))      strncpy(outl,  v, sizeof(outl)  - 1);
            else if (!strcmp(k, "shadows"))       strncpy(shad,  v, sizeof(shad)  - 1);
            else if (!strcmp(k, "draw_distance")) strncpy(dist,  v, sizeof(dist)  - 1);
            else if (!strcmp(k, "skinning"))      strncpy(skin,  v, sizeof(skin)  - 1);
            else if (!strcmp(k, "clock"))         strncpy(clk,   v, sizeof(clk)   - 1);
            else if (!strcmp(k, "anim_rate"))     strncpy(arate, v, sizeof(arate) - 1);
            else if (!strcmp(k, "detail"))        strncpy(det,   v, sizeof(det)   - 1);
            else if (!strcmp(k, "render_quality")) strncpy(rq,   v, sizeof(rq)    - 1);
            else if (!strcmp(k, "gpu_tier"))      strncpy(gt,   v, sizeof(gt)    - 1);
        }
        fclose(f);
    }

    int p = PROF_BALANCED;
    if      (!strcmp(prof, "quality"))     p = PROF_QUALITY;
    else if (!strcmp(prof, "performance")) p = PROF_PERFORMANCE;
    else if (!strcmp(prof, "battery"))     p = PROF_BATTERY;
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
    }

    g_cfg_loaded = 1;
    l_info("CONFIG(graphics): profile=%s res=%dx%d fps=%d vsync=%d outlines=%d shadows=%d dist=%d skinning=%s anim_rate=1/%d detail=%d/1000 rquality=%d gpu_tier=%d clock=%s(%dMHz)",
           prof, g_cfg.render_w, g_cfg.render_h, g_cfg.fps_cap, g_cfg.vsync, g_cfg.outlines,
           g_cfg.shadows, g_cfg.draw_distance, g_cfg.skinning_full ? "full" : "reduced", g_cfg.anim_rate, g_cfg.detail, g_cfg.render_quality, g_cfg.gpu_tier,
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
            if      (!strcmp(k, "language")) strncpy(lang, v, sizeof(lang) - 1);
            else if (!strcmp(k, "chapters")) strncpy(chap, v, sizeof(chap) - 1);
            else if (strlen(k) == 8 && !strncmp(k, "chapter", 7) && k[7] >= '1' && k[7] <= '8')
                strncpy(chov[k[7] - '1'], v, sizeof(chov[0]) - 1);   /* chapterN = on/off/auto */
        }
        fclose(f);
    }

    if (lang[0]) strncpy(g_game.language, lang, sizeof(g_game.language) - 1);

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
