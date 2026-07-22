/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#ifdef USE_PVR_PSP2
#include "utils/pvr_init.h"
#include "utils/loading_screen.h"
#endif
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/telemetry.h"
#include "java_runtime.h"

#include <reimpl/controls.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base addresses for Android native libraries.
#define LOAD_ADDR_LIBFMOD       0x98000000
#define LOAD_ADDR_LIBFMODSTUDIO 0x99000000
#define LOAD_ADDR_LIBSDL2       0x9A000000
#define LOAD_ADDR_LIBGAMEENGINE 0x9C000000
#define LOAD_ADDR_LIBMAIN       0xAE000000

extern so_module so_mod_fmod;
extern so_module so_mod_fmodstudio;
extern so_module so_mod_sdl2;
extern so_module so_mod_gameengine;
extern so_module so_mod_main;

typedef struct LoaderModule {
    const char *name;
    const char *path;
    uintptr_t load_addr;
    so_module *mod;
} LoaderModule;

static LoaderModule g_modules[] = {
    { "libfmod.so",       LIB_FMOD_SO_PATH,       LOAD_ADDR_LIBFMOD,       &so_mod_fmod },
    { "libfmodstudio.so", LIB_FMODSTUDIO_SO_PATH, LOAD_ADDR_LIBFMODSTUDIO, &so_mod_fmodstudio },
    { "libSDL2.so",       LIB_SDL2_SO_PATH,       LOAD_ADDR_LIBSDL2,       &so_mod_sdl2 },
    { "libGameEngine.so", LIB_GAMEENGINE_SO_PATH, LOAD_ADDR_LIBGAMEENGINE, &so_mod_gameengine },
    { "libmain.so",       LIB_MAIN_SO_PATH,       LOAD_ADDR_LIBMAIN,       &so_mod_main },
};

static void ensure_runtime_directories(void) {
    const struct {
        const char *dir;
        const char *marker_path;
    } dirs[] = {
        { DATA_PATH "Temp", DATA_PATH "Temp/.keep" },
        { DATA_PATH "User", DATA_PATH "User/.keep" },
        { DATA_PATH "Net", DATA_PATH "Net/.keep" },
        { DATA_PATH "assets", DATA_PATH "assets/.keep" },
        { DATA_PATH "diag", DATA_PATH "diag/.keep" },
        { DATA_PATH "diag/shaders", DATA_PATH "diag/shaders/.keep" },
    };

    for (int i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); ++i) {
        if (is_dir(dirs[i].dir)) {
            continue;
        }

        if (file_mkpath(dirs[i].marker_path, 0777) && is_dir(dirs[i].dir)) {
            l_info("Ensured runtime directory: %s", dirs[i].dir);
        } else {
            l_warn("Could not ensure runtime directory: %s", dirs[i].dir);
        }
    }
}

static void ensure_episode2_available_descriptor(void) {
    const char *data_path = DATA_PATH "assets/MCSM_android_101-102_Available_data.ttarch2";
    const char *desc_path = DATA_PATH "assets/_resdesc_50_101-102_Available.lua";

    if (!file_exists(data_path) || file_exists(desc_path)) {
        return;
    }

    if (!file_mkpath(desc_path, 0777)) {
        l_warn("CH2: could not create parent path for %s", desc_path);
        return;
    }

    FILE *fp = fopen(desc_path, "wb");
    if (!fp) {
        l_warn("CH2: could not create %s errno=%d", desc_path, errno);
        return;
    }

    static const char k_desc[] =
        "local set = {}\n"
        "set.name = \"101-102_Available\"\n"
        "set.setName = \"101-102_Available\"\n"
        "set.descriptionFilenameOverride = \"\"\n"
        "set.logicalName = \"<101-102_Available>\"\n"
        "set.logicalDestination = \"<>\"\n"
        "set.priority = 130\n"
        "set.localDir = _currentDirectory\n"
        "set.enableMode = \"constant\"\n"
        "set.version = \"patch\"\n"
        "set.descriptionPriority = 0\n"
        "set.gameDataName = \"101-102_Available Game Data\"\n"
        "set.gameDataPriority = 0\n"
        "set.gameDataEnableMode = \"constant\"\n"
        "set.localDirIncludeBase = true\n"
        "set.localDirRecurse = false\n"
        "set.localDirIncludeOnly = nil\n"
        "set.localDirExclude = \n"
        "{\n"
        "    \"_dev/\"\n"
        "}\n"
        "set.gameDataArchives = \n"
        "{\n"
        "    _currentDirectory .. \"MCSM_android_101-102_Available_data.ttarch2\"\n"
        "}\n"
        "RegisterSetDescription(set)\n";

    const size_t len = strlen(k_desc);
    const size_t written = fwrite(k_desc, 1, len, fp);
    if (fflush(fp) != 0 || written != len) {
        l_warn("CH2: failed writing %s errno=%d", desc_path, errno);
    } else {
        l_info("CH2: created missing 101-102 availability descriptor at %s", desc_path);
    }
    fclose(fp);
}

