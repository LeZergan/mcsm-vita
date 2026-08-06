/*
 * FEASIBILITY PROBE: can the engine be driven by the NATIVE Vita FMOD module
 * (libfmodstudio.suprx) instead of the Android libfmod.so + faked OpenSL ES?
 *
 * WHY THIS IS A PROBE AND NOT THE REWRITE
 * The native module is a real signed SELF: only its ELF header is plaintext and
 * every code segment is encrypted, so its export table cannot be read offline.
 * That leaves two questions unanswerable by inspection, both fatal if wrong:
 *
 *   1. Does it export the C++ API at all? libGameEngine imports 124 mangled
 *      C++ symbols (_ZN4FMOD...) and only 2 C-API ones. If the Vita build
 *      exports just the C API -- entirely plausible, since FMOD ships consoles
 *      as static libs and the C++ layer is often a thin wrapper -- then none of
 *      the engine's imports resolve and the approach is dead on arrival.
 *   2. Is it the same FMOD version? The engine was built against FMOD Studio
 *      1.x (380 FMOD5_* exports in libfmod.so). A different 1.x release changes
 *      struct layouts, and mismatched ABI crashes inside the mixer rather than
 *      failing cleanly.
 *
 * The Vita kernel CAN decrypt and load the module, and taihen_stub is linked, so
 * NIDs can be resolved at runtime. That makes the questions answerable ON DEVICE
 * for the cost of one load -- which is what this file does, and nothing more.
 *
 * OFF unless settings/fmod_native_probe.txt exists. It does not touch the
 * audio path, does not change what the engine links against, and unloads the
 * module before returning. A negative result costs one boot; a positive result
 * means the rewrite is worth doing and tells us exactly which symbols exist.
 *
 * NIDs below are the standard Vita export hash: first 4 bytes of SHA-1 of the
 * symbol name, read little-endian.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/modulemgr.h>
#include <psp2/io/stat.h>
#include <taihen.h>

#include "../utils/logger.h"
#include "../utils/config.h"   /* fmod_probe */

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/mcsm/"
#endif

static const struct { const char *name; uint32_t nid; } k_fmod_probe_nids[] = {
    /* C API -- what a console/plugin integration would need at minimum. */
    { "FMOD_System_Create",                  0xF2220F22u },
    { "FMOD_System_Init",                    0x01C144DAu },
    { "FMOD_System_GetVersion",              0x4B2CCD6Au },
    { "FMOD_System_RegisterOutput",          0x516A722Eu },
    { "FMOD_System_SetOutputByPlugin",       0xDF31037Du },
    /* FMOD Studio 1.x low-level C exports (libfmod.so has 380 of these). */
    { "FMOD5_System_Create",                 0x5DE750F9u },
    { "FMOD5_System_Init",                   0xC9A400B2u },
    { "FMOD5_System_GetVersion",             0xDE5DC9D5u },
    /* The decisive ones: mangled C++ symbols the ENGINE actually imports. If
     * these resolve, the native module can back libGameEngine directly. */
    { "_ZN4FMOD6System4initEijPv",           0xF31D252Au },
    { "_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE", 0xF7888256u },
    { "_ZN4FMOD6Studio6System6createEPPS1_j", 0xBA82A903u },
};

/* Library NIDs are unknown for this module, so ask taiHEN to search every
 * exporting library in it by passing TAI_ANY_LIBRARY. */
static int probe_symbol(const char *modname, const char *sym, uint32_t nid) {
    uintptr_t addr = 0;
    int rc = taiGetModuleExportFunc(modname, TAI_ANY_LIBRARY, nid, &addr);
    if (rc >= 0 && addr) {
        l_info("FMODNATIVE:   FOUND  nid=0x%08X addr=0x%08X  %s", nid, (unsigned)addr, sym);
        return 1;
    }
    l_info("FMODNATIVE:   absent nid=0x%08X rc=0x%08X  %s", nid, (unsigned)rc, sym);
    return 0;
}

void mcsm_fmod_native_probe(void) {
    if (!mcsm_game()->fmod_probe) return;   /* game.txt fmod_probe */

    /* r41 tried ux0:data/mcsm/ and got 0x8002D003 with the file definitely present
     * (verified 1156243 bytes over FTP). That is the sandbox, not a missing file:
     * a user app may read ux0:data freely but sceKernelLoadStartModule will only
     * take modules from the application's OWN mount. NetStream ships these under
     * its CONTENTS/module/ for exactly that reason. app0: is our package root, so
     * try there first and keep the old paths last to confirm the diagnosis. */
    const char *paths[] = {
        "app0:module/libfmodstudio.suprx",
        "app0:libfmodstudio.suprx",
        DATA_PATH "libfmodstudio.suprx",
    };

    /* libfmodngpext is FMOD's Vita ("next generation portable") platform layer.
     * If libfmodstudio depends on it, it must be resident FIRST or the load fails
     * on an unresolved import rather than telling us anything useful. Failure here
     * is not fatal to the probe -- log it and continue. */
    {
        const char *ext[] = { "app0:module/libfmodngpext.suprx", "app0:libfmodngpext.suprx" };
        for (unsigned e = 0; e < sizeof(ext) / sizeof(ext[0]); e++) {
            SceUID x = sceKernelLoadStartModule(ext[e], 0, NULL, 0, NULL, NULL);
            l_info("FMODNATIVE: dep %s -> 0x%08X%s", ext[e], (unsigned)x,
                   x >= 0 ? " (loaded)" : "");
            if (x >= 0) break;
        }
    }

    l_info("FMODNATIVE: probe enabled (fmod_native_probe.txt present)");
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        SceUID mod = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, NULL);
        if (mod < 0) {
            l_warn("FMODNATIVE: load FAILED %s rc=0x%08X", paths[i], (unsigned)mod);
            continue;
        }
        l_info("FMODNATIVE: LOADED %s modid=0x%08X — the kernel accepted and decrypted it",
               paths[i], (unsigned)mod);

        int found = 0;
        const int total = (int)(sizeof(k_fmod_probe_nids) / sizeof(k_fmod_probe_nids[0]));
        for (int k = 0; k < total; k++) {
            found += probe_symbol("libfmodstudio", k_fmod_probe_nids[k].name,
                                  k_fmod_probe_nids[k].nid);
        }
        l_info("FMODNATIVE: RESULT %d/%d probe symbols resolved. %s",
               found, total,
               found == 0
                   ? "None -- module exports nothing we recognise; native-FMOD route is dead."
               : found < 9
                   ? "C API only -- an output plugin against the Android lib remains the better path."
                   : "C++ API present -- the engine could be linked against the native module.");

        int stop_rc = sceKernelStopUnloadModule(mod, 0, NULL, 0, NULL, NULL);
        l_info("FMODNATIVE: unloaded modid=0x%08X rc=0x%08X (probe leaves nothing running)",
               (unsigned)mod, (unsigned)stop_rc);
        return;
    }
    l_warn("FMODNATIVE: every path failed. If all returned 0x8002D003 the module is present but the sandbox refuses it; if app0: paths differ, the module itself is being rejected (wrong authid/signature for this title).");
}
