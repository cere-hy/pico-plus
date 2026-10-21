/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 屏幕伽马查表：把 8 位线性值翻译成这块墨水屏真正该有的墨色深度。
 * Panel gamma LUT: translate an 8-bit linear value into the ink depth this panel actually needs.
 *
 * 这块屏的中间调比线性预期偏亮，所以取 value^γ（γ<1）抬中间段、端点保持不动，
 * 文字笔画与图片灰度由此共用同一套明暗尺度。
 * Midtones read lighter than linear on this panel, so value^γ with γ<1 lifts the middle while
 * pinning endpoints, giving glyph coverage and image grays one shared brightness scale.
 */

#pragma once

#include <stdint.h>

/// 伽马指数，小于 1 抬中间墨量；编译期可用 -DUI_GRAY_GAMMA=0.6f 覆盖。
/// Gamma exponent; below 1 lifts mid ink. Override at build time with -DUI_GRAY_GAMMA=0.6f.
#ifndef UI_GRAY_GAMMA
#define UI_GRAY_GAMMA 0.5f
#endif

/// 把 0..255 的线性值映射到本屏对应的墨量；0 仍是 0，255 仍是 255。
/// Map a linear 0..255 value to this panel's ink with both endpoints fixed.
/// 入参语义为“墨量/覆盖率”，值越大越黑；亮度口径请先翻转再调用。
/// The argument reads as ink/coverage where larger is darker; flip brightness before calling.
uint8_t ui_gray_lut_map(uint8_t value);