/* CH2 REAL MOUNT, take 2 (2026-07-02): the patch.c runtime approach (pcall
 * NetworkAPICloudMountAllContent / DoString+RegisterSetDescription from a
 * native hook) was confirmed non-functional by device log: all three calls
 * returned rc=0 (no Lua error) but ResourceSetExists('Minecraft102') stayed
 * false — RegisterSetDescription is not a real _G global reachable from an
 * ad-hoc DoString chunk; it's only injected into the sandboxed environment
 * the engine's own resource-description FILE LOADER builds when it scans a
 * directory for _resdesc_50_*.lua files (that's how NetworkAPICloudMount*
 * would normally deliver content on Android too — by dropping files where
 * the scanner looks, not by calling Lua directly).
 *
 * The 101-102_Available descriptor above proves that scanner path works:
 * a .lua file written into assets/ before boot gets discovered, executed
 * with a real RegisterSetDescription, and shows up as
 * "TRACE: ResourceSetEnable('101-102_Available')" in the log. Do the same
 * for the REAL Minecraft102 episode set — write its descriptor into
 * assets/ (where the scanner looks) with gameDataArchives pointing at
 * absolute Net/ paths (where the actual ~767MB of episode data lives, left
 * there so we don't copy it). Only the single verified-correct 1004-byte
 * upstream descriptor is used as source of truth; the other *_02*.lua
 * files the chapter-2 upload dropped into Net/ are 1-byte placeholders
 * (not real content) and must NOT be re-registered. */
/* CH2 take 3 (2026-07-02 production): the Net/-based descriptor registered
 * the set (menu showed Ep2 installed) but the engine NEVER opened the Net/
 * archives -- launching Ep2 hung forever on a blank screen with zero
 * archive I/O. Every archive that actually works in this port loads from
 * DATA_PATH "assets/" through the AAssetManager flow. So Episode 2's
 * archives were MOVED into assets/ (FTP, 2026-07-02) and the ORIGINAL
 * upstream 102 descriptors (Minecraft102 + Android102 + JesseMale102 +
 * Shaders102 + languages + D102 packs, all `_currentDirectory`-relative)
 * were dropped verbatim into assets/, where the boot ResDesc scanner
 * registers them exactly like the working 101 sets. This fallback
 * generator now only covers a fresh install where the descriptor is
 * missing but the archive is already in assets/ -- and emits the same
 * assets/-relative content as the upstream file. */
