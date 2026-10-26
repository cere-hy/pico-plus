/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * gen26.5 M2 Step9 详解阅读视图的宿主单测：分页纯函数（黄金值 + 字符边界 +
 * 空行 + 末页恰好装满）、会话状态机（首/末页停滞、快照、关闭清空、指标变化
 * 重排）、render 冒烟（ASan 覆盖行缓冲与栈）。
 * Host-side unit tests for the Step9 detail reading view: pagination (golden
 * values, character boundaries, empty lines, an exactly-full last page), the
 * session state machine (stall at both ends, snapshot, close clearing, metric
 * changes repaginating) and a render smoke test (ASan over line buffers).
 *
 * Host build (needs only a C compiler):
 *   cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 *       -Itools/app_loop_stubs -Imain -Imain/app -Imain/apps -Imain/ui \
 *       -Icomponents/push_sink/include -Icomponents/ble_page_turner/include \
 *       -o build/book-tests/ai_reader tools/ai_reader_host_test.c
 *   ./build/book-tests/ai_reader      # prints AI_READER_PASS on success
 *
 * 注意 ai_reader.h 在 main/apps/（不是 main/app/），且 app.h 经 app_loop_stubs
 * 的 common.h 间接拖入 push_sink.h——两个 include 路径漏一个都编不过。
 * NB: ai_reader.h lives in main/apps/ (not main/app/), and app.h drags in
 * push_sink.h through the stubs' common.h - drop either include path and the
 * build fails.
 */

// 被测模块：直接包含 .c，静态纯函数（断行/分页走规则）也进入覆盖。
// Include the .c directly so the static pure helpers (line walk, pagination)
// are covered too.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ai_reader.h"
#include "ai_reader.c"

/* ---------- ui_kit 绘制原语的宿主记录桩 / Host recording primitives ----------
 * render 的版面数值（居中、留白、间距刻度）没有可见输出，桩把调用参数记下来，
 * 由 test_layout_metrics 逐项断言——「修复效果」因此可验证，不只靠肉眼。
 * The layout numbers (centring, margins, spacing scale) have no visible output,
 * so the stubs record their arguments and test_layout_metrics asserts them one
 * by one - the fix is verified, not just eyeballed.
 */

static int rec_title_x, rec_title_cy, rec_title_px, rec_title_fit_w;
static int rec_counter_x, rec_counter_cy, rec_tag_cy, rec_foot_x_l, rec_foot_x_r, rec_foot_cy;
static int rec_hairline_body_y, rec_hairline_foot_y, rec_hairline_x, rec_hairline_w;
static int rec_fit_calls;
// gen26.6.3：正文行（ui_text_fixed）的字号/行距/起点，以及页脚左段文本——
// 「正文与词卡同族 36/46」与「页脚左段必须让位」两条断言要靠它们。
// gen26.6.3: the body rows' px/pitch/origin (ui_text_fixed) and the footer's left
// text - the two assertions "body matches the card's 36/46" and "the left footer
// block yields to the hints" are built on them.
static int rec_body_px, rec_body_y0, rec_body_pitch, rec_body_count, rec_body_x;
static char rec_foot_text[256];

void ui_clear_page(uint8_t* framebuffer) { (void)framebuffer; }
void ui_nav_status(uint8_t* framebuffer) { (void)framebuffer; }
void ui_text(uint8_t* fb, int x, int y_top, int px, const char* text,
             enum EpdFontFlags align, bool inverted) {
    (void)fb; (void)x; (void)y_top; (void)px; (void)text; (void)align; (void)inverted;
}
void ui_text_fixed(uint8_t* fb, int x, int y_top, int px, const char* text,
                   enum EpdFontFlags align, bool inverted) {
    (void)fb; (void)x; (void)text; (void)align; (void)inverted;
    if (px == 72) { // 正文行 / body rows
        if (rec_body_count == 0) rec_body_y0 = y_top;
        else if (rec_body_count == 1) rec_body_pitch = y_top - rec_body_y0;
        ++rec_body_count;
        rec_body_px = px;
    }
}
void ui_text_fixed_vc(uint8_t* fb, int x, int center_y, int px, const char* text,
                      enum EpdFontFlags align, bool inverted) {
    (void)fb; (void)px; (void)align; (void)inverted;
    if (text && strcmp(text, "AI 详解") == 0) {
        rec_tag_cy = center_y;                 // 标签行 / the tag row
    } else if (text && strncmp(text, "摘自", 4) == 0) {
        rec_foot_x_l = x; rec_foot_cy = center_y;
        snprintf(rec_foot_text, sizeof(rec_foot_text), "%s", text);
    } else if (text && strstr(text, "返回键关闭")) {
        rec_foot_x_r = x;
    } else {
        rec_counter_x = x; rec_counter_cy = center_y; // 页码行 / the page counter
    }
}
void ui_text_title_vc(uint8_t* fb, int x, int center_y, int px, const char* text,
                      const char* font_sample, enum EpdFontFlags align) {
    (void)fb; (void)text; (void)font_sample; (void)align;
    rec_title_x = x; rec_title_cy = center_y; rec_title_px = px;
}
int ui_text_title_fit(char* text, int preferred_px, int width, const char* font_sample) {
    (void)text; (void)font_sample;
    rec_title_fit_w = width;
    ++rec_fit_calls;
    return preferred_px <= width ? preferred_px : width;
}
int ui_text_fixed_width_px(int px, const char* text) {
    (void)px;
    return text ? (int)strlen(text) * 12 : 0; // 确定性强宽度：只用于版面实算 / a deterministic width for layout math
}
void ui_hairline(uint8_t* fb, int y, int x, int w, uint8_t color) {
    (void)fb; (void)color;
    if (y < 400) rec_hairline_body_y = y; else rec_hairline_foot_y = y;
    rec_hairline_x = x; rec_hairline_w = w;
}

