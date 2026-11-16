/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * gen30 伴读案 · 修复轮：多行块排版断行核心的实现。
 * 纯逻辑，不引用任何绘制/字体/RTOS 符号；宽度全部来自调用方回调。
 *
 * gen30 repair round: implementation of the block-layout line breaker.
 * Pure logic - no drawing/font/RTOS symbols; all widths come from the caller.
 */
#include "ui_text_wrap.h"

/* UTF-8 首字节 -> 该码点应有的字节数。非法首字节按 1 字节处理，与渲染层同判据。 */
/* UTF-8 lead byte -> expected sequence length. Illegal leads read as 1 byte,
 * matching the renderer's own rule. */
static size_t utf8_step(const char *p) {
    const unsigned char c = (unsigned char)p[0];
    size_t step = c < 0x80u ? 1u
                : (c & 0xe0u) == 0xc0u ? 2u
                : (c & 0xf0u) == 0xe0u ? 3u
                : (c & 0xf8u) == 0xf0u ? 4u : 1u;
    /* 续字节不足则退化为单字节，避免把下一个字符吞掉。 */
    size_t k = 1;
    while (k < step && p[k] && ((unsigned char)p[k] & 0xc0u) == 0x80u) ++k;
    if (k < step) step = 1u;
    return step;
}

size_t ui_text_wrap(const char *text, int max_width, size_t max_lines,
                    ui_text_advance_fn advance, void *ctx,
                    size_t *line_off, size_t *line_len, int *truncated) {
    if (truncated) *truncated = 0;
    if (!text || !text[0] || max_width <= 0 || max_lines == 0 || !advance
        || !line_off || !line_len) {
        return 0;
    }
    if (max_lines > UI_TEXT_WRAP_MAX_LINES) max_lines = UI_TEXT_WRAP_MAX_LINES;

    size_t n = 0;      /* 扫描位置（字节） */
    size_t lines = 0;

    while (text[n] && lines < max_lines) {
        const size_t start = n;
        size_t best = start;   /* 本行已吃下的最后一个完整码点之后的位置 */
        int width = 0;

        while (text[n]) {
            const unsigned char c = (unsigned char)text[n];
            /* 显式换行：断行且换行符本身不进入任何一行。 */
            if (c == '\n') {
                ++n;
                break;
            }
            const size_t step = utf8_step(text + n);
            /* 超宽行防御：单行字节数封顶，防止极小字号下 buffer 溢出。 */
            if (n - start + step > UI_TEXT_WRAP_MAX_LINE_BYTES) break;
            const int w = advance(ctx, text + n, step);
            /* 首个码点无条件吃下（width==0 时条件不成立），保证每行至少一个码点。 */
            if (width > 0 && width + w > max_width) break;
            if (w > 0) width += w;
            n += step;
            best = n;
        }

        line_off[lines] = start;
        line_len[lines] = best - start;
        ++lines;

        /* 行内既无内容又未前进（连续换行）：上面 '\n' 分支已让 n 前进，故不会死循环。
         * 若首码点即超宽，内层循环会先吃下它再在下一码点处 break，同样有进展。 */
    }

    if (truncated && text[n]) *truncated = 1;
    return lines;
}