static void ensure_minecraft102_descriptor(void) {
    const char *marker_archive = DATA_PATH "assets/MCSM_android_Minecraft102_data.ttarch2";
    const char *desc_path = DATA_PATH "assets/_resdesc_50_Minecraft102_android-pvr.lua";

    if (!file_exists(marker_archive) || file_exists(desc_path)) {
        return;
    }

    if (!file_mkpath(desc_path, 0777)) {
        l_warn("CH2: could not create parent path for %s", desc_path);
        return;
    }

    FILE *fp = fopen(desc_path, "wb");
    if (!fp) {
        l_warn("CH2: could not create %s errno=%d", desc_path, errno);
        return;
    }

    static const char k_desc[] =
        "local set = {}\n"
        "set.name = \"Minecraft102\"\n"
        "set.setName = \"Minecraft102\"\n"
        "set.descriptionFilenameOverride = \"_resdesc_50_Minecraft102_android-pvr.lua\"\n"
        "set.logicalName = \"<Minecraft102>\"\n"
        "set.logicalDestination = \"<>\"\n"
        "set.priority = 102\n"
        "set.localDir = _currentDirectory\n"
        "set.enableMode = \"bootable\"\n"
        "set.version = \"trunk\"\n"
        "set.descriptionPriority = 0\n"
        "set.gameDataName = \"Minecraft102 Game Data\"\n"
        "set.gameDataPriority = 0\n"
        "set.gameDataEnableMode = \"constant\"\n"
        "set.localDirIncludeBase = true\n"
        "set.localDirRecurse = false\n"
        "set.localDirIncludeOnly = nil\n"
        "set.localDirExclude = \n"
        "{\n"
        "    \"_dev/\"\n"
        "}\n"
        "set.gameDataArchives = \n"
        "{\n"
        "    _currentDirectory .. \"MCSM_android-pvr_Minecraft102_txmesh.ttarch2\",\n"
        "    _currentDirectory .. \"MCSM_android_Minecraft102_anichore.ttarch2\",\n"
        "    _currentDirectory .. \"MCSM_android_Minecraft102_data.ttarch2\",\n"
        "    _currentDirectory .. \"MCSM_android_Minecraft102_ms.ttarch2\",\n"
        "    _currentDirectory .. \"MCSM_android_Minecraft102_voice.ttarch2\"\n"
        "}\n"
        "RegisterSetDescription(set)\n";

    const size_t len = strlen(k_desc);
    const size_t written = fwrite(k_desc, 1, len, fp);
    if (fflush(fp) != 0 || written != len) {
        l_warn("CH2: failed writing %s errno=%d", desc_path, errno);
    } else {
        l_info("CH2: created real Minecraft102 episode descriptor at %s", desc_path);
    }
    fclose(fp);
}

/* GENERIC CHAPTER AUTO-DETECT (2026-07-06): for any chapter N whose data archive
 * is present in assets/ but has no descriptor yet, generate a working descriptor
 * (same shape as the Minecraft102 one, templated with the chapter number). This is
 * a FALLBACK — if the user drops the chapter's REAL _resdesc_50_*.lua alongside the
 * archives, file_exists(desc_path) is true and we skip (real one wins). Result:
 * drop a chapter's archives (+ optional real descriptors) into ux0:data/mcsm/assets/
 * and it is detected + mounts on next boot, with NO code change per chapter. */
static void ensure_chapter_descriptor(int ch) {
    char marker[256], desc_path[256], buf[2048];
    snprintf(marker, sizeof(marker), DATA_PATH "assets/MCSM_android_Minecraft10%d_data.ttarch2", ch);
    snprintf(desc_path, sizeof(desc_path), DATA_PATH "assets/_resdesc_50_Minecraft10%d_android-pvr.lua", ch);
    if (!file_exists(marker) || file_exists(desc_path)) return;
    if (!file_mkpath(desc_path, 0777)) { l_warn("CH%d: mkpath fail %s", ch, desc_path); return; }
    FILE *fp = fopen(desc_path, "wb");
    if (!fp) { l_warn("CH%d: create fail %s errno=%d", ch, desc_path, errno); return; }
    int n = snprintf(buf, sizeof(buf),
        "local set = {}\n"
        "set.name = \"Minecraft10%d\"\n"
        "set.setName = \"Minecraft10%d\"\n"
        "set.descriptionFilenameOverride = \"_resdesc_50_Minecraft10%d_android-pvr.lua\"\n"
        "set.logicalName = \"<Minecraft10%d>\"\n"
        "set.logicalDestination = \"<>\"\n"
        "set.priority = 10%d\n"
        "set.localDir = _currentDirectory\n"
        "set.enableMode = \"bootable\"\n"
        "set.version = \"trunk\"\n"
        "set.descriptionPriority = 0\n"
        "set.gameDataName = \"Minecraft10%d Game Data\"\n"
        "set.gameDataPriority = 0\n"
        "set.gameDataEnableMode = \"constant\"\n"
        "set.localDirIncludeBase = true\n"
        "set.localDirRecurse = false\n"
        "set.localDirIncludeOnly = nil\n"
        "set.localDirExclude = {\n    \"_dev/\"\n}\n"
        "set.gameDataArchives = {\n"
        "    _currentDirectory .. \"MCSM_android-pvr_Minecraft10%d_txmesh.ttarch2\",\n"
        "    _currentDirectory .. \"MCSM_android_Minecraft10%d_anichore.ttarch2\",\n"
        "    _currentDirectory .. \"MCSM_android_Minecraft10%d_data.ttarch2\",\n"
        "    _currentDirectory .. \"MCSM_android_Minecraft10%d_ms.ttarch2\",\n"
        "    _currentDirectory .. \"MCSM_android_Minecraft10%d_voice.ttarch2\"\n"
        "}\n"
        "RegisterSetDescription(set)\n",
        ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch);
    if (n > 0 && (size_t)n < sizeof(buf) && fwrite(buf, 1, (size_t)n, fp) == (size_t)n) {
        fflush(fp);
        l_info("CH%d: auto-generated Minecraft10%d descriptor (archive present, no real desc)", ch, ch);
    }
    fclose(fp);
}
static void ensure_all_chapter_descriptors(void) {
    for (int ch = 3; ch <= 8; ch++) ensure_chapter_descriptor(ch);
}

