/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 仿生阅读（bionic reading）· 纯逻辑层，零平台依赖。
 *
 * 定版口径（公主 2026-11-16 备案，不再讨论）：
 *  - 加粗比例固定 50%（前半截），无档位、无滑杆；
 *  - 只作用英文词（A-Za-z0-9），中文与 CJK 直通不处理；
 *  - 撇号与连字符视为词内字符（reader's、state-of-the-art 整词处理）。
 *
 * 本模块只回答两个问题：一个词该加粗几个字符、当前码点是否落在词首加粗段内。
 * 绘制（同位右移 1 px 的等宽伪加粗）由 ttf_font.c 消费，断行与字距完全不受影响。
 *
 * Bionic reading: pure logic with no platform dependency. It only answers how many
 * leading characters of a word are bold and whether the current code point falls in
 * that prefix. Painting (a 1 px right-shifted, advance-preserving faux bold) is done
 * in ttf_font.c; line breaking and tracking are untouched.
 */
#ifndef UI_BIONIC_H
#define UI_BIONIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 词首加粗字符数：`ceil(len / 2)`（整数实现 `(len + 1) / 2`），无上限。
 * 长度 >= 2 的词恒保留后半截常规显示；单字母词整词加粗。
 *
 * @param len 词长（词字符个数，含词内撇号／连字符），<= 0 视为空词返回 0
 * @return 1..len 的加粗字符数；空词返回 0
 *
 * Bold prefix length: ceil(len / 2) as the integer form (len + 1) / 2, uncapped.
 * Words of two characters or more always keep their back half regular.
 */
int br_fixation_len(int len);

/// 词首加粗状态机；由调用方按行持有，逐码点喂入。
/// Per-line state machine, fed one code point at a time.
typedef struct {
    int left; ///< 当前词剩余可加粗字符数（0 = 不在词内）/ bold chars left in the word
    int pos;  ///< 当前词已消费的字符数（0 = 不在词内）/ chars consumed in the word
} br_bold_state_t;

/// 重置状态机（每行开始时调用）。/ Reset at the start of every line.
void br_bold_reset(br_bold_state_t *st);

/**
 * 喂入一个码点，返回该码点是否应加粗。
 *
 * @param st          状态机
 * @param cp          当前码点
 * @param text_after  当前码点之后的文本（可为 NUL 结尾空串），用于向前看整词长度
 * @param new_word    输出：本码点是否开启了一个新词（供调用方统计加粗段数）；可为 NULL
 * @return true = 该码点属于词首加粗段；false = 常规显示或非词字符
 *
 * 词边界：ASCII 字母数字开启新词；词内撇号（' 与 U+2019）与连字符仅在紧跟字母数字时
 * 延续当前词；空白、标点、CJK 一律终止当前词。CJK 码点永远返回 false。
 */
bool br_bold_feed(br_bold_state_t *st, uint32_t cp, const char *text_after,
                  bool *new_word);

#ifdef __cplusplus
}
#endif

#endif /* UI_BIONIC_H */
