/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：双指识别器的主机触摸输入契约。与真实 cst836u_touch_t 同构（含 points[2]）。
 * English: Host touch input contract for the two-finger recognizer. Shaped like the
 * real cst836u_touch_t (points[2] included).
 *
 * 冻结：仅用于测试；不改动既有 ui_gesture_stubs，以免影响单指测试。
 * Frozen: tests only; the existing ui_gesture_stubs is left alone so the
 * single-finger tests cannot regress.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CST836U_MAX_POINTS 2

typedef struct {
    bool active;
    uint8_t id;
    uint8_t event;
    uint16_t x;
    uint16_t y;
} cst836u_point_t;

typedef struct {
    bool touched;
    uint8_t count;
    uint16_t x;
    uint16_t y;
    cst836u_point_t points[CST836U_MAX_POINTS];
} cst836u_touch_t;

typedef struct {
    uint8_t *fb;
    void *hl;
    int64_t now_ms;
    const cst836u_touch_t *touch;
    bool pressed, released, consumed, continuous_ready;
} app_ctx_t;
