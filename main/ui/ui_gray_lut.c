/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 屏幕伽马查表实现；首次调用时建好 256 项，之后只查表。
 * Panel gamma LUT implementation; the 256 entries are built on first use, then only looked up.
 */

#include "ui_gray_lut.h"

#include <math.h>
#include <stdbool.h>

static uint8_t s_map[256];
static bool s_ready;

static void ui_gray_lut_build(void) {
    for (int i = 0; i < 256; i++) {
        const float t = (float)i / 255.f;
        int v = (int)(255.f * powf(t, UI_GRAY_GAMMA) + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        s_map[i] = (uint8_t)v;
    }
    s_ready = true;
}

uint8_t ui_gray_lut_map(uint8_t value) {
    if (!s_ready) ui_gray_lut_build();
    return s_map[value];
}
