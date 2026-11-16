/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * gen30 伴读案 · 修复轮：多行块排版断行核心的宿主测试（纯逻辑，零 stub）。
 *
 * 为什么单独立一个二进制：既有 ui_kit 宿主测试带一条登记在案的 KNOWN-FAIL
 * （r93 4bpp packed 帧缓冲契约），把它和新断言混在一个进程里，断言即便全绿也会
 * 被那条已知失败吞掉、在门禁输出里不可判别。断行是纯逻辑（宽度全由回调注入），
 * 与帧缓冲无关，故独立编译、独立退出码，绿就是绿。
 *
 * gen30 repair round: host tests for the block-layout line breaker (pure logic,
 * no stubs). Kept in its own binary because the existing ui_kit host test carries
 * a registered KNOWN-FAIL (r93 packed framebuffer contract); mixing new asserts
 * into that process would hide a green result behind the known failure. Wrapping
 * is pure logic (all widths injected), so it compiles and exits on its own.
 */

#include <stdio.h>
#include <string.h>

#include "ui_text_wrap.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            printf("FAIL: %s  (line %d)\n", (msg), __LINE__);                   \
        }                                                                       \
    } while (0)

/* ---------------------------------------------------------------------------
 * 假宽度回调：CJK（首字节 >= 0xE0，三/四字节）每字 20 px，其余每字节 10 px。
 * 零宽模式用于验证「零宽码点不推进宽度」。
 * Fake advance: CJK (3/4-byte leads) 20 px each, everything else 10 px per byte.
 * ------------------------------------------------------------------------- */
static int g_cjk_px = 20;
static int g_ascii_px = 10;
static int g_zero_width = 0;

static int fake_advance(void *ctx, const char *cp, size_t step) {
    (void)ctx;
    (void)cp;
    if (g_zero_width) return 0;
    return (step >= 3) ? g_cjk_px : g_ascii_px;
}

/* 校验一行边界落在码点边界上：首字节不能是续字节，末字节之后也不能是续字节。 */
static int on_codepoint_boundary(const char *text, size_t off, size_t len) {
    if (((unsigned char)text[off] & 0xc0u) == 0x80u) return 0;
    const unsigned char after = (unsigned char)text[off + len];
    if (after && (after & 0xc0u) == 0x80u) return 0;
    return 1;
}

