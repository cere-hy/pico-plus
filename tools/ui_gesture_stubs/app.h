/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：识别器的主机触摸输入契约。/ English: Host touch input contract for the recognizer.
 * 冻结：仅用于测试。/ Frozen: Tests only.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool active;
    uint8_t id;
    uint8_t event;
    uint16_t x;
    uint16_t y;
} cst836u_point_t;

/* gen26.8：与真实 cst836u_touch_t 同构（含 points[2]）——双指层 ui_dualgesture.c
 * 直接读 points[]，桩缺字段就编不过。既有单指测试全部用指定初始化器，加字段零影响。
 * gen26.8: shaped like the real cst836u_touch_t (points[2] included) - the dual
 * layer reads points[] directly. Existing single-finger tests all use designated
 * initializers, so the extra fields change nothing. */
typedef struct {
    bool touched;
    uint8_t count;
    uint16_t x;
    uint16_t y;
    cst836u_point_t points[2];
} cst836u_touch_t;
typedef struct {
    uint8_t *fb;
    void *hl;
    int64_t now_ms;
    const cst836u_touch_t *touch;
    bool pressed, released, consumed, continuous_ready;
    /* 26.9.13：单指优先策略位。识别器靠它决定「一帧两点是否作废这次点按」，
     * 桩必须同构，否则 26.9.13 的两条缺陷根本进不了测试面。
     * 26.9.13: the single-finger priority bit. The recognizer uses it to decide whether a
     * two-contact frame voids the tap; the stub has to mirror it or the two 26.9.13
     * defects stay outside the tested surface. */
    bool single_finger_priority;
} app_ctx_t;
