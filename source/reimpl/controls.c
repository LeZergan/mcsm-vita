/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"
#include "utils/logger.h"

#include <math.h>
#include <pthread.h>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>

static pthread_mutex_t g_controls_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct AnalogRuntimeState {
    int active;
    float last_x;
    float last_y;
    unsigned polls_since_emit;
} AnalogRuntimeState;

#define ANALOG_MOVE_EPSILON 0.035f
#define ANALOG_MOVE_MAX_SKIP_POLLS 2U
#define ANALOG_ACTIVE_EPSILON 0.050f

static AnalogRuntimeState g_analog_state[2];

static float clamp_float(float value, float min_value, float max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

void coord_normalize(float * x, float * y, float deadzone) {
    float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (deadzone < 0.0f) {
        deadzone = 0.0f;
    } else if (deadzone >= 1.0f) {
        deadzone = 0.999f;
    }

    if (!(magnitude > deadzone)) {
        *x = 0;
        *y = 0;
        return;
    }

    const float nx = *x / magnitude;
    const float ny = *y / magnitude;

    if (magnitude > 1.0f) {
        magnitude = 1.0f;
    }

    const float multiplier = ((magnitude - deadzone) / (1.0f - deadzone));
    *x = clamp_float(nx * multiplier, -1.0f, 1.0f);
    *y = clamp_float(ny * multiplier, -1.0f, 1.0f);
}

static void analog_suppress_center_noise(float *x, float *y) {
    const float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (magnitude < ANALOG_ACTIVE_EPSILON) {
        *x = 0.0f;
        *y = 0.0f;
    }
}

void controls_init() {
    // Enable analog sticks and touchscreen
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);
    /* REAR panel drives the stick clicks the Vita has no buttons for (L3/R3).
     * The front panel is gameplay input, so it cannot be reused for this. */
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, 1);

    // Enable accelerometer
    sceMotionStartSampling();
}

void poll_touch();
void poll_touch_back();
void poll_pad();

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone);

void controls_poll() {
    pthread_mutex_lock(&g_controls_mutex);
    /* Same reasoning as the sampling-mode re-assert in poll_pad(): touch sampling is
     * process-global state that controls_init() enabled exactly once, and a common
     * dialog / PS-button overlay / resume can hand it back disabled. This game is
     * driven almost entirely by touch, so losing it is worse than losing the sticks;
     * enabling an already-enabled port is a no-op, and a dialog reading touch for
     * itself is unaffected by us keeping OUR sampling on. Two syscalls per poll. */
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, 1);
    poll_touch();
    poll_touch_back();
    poll_pad();
    pthread_mutex_unlock(&g_controls_mutex);
}

/* ---- REAR TOUCHPAD -> L3 / R3 ------------------------------------------------
 * The Vita's analog sticks do not click, so the two stick-click buttons have no
 * hardware to come from. The rear panel is otherwise completely unused here (the
 * front panel is the game's pointer), which makes it the natural home for them --
 * and it is what Vita ports conventionally do, so it is what players expect.
 *
 * Left half of the pad = L3, right half = R3. Held state is edge-detected per
 * half, so a press emits exactly one DOWN and one UP no matter how the finger
 * slides or how many fingers land. Fingers resting on the back therefore cost a
 * single stuck DOWN at worst, never a repeat storm.
 *
 * Deliberately crude: these actions are not precision inputs in this game, so the
 * whole pad is live rather than a small target the player has to find blind. */
typedef struct {
    int contact_prev;   /* was this half being touched on the previous poll? */
    int pulse_active;   /* a DOWN was emitted and still owes its UP          */
} BackClickState;

static BackClickState g_back_l3;
static BackClickState g_back_r3;

/* Emit a one-poll DOWN/UP PULSE on the rising edge of contact, then stay quiet
 * until the finger lifts and touches again.
 *
 * Deliberately not a held button: players rest their fingers on the back of a
 * Vita constantly, and mapping "contact" straight to "button held" would leave
 * L3/R3 stuck down for whole scenes. A pulse means a resting hand costs one
 * click when it lands and nothing afterwards, which is the failure mode we want.
 * These are momentary actions in this game, so nothing needs the held state. */
static void back_click_update(BackClickState *s, int contact, int32_t keycode) {
    if (contact && !s->contact_prev) {
        controls_handler_key(keycode, CONTROLS_ACTION_DOWN);
        s->pulse_active = 1;
    } else if (s->pulse_active) {
        controls_handler_key(keycode, CONTROLS_ACTION_UP);
        s->pulse_active = 0;
    }
    s->contact_prev = contact;
}