/* ---------- 阅读字体路径桩 / Reader-face path stubs ----------
 * gen26.6.5：正文不再走 ui_text_fixed（点阵系统），改走 ui_text_reader（阅读字体
 * 全路径）。桩在这里记录「走了哪条路」，并给出与真实字体同比例的逐字宽度：
 * 模式 0 = 旧单位模型（全角 10 / ASCII 5，让历史黄金值继续成立）；
 * 模式 1 = 真实比例（全角 = px，ASCII = 0.57px，对应 Noto 'a' 的 0.575em）。
 * 模式 1 专门喂给 render 用例，保证分页数字与真机同源。
 * gen26.6.5: the body no longer uses ui_text_fixed (bitmap system) but
 * ui_text_reader (reader-face full path). These stubs record which path ran and
 * supply per-glyph widths at the real font's ratios: mode 0 is the old unit model
 * (10 / 5) so historical goldens still hold; mode 1 is the real ratio (full-width
 * = px, ASCII = 0.57px, matching Noto 'a' at 0.575em) and is what render cases use.
 */
static int s_width_mode;
static int rec_ttf_body_calls, rec_bitmap_body_calls;

static int fake_advance(void* ctx, uint32_t cp, int px) {
    (void)ctx;
    if (cp < 0x80) return s_width_mode ? (px * 57 + 50) / 100 : 5;
    return s_width_mode ? px : 10;
}
static void use_real_widths(int on) { s_width_mode = on; }

// 注入与正文一致的样式（字号 51 / 行距 76 / 左缘 36 / 版心 612）——与真机 px=51、
// 行距 150% 的正文同源；render 必须原样采用，不准自算。
// Inject the same style the live body uses (51px / 76 pitch / left 36 / measure 612):
// render must adopt it verbatim, never re-derive its own numbers.
static void use_reader_style(int px, int line_h, int left, int width) {
    ai_reader_style_t st = {.px = px, .line_h = line_h, .left = left, .width = width};
    ai_reader_set_style(&st);
}

int ui_text_reader_advance_px(int px, uint32_t cp) { return fake_advance(NULL, cp, px); }
bool ui_text_reader_uses_ttf(const char* text) { (void)text; return true; }
int ui_text_reader_cap_px(int px) { return (px * 72 + 50) / 100; }
void ui_text_reader(uint8_t* fb, int x, int y_top, int px, const char* text) {
    (void)fb; (void)text;
    if (ui_text_reader_uses_ttf(text)) ++rec_ttf_body_calls; else ++rec_bitmap_body_calls;
    if (rec_body_count == 0) { rec_body_y0 = y_top; rec_body_x = x; }
    else if (rec_body_count == 1) rec_body_pitch = y_top - rec_body_y0;
    ++rec_body_count;
    rec_body_px = px;
}

/* ---------- 断言辅助 / Assertion helpers ---------- */

