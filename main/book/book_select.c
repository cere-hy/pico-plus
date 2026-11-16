/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：框选状态机实现；只在字节区间上做区间运算，见头文件的不变量。
 * English: Implementation of the box-selection state machine; interval arithmetic over
 * byte offsets only, see the invariants in the header.
 */
#include "book_select.h"

#include <string.h>

static int64_t deadline_from(int64_t now_ms) {
    return now_ms + (int64_t)BOX_SELECT_TIMEOUT_MS;
}

void box_select_begin(box_selection_t *sel, size_t page, int64_t now_ms) {
    if (!sel) return;
    memset(sel, 0, sizeof(*sel));
    sel->state = BOX_STATE_ARM;
    sel->page = page;
    sel->deadline_ms = deadline_from(now_ms);
}

bool box_select_tap(box_selection_t *sel, size_t ws, size_t we, int64_t now_ms) {
    if (!sel || we <= ws) return false;              // 空区间或无命中：不改状态 / empty span, state kept
    if (sel->state == BOX_STATE_ARM) {
        sel->anchor_ws = ws;
        sel->anchor_we = we;
        sel->off_start = ws;
        sel->off_end = we;
        sel->state = BOX_STATE_TAIL;
        sel->deadline_ms = deadline_from(now_ms);
        return true;
    }
    if (sel->state == BOX_STATE_TAIL) {
        // 反向选（尾点在锚前，含完全重叠）→ 交换；否则与锚取并集。
        // A tail before the anchor (overlap included) swaps; otherwise union with the anchor.
        if (we <= sel->anchor_ws) {
            sel->off_start = ws;
            sel->off_end = sel->anchor_we;
        } else {
            sel->off_start = sel->anchor_ws;
            sel->off_end = we > sel->anchor_we ? we : sel->anchor_we;
        }
        sel->state = BOX_STATE_READY;
        sel->deadline_ms = deadline_from(now_ms);
        return true;
    }
    // READY 之后再点：不重开选区（动作栏已接管），保持原选区。
    // A tap after READY does not restart the range: the action bar owns the screen now.
    return false;
}

// ---- 按句吸附（26.9.12）：整段代码只做字节与码点运算，不认识坐标、不调排版。----
// Sentence snapping (26.9.12): bytes and code points only - no coordinates, no layout.

// 最小 UTF-8 步进：非法序列按单字节前进（宁可漏一个句界，也不死循环或越界）。
// Minimal UTF-8 step: an invalid sequence advances one byte (a missed boundary beats a
// hang or an overrun).
static size_t utf8_step(const char *s, size_t len, size_t i, uint32_t *cp) {
    unsigned char c = (unsigned char)s[i];
    size_t n = 1;
    uint32_t v = c;
    if (c >= 0xF0 && c <= 0xF7) {
        n = 4; v = c & 0x07u;
    } else if (c >= 0xE0) {
        n = 3; v = c & 0x0Fu;
    } else if (c >= 0xC0) {
        n = 2; v = c & 0x1Fu;
    }
    if (i + n > len) { *cp = c; return 1; }
    for (size_t k = 1; k < n; k++) {
        if (((unsigned char)s[i + k] & 0xC0u) != 0x80u) { *cp = c; return 1; }
        v = (v << 6) | ((unsigned char)s[i + k] & 0x3Fu);
    }
    *cp = v;
    return n;
}

// 行首缩进用的空白：半角空格/制表/回车 + 全角空格。**不含换行**——换行本身是句界，
// 已经体现在「起点落在换行之后」这件事上。
// Indent whitespace: space/tab/CR plus the ideographic space. **Newline excluded** - a
// newline is itself a boundary and already handled by "the start lands after it".
static bool is_indent_cp(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == 0x3000u;
}

// 位置 i 是否是句界起点；next 给出该标点结束处（换行时为换行之后）。
// 全角标点无条件成界；ASCII 的 . ! ? ; 必须后跟空白或页尾，否则 3.14 / Mr. 会被切断。
// Is position i the start of a sentence terminator; next is just past it (past the
// newline too). Full-width marks always count; ASCII . ! ? ; need trailing whitespace or
// the page end, otherwise 3.14 and Mr. get cut.
static bool sentence_stop(const char *text, size_t page_end, size_t i, size_t *next) {
    uint32_t cp = 0;
    const size_t n = utf8_step(text, page_end, i, &cp);
    switch (cp) {
        case 0x3002u:  // 。
        case 0xFF01u:  // ！
        case 0xFF1Fu:  // ？
        case 0xFF1Bu:  // ；
        case 0x2026u:  // …
        case 0xFF0Eu:  // ．
        case '\n':
            *next = i + n;
            return true;
        case '.':
        case '!':
        case '?':
        case ';': {
            const size_t j = i + n;
            if (j >= page_end) { *next = j; return true; }
            const char c = text[j];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { *next = j; return true; }
            return false;
        }
        default:
            return false;
    }
}

// 位置 pos 之前紧邻的那个字符是不是句界（即「选区刚好停在句末」）。判定必须试 1..3 字节
// 三种起点：标点可能是 1 字节（.）也可以是 3 字节（。）；从续字节起解会得到一个非标点
// 码点，因此只有正好落在标点首字节上的那次会命中。
// Is the character ending exactly at pos a sentence terminator (i.e. "the range already
// stops at a sentence end")? All three start offsets must be tried: a terminator is one
// byte ('.') or three (。); decoding from a continuation byte yields a non-terminator code
// point, so only the attempt at the mark's first byte can match.
static bool boundary_ends_at(const char *text, size_t page_end, size_t pos) {
    for (size_t n = 1; n <= 3 && n <= pos; ++n) {
        size_t nx = 0;
        if (sentence_stop(text, page_end, pos - n, &nx) && nx == pos) return true;
    }
    return false;
}

