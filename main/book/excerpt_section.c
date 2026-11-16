/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：切节实现。三段：定位 → 折行 → 按句边界截。每一段都只做一件事，
 * 因为这三件事的失败形态完全不同（找不到 / 折坏行契约 / 砍在句中）。
 * English: the implementation, in three stages - locate, fold, sentence-cut. Each
 * does one thing because each fails differently.
 */
#include "excerpt_section.h"

#include <string.h>

#define SECTION_MARKER_LEN (sizeof(EXCERPT_SECTION_MARKER) - 1u)
#define SECTION_CUT_LEN    (sizeof(EXCERPT_SECTION_CUT) - 1u)

static bool is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// 折空白：与 excerpt_store 的 fold_ws 同一套规则（`\r\n\t` 与连续空白折成一个
/// 空格、**行首空白不落**、长度只减不增）。落盘前就已经是一行，存储层不必再变形
/// ——两处各折一次会折出不同的结果，那才是「同一段文字在卡上与屏上不一样」的根。
/// Same rules as fold_ws: whitespace runs collapse to one space, a leading run is
/// dropped, length never grows. Folding once at the entrance keeps the card and the
/// screen saying the same thing.
static size_t fold_into(char* dst, size_t cap, const char* src, size_t len) {
    if (cap == 0) return 0;
    size_t at = 0;
    bool in_ws = false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (is_ws(c)) {
            if (!in_ws && at) {
                if (at + 1u >= cap) break;
                dst[at++] = ' ';
            }
            in_ws = true;
        } else {
            if (at + 1u >= cap) break;
            dst[at++] = (char)c;
            in_ws = false;
        }
    }
    dst[at] = '\0';
    return at;
}

/// UTF-8 安全截断：回退续字节（与 fold_ws 的 utf8_cut 同源纪律）。
static size_t utf8_back(const char* s, size_t n) {
    while (n > 0 && ((unsigned char)s[n] & 0xC0u) == 0x80u) --n;
    return n;
}

/// 句边界：在 [0, lim) 内找**最后一个**可断处，返回断点之后的长度（0 = 没有）。
///
/// 认三种句末：全角 `。！？；`、半角 `. ! ?` 后接空白或串尾。半角句号额外排除
/// 「单字母缩写 + 点」（`J.`、`A.`）——那是人名首字母，不是句末；这一条做不完美
/// （`Mr. Smith` 仍会被当句末），但它把最常见的一类误判挡掉了，而且误判的代价
/// 只是这一刀提前落在句读处附近，仍然不会腰斩单词。
/// Last breakable point inside [0, lim): full-width terminators, and half-width ones
/// followed by whitespace or the end. A single-letter-plus-dot is treated as an
/// initial, not a sentence end; imperfect for "Mr. Smith" but it never cuts a word.
static size_t sentence_end(const char* s, size_t len, size_t lim) {
    size_t best = 0;
    const size_t stop = lim < len ? lim : len;
    for (size_t i = 0; i < stop; ++i) {
        const unsigned char c = (unsigned char)s[i];
        // 全角句末：。= E3 80 82，！= EF BC 81，？= EF BC 9F，；= EF BC 9B
        // 三字节句末必须**整组**落在预算内：i + 3 <= stop。少了这个约束，
        // best 会越过 room 三字节，末尾补标记时写穿 out 一字节（ASan 当场抓到）。
        // A three-byte terminator must fit WHOLE inside the budget; without this the
        // tail marker lands one byte past the buffer (caught by ASan on the host).
        if (c == 0xE3u && i + 3u <= stop && i + 2u < len && (unsigned char)s[i + 1] == 0x80u &&
            (unsigned char)s[i + 2] == 0x82u) {
            best = i + 3u;
            i += 2u;
            continue;
        }
        if (c == 0xEFu && i + 3u <= stop && i + 2u < len && (unsigned char)s[i + 1] == 0xBCu) {
            const unsigned char b2 = (unsigned char)s[i + 2];
            if (b2 == 0x81u || b2 == 0x9Fu || b2 == 0x9Bu) {
                best = i + 3u;
                i += 2u;
                continue;
            }
        }
        if (c == '.' || c == '!' || c == '?') {
            const bool tail_ok = (i + 1u >= len) || is_ws((unsigned char)s[i + 1]);
            if (!tail_ok) continue;
            bool initial = false;
            if (c == '.' && i >= 1u) {
                const unsigned char prev = (unsigned char)s[i - 1u];
                const bool is_alpha = (prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z');
                const bool alone = (i == 1u) || is_ws((unsigned char)s[i - 2u]);
                initial = is_alpha && alone;
            }
            if (!initial) best = i + 1u;
        }
    }
    return best;
}

excerpt_section_status_t excerpt_section_pick(const char* def, size_t def_len,
                                              char* out, size_t cap,
                                              size_t* out_len, bool* cut) {
    if (out_len) *out_len = 0;
    if (cut) *cut = false;
    if (!def || !out || cap < SECTION_CUT_LEN + 2u) return EXCERPT_SECTION_NO_MARKER;

    const size_t dlen = def_len ? def_len : strlen(def);
    const char* def_end = def + dlen;

    // ① 定位：只在 def_len 内找，不依赖 NUL（调用方给的是字节数）。
    //    Locate within def_len only; the caller's length is the truth, not a NUL.
    const char* marker = NULL;
    for (size_t i = 0; i + SECTION_MARKER_LEN <= dlen; ++i) {
        if (memcmp(def + i, EXCERPT_SECTION_MARKER, SECTION_MARKER_LEN) == 0) {
            marker = def + i;
            break;
        }
    }
    if (!marker) return EXCERPT_SECTION_NO_MARKER;

    // 标记行本身可能带尾巴（如「仅当选文是英文时输出」），整行跳过。
    // The marker line may carry trailing text; skip the whole line.
    const char* body = marker + SECTION_MARKER_LEN;
    while (body < def_end && *body != '\n') ++body;
    if (body < def_end) ++body;

    // ② 节尾：下一个**行首**的 `【`。按行首而不是任意位置，是因为正文里也可能
    //    出现「【原文对照】」这种引述（模型复述要求时），那样会把节尾判到行中。
    //    The next `【` that starts a line - a quoted mention inside the body would
    //    otherwise end the section mid-line.
    const char* sec_end = def_end;
    for (const char* p = body; p + 4u <= def_end; ++p) {
        if (*p == '\n' && (unsigned char)p[1] == 0xE3u && (unsigned char)p[2] == 0x80u &&
            (unsigned char)p[3] == 0x90u) {
            sec_end = p;
            break;
        }
    }
    while (sec_end > body && is_ws((unsigned char)sec_end[-1])) --sec_end;
    if (sec_end <= body) return EXCERPT_SECTION_EMPTY;

    // ③ 折行。
    size_t n = fold_into(out, cap, body, (size_t)(sec_end - body));
    if (n == 0) return EXCERPT_SECTION_EMPTY;

    // ④ 按句边界截。room 预留 NUL 与 `[截]`。
    const size_t room = cap - 1u - SECTION_CUT_LEN;
    if (n > room) {
        size_t keep = sentence_end(out, n, room);
        if (keep == 0) keep = utf8_back(out, room); // 整段无句末：诚实的硬切
        while (keep > 0 && out[keep - 1u] == ' ') --keep;
        memcpy(out + keep, EXCERPT_SECTION_CUT, SECTION_CUT_LEN);
        n = keep + SECTION_CUT_LEN;
        out[n] = '\0';
        if (cut) *cut = true;
    }
    if (out_len) *out_len = n;
    return EXCERPT_SECTION_OK;
}