/* SAVE SYSTEM SUPPORT (2026-07-02, production pass) ---------------------
 *
 * 1) migrate_stranded_saves(): one intermediate build (SAVEREDIRECT2, with
 *    the since-removed GetBaseUserDirectory override active) wrote the real
 *    save files to the doubled path DATA_PATH "User/Temp/..." instead of
 *    DATA_PATH "Temp/...". Any saves made on that build are invisible to
 *    the fixed builds. Move them to the right place at boot (copy + delete;
 *    skip if the destination already exists so we never clobber a newer
 *    save with a stranded older one).
 *
 * 2) seed_empty_prefs_files(): disasm of GameEngine::SavePrefs shows it
 *    starts with ResourceFinder::LocateResource("prefs.prop") and EARLY-
 *    OUTS if the resource does not exist ANYWHERE -- prefs are only ever
 *    written over an existing file, never created (on Android the OS-side
 *    first-run flow provides one). The save path then re-serializes the
 *    whole PropertySet via a temp MetaStream (never reads old content), so
 *    an EMPTY file is a valid seed: boot-load of the empty file fails
 *    parse and falls back to defaults (harmless, matches how the device
 *    behaved for weeks with 0-byte prefs), and the first SavePrefs
 *    overwrites it with real data, after which it round-trips normally.
 *    Seed both prefs.prop (game prefs: character choice, current save
 *    slot, options) and user.prop (account/user prefs) under Temp/ --
 *    the location the whole save system now uses. */
static void migrate_stranded_saves(void) {
    static const char *k_names[] = {
        "saveslot1.bundle",
        "_saveslot1_autosave.bundle",
        "_saveslot1_checkpoint1.bundle",
        "_saveslot1_checkpoint2.bundle",
        "_saveslot1_checkpoint3.bundle",
        "_saveslot1_id.estore",
        "prefs.prop",
        "user.prop",
    };

    for (int i = 0; i < (int)(sizeof(k_names) / sizeof(k_names[0])); ++i) {
        char src[256];
        char dst[256];
        sceClibSnprintf(src, sizeof(src), DATA_PATH "User/Temp/%s", k_names[i]);
        sceClibSnprintf(dst, sizeof(dst), DATA_PATH "Temp/%s", k_names[i]);

        if (!file_exists(src) || file_exists(dst)) {
            continue;
        }

        FILE *in = fopen(src, "rb");
        if (!in) {
            continue;
        }
        FILE *out = fopen(dst, "wb");
        if (!out) {
            fclose(in);
            l_warn("SAVEMIGRATE: could not open %s for write errno=%d", dst, errno);
            continue;
        }

        char buf[16384];
        size_t total = 0;
        int failed = 0;
        for (;;) {
            size_t n = fread(buf, 1, sizeof(buf), in);
            if (n > 0) {
                if (fwrite(buf, 1, n, out) != n) {
                    failed = 1;
                    break;
                }
                total += n;
            }
            if (n < sizeof(buf)) {
                if (ferror(in)) {
                    failed = 1;
                }
                break;
            }
        }
        if (fflush(out) != 0) {
            failed = 1;
        }
        fclose(out);
        fclose(in);

        if (failed) {
            l_warn("SAVEMIGRATE: copy failed %s -> %s", src, dst);
            remove(dst);
        } else {
            remove(src);
            l_info("SAVEMIGRATE: %s -> %s (%u bytes)", src, dst, (unsigned)total);
        }
    }
}

