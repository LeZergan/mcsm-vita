/*
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2021 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/dialog.h"
#include "utils/logger.h"
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

/* Frames to keep telling the presenter a common dialog is up AFTER sceImeDialogTerm().
 * See get_ime_dialog_result(). Declared here because init/teardown live above the IME
 * state block below. */
static int g_ime_compositing_grace = 0;

static uint16_t ime_title_utf16[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static uint16_t ime_initial_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH];
static uint16_t ime_input_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
/* ☠ 4 BYTES PER UTF-16 UNIT, NOT 1. This was sized MAX_TEXT_LENGTH+1, which only
 * survives because SCE_IME_TYPE_BASIC_LATIN keeps every character to a single UTF-8
 * byte -- 2048 chars + NUL is exactly 2049, fitting by ONE byte with no margin at all.
 * _utf16_to_utf8() has no output bound, so the moment the dialog type changes (or the
 * type turns out not to fully constrain input -- supportedLanguages is 0x0001FFFF,
 * i.e. every language) a 2-byte codepoint overflows this by 2048 bytes and a 3-byte one
 * by 4096, straight over whatever static follows it. Sizing for the true worst case
 * costs 6KB of .bss and removes the knife-edge entirely. */
static uint8_t ime_input_text_utf8[SCE_IME_DIALOG_MAX_TEXT_LENGTH * 4 + 1];

/* `cap` is the size of dst INCLUDING the terminator. Bounded because the caller's
 * buffer sizing is an invariant that has already been wrong once. */