void poll_touch_back() {
    SceTouchData back;
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1) < 0) {
        return;
    }

    /* Rear panel reports in the same 1920-wide space as the front. */
    int contact_l = 0, contact_r = 0;
    for (int i = 0; i < back.reportNum; i++) {
        if (back.report[i].x < 960) contact_l = 1;
        else                        contact_r = 1;
    }

    back_click_update(&g_back_l3, contact_l, AKEYCODE_BUTTON_THUMBL);
    back_click_update(&g_back_r3, contact_r, AKEYCODE_BUTTON_THUMBR);
}

SceTouchData touch;
SceTouchData touch_old;

void poll_touch() {
    /* A failed peek leaves `touch` holding the PREVIOUS poll's contents, which
     * this function would then diff against touch_old as if it were fresh --
     * re-reporting stale fingers, or (worse) never producing the UP that frees a
     * slot in the engine-facing table. Bail instead: skipping a poll is harmless,
     * inventing input is not. */
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 0) {
        return;
    }

    for (int i = 0; i < touch.reportNum; i++) {
        float x = (float) touch.report[i].x * 960.f / 1920.0f;
        float y = (float) touch.report[i].y * 544.f / 1088.0f;

        // Check if the finger was down before to distinguish between the Move and Down events
        int old_index = -1;

        if (touch_old.reportNum > 0) {
            for (int j = 0; j < touch_old.reportNum; j++) {
                if (touch.report[i].id == touch_old.report[j].id) {
                    old_index = j;
                    break;
                }
            }
        }

        if (old_index < 0) {
            controls_handler_touch(touch.report[i].id, x, y, CONTROLS_ACTION_DOWN);
        } else {
            const float old_x = (float) touch_old.report[old_index].x * 960.f / 1920.0f;
            const float old_y = (float) touch_old.report[old_index].y * 544.f / 1088.0f;
            if (fabsf(x - old_x) >= 1.0f || fabsf(y - old_y) >= 1.0f) {
                controls_handler_touch(touch.report[i].id, x, y, CONTROLS_ACTION_MOVE);
            }
        }
    }

    for (int i = 0; i < touch_old.reportNum; i++) {
        int finger_up = 1;

        for (int j = 0; j < touch.reportNum; j++) {
            if (touch.report[j].id == touch_old.report[i].id ) {
                finger_up = 0;
                break;
            }
        }

        if (finger_up == 1) {
            float x = (float) touch_old.report[i].x * 960.f / 1920.0f;
            float y = (float) touch_old.report[i].y * 544.f / 1088.0f;

            controls_handler_touch(touch_old.report[i].id, x, y, CONTROLS_ACTION_UP);
        }
    }

    sceClibMemcpy(&touch_old, &touch, sizeof(touch));
}

static ButtonMapping mapping[] = {
        { SCE_CTRL_UP,        AKEYCODE_DPAD_UP },
        { SCE_CTRL_DOWN,      AKEYCODE_DPAD_DOWN },
        { SCE_CTRL_LEFT,      AKEYCODE_DPAD_LEFT },
        { SCE_CTRL_RIGHT,     AKEYCODE_DPAD_RIGHT },
        { SCE_CTRL_CROSS,     AKEYCODE_BUTTON_A },
        { SCE_CTRL_CIRCLE,    AKEYCODE_BUTTON_B },
        { SCE_CTRL_SQUARE,    AKEYCODE_BUTTON_X },
        { SCE_CTRL_TRIANGLE,  AKEYCODE_BUTTON_Y },
        { SCE_CTRL_L1,        AKEYCODE_BUTTON_L1 },
        { SCE_CTRL_R1,        AKEYCODE_BUTTON_R1 },
        { SCE_CTRL_START,     AKEYCODE_BUTTON_START },
        { SCE_CTRL_SELECT,    AKEYCODE_BUTTON_SELECT },
};

uint32_t old_buttons = 0, current_buttons = 0, pressed_buttons = 0, released_buttons = 0;

float analog_lx[3] = { 0 };
float analog_ly[3] = { 0 };
float analog_rx[3] = { 0 };
float analog_ry[3] = { 0 };