int main(void) {
    size_t off[UI_TEXT_WRAP_MAX_LINES];
    size_t len[UI_TEXT_WRAP_MAX_LINES];

    /* ---- 1. CJK 贪心断行：每字 20 px，行宽 60 → 每行 3 字 ---- */
    {
        const char *s = "一二三四五六七八九"; /* 9 字 × 3 B = 27 B */
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 60, 16, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 3, "CJK 9 字 / 行宽 60(3 字) 应为 3 行");
        CHECK(trunc == 0, "CJK 全部排下，truncated 应为 0");
        CHECK(off[0] == 0 && len[0] == 9, "第 1 行 = 3 个字（9 B）");
        CHECK(off[1] == 9 && len[1] == 9, "第 2 行从 9 B 起，长 9 B");
        CHECK(off[2] == 18 && len[2] == 9, "第 3 行从 18 B 起，长 9 B");
        for (size_t i = 0; i < n; ++i) {
            CHECK(on_codepoint_boundary(s, off[i], len[i]), "CJK 行边界落在码点边界");
        }
    }

    /* ---- 2. 行宽刚好差 1 px：61 → 仍是 3 字/行（3×20=60 ≤ 61，4×20=80 > 61） ---- */
    {
        const char *s = "一二三四五六";
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 61, 16, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 2, "行宽 61 时 6 字应为 2 行");
        CHECK(len[0] == 9 && len[1] == 9, "每行仍为 3 字");
        CHECK(trunc == 0, "无截断");
    }

    /* ---- 3. 窄栏：行宽 40 → 每行 2 字，9 字 → 5 行 ---- */
    {
        const char *s = "一二三四五六七八九";
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 40, 16, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 5, "行宽 40(2 字) 时 9 字应为 5 行");
        CHECK(len[0] == 6 && len[3] == 6 && len[4] == 3, "前四行 2 字、末行 1 字");
        CHECK(trunc == 0, "无截断");
    }

    /* ---- 4. 行数上限截断：max_lines=2 → 只出 2 行且 truncated=1 ---- */
    {
        const char *s = "一二三四五六七八九";
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 60, 2, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 2, "max_lines=2 应只出 2 行");
        CHECK(trunc == 1, "仍有文字未排下，truncated 应为 1");
        CHECK(off[1] == 9 && len[1] == 9, "第 2 行内容完整");
    }

    /* ---- 5. 单字超宽：行宽 10 < 单字 20 → 每行强制 1 字，不死循环、不吞字 ---- */
    {
        const char *s = "一二三";
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 10, 16, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 3, "超宽单字应每行 1 字，共 3 行");
        CHECK(len[0] == 3 && len[1] == 3 && len[2] == 3, "每行恰 1 个 3 字节码点");
        CHECK(trunc == 0, "无截断");
    }

    /* ---- 6. ASCII 断行：每字 10 px，行宽 35 → 每行 3 字符 ---- */
    {
        const char *s = "abcdefg";
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 35, 16, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 3, "ASCII 7 字符 / 行宽 35 应为 3 行");
        CHECK(len[0] == 3 && len[1] == 3 && len[2] == 1, "3/3/1 分配");
        CHECK(trunc == 0, "无截断");
    }

    /* ---- 7. 显式换行：'\n' 强制断行且不进入任何一行 ---- */
    {
        const char *s = "ab\ncd";
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 100, 16, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 2, "含 '\\n' 应断成 2 行");
        CHECK(off[0] == 0 && len[0] == 2, "第 1 行 = 'ab'");
        CHECK(off[1] == 3 && len[1] == 2, "第 2 行跳过换行符，从索引 3 起 = 'cd'");
        CHECK(trunc == 0, "无截断");
    }

    /* ---- 8. 换行在最前：产生一个空行，不吞后续内容 ---- */
    {
        const char *s = "\nab";
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 100, 16, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 2, "前导 '\\n' 应产生空行 + 内容行");
        CHECK(len[0] == 0, "第 1 行为空行（长度 0）");
        CHECK(off[1] == 1 && len[1] == 2, "第 2 行 = 'ab'");
    }

    /* ---- 9. 零宽码点：全部 0 宽时不无限断行，整段落 1 行 ---- */
    {
        const char *s = "abc";
        int trunc = -1;
        g_zero_width = 1;
        const size_t n = ui_text_wrap(s, 10, 16, fake_advance, NULL, off, len, &trunc);
        g_zero_width = 0;
        CHECK(n == 1, "全零宽时整段落 1 行");
        CHECK(len[0] == 3, "零宽码点全部保留在同一行");
    }

    /* ---- 10. 非法 UTF-8 续字节：按单字节处理，不越界 ---- */
    {
        const char *s = "\x80\x80"; /* 孤立续字节 */
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 10, 16, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 2, "孤立续字节按 1 字节/字，行宽 10 时每行 1 字节");
        CHECK(len[0] == 1 && len[1] == 1, "每次前进 1 字节，不吞字符");
    }

    /* ---- 11. 参数边界：空串 / 零宽 / 零行 / NULL 回调 → 0 行 ---- */
    {
        int trunc = -1;
        CHECK(ui_text_wrap("", 60, 4, fake_advance, NULL, off, len, &trunc) == 0, "空串 → 0 行");
        CHECK(ui_text_wrap("abc", 0, 4, fake_advance, NULL, off, len, &trunc) == 0, "行宽 0 → 0 行");
        CHECK(ui_text_wrap("abc", 60, 0, fake_advance, NULL, off, len, &trunc) == 0, "max_lines 0 → 0 行");
        CHECK(ui_text_wrap("abc", 60, 4, NULL, NULL, off, len, &trunc) == 0, "空回调 → 0 行");
        CHECK(ui_text_wrap(NULL, 60, 4, fake_advance, NULL, off, len, &trunc) == 0, "NULL 文本 → 0 行");
    }

    /* ---- 12. 长文本 + 行数封顶：截断标志与行数上限自洽 ---- */
    {
        char s[200];
        for (size_t i = 0; i < sizeof(s) - 1; ++i) s[i] = 'x';
        s[sizeof(s) - 1] = 0;
        int trunc = -1;
        const size_t n = ui_text_wrap(s, 35, 5, fake_advance, NULL, off, len, &trunc);
        CHECK(n == 5, "长文本受 max_lines=5 封顶");
        CHECK(trunc == 1, "长文本必有截断标志");
        CHECK(off[4] + len[4] < strlen(s), "末行结束位置早于文本结尾");
    }

    printf("ui_text_wrap host tests: %d passed, %d failed — %s\n",
           g_pass, g_fail, g_fail == 0 ? "ALL PASS" : "HAS FAILURES");
    return g_fail == 0 ? 0 : 1;
}