void _utf16_to_utf8_n(const uint16_t *src, uint8_t *dst, int cap) {
    uint8_t *const end = dst + cap - 4;   /* worst single codepoint writes 4 bytes */
    for (int i = 0; src[i]; i++) {
        if (dst >= end) break;
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

/* ☠ BOUNDED AND NUL-TERMINATED. Both loops here used to copy until the SOURCE ran out,
 * writing no terminator and checking no length:
 *   - Nothing terminated the destination. It only ever looked right because the arrays
 *     are static (zero-filled at startup) and every call so far passed the SAME strings.
 *     Feed a shorter string after a longer one and the tail of the previous one is
 *     still there: "Save 2" after "Enter Save Game Name" reads back as
 *     "Save 2Save Game Name". This became reachable the moment the dialog started
 *     being prefilled with the CURRENT save name, which varies in length every time.
 *   - Nothing bounded the copy. Titles cap at 128 and text at 2048 UTF-16 units, and
 *     the initial text is a user-chosen save name, so a long enough one walked off the
 *     end of a static buffer into whatever follows it.
 * Neither had bitten yet; both were one behaviour change away from doing so. */
static void utf8_to_utf16_bounded(const char *src, uint16_t *dst, int cap) {
    int i = 0;
    if (src) {
        for (; src[i] && i < cap - 1; i++) dst[i] = (uint16_t)(unsigned char)src[i];
    }
    dst[i] = 0;
}

int init_ime_dialog(const char *title, const char *initial_text) {
    utf8_to_utf16_bounded(title, ime_title_utf16, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
    utf8_to_utf16_bounded(initial_text, ime_initial_text_utf16, SCE_IME_DIALOG_MAX_TEXT_LENGTH);

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
    /* ☠ MUST BE ZEROED. SceImeDialogResult carries `SceChar8 reserved[28]`, and this
     * was passed straight off the stack with those 28 bytes holding whatever garbage
     * was there. The IME then came up exactly ONCE per boot and every later
     * sceImeDialogInit returned 0x8002047F = SCE_COMMON_DIALOG_ERROR_UNEXPECTED_FATAL
     * -- the common-dialog subsystem is knocked into a fatal state it never leaves,
     * so this poisons every dialog in the process, not just the keyboard.
     * Sony's own sceImeDialogParamInit() memsets its struct for exactly this reason;
     * the result struct needs the same treatment and never got it. */
    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    const int entered = (res.button == SCE_IME_DIALOG_BUTTON_ENTER);
    if (entered) _utf16_to_utf8_n(ime_input_text_utf16, ime_input_text_utf8,
                                  (int)sizeof(ime_input_text_utf8));
    sceImeDialogTerm();
    /* Hold the presenter's has_commondialog flag on for a few more frames: Term is a
     * request, and the compositor still has teardown to do on subsequent presents.
     * Dropping it the instant Term is called is the other way to corrupt this state. */
    g_ime_compositing_grace = 8;
    return entered ? (char *)ime_input_text_utf8 : NULL;
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

/* ☠ CROSS-THREAD. mcsm_ime_poll runs on the RENDER thread and publishes the typed name;
 * the engine reads it from the SIM thread via the JNI text-dialog methods. The writes
 * must not become visible out of order, or the engine can observe finished=1 while
 * g_vkbd_result still holds the previous name (or an empty string) -- a silent
 * wrong-name rename, not a crash, so nothing would ever flag it. Publish with a release
 * barrier, consume with an acquire one. */
void mcsm_ime_begin_vkbd(const char *title, const char *initial) {
    g_vkbd_finished = 0; g_vkbd_cancelled = 0; g_vkbd_result[0] = '\0';
    __atomic_store_n(&g_vkbd_mode, 1, __ATOMIC_RELEASE);
    mcsm_ime_begin_titled(title, initial);
    if (!g_ime_active) {           /* init failed -> report cancelled, never hang */
        g_vkbd_cancelled = 1;
        __atomic_store_n(&g_vkbd_finished, 1, __ATOMIC_RELEASE);
    }
}

/* ☠ MUST report "finished" when no IME was ever raised, or the engine waits forever.
 * The original engine stub unconditionally returned 1, so every caller is written
 * expecting a keyboard that finishes; returning a bare 0 here means a rename screen
 * that never exits. That was live: the inline hook on IsVirtualKeyboardFinished
 * installs (8-byte function) while the one on OpenVirtualKeyboard does NOT (4 bytes,
 * below INLINE_HOOK_BYTES), so the engine polled a keyboard that had never been
 * opened and got 0 on every call. Only claim "not finished" while an IME is actually
 * up -- outside vkbd mode, answer exactly what the stub answered. */
int mcsm_vkbd_finished(void) {
    if (!__atomic_load_n(&g_vkbd_mode, __ATOMIC_ACQUIRE)) return 1;
    return __atomic_load_n(&g_vkbd_finished, __ATOMIC_ACQUIRE);
}
const char *mcsm_vkbd_result(int *cancelled) { if (cancelled) *cancelled = g_vkbd_cancelled; return g_vkbd_result; }
void mcsm_vkbd_reset(void) { g_vkbd_mode = 0; g_vkbd_finished = 0; g_vkbd_cancelled = 0; g_vkbd_result[0] = '\0'; }

/* ALWAYS log the init result. A failed sceImeDialogInit was previously silent, so
 * "the keyboard doesn't show up" could not be told apart from "nothing ever asked for
 * one" -- and those have completely different causes. The rc is the whole diagnosis:
 * absent line = nothing called us; rc<0 = the Vita refused the dialog (0x80100906 =
 * ANOTHER common dialog is already up, which is the usual one). */
void mcsm_ime_begin_titled(const char *title, const char *initial) {
    if (g_ime_active) {
        l_info("KEYBOARD: mcsm_ime_begin ignored — IME already active");
        return;
    }
    int rc = init_ime_dialog((title && title[0]) ? title : "Enter name",
                             (initial && initial[0]) ? initial : " ");
    if (rc >= 0) g_ime_active = 1;
    l_info("KEYBOARD: sceImeDialogInit rc=0x%08X -> %s", (unsigned)rc,
           rc >= 0 ? "IME UP" : "FAILED (no keyboard will appear)");
}

void mcsm_ime_begin(const char *initial) { mcsm_ime_begin_titled(NULL, initial); }

int mcsm_ime_is_active(void) { return g_ime_active; }

/* What the PRESENTER must ask, rather than mcsm_ime_is_active(): true while the dialog
 * is up AND for a short grace period after Term, so the compositor can finish tearing
 * it down. Consumes one grace frame per call, so call it exactly once per present. */
int mcsm_ime_needs_compositing(void) {
    if (g_ime_active) return 1;
    if (g_ime_compositing_grace > 0) { g_ime_compositing_grace--; return 1; }
    return 0;
}

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
        /* RELEASE: the name and the cancelled flag must be visible BEFORE `finished`
         * flips, or the sim thread can see "done" and then read the previous name. */
        __atomic_store_n(&g_vkbd_finished, 1, __ATOMIC_RELEASE);
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
