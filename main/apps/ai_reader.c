/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * gen26.5 M2 Step9：详解阅读视图会话。形态与五设计点落点见 ai_reader.h 顶部
 * 设计记录——覆盖会话，不开书、不卸载原书章节缓冲。
 * The gen26.5 M2 Step9 detail-view session. See the design record at the top
 * of ai_reader.h: an overlay session that opens no book and never unloads the
 * source chapter buffer.
 */

#include "ai_reader.h"

#include <stdio.h>
#include <string.h>

#include "push_proto.h" // 释义长度上限链：详解缓冲必须容下协议槽 / length chain: detail buffer >= protocol slot
#include "ui_kit.h"
#include "ui_nav.h"

// 长度上限链（gen26.6）：详解视图要承接词卡装不下的长释义，缓冲不得窄于协议槽，
// 否则同一段释义在详解里也会被截断。与 dict_popup 的 SEG > DEF_MAX、
// vocab_log 的 VOCAB_AI_DEF_MAX >= DEF_MAX 共同构成一条编译期钉死的链。
// Length chain (gen26.6): the detail view takes definitions the card cannot hold,
// so its buffer must not be narrower than the protocol slot, or the same text is
// truncated there too. Together with dict_popup's SEG > DEF_MAX and vocab_log's
// VOCAB_AI_DEF_MAX >= DEF_MAX this forms one compile-time chain.
_Static_assert(AI_READER_DEF_MAX >= PUSH_AI_DEF_MAX,
               "gen26.6 length chain: the detail buffer must hold any protocol-sized definition");

/* ---------- 会话状态 / The session state ---------- */

typedef struct {
    bool active;
    char word[AI_READER_WORD_MAX];
    char def[AI_READER_DEF_MAX];
    size_t def_len;
    int page;
    int page_count;
    size_t pages[AI_READER_MAX_PAGES]; // 每页首字符字节偏移 / each page's first byte
    int chars_per_line, lines_per_page;
    ai_reader_source_t source;
} ai_reader_session_t;

static ai_reader_session_t s_session;

/* ---------- 断行与分页（纯逻辑，宿主可测）/ Wrapping and pagination ---------- */

// 从 off 起一行的字节长度（不含行尾 '\n'）。按 UTF-8 字符推进：前导字节
// （非 10xxxxxx）计一个字符，ASCII 也占一个全角位——与 dict_popup 容量公式
// （text_w/27px 全角 advance）同口径，保守方向是少装不错装。显式 '\n' 断行；
// 空行（'\n' 紧邻）返回 0 并保留占位，段落不塌陷。off 越界时返回 0。
// Byte length of one line starting at off (excluding the trailing '\n').
// Advances by UTF-8 character: a lead byte (not 10xxxxxx) counts one
// character, and ASCII also takes one full-width slot - the same measure as
// dict_popup's capacity formula (text_w / 27px advance), erring toward
// under-filling. An explicit '\n' breaks; an empty line returns 0 and keeps
// its slot so paragraphs do not collapse. Out-of-range off returns 0.
static size_t line_len_at(const char* text, size_t len, size_t off, int chars_per_line) {
    if (off >= len || chars_per_line <= 0) return 0;
    size_t i = off;
    int chars = 0;
    while (i < len && text[i] != '\n') {
        const unsigned char c = (unsigned char)text[i];
        if ((c & 0xC0) != 0x80) { // 前导字节 = 一个完整字符的开始 / lead byte starts a character
            if (chars >= chars_per_line) break; // 行满：下一个字符属于下一行 / line full: next char goes to the next line
            ++chars;
        }
        ++i;
    }
    return i - off;
}

// 跳过行尾 '\n'。/ Skip the line-ending '\n'.
static size_t next_line_off(const char* text, size_t len, size_t off) {
    return off < len && text[off] == '\n' ? off + 1 : off;
}

