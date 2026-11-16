/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：主机字体接口替身。/ English: Host font interface shim.
 * 冻结：仅用于测试。/ Frozen: Tests only.
 */
#pragma once
/* 与真实头共用同一份对齐计划（单源；桩头不许把规则抄一遍）。 */
#include "../../main/font/ttf_justify.h"
#include "../../main/font/ttf_kern.h"
#include <stdbool.h>
#include "epdiy.h"
bool ttf_font_ready(void);
bool ttf_font_has_text(const char *text);
bool ttf_font_is_builtin(void);
int ttf_ascender_px(int px);
int ttf_text_width_px(int px, const char *text);
void ttf_measure_line_px(int px, const char *text, int *above, int *below);
void ttf_draw_text_px(uint8_t *fb, int x, int y, int px, const char *text, enum EpdFontFlags align, uint8_t fg, uint8_t bg);
void ttf_draw_text_px_bw(uint8_t *fb, int x, int y, int px, const char *text, enum EpdFontFlags align, uint8_t fg, uint8_t bg);
// r93 起 ui_kit 的定点抗锯齿路径会临时开抗锯齿再还原；替身只需记住开关，不参与绘制。
// Since r93 the fixed-context AA path in ui_kit toggles antialiasing around one draw; the shim
// only records the flag and does not affect painting.
bool ttf_get_antialias(void);
void ttf_set_antialias(bool enabled);

/// 单个码位的前进宽度与覆盖判断：与真实 ttf_font.h 同名同签名（gen26.6 装行轮起
/// ui_kit 直接调用，桩头缺声明会让 ui_kit 子测试隐式声明报错）。
/// Per-codepoint advance and coverage: same names and signatures as the real
/// ttf_font.h (ui_kit calls them since the gen26.6 line-wrap round; a missing
/// declaration turns into an implicit-declaration error in the ui_kit test).
int ttf_codepoint_advance_px(int pixel_height, uint32_t codepoint);
bool ttf_codepoint_covered(uint32_t codepoint);
