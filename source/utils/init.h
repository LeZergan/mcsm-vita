/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  init.h
 * @brief so-loader initialization routines.
 */

#ifndef SOLOADER_INIT_H
#define SOLOADER_INIT_H

#include <so_util/so_util.h>

#ifdef __cplusplus
extern "C" {
#endif

void resolve_imports(so_module *mod);

void so_patch();

void soloader_init_all();

/* Keep native and Lua preference-resource locations synchronized.
 *
 * The snapshot/after pair MUST bracket the native SavePrefs call: the mirror decides
 * which of the two copies is authoritative by seeing which one the engine actually
 * changed. Calling the "after" half without the "before" half falls back to
 * newest-wins, which is safe but coarser. */
void mcsm_prefs_snapshot_before_save(void);
void mcsm_sync_prefs_after_save(void);
void mcsm_sync_prefs_files(void);

/* Boot-time save-data integrity pass: restores a save file from its backup when the
 * live one is provably broken, and refreshes the backup otherwise. */
void mcsm_save_backup_pass(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_INIT_H
