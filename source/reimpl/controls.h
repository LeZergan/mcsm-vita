/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  controls.h
 * @brief Implementations for the touch screen, buttons, and other controls.
 */

#ifndef SOLOADER_CONTROLS_H
#define SOLOADER_CONTROLS_H

#include <stdint.h>

#define LEFT_ANALOG_DEADZONE  0.18f
#define RIGHT_ANALOG_DEADZONE 0.18f

typedef enum ControlsAction {
    CONTROLS_ACTION_UP = 0,
    CONTROLS_ACTION_DOWN = 1,
    CONTROLS_ACTION_MOVE = 2
} ControlsAction;

typedef enum ControlsStickId {
    CONTROLS_STICK_LEFT = 0,
    CONTROLS_STICK_RIGHT = 1
} ControlsStickId;

extern void controls_handler_key(int32_t keycode, ControlsAction action);
/* Reconciles the shoulder press latch against the live button levels so a
 * missed edge cannot leave L or R permanently swallowed. Called every poll. */
extern void controls_shoulder_level_sync(int left_down, int right_down);
extern void controls_handler_touch(int32_t id, float x, float y, ControlsAction action);
extern void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action);

enum {
    AKEYCODE_BACK = 4,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_DPAD_CENTER = 23,
    AKEYCODE_A = 29,
    AKEYCODE_B = 30,
    AKEYCODE_BUTTON_A = 96,
    AKEYCODE_BUTTON_B = 97,
    AKEYCODE_BUTTON_X = 99,
    AKEYCODE_BUTTON_Y = 100,
    AKEYCODE_BUTTON_L1 = 102,
    AKEYCODE_BUTTON_R1 = 103,
    /* Stick click. The Vita's sticks do not physically click, so these are
     * synthesized from the REAR touchpad (left half = L3, right half = R3) --
     * see poll_touch_back() in controls.c. */
    AKEYCODE_BUTTON_THUMBL = 106,
    AKEYCODE_BUTTON_THUMBR = 107,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109,
};

typedef struct {
    uint32_t sce_button;
    uint32_t android_button;
} ButtonMapping;

void controls_init();
void controls_poll();

#endif // SOLOADER_CONTROLS_H
