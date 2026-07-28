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
    int  anim_rate;           /* advance animation every Nth frame: 1=every frame
                               * (default), 2=half, 3=third. Animation blend across
                               * PlaybackControllers dominates the sim, so N>1 is the
                               * biggest remaining CPU lever -- at the cost of motion
                               * and lip-sync stepping at 30/N Hz.                  */
    int  detail;              /* brush-geometry LOD scale x1000 (100..1000, 1000 =
                               * engine default / no change). Scene::SetBrushNear/
                               * FarDetail biases the engine toward coarser BAKED
                               * LODs, so it is the only runtime lever that attacks
                               * the vertex count itself -- and vertex count is what
                               * slow frames actually track. Stored x1000 to keep the
                               * config struct integer-only.                        */
    int  render_quality;      /* engine RenderConfiguration quality, 0..15. MCSM boots
                               * at 15 (its mobile build uses a wider scale than the
                               * 0-4 desktop enum). -1 = leave the engine's own value. */
    int  gpu_tier;            /* device GPU tier reported to the game's Lua via
                               * PlatformGetGPUQuality: 0 = weakest .. 3 = strongest,
                               * -1 = leave the engine's own answer alone. The game
                               * configures its own quality from this, so forcing 0
                               * selects the engine's built-in low-spec path -- the
                               * same one budget Android phones get.               */
    /* system */
    int  clock_adaptive;      /* 0 = ARM pinned, 1 = adaptive floor (battery)    */
    int  clock_mhz;           /* ARM target MHz. 444 = stock max. Higher only has
                               * an effect with a CPU-overclock plugin installed;
                               * without one the kernel clamps back to 444.       */
} McsmCfg;

typedef struct {
    char language[16];        /* locale, "" = English                            */
    int  chapters[8];         /* episode 1..8: 1 show / 0 hide / -1 engine picks  */
} McsmGame;

/* Lazily parse graphics.txt / game.txt on first call, then return the result. */
const McsmCfg  *mcsm_cfg(void);
const McsmGame *mcsm_game(void);

#endif /* MCSM_CONFIG_H */
