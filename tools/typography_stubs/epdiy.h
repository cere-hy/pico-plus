/* SPDX-License-Identifier: Apache-2.0
 * 排版宿主门（门 1）的显示桩：只留 ttf_font.c 用到的三个类型与像素落点。
 * Display shim for the typography gate: only the types and the pixel sink that
 * ttf_font.c touches. The flag values are copied from components/epdiy/src/epdiy.h so
 * a mis-aligned enum cannot silently change alignment behaviour on the host.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { int x, y, width, height; } EpdRect;

enum EpdFontFlags {
    EPD_DRAW_BACKGROUND = 0x1,
    EPD_DRAW_ALIGN_LEFT = 0x2,
    EPD_DRAW_ALIGN_RIGHT = 0x4,
    EPD_DRAW_ALIGN_CENTER = 0x8,
};

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* framebuffer);
