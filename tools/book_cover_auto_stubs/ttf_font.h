#pragma once
/* 与真实头共用同一份对齐计划（单源；桩头不许把规则抄一遍）。 */
#include "../../main/font/ttf_justify.h"
#include "../../main/font/ttf_kern.h"
#include <stdbool.h>
#include <stdint.h>
#include "epdiy.h"
bool ttf_font_ready(void);
int ttf_ascender_px(int px);
int ttf_text_width_px(int px, const char *text);
void ttf_draw_text_px(uint8_t *fb, int x, int baseline, int px, const char *text,
                      enum EpdFontFlags align, uint8_t fg, uint8_t bg);
