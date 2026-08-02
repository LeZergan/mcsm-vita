/*
 * PS Vita trophy backend for the MCSM loader.
 *
 * The engine's Lua already calls PlatformUnlockAchievement("<name>") at the right
 * story beats; on Android that reached Google Play Games, and on Vita it dead-ends in
 * Platform_Android::UnlockAchievement. This module gives that call somewhere to go:
 * an achievement NAME is mapped to a Vita trophy ID and unlocked through sceNpTrophy.
 *
 * EVERYTHING HERE FAILS SOFT. Trophies need two things this loader cannot ship:
 *   1. an unencrypted Vita-format TROPHY.TRP under
 *        ux0:app/<TITLEID>/sce_sys/trophy/<COMMID>_00/TROPHY.TRP
 *      (the path is keyed on the NP COMM ID, not the title id -- see trophies.c)
 *   2. the NoTrpDrm plugin, which disables the NP communication-signature check and
 *      the TRP signature check (homebrew cannot sign either)
 * If either is missing, mcsm_trophies_init() logs why and returns < 0, and every
 * later unlock is a no-op. The game must never fail to boot because trophies are
 * unavailable.
 */
#ifndef MCSM_TROPHIES_H
#define MCSM_TROPHIES_H

#include <stdint.h>

/* Bring up sceNpTrophy, registering the set if the system does not already have it.
 * MUST be called from the thread that owns the GL context, after gl_init(), because
 * the setup dialog is a Vita common dialog and has to be pumped with a buffer swap.
 * The dialog runs ONLY on a launch where the set is not yet registered -- it is the
 * registration, and re-running it every boot both presents frames (killing the boot
 * picture) and repeats an operation whose stored state this port has seen go bad.
 * Returns 0 on success, negative if trophies are unavailable (never fatal). */
int mcsm_trophies_init(void);

/* Unlock by trophy ID. Safe to call from any thread and from a hot path: it only
 * flags the ID and signals a worker, because sceNpTrophyUnlockTrophy BLOCKS (it
 * writes the trophy file and raises the system notification) and must never run on
 * the engine's script or render thread. Re-unlocking an already-unlocked ID is
 * ignored. */
void mcsm_trophies_unlock(uint32_t id);

/* Unlock by the engine's achievement name, resolved through the name->ID map read
 * from ux0:data/mcsm/settings/trophies.txt. Unknown names are logged (throttled) rather than
 * dropped, so a single playthrough reveals the exact strings the game uses -- they
 * live in packed .ttarch2 Lua and cannot be extracted offline. */
void mcsm_trophies_unlock_by_name(const char *name);

/* 1 if the trophy system came up and unlocks will actually be delivered. */
int mcsm_trophies_available(void);

/* Drive the first-boot setup dialog from the presenter. Call once per
 * present; returns 1 while the dialog is up (composite it). */
int mcsm_trophies_setup_poll(void);
int mcsm_trophies_setup_active(void);

/* 1 if this trophy is already held -- seeded at init from the SYSTEM's own store, so
 * it covers earlier sessions, not just this one. Used to drop the engine's repeated
 * unlock calls silently when a story beat replays. */
int mcsm_trophies_already_unlocked(uint32_t id);

#endif /* MCSM_TROPHIES_H */