static void seed_empty_prefs_files(void) {
    /* GameEngine::SavePrefs (native) serializes GetPreferences() to the
     * resource "prefs.prop" but EARLY-OUTS unless that resource is already
     * locatable (ResourceFinder::LocateResource != null). The Android files
     * dir maps to DATA_PATH (ux0:data/mcsm/) via our getFilesDir stub, so
     * seed the empty file at BOTH the data-root (where the engine's default
     * resource location searches) and Temp/ (where our save system lives),
     * plus the user.prop + game_prefs.prop variants the engine also probes.
     * Empty is a valid seed: SavePrefs re-serializes wholesale (never reads
     * the old bytes); the first save fills it. */
    static const char *k_paths[] = {
        DATA_PATH "prefs.prop",
        DATA_PATH "user.prop",
        DATA_PATH "game_prefs.prop",
        DATA_PATH "Temp/prefs.prop",
        DATA_PATH "Temp/user.prop",
        DATA_PATH "Temp/game_prefs.prop",
        /* End-of-chapter player choices (ChoiceStats_SaveSeen serializes them to
         * "choicestats.prop") save via the SAME write-over-existing-file-only path
         * as SavePrefs, so without a pre-existing file the save early-outs and the
         * choices never persist. Seed it like the prefs files. */
        DATA_PATH "choicestats.prop",
        DATA_PATH "Temp/choicestats.prop",
        /* Cross-chapter choice carryover ("next chapter" presenter): the game
         * saves the choice_tracker document to logical:<User>/choice.prop via
         * SaveDownloadedDocumentAsPropertySet. Same write-over-existing rule ->
         * seed it too, or the carryover is inconsistent/empty. */
        DATA_PATH "choice.prop",
        DATA_PATH "Temp/choice.prop",
    };

    for (int i = 0; i < (int)(sizeof(k_paths) / sizeof(k_paths[0])); ++i) {
        if (file_exists(k_paths[i])) {
            continue;
        }
        FILE *fp = fopen(k_paths[i], "wb");
        if (fp) {
            fclose(fp);
            l_info("SAVESEED: created empty %s (SavePrefs needs the file to pre-exist)", k_paths[i]);
        } else {
            l_warn("SAVESEED: could not create %s errno=%d", k_paths[i], errno);
        }
    }
}

/* PRE-WARMED SHADER CACHE (2026-07-03) -------------------------------------
 * The stutters during scene streaming (luaScenePreload bursts) are ShaccCg
 * GLSL->GXM compiles, 400-900ms each, once per never-before-seen shader --
 * a Vita hardware limit (shaders compile at runtime, there is no offline
 * path). The loader-side program cache (glutil.c) already eliminates the
 * SECOND+ encounter of every shader, but a FRESH install / cleared data
 * starts cold and stutters through the first playthrough while the cache
 * fills. Ship the cache pre-filled: a full-playthrough's worth of compiled
 * .gxp binaries (1001 programs, ~2.3MB) is bundled in the VPK as
 * app0:/progcache_seed.bin (a simple [MCPB|count|{nameLen,name,dataLen,data}
 * ...] blob) and unpacked into ux0:data/mcsm_progcache/ on first boot. Files
 * that already exist are SKIPPED (never clobber a newer/user-compiled entry),
 * so this is a one-time cold-start seed that self-disables once populated.
 * Result: a fresh device plays already-covered content with ZERO shader
 * stutters -- the compiles happened once, on the dev device, and ship
 * pre-done (the console "precompiled PSO cache" model). */
