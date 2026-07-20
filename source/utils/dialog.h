/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      fgsfds
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dialog.h
 * @brief Common dialog for error messages and cheats input.
 */

#ifndef SOLOADER_DIALOG_H
#define SOLOADER_DIALOG_H

#ifdef __cplusplus
extern "C" {
#endif

int init_ime_dialog(const char *title, const char *initial_text);

char *get_ime_dialog_result(void);

/* Save-rename keyboard: non-blocking IME driven from gl_swap (see dialog.c). */
void mcsm_ime_begin(const char *initial);
int mcsm_ime_is_active(void);
int mcsm_ime_poll(char **out);

/* Engine virtual-keyboard bridge (drives the SAME Vita IME, but captures the result
 * for TTPlatform::GetVirtualKeyboardResult instead of injecting keys). */
void mcsm_ime_begin_vkbd(const char *initial);
int mcsm_vkbd_finished(void);
const char *mcsm_vkbd_result(int *cancelled);
void mcsm_vkbd_reset(void);

int init_msg_dialog(const char *msg);

int get_msg_dialog_result(void);

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_DIALOG_H
