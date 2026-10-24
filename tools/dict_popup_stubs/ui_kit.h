/* 中文：ui_kit 替身：尺寸宏 + 绘制/度量原语。/ English: ui_kit shim: size macros plus draw/metric primitives. */
#pragma once
#include "common.h"

static inline int ui_w(void) { return epd_rotated_display_width(); }
static inline int ui_h(void) { return epd_rotated_display_height(); }
#define UI_LOCK_WIDTH ui_w()
#define UI_LOCK_HEIGHT ui_h()

#define UI_GRAY_WHITE 0xFF
#define UI_GRAY_BLACK 0x00

void ui_text_fixed(uint8_t* framebuffer, int x, int y_top, int px, const char* text,
                   enum EpdFontFlags align, bool inverted);
int ui_text_fixed_width_px(int px, const char* text);
void ui_fill_round_rect(uint8_t* framebuffer, EpdRect rect, int radius, uint8_t color);
void ui_draw_round_rect(uint8_t* framebuffer, EpdRect rect, int radius, uint8_t color);
void ui_draw_choice_round_rect(uint8_t* framebuffer, EpdRect rect, int radius, bool on);
bool ui_rect_hit(EpdRect rect, uint16_t x, uint16_t y);
