/*
 * Consolidated settings for the MCSM Vita loader, split into two files:
 *
 *   graphics.txt  -> McsmCfg  : a `profile =` preset (or `custom`) that drives
 *                               every performance/visual lever.
 *   game.txt      -> McsmGame : content settings (language, which chapters show).
 *
 * Each file is parsed once, lazily, on first access. Every scattered getter now
 * reads one of these structs, so there are exactly two files to edit/ship.
 */
#ifndef MCSM_CONFIG_H
#define MCSM_CONFIG_H

typedef struct {
    /* display */
    int  render_w, render_h;  /* render size; 0 = engine/compile default        */
    int  fps_cap;             /* target fps; 0 = uncapped                        */
    int  vsync;               /* 1 = on (smooth), 0 = off (higher fps, tearing)  */
    /* graphics */
    int  outlines;            /* 1 = toon outlines on, 0 = off (fewer draws)     */
    int  shadows;             /* 1 = cast shadows on, 0 = off (fewer draws)      */
    int  draw_distance;       /* far-clip cap in world units; 0 = engine default */
    int  skinning_full;       /* 1 = full char animation, 0 = reduced (less CPU) */
    /* system */
    int  clock_adaptive;      /* 0 = ARM pinned 444, 1 = adaptive (battery)      */
} McsmCfg;

typedef struct {
    char language[16];        /* locale, "" = English                            */
    int  chapters[8];         /* episode 1..8: 1 show / 0 hide / -1 engine picks  */
} McsmGame;

/* Lazily parse graphics.txt / game.txt on first call, then return the result. */
const McsmCfg  *mcsm_cfg(void);
const McsmGame *mcsm_game(void);

#endif /* MCSM_CONFIG_H */
