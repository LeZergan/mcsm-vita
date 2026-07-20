/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"

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

    // Enable accelerometer
    sceMotionStartSampling();
}

void poll_touch();
void poll_pad();

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone);

void controls_poll() {
    pthread_mutex_lock(&g_controls_mutex);
    poll_touch();
    poll_pad();
    pthread_mutex_unlock(&g_controls_mutex);
}

SceTouchData touch;
SceTouchData touch_old;

void poll_touch() {
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

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
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

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
