/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * gen30 伴读案 · 修复轮：多行块排版的断行核心（纯逻辑，零平台依赖）。
 * 与绘制无关：调用方注入「单码点前进宽度」回调，本模块只决定「哪几个字节算一行」，
 * 因此可被宿主测试以假宽度函数完整覆盖（见 tools/ui_text_wrap_host_test.c）。
 *
 * gen30 repair round: the line-breaking core of the multi-line block layout.
 * Pure logic with no platform dependency - the caller injects a per-code-point
 * advance callback, so the module only decides which byte ranges form a line and
 * is fully coverable by host tests with a fake advance function.
 */
#ifndef UI_TEXT_WRAP_H
#define UI_TEXT_WRAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单行字节上限：一行最多容纳的 UTF-8 字节数，超出即强制断行（防御超宽行）。
 *  取 255 而非 511：右栏内宽约 228 px，最小字号 14 px 时一行最多约 16 汉字
 *  （48 B），255 B 已有五倍余量；而调用方要为此在栈上开行缓冲，越小越安全。 */
#define UI_TEXT_WRAP_MAX_LINE_BYTES 255u

/** 单次断行最多产出的行数上限（调用方数组容量口径）。
 *  取 32：右栏高度约 1000 px、行高约 30 px，最多约 33 行，32 足够且把调用方
 *  在栈上的 off/len 数组压到各 128 B（原先 64 时各 256 B）。 */
#define UI_TEXT_WRAP_MAX_LINES 32u

/**
 * 单码点前进宽度回调：cp 指向该码点首字节，step 为其字节数（1..4），返回像素宽。
 * 返回 <= 0 视为「零宽码点」（如组合符），仍计入本行但不消耗宽度。
 *
 * Per-code-point advance: cp points at the first byte, step is 1..4 bytes.
 * A non-positive return means a zero-width code point (still kept on the line).
 */
typedef int (*ui_text_advance_fn)(void *ctx, const char *cp, size_t step);

/**
 * 按码点贪心断行。
 *
 * @param text        以 NUL 结尾的 UTF-8 文本（可为空串）
 * @param max_width   行宽上限（像素），<= 0 视为不可排版，返回 0
 * @param max_lines   行数上限（>=1），超出即截断
 * @param advance     单码点前进宽度回调
 * @param ctx         回调上下文
 * @param line_off    输出：每行起始字节偏移（容量 >= max_lines）
 * @param line_len    输出：每行字节长度（容量 >= max_lines）
 * @param truncated   输出：1 = 尚有文字未排下（溢出截断），0 = 全部排完；可为 NULL
 * @return 实际行数（<= max_lines）
 *
 * 断行规则：
 *  1. 逐码点推进，宽度累加超过 max_width 且当前行非空 → 在该码点前断行；
 *  2. 单个码点自身宽于 max_width → 独占一行强制放下（不死循环、不吞字）；
 *  3. 显式 '\n' → 强制断行且不写入任何一行（换行符本身不绘制）；
 *  4. 绝不从 UTF-8 多字节序列中间切断（非法续字节按单字节处理，与渲染层同判据）。
 */
size_t ui_text_wrap(const char *text, int max_width, size_t max_lines,
                    ui_text_advance_fn advance, void *ctx,
                    size_t *line_off, size_t *line_len, int *truncated);

#ifdef __cplusplus
}
#endif

#endif /* UI_TEXT_WRAP_H */
