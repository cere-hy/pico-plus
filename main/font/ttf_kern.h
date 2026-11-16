/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 字距（kerning）契约：相邻字形对之间的**额外像素推进**。
 *
 * 三个消费者（度量 `measure_width`、绘制三个入口、布局的逐字累加）必须调同一个函数——
 * 门 1 咬的就是这条：只在一侧加 kern，行宽与笔位当场分家。
 *
 * 只在**内置字体**上有效：展平表是按 `builtin.ttf` 的字形 id 建键的，换任何别的字体，
 * 键就会指向不相干的字形。所以非内置字体一律返回 0——**静默无 kerning 是判据**
 * （[124] 细则 1），不报错、不告警、不回落成假字距。
 *
 * Kerning contract: the extra advance between two adjacent glyphs. Measure, draw and the
 * layout accumulator must all call this one function. Valid for the builtin face only:
 * the flattened table is keyed by its glyph ids, so any other face returns 0 silently.
 */
#pragma once

#include <stdint.h>

/// 相邻字形对的额外像素推进（可为负）。非内置字体返回 0。
/// Extra advance in pixels for the pair (may be negative); 0 for any non-builtin face.
int ttf_kern_px(int pixel_height, uint32_t left_cp, uint32_t right_cp);