int ai_reader_paginate(const char* text, int chars_per_line, int lines_per_page,
                       size_t* out_starts, int max_pages) {
    if (!text || !*text || chars_per_line <= 0 || lines_per_page <= 0) return 0;
    const size_t len = strlen(text);
    int pages = 0;
    if (max_pages > 0 && out_starts) out_starts[pages] = 0;
    ++pages;
    size_t off = 0;
    int lines = 0;
    while (off < len) {
        if (lines >= lines_per_page) {
            // 截断保护只作用于「有输出缓冲」的场景：只数页（NULL）必须数完整，
            // 否则调用方拿到的页数偏小、末页停滞判定会提前放行。
            // The truncation guard applies only when an output buffer exists:
            // count-only (NULL) must count fully, or the caller gets a short
            // page count and the last-page stall check would release early.
            if (out_starts) {
                if (pages >= max_pages) return pages; // 页表满 / map full
                out_starts[pages] = off;
            }
            ++pages;
            lines = 0;
        }
        off = next_line_off(text, len, off + line_len_at(text, len, off, chars_per_line));
        ++lines;
    }
    return pages;
}

/* ---------- 会话操作 / Session operations ---------- */

static void copy_bounded(char* dst, size_t cap, const char* src) {
    if (!dst || !cap) return;
    if (!src) { dst[0] = 0; return; }
    snprintf(dst, cap, "%s", src);
}

bool ai_reader_open(const char* word, const char* def, const ai_reader_source_t* source) {
    if (!word || !*word || !def || !*def) return false;
    memset(&s_session, 0, sizeof(s_session));
    copy_bounded(s_session.word, sizeof(s_session.word), word);
    copy_bounded(s_session.def, sizeof(s_session.def), def);
    s_session.def_len = strlen(s_session.def);
    if (source) {
        s_session.source = *source;
    } else {
        memset(&s_session.source, 0, sizeof(s_session.source));
    }
    s_session.active = true; // 文本已就位才置活跃：空释义根本到不了这里 / active only once the text is in place
    return true;
}

void ai_reader_close(void) {
    memset(&s_session, 0, sizeof(s_session));
}

bool ai_reader_active(void) { return s_session.active; }

// 参数变化才重排；页码钳到新页数内。/ Repaginate only on a metric change;
// clamp the page into the new count.
static void ensure_paginated(int chars_per_line, int lines_per_page) {
    if (!s_session.active) return;
    if (chars_per_line == s_session.chars_per_line &&
        lines_per_page == s_session.lines_per_page) return;
    s_session.chars_per_line = chars_per_line;
    s_session.lines_per_page = lines_per_page;
    s_session.page_count = ai_reader_paginate(s_session.def, chars_per_line, lines_per_page,
                                               s_session.pages, AI_READER_MAX_PAGES);
    if (s_session.page >= s_session.page_count) s_session.page = s_session.page_count - 1;
    if (s_session.page < 0) s_session.page = 0;
}

void ai_reader_set_metrics(int chars_per_line, int lines_per_page) {
    if (chars_per_line <= 0 || lines_per_page <= 0) return;
    ensure_paginated(chars_per_line, lines_per_page);
}

app_redraw_t ai_reader_turn(int dir) {
    if (!s_session.active) return APP_REDRAW_NONE;
    if (s_session.page_count <= 0) return APP_REDRAW_NONE; // 未分页（render 未跑过）：不翻 / unpaginated: refuse
    const int next = s_session.page + (dir < 0 ? -1 : 1);
    if (next < 0 || next >= s_session.page_count) return APP_REDRAW_NONE; // 首/末页停滞 / stall at both ends
    s_session.page = next;
    return APP_REDRAW_PAGE;
}

int ai_reader_page(void) { return s_session.active ? s_session.page : 0; }
int ai_reader_page_count(void) { return s_session.active ? s_session.page_count : 0; }
const char* ai_reader_word(void) { return s_session.active ? s_session.word : ""; }
const ai_reader_source_t* ai_reader_source(void) {
    return s_session.active ? &s_session.source : NULL;
}

