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
    int  outlines;            /* 1 = outlines on, 0 = off (fewer draws)          */
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
    char gpu_name[32];        /* exact GL_RENDERER string to report; "" = use gpu_tier.
                               * The engine matches this against its own GPU table and
                               * derives a rank from it (SGX 543MP=15, 541=7, 540=6,
                               * Mali-400=5, Adreno 305=4, Tegra 3=2, GC1000=1), then
                               * reduces that rank to LOW or HIGH. Naming a device
                               * directly lets any rank be tested, not just the four
                               * gpu_tier exposes.                                    */
    int  shader_opt;          /* runtime shader compiler optimisation level, 0..4
                               * (0=O0 .. 2=O2 default .. 4=Ofast). Was hardcoded to 0
                               * as an anti-stutter measure back when the progcache never
                               * hit; now that it does, compile cost is paid once per
                               * console while the O0 penalty was paid every frame.
                               * The progcache key includes this value, so each level
                               * keeps its OWN binaries -- flipping back to 0 reuses the
                               * pre-existing cache with no recompile.               */
    /* ☠ DEVICE-PROVEN TO CRASH ON BOOT AT 1 (2026-07-30). Leave at 0 until the
     * allocation gap below is closed. The shipped libvitaGL.a is built NO_DEBUG
     * (SKIP_ERROR_HANDLING), which reduces glMapNamedBufferRange to
     *     ldr r3,[r0,#0] / strb #1,[r0,#16] / adds r0,r3,r1 / bx lr
     * i.e. `return gpu_buf->ptr + offset` with the NULL check AND the
     * offset+length>size check compiled out. The four hooks this flag removes were
     * the thing that allocated that buffer, so with them gone ptr is NULL on the
     * first lock and the skinner writes hundreds of KB to a near-null address.
     * The ENGINE half of the chain is confirmed correct on device (OES_mapbuffer=1
     * and EXT_map_buffer_range=1, so caps 21 and 22 are both set and MapGLBuffer
     * really is reached) -- the break is purely that the vitaGL entry point those
     * function pointers resolve to is unbacked and unchecked.
     * TO FIX PROPERLY: keep a lock hook that does glBufferData(size, NULL) once per
     * buffer (and on size change) so ptr is valid, then let the engine map it. */
    int  upscale_nearest;     /* 1 = GL_NEAREST for the low-res -> native upscale,
                               * 0 = GL_LINEAR (default).
                               * Only meaningful with a non-native resolution. TEXT is
                               * the reason this exists: the whole frame -- 3D, UI and
                               * every glyph -- is rendered into ONE low-res FBO and
                               * then stretched to 960x544 in a single blit, so at
                               * 576x326 every glyph edge is smeared across a 1.667x
                               * bilinear filter. NEAREST keeps edges hard, which is
                               * legible where LINEAR is mush -- and at an EXACT
                               * integer ratio (480x272 -> 960x544 is precisely 2x) it
                               * is true pixel doubling with no resampling at all.
                               * At non-integer ratios NEAREST trades blur for uneven
                               * glyph stems, so it is a preference, not a win.       */
    int  trophy_test;         /* >0 = unlock this trophy id a few seconds after boot,
                               * purely to prove the pipeline. Telltale achievements fire
                               * from story-progress Lua, so without this the only way to
                               * test is to PLAY to a beat -- this exercises the identical
                               * path (context -> worker thread -> sceNpTrophyUnlockTrophy
                               * -> system popup) in seconds. 0 = off. Set it back to 0
                               * once proven; it re-fires every boot otherwise (harmless,
                               * an already-unlocked trophy is ignored). */
    int  fmod_channels;       /* >0 = override AudioThread::snMaxFmodChannels (ships 128).
                               * A CAP, not a count: only helps if the game really opens
                               * many channels -- the ENGTUNE log line reports the live
                               * value so this can be set from data. 0 = leave alone. */
    int  scene_prio_min;      /* Scene::sMin/sMaxRenderedScenePriority (ship -10000/+10000).
                               * Narrowing the range stops the engine rendering scenes
                               * outside it -- a DIRECT draw-count lever, and an equally
                               * direct way to make the UI or world vanish. Both must be
                               * set together and min < max. 0/0 = leave alone. */
    int  scene_prio_max;
    int  platform_vita;       /* 1 = report ePlatform_Vita (9) from
                               * TTPlatform::GetPlatformType(), 0 = leave it at the
                               * engine's hardcoded ePlatform_Android (8).
                               *
                               * ☠☠ TESTED ON DEVICE 2026-07-31: DO NOT ENABLE.
                               * The long-standing "most direct untested perf lever"
                               * question is now CLOSED, and the answer is no:
                               *   - frame rate was essentially UNCHANGED, and
                               *   - it TURNS OFF TOUCH CONTROLS.
                               * That second part is fatal on its own. This is a
                               * point-and-click game whose primary input is the
                               * front panel: the engine gates its touch paths on the
                               * platform type (a real Vita title would be expected to
                               * use buttons), so telling it we are a Vita makes it
                               * stop asking for touch. Trading the main input method
                               * for no measurable gain is not a trade.
                               * Left in place as a 0-default knob only so the finding
                               * is reproducible; it is deliberately NOT documented in
                               * the player-facing graphics.txt any more.
                               *
                               * That function is a TWO-INSTRUCTION STUB (`mov r0,#8;
                               * bx lr`) and feeds exactly five call sites, three of
                               * them render-critical:
                               *   RenderConfiguration::Initialize()
                               *   RenderConfiguration::GetSupportedQualityTypes()
                               *   T3EffectCacheInternal::GetProgram(...)  <- per-draw
                               *                                              shader
                               *                                              selection
                               * The engine filters shader features by PLATFORM and
                               * QUALITY (T3EffectUtil::GetValidDynamicFeatures takes
                               * both), so this selects which shader PROGRAM each draw
                               * uses -- and render is 75% of the frame.
                               *
                               * OFF BY DEFAULT. The per-platform quality table lives in
                               * .bss and is built during init, so whether Vita and
                               * Android actually differ CANNOT be determined offline --
                               * the PLATQUAL log line answers it on device. Turning
                               * this on could also ask for a shader feature set the
                               * Android shader data cannot satisfy. */
    int  blit_flip;           /* render-scale FBO -> display blit Y orientation.
                               * 1 = swap dstY0/dstY1 (correct for the aa75c61-era
                               * libvitaGL.a), 0 = straight blit (correct for upstream
                               * 96c41a1 and later, where the FBO-bind path applies the
                               * Y flip itself via the viewport transform). Exists as a
                               * runtime lever because getting it wrong presents the
                               * ENTIRE GAME UPSIDE-DOWN, and a rebuild+redeploy cycle
                               * to test the other sign is expensive. See gl_swap(). */
    int  map_buffers;         /* 1 = do NOT install the T3Vertex/IndexBuffer
                               * Platform(Un)Lock hooks, handing skinned-mesh
                               * uploads back to the engine's own zero-copy
                               * glMapBuffer path (RenderDevice::mRenderCaps bit
                               * 21, which the engine already sets because vitaGL
                               * advertises GL_OES_mapbuffer). Removes a malloc,
                               * a GPU realloc and a FULL-buffer memcpy per
                               * skinned mesh per frame -- the cost that scales
                               * with character count. 0 = today's behaviour.
                               * Off by default only because vitaGL's
                               * glMapNamedBufferRange ignores 'last_frame', so
                               * in-place writes could tear geometry; that needs
                               * a device to judge. See so_patch() for the full
                               * chain and PERF_FINDINGS_2026-07-30.md.        */
    int  anim_engine_flags;   /* 1 = override GameEngine's own animation-correctness
                               * flags (SetFixRecursiveAnimationContribution and the
                               * non-skeleton chore filter). 0 = LEAVE THE ENGINE ALONE,
                               * the default.
                               * ☠ Forcing these caused character heads to detach from
                               * bodies intermittently: recursive bone contribution is
                               * how a head inherits its neck, and the loader was
                               * stomping that global from another thread while the
                               * engine's animation update read it. Never measured to
                               * help. Kept only so the experiment is repeatable.   */
    int  nearest_filter;      /* 1 = sample the COMPRESSED world atlas with GL_NEAREST.
                               * Minecraft tile art bilinearly interpolated across atlas
                               * edges shows bright/white seams between blocks (a BASE
                               * level bleed, so turning mipmaps off cannot fix it). UI
                               * and 2D stay LINEAR so fonts remain smooth. 0 = engine
                               * default. Controlled only by graphics.txt.              */
    int  fbfetch_zero;        /* framebuffer-fetch stub value. vitaGL has no
                               * gl_LastFragData, so it is replaced by a constant: 1 =
                               * vec4(0.0) (identity for ADDITIVE light accumulation),
                               * 0 = vec4(1.0) (identity for MODULATE). The wrong one
                               * turns whole surfaces solid white -- glass and other
                               * blended surfaces are exactly where this shows. The
                               * value comes only from graphics.txt.                    */
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
