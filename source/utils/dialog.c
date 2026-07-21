/*
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2021 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/dialog.h"
#include "utils/telemetry.h"

#include <string.h>
#include <stdarg.h>
#include <psp2/appmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/message_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#ifdef USE_PVR_PSP2
#include <EGL/egl.h>
#else
#include <vitaGL.h>
#endif

static uint16_t ime_title_utf16[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static uint16_t ime_initial_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH];
static uint16_t ime_input_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint8_t ime_input_text_utf8[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

void _utf16_to_utf8(const uint16_t *src, uint8_t *dst) {
    for (int i = 0; src[i]; i++) {
        if ((src[i] & 0xFF80) == 0) {
            *(dst++) = src[i] & 0xFF;
        } else if ((src[i] & 0xF800) == 0) {
            *(dst++) = ((src[i] >> 6) & 0xFF) | 0xC0;
            *(dst++) = (src[i] & 0x3F) | 0x80;
        } else if ((src[i] & 0xFC00) == 0xD800 && (src[i + 1] & 0xFC00) == 0xDC00) {
            uint32_t cp = 0x10000 + ((src[i] - 0xD800) << 10) + (src[i + 1] - 0xDC00);
            *(dst++) = ((cp >> 18) & 0x07) | 0xF0;
            *(dst++) = ((cp >> 12) & 0x3F) | 0x80;
            *(dst++) = ((cp >> 6) & 0x3F) | 0x80;
            *(dst++) = (cp & 0x3F) | 0x80;
            i++;
        } else {
            *(dst++) = ((src[i] >> 12) & 0x0F) | 0xE0;
            *(dst++) = ((src[i] >> 6) & 0x3F) | 0x80;
            *(dst++) = (src[i] & 0x3F) | 0x80;
        }
    }
    *dst = '\0';
}

int init_ime_dialog(const char *title, const char *initial_text) {
    for (int i = 0; title[i]; i++) {
        ime_title_utf16[i] = title[i];
    }
    for (int i = 0; initial_text[i]; i++) {
        ime_initial_text_utf16[i] = initial_text[i];
    }

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);

    param.supportedLanguages = 0x0001FFFF;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = 0;
    param.title = ime_title_utf16;
    param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    param.initialText = ime_initial_text_utf16;
    param.inputTextBuffer = ime_input_text_utf16;

    return sceImeDialogInit(&param);
}

char *get_ime_dialog_result(void) {
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return NULL;
    SceImeDialogResult res;
    sceImeDialogGetResult(&res);
    if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        _utf16_to_utf8(ime_input_text_utf16, ime_input_text_utf8);
        sceImeDialogTerm();
        return (char *)ime_input_text_utf8;
    }
    sceImeDialogTerm();
    return NULL;
}

/* SAVE-RENAME KEYBOARD (2026-07-18): non-blocking IME driven from the render
 * loop. The game calls luaPlatformShowKeyboard (hooked in patch.c) -> mcsm_ime_begin;
 * gl_swap calls mcsm_ime_poll every frame (the Vita common dialog draws itself
 * during vglSwapBuffers), and on ENTER the typed UTF8 is fed back to the engine. */
static int g_ime_active = 0;

/* VIRTUAL-KEYBOARD (2026-07-20): the engine's TTPlatform::{Open,IsFinished,GetResult}
 * VirtualKeyboard are vtable stubs (Open=nop, IsFinished=return 1, GetResult=nop) —
 * so the rename flow "instantly finishes" with an empty name. Hooking them (patch.c)
 * to drive this same Vita IME makes rename work. This mode captures the result HERE
 * (instead of the SELECT-button key-injection path) so GetVirtualKeyboardResult can
 * hand it back. */
static int  g_vkbd_mode = 0;        /* IME was raised via the engine vkbd hook */
static int  g_vkbd_finished = 0;    /* set by mcsm_ime_poll (render thread) on finish */
static int  g_vkbd_cancelled = 0;
static char g_vkbd_result[128];

void mcsm_ime_begin_vkbd(const char *initial) {
    g_vkbd_mode = 1; g_vkbd_finished = 0; g_vkbd_cancelled = 0; g_vkbd_result[0] = '\0';
    mcsm_ime_begin(initial);
    if (!g_ime_active) { g_vkbd_finished = 1; g_vkbd_cancelled = 1; }  /* init failed -> report cancelled */
}
int mcsm_vkbd_finished(void) { return g_vkbd_finished; }
const char *mcsm_vkbd_result(int *cancelled) { if (cancelled) *cancelled = g_vkbd_cancelled; return g_vkbd_result; }
void mcsm_vkbd_reset(void) { g_vkbd_mode = 0; g_vkbd_finished = 0; g_vkbd_cancelled = 0; g_vkbd_result[0] = '\0'; }

void mcsm_ime_begin(const char *initial) {
    if (g_ime_active) return;
    int rc = init_ime_dialog("Enter name", (initial && initial[0]) ? initial : " ");
    if (rc >= 0) g_ime_active = 1;
}

int mcsm_ime_is_active(void) { return g_ime_active; }

/* Non-blocking. Returns 1 exactly once when the dialog finishes, with *out = the
 * entered UTF8 string (or NULL if the user cancelled); 0 while running/inactive. */
int mcsm_ime_poll(char **out) {
    if (!g_ime_active) return 0;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
    g_ime_active = 0;
    char *res = get_ime_dialog_result();   /* terminates the dialog */
    if (g_vkbd_mode) {
        /* engine-vkbd path: stash the result for GetVirtualKeyboardResult and
         * DON'T feed keys (return 0 so the gl_swap key-injection path is skipped). */
        if (res) { int i = 0; for (; res[i] && i < (int)sizeof(g_vkbd_result) - 1; ++i) g_vkbd_result[i] = res[i]; g_vkbd_result[i] = '\0'; g_vkbd_cancelled = 0; }
        else { g_vkbd_result[0] = '\0'; g_vkbd_cancelled = 1; }
        g_vkbd_finished = 1;
        return 0;
    }
    if (out) *out = res;
    return 1;
}

int init_msg_dialog(const char *msg) {
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    SceMsgDialogUserMessageParam user_msg;
    memset(&user_msg, 0, sizeof(user_msg));
    user_msg.msg = (SceChar8 *)msg;
    user_msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    param.userMsgParam = &user_msg;
    return sceMsgDialogInit(&param);
}

int get_msg_dialog_result(void) {
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;
    sceMsgDialogTerm();
    return 1;
}

void fatal_error(const char *fmt, ...) {
    va_list list;
    char string[512];

    va_start(list, fmt);
    sceClibVsnprintf(string, sizeof(string), fmt, list);
    va_end(list);

    telemetry_log("FATAL", "%s", string);

#ifdef USE_PVR_PSP2
    /* PVR display already initialized by gl_init(); rely on
     * the global EGL display/surface set by pvr_init_gl. */
    extern void *g_pvr_egl_display;
    extern void *g_pvr_egl_surface;
#else
    vglInit(0);
#endif

    init_msg_dialog(string);

    while (!get_msg_dialog_result())
#ifdef USE_PVR_PSP2
        eglSwapBuffers(g_pvr_egl_display, g_pvr_egl_surface);
#else
        vglSwapBuffers(GL_TRUE);
#endif

    sceKernelExitProcess(0);

    sceKernelExitProcess(0);
    while (1);
}