void poll_pad() {
    SceCtrlData pad;

    /* ★ RE-ASSERT THE ANALOG SAMPLING MODE EVERY POLL. This is the fix for "the
     * sticks work, then stop working".
     *
     * sceCtrlSetSamplingModeExt() is PROCESS-GLOBAL state, and controls_init() set it
     * exactly once at boot. Anything that takes the controller away and hands it back
     * restores the SYSTEM default (digital), not ours -- and this port opens two
     * common dialogs that do precisely that: the trophy setup dialog on first
     * registration (utils/trophies.c) and the save-rename IME on SELECT
     * (utils/dialog.c). The PS-button overlay and a suspend/resume cycle are the same
     * story. Once the mode is lost, sceCtrlPeekBufferPositiveExt2 reports every axis
     * at dead centre (128) forever, so the sticks are silently, permanently dead while
     * the buttons -- which do not need the extended mode -- keep working. That is
     * exactly the reported symptom, and it is why it seemed intermittent: it depended
     * on whether you had opened a dialog yet.
     *
     * Re-asserting is idempotent and costs one syscall per poll on a dedicated 60Hz
     * input thread -- ~60/second, against the ~47,000/second this pass just removed
     * from the buffer path. Cheap enough that "make it impossible to lose" beats any
     * cleverer detection.
     *
     * The call returns the PREVIOUS mode, so it also tells us when something really
     * did steal it. Logged once, because if this hypothesis is wrong the log will say
     * so instead of silently claiming a fix. */
    {
        const int rc = sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
        /* Report the raw return ONCE, without interpreting it. sceCtrlSetSamplingMode
         * is widely said to return the PREVIOUS mode, but psp2/ctrl.h documents no
         * return value at all -- so a log line claiming "the mode had been reset to N"
         * would be asserting a contract this header does not state. One factual line
         * settles it from a real device instead: if it is the previous mode, a healthy
         * first poll reads 2 (ANALOG_WIDE, set by controls_init) and anything else
         * means something took the pad. The re-assert above does not depend on any of
         * this -- it is unconditional and idempotent. */
        static int s_reported = 0;
        if (!s_reported) {
            s_reported = 1;
            l_info("INPUT: ctrl sampling mode re-asserted every poll; first call "
                   "returned %d (expected 2 = ANALOG_WIDE if this returns the "
                   "previous mode)", rc);
        }
    }

    /* ☠ CHECK THE PEEK. `pad` is an uninitialised stack struct, and this call's
     * return value was being discarded -- so any failed peek fed STACK GARBAGE
     * straight into the input system: pad.buttons became a random bitmask (spurious
     * presses AND releases, since the code diffs against the previous frame) and
     * pad.lx/ly/rx/ry became random stick deflections. That is "the controls act
     * weird sometimes" exactly.
     *
     * poll_touch() twenty lines below already got this right, and its comment states
     * the rule this path was missing: skipping a poll is harmless, inventing input is
     * not. Same rule, same fix. */
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0) {
        return;
    }

    // Gamepad buttons
    old_buttons = current_buttons;
    current_buttons = pad.buttons;
    pressed_buttons = current_buttons & ~old_buttons;
    released_buttons = ~current_buttons & old_buttons;

    for (int i = 0; i < sizeof(mapping) / sizeof(ButtonMapping); i++) {
        if (pressed_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_DOWN);
        }
        if (released_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_UP);
        }
    }

    // Analog sticks
    poll_stick(CONTROLS_STICK_LEFT, (float)pad.lx, (float)pad.ly, analog_lx, analog_ly, LEFT_ANALOG_DEADZONE);
    poll_stick(CONTROLS_STICK_RIGHT, (float)pad.rx, (float)pad.ry, analog_rx, analog_ry, RIGHT_ANALOG_DEADZONE);
}

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone) {
    readings_x[0] = (raw_x - 128.0f) / 127.0f;
    readings_y[0] = (raw_y - 128.0f) / 127.0f;

    coord_normalize(&readings_x[0], &readings_y[0], deadzone);
    analog_suppress_center_noise(&readings_x[0], &readings_y[0]);

    const unsigned state_index = (which == CONTROLS_STICK_RIGHT) ? 1U : 0U;
    AnalogRuntimeState *state = &g_analog_state[state_index];
    const float x = readings_x[0];
    const float y = readings_y[0];
    const int centered = (x == 0.0f && y == 0.0f);

    if (centered) {
        if (state->active) {
            controls_handler_analog(which, 0.0f, 0.0f, CONTROLS_ACTION_UP);
        }
        state->active = 0;
        state->last_x = 0.0f;
        state->last_y = 0.0f;
        state->polls_since_emit = 0;
    } else if (!state->active) {
        controls_handler_analog(which, x, y, CONTROLS_ACTION_DOWN);
        state->active = 1;
        state->last_x = x;
        state->last_y = y;
        state->polls_since_emit = 0;
    } else {
        state->polls_since_emit++;
        if (fabsf(x - state->last_x) >= ANALOG_MOVE_EPSILON ||
            fabsf(y - state->last_y) >= ANALOG_MOVE_EPSILON ||
            state->polls_since_emit >= ANALOG_MOVE_MAX_SKIP_POLLS) {
            controls_handler_analog(which, x, y, CONTROLS_ACTION_MOVE);
            state->last_x = x;
            state->last_y = y;
            state->polls_since_emit = 0;
        }
    }

    readings_x[2] = readings_x[1];
    readings_y[2] = readings_y[1];
    readings_x[1] = readings_x[0];
    readings_y[1] = readings_y[0];
}