bool box_select_snap_sentence(const char *text, size_t text_len,
                              size_t page_start, size_t page_end,
                              size_t ws, size_t we, int dir,
                              size_t *out_s, size_t *out_e) {
    if (!text || !out_s || !out_e) return false;
    if (page_end > text_len) page_end = text_len;
    if (page_start > page_end) return false;
    if (we <= ws || ws < page_start || we > page_end) return false;

    size_t s = ws, e = we;
    if (dir < 0) {
        // 跨页句：本页可见的部分从句首开始，所以扫不到句界时落在**页首**（R-S4）。
        // 向后方向不动 s——那是上一次点按定下的锚，已经吸附过了。
        // A sentence crossing from the previous page is visible from the page start, so
        // "no boundary found" lands on page_start here (R-S4). The forward direction never
        // touches s: it is the anchor the previous tap already snapped.
        s = page_start;
        // 向前：从头扫到 ws，记住最后一个「结束点不超过 ws」的句界。
        // Backwards: sweep from the page start to ws and keep the last boundary that ends
        // at or before ws.
        size_t i = page_start;
        while (i < ws) {
            size_t nx = 0;
            if (sentence_stop(text, page_end, i, &nx) && nx <= ws) s = nx;
            uint32_t cp = 0;
            i += utf8_step(text, page_end, i, &cp);
        }
        // 跳过行首缩进（全角空格也算），但绝不吃掉点中的字本身。
        // Skip the leading indent (ideographic space included), never the tapped glyph.
        while (s < we) {
            uint32_t cp = 0;
            const size_t n = utf8_step(text, page_end, s, &cp);
            if (!is_indent_cp(cp)) break;
            s += n;
        }
    } else if (!boundary_ends_at(text, page_end, we)) {
        // 尾点恰好停在句末（点中的就是标点前的最后一个字，或标点本身）→ 不必再向后找，
        // 否则会一路吃到下一句、甚至吃到页尾。这条行为由用例 S4 锁住。
        // The tail already stops at a sentence end (the tap hit the last word before the
        // terminator, or the terminator itself) → do not scan forward, or it would swallow
        // the next sentence and possibly the rest of the page. Test S4 pins this.
        // 向后：从 we 扫到页尾，取第一个句界；标点计入，换行本身不计入。
        // 连续标点（？！、……、。」一类的连写）必须并入同一个句末——停在第一个上会把
        // 「真的吗？！」截成「真的吗？」，正好丢掉的还是语气最强的那一下。
        // Forwards: from we to the page end, take the first boundary. A terminator is
        // included; a newline itself is not. Runs of terminators (?!, ellipses, and the
        // like) merge into one sentence end - stopping at the first would cut "really?!"
        // down to "really?", dropping exactly the strongest mark.
        size_t i = we;
        bool hit = false;
        while (i < page_end) {
            size_t nx = 0;
            if (sentence_stop(text, page_end, i, &nx)) {
                hit = true;
                if (text[i] == '\n') { e = i; break; }
                e = nx;
                i = nx;
                while (i < page_end) {
                    size_t run = 0;
                    if (!sentence_stop(text, page_end, i, &run)) break;
                    if (text[i] == '\n') break;  // 换行自成一句，不并入前一个标点
                    e = run;
                    i = run;
                }
                break;
            }
            uint32_t cp = 0;
            i += utf8_step(text, page_end, i, &cp);
        }
        // 本页扫不到句末 = 句子跨到下一页：按页截断（不变量 R2 不破）。
        // scan 到页尾都没有句界，说明句子延续到下一页，选区停在页尾。
        // No terminator on this page means the sentence continues on the next one, so the
        // range stops at the page end (invariant R2 still holds).
        if (!hit) e = page_end;
    }
    if (s > ws || e < we || e <= s) return false;
    *out_s = s;
    *out_e = e;
    return true;
}

bool box_select_expired(box_selection_t *sel, int64_t now_ms) {
    if (!sel || !box_select_active(sel)) return false;
    if (!sel->deadline_ms || now_ms < sel->deadline_ms) return false;
    box_select_cancel(sel);
    return true;
}

void box_select_cancel(box_selection_t *sel) {
    if (!sel) return;
    memset(sel, 0, sizeof(*sel));
    sel->state = BOX_STATE_OFF;
}

bool box_select_active(const box_selection_t *sel) {
    return sel && sel->state != BOX_STATE_OFF;
}

bool box_select_ready(const box_selection_t *sel) {
    return sel && sel->state == BOX_STATE_READY;
}

size_t box_select_length(const box_selection_t *sel) {
    if (!box_select_ready(sel)) return 0;
    return sel->off_end > sel->off_start ? sel->off_end - sel->off_start : 0;
}

size_t box_select_copy_text(const box_selection_t *sel, const char *text, size_t text_len,
                            char *buf, size_t cap) {
    size_t n = box_select_length(sel);
    if (!text || !buf || !n || !cap) return 0;
    if (sel->off_end > text_len) return 0;            // 区间越界：宁可不拷 / refuse rather than clip
    if (n + 1 > cap) return 0;                        // 装不下（含 NUL）：不写半截 / need room for NUL
    memcpy(buf, text + sel->off_start, n);
    buf[n] = '\0';
    return n;
}