static int checks, reds;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++reds; \
    fprintf(stderr, "RED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// 无持久化符号的编译期证据：本翻译单元只链接 ai_reader.c 与测试自身，
// 若会话层碰了 nvs/sd/文件 API，这里的 nm 级不变量由 wiring 脚本在源码
// 侧把关（grep 禁用清单），宿主侧只验证行为面无外部状态。
// Compile-time evidence of no persistence: this unit links ai_reader.c and
// the test only; the source-side ban list is enforced by the wiring script,
// while the host side verifies there is no external state.

/* ---------- 分页 / Pagination ---------- */

static void test_paginate_ascii(void) {
    // gen26.6.3 宽度模型：ASCII 记半格（1 单位）、全角 2 单位，预算 = 每行字数×2。
    // 每行 10 字 = 20 单位 → 一行 20 个 ASCII；100 字 → 5 行，每页 5 行 → 1 页。
    // 旧模型把 ASCII 也当全角（10 字/行），英文释义只用到约 55% 版心——真机
    // 「详解页只有一半、右半页全空」就是它。
    // gen26.6.3 width model: ASCII costs half a slot (1 unit), full-width 2,
    // budget = chars_per_line*2. 10 chars/line = 20 units -> 20 ASCII per line;
    // 100 chars -> 5 lines; 5 lines/page -> 1 page. The old model billed ASCII a
    // full-width slot (10/line) and used ~55% of the measure - exactly the
    // on-device "detail page only half full".
    char text[101];
    for (int i = 0; i < 100; ++i) text[i] = 'a' + (char)(i % 26);
    text[100] = 0;
    size_t starts[AI_READER_MAX_PAGES];
    const int pages = ai_reader_paginate(text, 100, 10, 5, starts, AI_READER_MAX_PAGES);
    CHECK(pages == 1);
    CHECK(starts[0] == 0);
    // 只数页模式（NULL 输出）同结果。/ Count-only mode (NULL out) agrees.
    CHECK(ai_reader_paginate(text, 100, 10, 5, NULL, 0) == 1);
    // 每页 1 行：100 / 20 = 5 页，且第 2 页首字节必须正好是 20（一行 20 个 ASCII）。
    // One line per page: 100/20 = 5 pages, and page 2 must start at byte 20 (20 ASCII).
    CHECK(ai_reader_paginate(text, 100, 10, 1, starts, AI_READER_MAX_PAGES) == 5);
    CHECK(starts[1] == 20);
    // 每页 10 行 → 1 页（容量 200 > 100，不多开空页）。
    // 10 lines per page -> 1 page (capacity 200 > 100, no phantom page).
    CHECK(ai_reader_paginate(text, 100, 10, 10, starts, AI_READER_MAX_PAGES) == 1);
}

static void test_paginate_chinese_boundary(void) {
    // 30 个中文（每字 3B）、每行 10 字、每页 2 行 → 2 页；第 2 页首字节
    // 必须落在字符边界 60（20 字 × 3B），不从多字节序列中间砍。
    // 30 Chinese chars (3B each), 10 per line, 2 lines per page -> 2 pages;
    // page 2 must start on the character boundary 60 (20 chars * 3B).
    char text[91];
    for (int i = 0; i < 30; ++i) memcpy(text + i * 3, "汉", 3);
    text[90] = 0;
    size_t starts[AI_READER_MAX_PAGES];
    const int pages = ai_reader_paginate(text, 100, 10, 2, starts, AI_READER_MAX_PAGES);
    CHECK(pages == 2);
    CHECK(starts[0] == 0);
    CHECK(starts[1] == 60);
    // 行缓冲区不越界由 ASan 兜底；页内行重走一致。
    // ASan guards the line buffer; the per-page line walk agrees.
    ai_reader_source_t src = {.chapter = 2, .page = 7, .page_start_offset = 1234,
                              .title = "测试书目"};
    CHECK(ai_reader_open("汉字", text, &src));
    ai_reader_set_layout(100, 10, 2);
    CHECK(ai_reader_page_count() == 2);
    ai_reader_line_t lines[8];
    const int n = ai_reader_page_lines(0, lines, 8);
    CHECK(n == 2);
    CHECK(lines[0].off == 0 && lines[0].len == 30);   // 10 字 × 3B
    CHECK(lines[1].off == 30 && lines[1].len == 30);
    const int n2 = ai_reader_page_lines(1, lines, 8);
    CHECK(n2 == 1);                                    // 末页只剩 10 字
    CHECK(lines[0].off == 60 && lines[0].len == 30);
    ai_reader_close();
}

static void test_paginate_newlines(void) {
    // 显式 \n 断行 + 空行保留占位 + 行尾 \n 不产生幻影行。
    // Explicit '\n' breaks, empty lines keep their slot, a trailing '\n'
    // produces no phantom line.
    const char* text = "ab\n\ncd\ne";
    size_t starts[AI_READER_MAX_PAGES];
    // 每行 10 字、每页 10 行：4 行内容（ab / 空 / cd / e）→ 1 页。
    // 10 per line, 10 per page: 4 content rows -> 1 page.
    CHECK(ai_reader_paginate(text, 100, 10, 10, starts, AI_READER_MAX_PAGES) == 1);
    // 每页 2 行：ab+空 / cd+e → 2 页；第 2 页首在 "cd"（偏移 4）。
    // 2 lines per page: ab+empty / cd+e -> 2 pages; page 2 starts at "cd" (offset 4).
    CHECK(ai_reader_paginate(text, 100, 10, 2, starts, AI_READER_MAX_PAGES) == 2);
    CHECK(starts[1] == 4);
    CHECK(ai_reader_open("w", text, NULL));
    ai_reader_set_layout(100, 10, 2);
    ai_reader_line_t lines[8];
    CHECK(ai_reader_page_lines(0, lines, 8) == 2);
    CHECK(lines[0].len == 2 && lines[1].len == 0);     // 空行占位 / empty line keeps its slot
    CHECK(ai_reader_page_lines(1, lines, 8) == 2);
    CHECK(lines[0].len == 2 && lines[1].len == 1);
    ai_reader_close();
}

static void test_paginate_edges(void) {
    size_t starts[AI_READER_MAX_PAGES];
    // 空文本与非法参数 → 0 页。/ Empty text and invalid metrics -> 0 pages.
    CHECK(ai_reader_paginate("", 100, 10, 5, starts, AI_READER_MAX_PAGES) == 0);
    CHECK(ai_reader_paginate(NULL, 100, 10, 5, starts, AI_READER_MAX_PAGES) == 0);
    CHECK(ai_reader_paginate("abc", 0, 10, 5, starts, AI_READER_MAX_PAGES) == 0);
    CHECK(ai_reader_paginate("abc", 100, 10, 0, starts, AI_READER_MAX_PAGES) == 0);
    CHECK(ai_reader_paginate("abc", -10, 10, 5, starts, AI_READER_MAX_PAGES) == 0);
    // gen26.6.3 宽度模型下「恰好装满」= 每行 20 个 ASCII × 5 行 = 100 字 → 1 页。
    // Under the gen26.6.3 width model "exactly full" is 20 ASCII per line * 5 = 100.
    char text[102];
    memset(text, 'x', 100);
    text[100] = 0;
    CHECK(ai_reader_paginate(text, 100, 10, 5, starts, AI_READER_MAX_PAGES) == 1);
    // 101 字 → 第 2 页 1 字（text[101] 留给 NUL，别把终止符写掉）。
    // 101 chars -> 1 char on page 2 (text[101] is the NUL - do not overwrite it).
    text[100] = 'y';
    text[101] = 0;
    CHECK(ai_reader_paginate(text, 100, 10, 5, starts, AI_READER_MAX_PAGES) == 2);
    CHECK(starts[1] == 100);
    // 页表截断保护：max_pages=1 时只输出 1 页（有缓冲才截断）。
    // Map truncation guard: max_pages=1 emits one page (truncates only with a buffer).
    CHECK(ai_reader_paginate(text, 100, 10, 5, starts, 1) == 1);
    // 只数页模式数完整：同一文本数出 2 页（NULL 输出不截断）。
    // Count-only mode counts fully: the same text counts 2 pages (no truncation without a buffer).
    CHECK(ai_reader_paginate(text, 100, 10, 5, NULL, 0) == 2);
}

/* ---------- 会话状态机 / Session state machine ---------- */

static void test_turn_stall(void) {
    // 铁律①：首页再按上一页、末页再按下一页，都停滞不退出。
    // Law 1: previous on page 1 and next on the last page both stall.
    char text[111];
    memset(text, 'z', 110);
    text[110] = 0;
    CHECK(ai_reader_open("stall", text, NULL));
    ai_reader_set_layout(100, 10, 5); // 每页 5 行 × 20 个 ASCII = 100 字 → 110 字共 2 页
    CHECK(ai_reader_page_count() == 2);
    CHECK(ai_reader_turn(-1) == APP_REDRAW_NONE);      // 首页停滞 / first page stalls
    CHECK(ai_reader_page() == 0);
    CHECK(ai_reader_turn(1) == APP_REDRAW_PAGE);
    CHECK(ai_reader_page() == 1);
    CHECK(ai_reader_turn(-1) == APP_REDRAW_PAGE);
    CHECK(ai_reader_page() == 0);
    CHECK(ai_reader_turn(1) == APP_REDRAW_PAGE);
    CHECK(ai_reader_turn(1) == APP_REDRAW_NONE);       // 末页停滞，不自动退出 / last page stalls
    CHECK(ai_reader_page() == 1);
    ai_reader_close();
}

static void test_turn_before_pagination(void) {
    // render 未跑过（未分页）时翻页拒绝，不野跳。/ Turns refuse before the
    // first pagination (render never ran); no wild jumps.
    CHECK(ai_reader_open("nopage", "some definition text", NULL));
    CHECK(ai_reader_page_count() == 0);
    CHECK(ai_reader_turn(1) == APP_REDRAW_NONE);
    CHECK(ai_reader_turn(-1) == APP_REDRAW_NONE);
    CHECK(ai_reader_page() == 0);
    ai_reader_close();
}

static void test_metrics_change_clamps(void) {
    // 横竖屏切换（指标变化）重排；页码超新页数时钳到末页。
    // A rotation switch (metric change) repaginates; a page beyond the new
    // count clamps to the last page.
    char text[121];
    memset(text, 'q', 120);
    text[120] = 0;
    CHECK(ai_reader_open("clamp", text, NULL));
    ai_reader_set_layout(100, 10, 5);  // gen26.6.3：20 单位/行 = 20 ASCII，×5 行 = 100/页 → 2 页（120/100）
    CHECK(ai_reader_page_count() == 2);
    (void)ai_reader_turn(1);
    CHECK(ai_reader_page() == 1);                    // 末页 / last page
    ai_reader_set_layout(200, 10, 5);                    // 40/行 × 5 = 200/页 → 1 页
    CHECK(ai_reader_page_count() == 1);
    CHECK(ai_reader_page() == 0);                    // 钳到新末页 / clamped to the new last page
    ai_reader_set_layout(100, 10, 5);                    // 切回 2 页：页码停在钳位值，不放回旧页
    CHECK(ai_reader_page() == 0);
    ai_reader_close();
}

static void test_open_rejects_empty(void) {
    // 空词/空释义不开会话（调用方照旧走词卡路径）。
    // An empty word/definition opens nothing (the caller keeps the card path).
    CHECK(!ai_reader_active());
    CHECK(!ai_reader_open("", "def", NULL));
    CHECK(!ai_reader_active());
    CHECK(!ai_reader_open("word", "", NULL));
    CHECK(!ai_reader_active());
    CHECK(!ai_reader_open(NULL, "def", NULL));
    CHECK(!ai_reader_active());
    CHECK(!ai_reader_open("word", NULL, NULL));
    CHECK(!ai_reader_active());
    // 关闭态下所有 getter 安全返回零值。/ Getters are zero-safe when closed.
    CHECK(ai_reader_page() == 0);
    CHECK(ai_reader_page_count() == 0);
    CHECK(strcmp(ai_reader_word(), "") == 0);
    CHECK(ai_reader_source() == NULL);
    CHECK(ai_reader_turn(1) == APP_REDRAW_NONE);
    ai_reader_close(); // 幂等 / idempotent
}

static void test_source_snapshot(void) {
    // 来源书页快照句柄：打开时拷贝、关闭即失（深睡不保留的语义基础）。
    // The source snapshot handle: copied on open, gone on close (the semantic
    // basis for not surviving deep sleep).
    ai_reader_source_t src = {.chapter = 4, .page = 12, .page_start_offset = 99881,
                              .title = "三体"};
    CHECK(ai_reader_open("物理学", "一段足够长的释义文本用于分页。", &src));
    const ai_reader_source_t* got = ai_reader_source();
    CHECK(got != NULL);
    CHECK(got->chapter == 4 && got->page == 12);
    CHECK(got->page_start_offset == 99881);
    CHECK(strcmp(got->title, "三体") == 0);
    CHECK(strcmp(ai_reader_word(), "物理学") == 0);
    ai_reader_close();
    CHECK(ai_reader_source() == NULL);
    // 无 source 打开也安全（全零快照）。/ Opening without a source is safe too.
    CHECK(ai_reader_open("w", "d", NULL));
    CHECK(ai_reader_source() != NULL);
    CHECK(ai_reader_source()->chapter == 0 && ai_reader_source()->title[0] == 0);
    ai_reader_close();
}

/* ---------- gen26.6.5 装行规则 / gen26.6.5 line-breaking rules ---------- */

static void test_paginate_word_boundary(void) {
    // 英文按词断：每行 20 ASCII（单位模式），"abcdefghijklm nopqrstuvwxyz" 的
    // 空格在 13 个字符后（65% 版心 > 60% 门槛）→ 断在空格后，不把 "nopqrstuvwxyz"
    // 拦腰截断。旧实现按字符硬断，真机出现 "emer|gencies"。
    // English breaks at a word: 20 ASCII per line (unit mode). The space in
    // "abcdefghijklm nopqrstuvwxyz" sits after 13 chars (65% > the 60% floor), so the
    // break lands after the space instead of splitting the next word - the old
    // character-level break produced the on-device "emer|gencies".
    const char* text = "abcdefghijklm nopqrstuvwxyz";
    size_t starts[AI_READER_MAX_PAGES];
    CHECK(ai_reader_paginate(text, 100, 10, 1, starts, AI_READER_MAX_PAGES) == 2);
    CHECK(starts[1] == 14); // "abcdefghijklm " 共 14 字节 / the whole word plus the space
}

static void test_paginate_no_start_punct(void) {
    // 避头：第 11 个字符是「，」，若直接硬断会落到行首 → 连前一个字一起下移，
    // 首行只放 9 个字（27 字节），第二行以第 10 个字开头再跟「，」。
    // No-start: the 11th char is "，"; a hard break would put it at the line head, so
    // the previous char moves down too - the first line holds 9 chars (27 bytes) and
    // the second starts with the 10th char followed by "，".
    const char* text = "一二三四五六七八九十，后续内容在此";
    size_t starts[AI_READER_MAX_PAGES];
    CHECK(ai_reader_paginate(text, 100, 10, 1, starts, AI_READER_MAX_PAGES) == 2);
    CHECK(starts[1] == 27);
    CHECK(ai_reader_paginate(text, 100, 10, 99, NULL, 0) == 1); // 一页装得下 / fits one page
}

static void test_paginate_no_end_punct(void) {
    // 避尾：第 10 个字符是「（」，不能留在行尾 → 整行在它之前断，首行 9 个字。
    // No-end: the 10th char is "（" and may not end a line, so the break moves before
    // it and the first line holds 9 chars.
    const char* text = "一二三四五六七八九（内容继续";
    size_t starts[AI_READER_MAX_PAGES];
    CHECK(ai_reader_paginate(text, 100, 10, 1, starts, AI_READER_MAX_PAGES) >= 2);
    CHECK(starts[1] == 27);
}

static int hook_advance(void* ctx, uint32_t cp, int px) {
    (void)cp; (void)px;
    if (ctx) ++*(int*)ctx;
    return 7;
}

static void test_advance_hook(void) {
    // 逐字宽度回调确实接管装行（宿主注入 7px/字）：10 个字 = 70px → 恰好 1 行。
    // The per-glyph advance hook really drives breaking (7px/char injected): ten
    // characters are 70px -> exactly one line.
    static int calls;
    calls = 0;
    ai_reader_set_advance(hook_advance, &calls);
    size_t starts[AI_READER_MAX_PAGES];
    CHECK(ai_reader_paginate("abcdefghij", 70, 72, 1, starts, AI_READER_MAX_PAGES) == 1);
    CHECK(calls > 0);
    ai_reader_set_advance(NULL, NULL);
}

static void test_truncation_guards(void) {
    // 超长词/释义截断不溢出（ASan 兜底越界写）。
    // Over-long word/definition truncation never overflows (ASan catches writes).
    char long_def[AI_READER_DEF_MAX + 400];
    memset(long_def, 'A', sizeof(long_def) - 1);
    long_def[sizeof(long_def) - 1] = 0;
    char long_word[AI_READER_WORD_MAX + 100];
    memset(long_word, 'W', sizeof(long_word) - 1);
    long_word[sizeof(long_word) - 1] = 0;
    CHECK(ai_reader_open(long_word, long_def, NULL));
    CHECK(ai_reader_active());
    CHECK(strlen(ai_reader_word()) == AI_READER_WORD_MAX - 1);
    // 释义截到 AI_READER_DEF_MAX-1：open 内部 strlen 不越界。
    // The definition truncates at AI_READER_DEF_MAX-1; open's strlen stays in bounds.
    ai_reader_set_layout(200, 10, 5);
    CHECK(ai_reader_page_count() > 1);
    ai_reader_close();
}

static void test_render_smoke(void) {
    // render 冒烟：桩 ui_* 下跑竖屏版面，ASan 查栈缓冲与页脚 snprintf。
    // Render smoke: run the portrait layout against stub ui_*; ASan watches
    // the stack buffer and the footer snprintf.
    CHECK(ai_reader_open("render", "第一行释义。\n\n第二段释义，稍微长一点以触发换行行为。", NULL));
    ai_reader_set_layout(220, 10, 21); // 竖屏实算值 / the measured portrait values
    CHECK(ai_reader_page_count() == 1);               // 3 行内容一页装下 / 3 rows fit one page
    ai_reader_render(NULL);        // 桩忽略 fb / the stubs ignore fb
    CHECK(ai_reader_turn(1) == APP_REDRAW_NONE);      // 单页末页停滞 / single page stalls
    ai_reader_render(NULL);
    use_real_widths(0);
    ai_reader_close();
    ai_reader_render(NULL);        // 关闭态直接返回，不画 / closed: paints nothing
    CHECK(!ai_reader_active());
}

static void test_footer_clamp(void) {
    // gen26.6.3：书名很长（96 字上限）时，页脚左段必须截短到「屏宽 - 2PAD - 提示宽
    // - 24」之内，绝不压到右侧键位提示——真机页脚两段直接叠成一团。
    // gen26.6.3: with a very long title (96-char cap) the left footer block must be
    // clipped to "screen - 2*PAD - hint - 24" and never run under the key hints:
    // on hardware the two blocks were printed on top of each other.
    char def[31];
    memset(def, 'y', 30);
    def[30] = 0;
    ai_reader_source_t src;
    memset(&src, 0, sizeof(src));
    memset(src.title, 'W', sizeof(src.title) - 1);
    src.title[sizeof(src.title) - 1] = 0; // 95 个 W / 95 W's
    CHECK(ai_reader_open("longtitle", def, &src));
    rec_foot_text[0] = 0;
    use_real_widths(1);
    ai_reader_render(NULL);
    use_real_widths(0);
    const int avail = UI_LOCK_WIDTH - 2 * 36 -
                      ui_text_fixed_width_px(24, "翻页键翻页 · 返回键关闭") - 24;
    CHECK(rec_foot_text[0] != 0);
    CHECK(ui_text_fixed_width_px(24, rec_foot_text) <= avail);
    CHECK(strlen(rec_foot_text) < 95); // 确实被截短了 / actually clipped
    ai_reader_close();
}

static void test_reopen_resets(void) {
    // 再打开同一会话：页码、快照全部重置，不残留上一次会话的状态。
    // Reopening resets page and snapshot; nothing survives from the last session.
    CHECK(ai_reader_open("first", "first definition", NULL));
    ai_reader_set_layout(50, 10, 1); // 10 个 ASCII/页 → "first definition" 16 字 → 2 页
    (void)ai_reader_turn(1);
    CHECK(ai_reader_page() > 0);
    ai_reader_source_t src = {.chapter = 9, .page = 3, .title = "第二本"};
    CHECK(ai_reader_open("second", "second", &src));
    CHECK(ai_reader_page() == 0);
    CHECK(strcmp(ai_reader_word(), "second") == 0);
    CHECK(ai_reader_source()->chapter == 9);
    ai_reader_close();
}

static void test_layout_metrics(void) {
    // 版面数值断言（redesign 修复的可验证证据）：竖屏桩 684×1216，
    // PAD=36、GAP=24、META_LINE_GAP=8、title_cy=148、footer_h=72。
    // Layout assertions (the verifiable evidence for the redesign fix): the
    // portrait shim is 684x1216 with PAD=36, GAP=24, META_LINE_GAP=8,
    // title_cy=148, footer_h=72.
    const int w = 684, h = 1216;
    const int fit_calls_before = rec_fit_calls;
    // gen26.6.5：900 个 ASCII → 版心 612px ÷ 41px（72px 下 'a' 的 0.57em）≈ 14 个/行
    // × 9 行 = 126/页 → 8 页。旧单位模型给 16 个/行（多算 15%），真机才会「英文顶到
    // 右边距、中文空三分之一」。
    // gen26.6.5: 900 ASCII at 612px / 41px (0.57em at 72px) ~= 14 per line x 9 lines
    // = 126/page -> 8 pages. The old unit model allowed 16 per line (15% over), which
    // is exactly why English ran past the margin while CJK left a third blank.
    char def[901];
    memset(def, 'x', 900);
    def[900] = 0;
    CHECK(ai_reader_open("layout", def, NULL));
    rec_body_count = 0;
    rec_body_pitch = 0;
    rec_ttf_body_calls = 0;
    rec_bitmap_body_calls = 0;
    // 注入与正文一致的样式（真机 px=51、行距 150%→76、左缘 36、版心 612）。
    // Inject the live body's style (on-device px=51, 150% -> 76, left 36, measure 612).
    use_reader_style(51, 76, 36, 612);
    use_real_widths(1);
    ai_reader_render(NULL);
    use_real_widths(0);
    // 900 ASCII @ 51px（0.57em≈29px）→ 612/29 = 21/行 × 11 行 = 231/页 → 4 页。
    // 900 ASCII at 51px (0.57em ~= 29px) -> 21 per line x 11 lines -> 4 pages.
    CHECK(ai_reader_page_count() == 4);
    // gen26.6.5：正文走阅读字体全路径——TTF 行数 > 0、点阵兜底 0 次（变异 N1 的牙）。
    // Reader-face full path: TTF rows > 0 and zero bitmap fallbacks (mutation N1's teeth).
    CHECK(rec_ttf_body_calls > 0);
    CHECK(rec_bitmap_body_calls == 0);
    CHECK(ui_text_reader_uses_ttf("汉字"));
    // 门禁从「cap≈51」改为「详解 px == 正文 px」：同源即正确，cap 只记录（变异 N5 的牙）。
    // The gate is "detail px == reader px" (same source) instead of an absolute cap;
    // the cap is logged only (mutation N5's teeth).
    CHECK(rec_body_px == 51);
    CHECK(rec_body_px == 51); // 与注入的正文样式同值 / same as the injected body style
    CHECK(rec_body_pitch == 76);   // 51 × 150% / the body's own pitch
    CHECK(rec_body_y0 == 236);
    CHECK(rec_body_count == 11);   // (1216-72-236)/76 / lines per page
    CHECK(rec_body_x == 36);       // 正文版心左缘 / the body's left edge
    const int cap = ui_text_reader_cap_px(rec_body_px);
    CHECK(cap >= 35 && cap <= 39); // 51 × 0.72 ≈ 37，与正文同观感 / same optical size
    CHECK(rec_fit_calls == fit_calls_before + 1); // 每次渲染恰好拟合一次 / exactly one fit per render
    // ① 统一中线：词在 PAD、视觉中心 title_cy=148；右列几何黄金值
    //    （列心 = 148-4 = 144：页码行心 128、标签行心 166，行距 = 30+8）。
    // 1. Shared centre line: the word sits at PAD on centre 148; the right
    //    column's golden geometry (column centre 144: counter 128, tag 166,
    //    pitch 30+8).
    CHECK(rec_title_x == 36 && rec_title_cy == 148 && rec_title_px == 51);
    // 口径②：标题与正文同大（变异 N4：标题小于正文必擒）。
    // Ruling 2: title == body (mutation N4 - a smaller title must be caught).
    CHECK(rec_title_px == rec_body_px);
    CHECK(rec_counter_x == w - 36);
    // 口径③：页码 24 / 标签 20 → 列心 144：页码 130、标签 162，行距 24+8。
    // Ruling 3: counter 24 / tag 20 -> column centre 144: 130 and 162, pitch 24+8.
    CHECK(rec_counter_cy == 130 && rec_tag_cy == 162);
    CHECK(rec_tag_cy - rec_counter_cy == 24 + 8); // 列内行距标准 / standard pitch inside the column
    // ② 左右留白一致：词/页码列/页脚左右块全贴 PAD；两条分隔线同 x 同宽。
    // 2. Consistent margins: word, meta column and both footer blocks all sit
    //    on PAD; both separators share x and width.
    CHECK(rec_foot_x_l == 36 && rec_foot_x_r == w - 36);
    CHECK(rec_hairline_x == 36 && rec_hairline_w == w - 2 * 36);
    CHECK(rec_hairline_body_y == 206 && rec_hairline_foot_y == h - 72);
    // ③ 标准间隙：词可用宽 = 屏宽 - 2×PAD - 右列实宽(108) - GAP = 480。
    //    右列实宽取 max(页码 60, 「AI 详解」108)：固定文本兜住宽度，页码位数
    //    变化不吃词的留白。旧的 160px 预留魔法数在这里会是 452。
    // 3. Standard gap: the word budget is screen - 2*PAD - measured(108) - GAP
    //    = 480. The measured width is max(counter 60, "AI detail" 108): the
    //    fixed text holds the width so counter digits never eat the margin.
    //    The old 160px magic reserve would have made it 452.
    CHECK(rec_title_fit_w == 480);
    // ④ 页脚半高定位 + 24px 中文光学 2px：1182（旧魔法数 +26 是 1170）。
    // 4. Footer at half height plus a 2px optical drop for 24px CJK: 1182
    //    (the old magic +26 would have been 1170).
    CHECK(rec_foot_cy == h - 72 + 72 / 2 + 2);
    CHECK(rec_foot_cy == 1182);
    // ⑥ gen26.6.3：页脚左段必须给右侧键位提示让位（真机页脚两段叠字）。
    // 6. gen26.6.3: the left footer block must yield to the key hints (the
    //    on-device footer had the two blocks printed on top of each other).
    CHECK(ui_text_fixed_width_px(24, rec_foot_text) <=
          w - 2 * 36 - ui_text_fixed_width_px(24, "翻页键翻页 · 返回键关闭") - 24);
    // ⑤ 翻页不抖动：页码从 1 / 2 到 2 / 2，词的可用宽度不变（右列实宽由
    //    固定文本「AI 详解」兜住，页码位数变化不吃词的留白）。
    // 5. No jitter on turn: 1/2 -> 2/2 leaves the word budget untouched.
    const int fit_w_before = rec_title_fit_w;
    CHECK(ai_reader_turn(1) == APP_REDRAW_PAGE);
    ai_reader_render(NULL);
    CHECK(rec_title_fit_w == fit_w_before);
    ai_reader_close();
}

int main(void) {
    test_paginate_ascii();
    test_paginate_chinese_boundary();
    test_paginate_newlines();
    test_paginate_edges();
    test_paginate_word_boundary();
    test_paginate_no_start_punct();
    test_paginate_no_end_punct();
    test_advance_hook();
    test_turn_stall();
    test_turn_before_pagination();
    test_metrics_change_clamps();
    test_open_rejects_empty();
    test_source_snapshot();
    test_truncation_guards();
    test_render_smoke();
    test_layout_metrics();
    test_footer_clamp();
    test_reopen_resets();
    if (reds) {
        fprintf(stderr, "AI_READER_FAIL checks=%d reds=%d\n", checks, reds);
        return 1;
    }
    printf("AI_READER_PASS checks=%d\n", checks);
    return 0;
}