int ai_reader_page_lines(int page, ai_reader_line_t* out, int max) {
    if (!s_session.active || page < 0 || page >= s_session.page_count) return 0;
    if (s_session.chars_per_line <= 0 || s_session.lines_per_page <= 0) return 0;
    size_t off = s_session.pages[page];
    int n = 0;
    while (n < s_session.lines_per_page && off < s_session.def_len) {
        const size_t ll = line_len_at(s_session.def, s_session.def_len, off,
                                       s_session.chars_per_line);
        if (out && n < max) {
            out[n].off = off;
            out[n].len = ll;
        }
        ++n;
        off = next_line_off(s_session.def, s_session.def_len, off + ll);
    }
    return n;
}

/* ---------- 绘制（设备侧）/ Painting (device side) ---------- */

// 版面：状态区（时间电量）→ 词标题 + 右元信息列（页码/「AI 详解」）→ 分隔线 →
// 正文（与词卡同族：27px 全角、42px 行高）→ 页脚（来源书名 + 键位提示）。
// Layout: status area -> word title + a right meta column (page counter /
// "AI detail" tag) -> separator -> body (same family as the card: 27px
// full-width, 42px lines) -> footer (source title + key hints).
//
// 方向由调用方（app_book.render）设好：横竖屏共用本函数，UI_LOCK_WIDTH/HEIGHT
// 随 rotation 动态取值，每行字数/每页行数按实算版面走，自动适配。
// The caller (app_book.render) has set the rotation: portrait and landscape
// share this function through the dynamic UI_LOCK_WIDTH/HEIGHT, with per-line
// and per-page counts derived from the measured layout.
//
// 间距刻度（全版面唯一来源，禁止局部再设魔法数）/ The spacing scale - the
// single source for the whole layout; no local magic numbers:
//   PAD   页边距（左右同值，留白一致）/ page margin (equal left/right)
//   GAP   词与右元信息列之间的标准间隙 / standard gap between word and meta column
//   META_LINE_GAP 元信息列内两行的行距 / line pitch inside the meta column
void ai_reader_render(uint8_t* fb) {
    if (!s_session.active) return;
    const int w = UI_LOCK_WIDTH, h = UI_LOCK_HEIGHT;
    const int pad = 36;          // PAD
    const int gap = 24;          // GAP
    const int meta_line_gap = 8; // META_LINE_GAP
    const int line_h = 42, text_px = 27;
    const int body_top = 236;
    const int footer_h = 72;
    ai_reader_set_metrics((w - 2 * pad) / text_px, (h - footer_h - body_top) / line_h);

    ui_clear_page(fb);
    ui_nav_status(fb);

    // 标题区：左「词」+ 右「元信息列」（页码在上、「AI 详解」标签在下），两列
    // 共享同一视觉中心线；右列宽按内容实算，词的可用宽度由「屏宽 - 2×PAD -
    // 右列实宽 - GAP」推出——左右留白恒为 PAD、中间空隙恒为 GAP，不随页码位数
    // 抖动（原来的 160px 预留是魔法数：窄页码右边空一块、三位数页码又挤词）。
    // Title zone: the word on the left, a meta column on the right (counter
    // above, tag below), both sharing one visual centre line. The column width
    // is measured from its content and the word's budget is derived as
    // "screen - 2*PAD - measured - GAP", so margins stay PAD on both sides and
    // the gutter stays GAP regardless of the counter's digit count (the old
    // 160px reserve was a magic number: slack on narrow counters, crowding on
    // three-digit ones).
    //
    // 光学调整：48px 大字的视觉重心比数学中心偏低，右侧 24-30px 小字列整体
    // 上移 4px 才显得齐心（数学居正在墨水屏上看着是散的）。
    // Optical adjustment: a 48px word's visual centre of gravity sits below
    // its mathematical centre, so the small 24-30px column shifts up 4px to
    // read as aligned (pure math centring looks scattered on e-paper).
    char title[AI_READER_WORD_MAX];
    copy_bounded(title, sizeof(title), s_session.word);
    char page_label[32];
    snprintf(page_label, sizeof(page_label), "%d / %d", s_session.page + 1, s_session.page_count);
    const int counter_w = ui_text_fixed_width_px(30, page_label);
    const int tag_w = ui_text_fixed_width_px(24, "AI 详解");
    const int meta_w = (counter_w > tag_w ? counter_w : tag_w);
    const int title_cy = 148;                    // 词列的视觉中心线 / the word column's centre line
    (void)ui_text_title_fit(title, 48, w - 2 * pad - meta_w - gap, title);
    ui_text_title_vc(fb, pad, title_cy, 48, title, title, EPD_DRAW_ALIGN_LEFT);
    // 右列：列视觉中心对齐 title_cy - 4（光学上移）；列内两行按 META_LINE_GAP
    // 排——行1中心 = 列中心 - (行距 + 行2高)/2，行2 = 行1 + 行1高 + 行距。
    // Right column: its centre aligns to title_cy - 4 (the optical lift); the
    // two rows use META_LINE_GAP - row 1's centre is the column centre minus
    // (gap + row-2 height)/2, and row 2 sits a row-1 height plus gap below it.
    const int meta_col_cy = title_cy - 4;
    const int counter_cy = meta_col_cy - (meta_line_gap + 24) / 2;
    const int tag_cy = counter_cy + 30 + meta_line_gap;
    ui_text_fixed_vc(fb, w - pad, counter_cy, 30, page_label, EPD_DRAW_ALIGN_RIGHT, false);
    ui_text_fixed_vc(fb, w - pad, tag_cy, 24, "AI 详解", EPD_DRAW_ALIGN_RIGHT, false);
    ui_hairline(fb, 206, pad, w - 2 * pad, 0x70);

    // 正文：按页取行，行文本落到栈缓冲（行长 ≤ 每行字数 × 4B + 1；横屏最宽
    // 42 字也才 169B，256 宽裕且不占栈）。
    // Body: take the page's lines into a stack buffer (line <= chars_per_line *
    // 4 + 1; even the widest landscape line of 42 chars is 169B, so 256 is
    // roomy without costing stack).
    ai_reader_line_t lines[64];
    char line[256];
    const int n = ai_reader_page_lines(s_session.page, lines, 64);
    int y = body_top;
    for (int i = 0; i < n; ++i) {
        const size_t ll = lines[i].len < sizeof(line) - 1 ? lines[i].len : sizeof(line) - 1;
        memcpy(line, s_session.def + lines[i].off, ll);
        line[ll] = 0;
        ui_text(fb, pad, y, text_px, line, EPD_DRAW_ALIGN_LEFT, false);
        y += line_h;
    }

    // 页脚：来源书名（章节/页号）+ 键位提示，左右同 PAD、同一条视觉中心线
    // （分隔线 + footer_h/2，24px 中文光学下沉 2px）。深睡不保留，只读快照。
    // Footer: source title (chapter/page) + key hints, both at PAD margins on
    // one shared centre line (separator + footer_h/2, with a 2px optical drop
    // for 24px CJK). Nothing persists, so this only reads the snapshot.
    ui_hairline(fb, h - footer_h, pad, w - 2 * pad, 0x70);
    const int foot_cy = h - footer_h + footer_h / 2 + 2;
    char foot[192];
    const ai_reader_source_t* src = &s_session.source;
    if (src->title[0]) {
        snprintf(foot, sizeof(foot), "摘自：%s · 第%u章 第%u页",
                 src->title, (unsigned)(src->chapter + 1), (unsigned)(src->page + 1));
    } else {
        snprintf(foot, sizeof(foot), "摘自：未知书目");
    }
    ui_text_fixed_vc(fb, pad, foot_cy, 24, foot, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, w - pad, foot_cy, 24, "翻页键翻页 · 返回键关闭",
                     EPD_DRAW_ALIGN_RIGHT, false);
}