static void seed_shader_cache(void) {
    const char *dir = "ux0:data/mcsm_progcache";
    const char *blob_path = "app0:/progcache_seed.bin";

    /* Always ensure the progcache dir exists at boot, even when NO seed blob is
     * bundled (the shipping case — cache accumulates on-device). Previously the
     * mkdir lived AFTER the no-blob early-return, so a fresh install had no dir
     * until progcache_save happened to create it. Create it up front. */
    sceIoMkdir(dir, 0777);

    if (!file_exists(blob_path)) {
        return; /* no bundled cache — dir is ready, device will fill it */
    }

    FILE *bf = fopen(blob_path, "rb");
    if (!bf) {
        return;
    }

    unsigned char hdr[8];
    if (fread(hdr, 1, 8, bf) != 8 || hdr[0] != 'M' || hdr[1] != 'C' ||
        hdr[2] != 'P' || hdr[3] != 'B') {
        fclose(bf);
        l_warn("SHADERSEED: bad blob header");
        return;
    }
    sceIoMkdir(dir, 0777);
    uint32_t count = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                     ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);

    static unsigned char buf[65536];
    uint32_t written = 0, skipped = 0;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char nl[2];
        if (fread(nl, 1, 2, bf) != 2) break;
        uint32_t name_len = (uint32_t)nl[0] | ((uint32_t)nl[1] << 8);
        char name[128];
        if (name_len == 0 || name_len >= sizeof(name)) break;
        if (fread(name, 1, name_len, bf) != name_len) break;
        name[name_len] = '\0';

        unsigned char dl[4];
        if (fread(dl, 1, 4, bf) != 4) break;
        uint32_t data_len = (uint32_t)dl[0] | ((uint32_t)dl[1] << 8) |
                            ((uint32_t)dl[2] << 16) | ((uint32_t)dl[3] << 24);

        char out_path[192];
        sceClibSnprintf(out_path, sizeof(out_path), "%s/%s", dir, name);

        if (file_exists(out_path) || data_len > (16u * 1024u * 1024u)) {
            /* already present (user/newer) -- skip its bytes without writing */
            uint32_t remaining = data_len;
            while (remaining > 0) {
                uint32_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
                if (fread(buf, 1, chunk, bf) != chunk) { remaining = 0; break; }
                remaining -= chunk;
            }
            skipped++;
            continue;
        }

        FILE *of = fopen(out_path, "wb");
        int ok = of != NULL;
        uint32_t remaining = data_len;
        while (remaining > 0) {
            uint32_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
            if (fread(buf, 1, chunk, bf) != chunk) { ok = 0; break; }
            if (of && fwrite(buf, 1, chunk, of) != chunk) { ok = 0; }
            remaining -= chunk;
        }
        if (of) { fflush(of); fclose(of); }
        if (ok) {
            written++;
        } else {
            if (of) remove(out_path);
        }
    }
    fclose(bf);
    l_info("SHADERSEED: pre-warmed shader cache seeded (%u written, %u already present, %u total)",
           written, skipped, count);
}

static void verify_data_files(void) {
    for (int i = 0; i < (int)(sizeof(g_modules) / sizeof(g_modules[0])); ++i) {
        if (!file_exists(g_modules[i].path)) {
            fatal_error("Missing %s at %s.\n\nCopy Android native libraries from your legally obtained APK to this path.",
                        g_modules[i].name, g_modules[i].path);
        }
    }
}

static void load_all_modules(void) {
    for (int i = 0; i < (int)(sizeof(g_modules) / sizeof(g_modules[0])); ++i) {
        if (so_file_load(g_modules[i].mod, g_modules[i].path, g_modules[i].load_addr) < 0) {
            l_fatal("Could not load %s.", g_modules[i].name);
            fatal_error("Error: could not load %s from %s.", g_modules[i].name, g_modules[i].path);
        }
        l_success("Loaded %s.", g_modules[i].name);
    }
}

static void relocate_all_modules(void) {
    for (int i = 0; i < (int)(sizeof(g_modules) / sizeof(g_modules[0])); ++i) {
        so_relocate(g_modules[i].mod);
        l_success("Relocated %s.", g_modules[i].name);
    }
}

static void resolve_all_imports(void) {
    for (int i = 0; i < (int)(sizeof(g_modules) / sizeof(g_modules[0])); ++i) {
        resolve_imports(g_modules[i].mod);
        l_success("Resolved imports for %s.", g_modules[i].name);
    }
}

static void flush_all_caches(void) {
    for (int i = 0; i < (int)(sizeof(g_modules) / sizeof(g_modules[0])); ++i) {
        so_flush_caches(g_modules[i].mod);
        l_success("Flushed caches for %s.", g_modules[i].name);
    }
}

static void initialize_all_modules(void) {
    for (int i = 0; i < (int)(sizeof(g_modules) / sizeof(g_modules[0])); ++i) {
        so_initialize(g_modules[i].mod);
        l_success("Initialized %s.", g_modules[i].name);
    }
}

