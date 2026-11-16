/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 英文音节断点（Liang 算法 / Liang's hyphenation algorithm）。
 *
 * 算法是公开的、自己写；**模式表是从 TeX `hyph-en-us` 生成的数据**
 * （`tools/gen_hyphen_table.py` → `hyph_en_us_data.h`），不手抄、不近似。
 * 表头里钉着源文件 sha256 与条数，`tools/check_hyphen_table.py` 每次重生成比对。
 *
 * The algorithm is public and written here; the pattern table is generated data from
 * the TeX `hyph-en-us` source, pinned by sha256 and checked by regeneration.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// TeX 的左/右最小切长（英语 2/3）。断点两侧都不得少于这些字母数。
/// TeX's left/right minima for English; a break may not leave fewer letters on either side.
#define HYPH_LEFT_MIN 2
#define HYPH_RIGHT_MIN 3

/// 一个词最多保留的断点数（英文长词实测 ≤5，8 是余量）。
/// Max break points kept per word (English long words measure <=5; 8 is headroom).
#define HYPH_MAX_POINTS 8

typedef struct {
    uint8_t count;
    uint8_t pos[HYPH_MAX_POINTS]; ///< 词内字节位置 1..len-1，升序 / byte offsets into the word
} hyph_points_t;

/// 求一个英文词的允许断点；非 ASCII、太短或无断点返回 false 且 count=0。
/// Break points for one English word; false (and count=0) for non-ASCII, short or
/// unsplittable words.
bool hyph_points(const char *word, size_t len, hyph_points_t *out);