void soloader_init_all() {
    log_reset_file();
    /* BUILD STAMP — first line of every log so old vs new eboot is unmistakable.
     * If this line is ABSENT, an OLD eboot is running (VitaShell kept it on a
     * same-Title-ID install → delete the bubble and reinstall). */
    telemetry_log("BOOT", "BUILD=2026-07-22-r4-coreFIX+settings (GC user-core fallback; profile+override cfg; chapters auto/toggles)");
    telemetry_log("BOOT", "soloader_init_all start");

	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Clocks. Stock scePowerSetArmClockFrequency caps at 444MHz; going higher
    // (up to 500) needs a kernel OC plugin that unlocks the PLL. We ATTEMPT the
    // requested value, fall back to 444 if the set is rejected, then read the
    // clock BACK so the log tells us exactly what the hardware accepted.
    // ONE-DOC config: ux0:data/mcsm/settings/clock.txt (or the data root) sets it all
    //   "off"      -> disable the in-game governor (ARM pinned to this boot clock)
    //   "min <MHz>"/"max <MHz>" -> governor floor / ceiling
    //   "gpu <MHz>" -> GPU clock
    // The ARM clock set here is the BOOT = governor CEILING (arm_max); during gameplay
    // the adaptive governor (patch.c mcsm_clock_governor_tick) scales ARM DOWN toward
    // arm_min when scenes are light+smooth and jumps back up the instant a frame is
    // heavy OR the render is dropping. Booting at the ceiling keeps module-load +
    // first-scene shader compiles fast.
    McsmClockCfg clkcfg;
    mcsm_read_clock_cfg(&clkcfg);
    int want_arm = clkcfg.arm_max;   /* boot at the governor ceiling */
    int want_gpu = clkcfg.gpu;
    int rc_arm = scePowerSetArmClockFrequency(want_arm);
    if (rc_arm < 0 && want_arm > 444) { scePowerSetArmClockFrequency(444); want_arm = 444; }
    scePowerSetBusClockFrequency(222);
    int rc_gpu = scePowerSetGpuClockFrequency(want_gpu);
    if (rc_gpu < 0 && want_gpu > 222) { scePowerSetGpuClockFrequency(222); want_gpu = 222; }
    scePowerSetGpuXbarClockFrequency(166);
    l_info("CLOCK: arm req=%d got=%dMHz (rc=0x%08X) | gpu req=%d got=%dMHz (rc=0x%08X) | bus=%dMHz",
           want_arm, scePowerGetArmClockFrequency(), (unsigned)rc_arm,
           want_gpu, scePowerGetGpuClockFrequency(), (unsigned)rc_gpu,
           scePowerGetBusClockFrequency());

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    verify_data_files();

#ifdef USE_PVR_PSP2
    l_info("PVR: loading PVR kernel modules...");
    pvr_load_modules();
    l_info("PVR: initializing PVR apphint...");
    pvr_init_apphint();
    {
        const int fb_w = mcsm_get_framebuffer_width();
        const int fb_h = mcsm_get_framebuffer_height();
        l_info("PVR: initializing EGL early before Android module init...");
        pvr_init_gl(fb_w, fb_h);
        loading_screen_init(fb_w, fb_h);
        loading_screen_set_status("Native libraries loading...");
        loading_screen_set_progress(0.15f);
        loading_screen_render();
        pvr_release_current();
    }
#endif

    load_all_modules();

    ensure_runtime_directories();
    ensure_episode2_available_descriptor();
    ensure_minecraft102_descriptor();
    ensure_all_chapter_descriptors();   /* CH3-CH8: auto-detect any chapter whose archives are present */
    migrate_stranded_saves();
    seed_empty_prefs_files();
    seed_shader_cache();

    relocate_all_modules();
    resolve_all_imports();

    so_patch();
    l_success("Patches applied.");

    flush_all_caches();
    initialize_all_modules();

#ifdef USE_PVR_PSP2
    l_info("PVR: EGL was initialized before Android module init; skipping gl_preload.");
#else
    gl_preload();
    l_success("OpenGL preloaded.");
#endif

    jni_init();
    l_success("FalsoJNI initialized.");

    controls_init();
    l_success("Controls initialized.");
}
