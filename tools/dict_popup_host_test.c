/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：gen26.3 ④/⑤ 词卡查询态与 AI 释义容量宿主回环。覆盖：
 *   1. show_pending 暂存「AI 查询中…」真实状态卡（词头 + 状态行 + 保存 chip 初值）；
 *   2. is_pending 按词比对——本词真、别词假（app_book 双门门 B 的判定核心）；
 *      超长词按截断词头匹配（0x07 带回全词也要能原地更新）；
 *   3. show_def 原地更新：查询态标志清除、释义行落定、状态行消失；
 *   4. pending_expired 15s 超时判定（非查询态恒假、时钟推进后转真、释义落定后转假）；
 *   5. ai_capacity_chars 按布局实算：竖屏 10 行 x 20 全角字 = 200、横屏 400——
 *      40 字预算的校准锚；
 *   6. 折行边界：60 个全角字 3 行不截断，300 个全角字 10 行截断 + 省略号；
 *   7. 查询态 chip 点击忽略（NOT_READY），释义态 chip 保存/移除照旧。
 * English: gen26.3 items 4/5 host round trip for the querying card and the AI
 * definition capacity. See the Chinese header for the case list.
 *
 * 桩口径：esp_heap_caps 直通 libc；esp_timer 为可控时钟；ui_kit 度量近似
 * （全角字形按字号整宽、ASCII 按半宽——与容量推导同口径，容量由全角字宽决定）；
 * dict_lookup / vocab_log 为可配置替身。真头文件（dict_popup.h / dict_lookup.h /
 * vocab_log.h）直接参与编译，API 契约即产品契约。
 * Shim contract: esp_heap_caps goes straight to libc; esp_timer is a
 * controllable clock; ui_kit metrics approximate (full-width glyphs take the
 * full font size, ASCII half - the same yardstick the capacity derivation uses);
 * dict_lookup / vocab_log are configurable doubles. The real headers
 * (dict_popup.h / dict_lookup.h / vocab_log.h) compile in, so the API contract
 * under test is the product's own.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dict_popup.h"
#include "dict_lookup.h"
#include "vocab_log.h"

/* ---- 可控时钟与旋转 / controllable clock and rotation ---- */
int64_t dict_test_now_us = 1000000;
static enum EpdRotation g_rotation = EPD_ROT_PORTRAIT;

int64_t esp_timer_get_time(void) { return dict_test_now_us; }
enum EpdRotation epd_get_rotation(void) { return g_rotation; }

/* ---- ui_kit 替身 / ui_kit doubles ---- */
#define MAX_TEXT_CALLS 48
static struct { int px; char text[192]; } g_texts[MAX_TEXT_CALLS];
static int g_text_count;

void ui_text_fixed(uint8_t* framebuffer, int x, int y_top, int px, const char* text,
                   enum EpdFontFlags align, bool inverted) {
    (void)framebuffer; (void)x; (void)y_top; (void)align; (void)inverted;
    if (g_text_count < MAX_TEXT_CALLS && text) {
        g_texts[g_text_count].px = px;
        snprintf(g_texts[g_text_count].text, sizeof(g_texts[g_text_count].text), "%s", text);
        ++g_text_count;
    }
}

// 与 dict_popup.c 的 utf8_step 同一跨步；全角按字号整宽、ASCII 按半宽。
// Same stepping as dict_popup.c's utf8_step; full-width takes the font size, ASCII half.
int ui_text_fixed_width_px(int px, const char* text) {
    int w = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; ) {
        int n = 1;
        if (*p >= 0x80) {
            if (*p >= 0xf0) n = 4;
            else if (*p >= 0xe0) n = 3;
            else if (*p >= 0xc2) n = 2;
            w += px;
        } else {
            w += (px + 1) / 2;
        }
        p += n;
    }
    return w;
}

void ui_fill_round_rect(uint8_t* framebuffer, EpdRect rect, int radius, uint8_t color) {
    (void)framebuffer; (void)rect; (void)radius; (void)color;
}
void ui_draw_round_rect(uint8_t* framebuffer, EpdRect rect, int radius, uint8_t color) {
    (void)framebuffer; (void)rect; (void)radius; (void)color;
}
void ui_draw_choice_round_rect(uint8_t* framebuffer, EpdRect rect, int radius, bool on) {
    (void)framebuffer; (void)rect; (void)radius; (void)on;
}
bool ui_rect_hit(EpdRect rect, uint16_t x, uint16_t y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

/* ---- dict_lookup 替身 / dict_lookup doubles ---- */
static bool g_dict_ready;
static bool g_dict_hit;
static char g_dict_def[DICT_ENTRY_MAX]; // 长释义走真实容量 / long entries at the real capacity

bool dict_ready(void) { return g_dict_ready; }
bool dict_open(void) { return g_dict_ready; }
bool dict_lookup_word(const char* word, char* out, size_t cap) {
    if (!g_dict_hit || !word || !*word) return false;
    snprintf(out, cap, "%s", g_dict_def);
    return true;
}

/* ---- vocab_log 替身 / vocab_log doubles ---- */
#define VOCAB_SLOTS 8
static char g_vocab[VOCAB_SLOTS][VOCAB_WORD_MAX];
static int g_vocab_count;
static char g_saved_def[288];
static int g_save_calls, g_remove_calls;

bool vocab_log_contains(const char* word) {
    for (int i = 0; i < g_vocab_count; ++i) {
        if (g_vocab[i][0] && strcmp(g_vocab[i], word) == 0) return true;
    }
    return false;
}
bool vocab_log_save(const char* word, const char* definition) {
    ++g_save_calls;
    if (definition) snprintf(g_saved_def, sizeof(g_saved_def), "%s", definition);
    if (!word || !*word) return false;
    for (int i = 0; i < g_vocab_count; ++i) {
        if (g_vocab[i][0] && strcmp(g_vocab[i], word) == 0) return true; // 已在即写回 / present: rewrite
    }
    if (g_vocab_count < VOCAB_SLOTS) {
        snprintf(g_vocab[g_vocab_count], VOCAB_WORD_MAX, "%s", word);
        ++g_vocab_count;
        return true;
    }
    return false;
}
bool vocab_log_remove(const char* word) {
    ++g_remove_calls;
    for (int i = 0; i < g_vocab_count; ++i) {
        if (g_vocab[i][0] && strcmp(g_vocab[i], word) == 0) {
            for (int j = i; j + 1 < g_vocab_count; ++j) memcpy(g_vocab[j], g_vocab[j + 1], VOCAB_WORD_MAX);
            --g_vocab_count;
            g_vocab[g_vocab_count][0] = 0;
            return true;
        }
    }
    return false;
}

/* ---- 断言与观察助手 / assertions and observation helpers ---- */
static int g_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void reset_observers(void) {
    g_text_count = 0;
    g_save_calls = g_remove_calls = 0;
    g_saved_def[0] = 0;
}
static bool drew_text(const char* text) {
    for (int i = 0; i < g_text_count; ++i) {
        if (strcmp(g_texts[i].text, text) == 0) return true;
    }
    return false;
}
// 正文行 = 27px 字号的绘制（省略号是 21px，词头 46px，chip 标签 26px）。
// Body lines are the 27px draws (the ellipsis is 21px, the head 46px, the chip label 26px).
static int body_lines_drawn(void) {
    int n = 0;
    for (int i = 0; i < g_text_count; ++i) {
        if (g_texts[i].px == 27) ++n;
    }
    return n;
}
static void draw_card(void) {
    reset_observers();
    uint8_t fb = 0;
    dict_popup_draw(&fb);
}

// chip 矩形中心（竖屏卡 {24,540,636,528} → chip {504,562,120,52}）。
// The chip rect's centre (portrait card {24,540,636,528} -> chip {504,562,120,52}).
static void tap_chip(void) {
    EpdRect chip = dict_popup_save_rect();
    (void)dict_popup_tap((uint16_t)(chip.x + chip.width / 2), (uint16_t)(chip.y + chip.height / 2));
}

static void test_open_miss_contract(void) {
    // 词典整体不可用：open 失败且不留任何卡面。/ Dictionary entirely unavailable:
    // open fails and stages nothing.
    g_dict_ready = false;
    CHECK(!dict_popup_open("serendipity"));
    CHECK(!dict_popup_is_pending(NULL));
    draw_card();
    CHECK(g_text_count == 0);
    // 词典就绪但未收录：同样失败，卡面干净。/ Dictionary ready but no entry: same
    // failure, clean card.
    g_dict_ready = true;
    g_dict_hit = false;
    CHECK(!dict_popup_open("serendipity"));
    CHECK(!dict_popup_is_pending(NULL));
    draw_card();
    CHECK(g_text_count == 0);
    // 命中路径照旧（回归）：开卡 + chip 初值取自生词本。/ The hit path still works
    // (regression): the card opens and the chip's initial value comes from the book.
    g_dict_hit = true;
    snprintf(g_dict_def, sizeof(g_dict_def), "偶然发现珍宝的运气");
    CHECK(dict_popup_open("serendipity"));
    CHECK(!dict_popup_is_pending(NULL));
    draw_card();
    CHECK(drew_text("serendipity"));
    CHECK(body_lines_drawn() == 1);
}

static void test_pending_card(void) {
    dict_popup_reset();
    CHECK(dict_popup_show_pending("serendipity"));
    // 双门判定的核心：本词真、别词假、NULL 只问状态。/ The core of the double gate:
    // this word true, another word false, NULL asks only about the state.
    CHECK(dict_popup_is_pending("serendipity"));
    CHECK(!dict_popup_is_pending("other"));
    CHECK(!dict_popup_is_pending("serendipit"));
    CHECK(dict_popup_is_pending(NULL));
    // 卡面：词头 + 保存 chip + 一行真实状态；没有省略号。/ Card face: head, save
    // chip and one real status line; no ellipsis.
    draw_card();
    CHECK(drew_text("serendipity"));
    CHECK(drew_text("保存"));
    CHECK(drew_text("AI 查询中…"));
    CHECK(body_lines_drawn() == 1);
    CHECK(!drew_text("……"));
    // 查询态 chip 点击被忽略：不落盘、卡片保持。/ A chip tap on the querying card
    // is ignored: nothing lands and the card stays.
    tap_chip();
    CHECK(g_save_calls == 0);
    CHECK(dict_popup_is_pending("serendipity"));
    // chip 之外的点击照旧关闭（MISSED 语义不变）。/ Taps off the chip still close
    // the card (MISSED semantics unchanged).
    CHECK(dict_popup_tap(100, 700) == DICT_POPUP_TAP_MISSED);
    dict_popup_reset();
    CHECK(!dict_popup_is_pending(NULL));
}

static void test_in_place_update(void) {
    CHECK(dict_popup_show_pending("serendipity"));
    // 0x07 到达：原地更新为释义——查询态标志清除，状态行消失，释义行落定。
    // 0x07 lands: the in-place update to the definition - the query state clears,
    // the status line is gone and the definition lines land.
    CHECK(dict_popup_show_def("serendipity", "a pleasant surprise\n第二次出现"));
    CHECK(!dict_popup_is_pending("serendipity"));
    CHECK(!dict_popup_is_pending(NULL));
    draw_card();
    CHECK(!drew_text("AI 查询中…"));
    CHECK(drew_text("a pleasant surprise"));
    CHECK(drew_text("第二次出现"));
    CHECK(body_lines_drawn() == 2);
    // 释义态 chip 照旧工作：保存写入的是释义摘要，不是状态行。/ The chip works as
    // before on a definition card: the save stores the definition summary, not the
    // status line.
    tap_chip();
    CHECK(g_save_calls == 1);
    CHECK(strcmp(g_saved_def, "a pleasant surprise / 第二次出现") == 0);
    CHECK(dict_popup_is_pending(NULL) == false);
    // 已存：chip 标签翻转，再点移除。/ Saved: the chip label flips; another tap removes.
    draw_card();
    CHECK(drew_text("已存"));
    tap_chip();
    CHECK(g_remove_calls == 1);
    draw_card();
    CHECK(drew_text("保存"));
}

static void test_word_match_edges(void) {
    // 超长词：卡面存的是 47 字截断头，0x07 带回全词（≤63）也要能原地更新——
    // 谓词按截断头前缀匹配。/ Overlong word: the card holds the 47-char truncated
    // head and the 0x07 full word (<=63) must still update it - the predicate
    // matches on the truncated head's prefix.
    char long_word[DICT_WORD_MAX];
    memset(long_word, 'x', sizeof(long_word) - 1);
    long_word[sizeof(long_word) - 1] = 0;
    CHECK(dict_popup_show_pending(long_word));
    CHECK(dict_popup_is_pending(long_word));
    // 第 47 字就不同的词不匹配（前缀比较，不是 contains）。/ A word that already
    // differs at char 47 does not match (prefix compare, not contains).
    char other[DICT_WORD_MAX];
    memset(other, 'x', 46);
    other[46] = 'y';
    other[47] = 0;
    CHECK(!dict_popup_is_pending(other));
    // 更短的词不匹配（NUL 与非 NUL 相接即不等）。/ A shorter word does not match
    // (NUL against a non-NUL byte is a difference).
    CHECK(!dict_popup_is_pending("xy"));
    dict_popup_reset();
}

static void test_capacity(void) {
    // 竖屏：10 行 x 20 全角字 = 200——40 字预算的校准锚。/ Portrait: 10 lines x 20
    // full-width chars = 200 - the yardstick for the 40-char budget.
    g_rotation = EPD_ROT_PORTRAIT;
    CHECK(dict_popup_ai_capacity_chars() == 200);
    // 横屏：同公式 10 行 x 40 字 = 400；预算校准取竖屏的保守值。/ Landscape: the
    // same formula gives 10 x 40 = 400; calibration keeps the conservative portrait
    // value.
    g_rotation = EPD_ROT_LANDSCAPE;
    CHECK(dict_popup_ai_capacity_chars() == 400);
    g_rotation = EPD_ROT_PORTRAIT;
    CHECK(dict_popup_ai_capacity_chars() == 200);
}

static void test_wrap_bounds(void) {
    // AI 路径（show_def）：手机协议帽 PUSH_AI_DEF_MAX=170，单条释义必然放得下——
    // 60 个全角字 3 行不截断即「40 字预算完整显示」的实证富余量。
    // AI path (show_def): the phone protocol cap PUSH_AI_DEF_MAX=170 means a def
    // always fits - 60 full-width chars wrapping to 3 lines with no truncation is
    // the empirical slack behind "the 40-char budget shows in full".
    static const char unit[] = "\xE4\xB8\xAD"; // 中 / a full-width glyph
    char full[3 * 300 + 1];
    size_t o = 0;
    for (int i = 0; i < 60; ++i) { memcpy(full + o, unit, 3); o += 3; }
    full[o] = 0;
    CHECK(dict_popup_show_def("capacity", full));
    draw_card();
    CHECK(body_lines_drawn() == 3);
    CHECK(!drew_text("……"));
    // 钳位防御（协议不可达，ASan 曾在此实证越界）：单个超长段被钳到字符边界
    // （189 字节 = 63 字），折行器不得踩过 NUL；落定 4 行、卡片照常可用。
    // Clamp defence (unreachable via the protocol; ASan once proved an overflow
    // here): an overlong single segment clamps to a character boundary (189 bytes
    // = 63 chars) and the wrapper must not step over the NUL; 4 lines land and the
    // card stays usable.
    o = 0;
    for (int i = 0; i < 300; ++i) { memcpy(full + o, unit, 3); o += 3; }
    full[o] = 0;
    CHECK(dict_popup_show_def("capacity", full));
    draw_card();
    CHECK(body_lines_drawn() == 4);
    CHECK(dict_popup_is_pending(NULL) == false);
    dict_popup_reset();
    // 本地词典路径（dict_popup_open）：DICT_ENTRY_MAX=1280 的长释义才是 10 行
    // 显示帽 + 省略号的真实到达路径——300 字 → 暂存 14 行封顶，显示 10 行 + 「……」。
    // Local dictionary path (dict_popup_open): a long DICT_ENTRY_MAX=1280 entry is
    // the real route to the 10-line display cap plus the ellipsis - 300 chars ->
    // staging caps at 14 lines, the display shows 10 plus the ellipsis.
    g_dict_ready = true;
    g_dict_hit = true;
    o = 0;
    for (int i = 0; i < 300; ++i) { memcpy(g_dict_def + o, unit, 3); o += 3; }
    g_dict_def[o] = 0;
    CHECK(dict_popup_open("longdef"));
    draw_card();
    CHECK(body_lines_drawn() == 10);
    CHECK(drew_text("……"));
    dict_popup_reset();
}

static void test_pending_timeout(void) {
    CHECK(!dict_popup_pending_expired(15000000)); // 非查询态恒假 / never outside the query state
    dict_test_now_us = 1000000;
    CHECK(dict_popup_show_pending("gamma"));
    CHECK(!dict_popup_pending_expired(15000000));
    dict_test_now_us += 14999999;
    CHECK(!dict_popup_pending_expired(15000000));
    dict_test_now_us += 1;
    CHECK(dict_popup_pending_expired(15000000)); // 15s 整点转真 / fires at exactly 15s
    // 释义落定后超时不再触发（查询态已清）。/ Once the definition lands the timeout
    // never fires again (the query state is cleared).
    CHECK(dict_popup_show_def("gamma", "希腊字母"));
    dict_test_now_us += 60000000;
    CHECK(!dict_popup_pending_expired(15000000));
    dict_popup_reset();
}

static void test_reset_clears_pending(void) {
    CHECK(dict_popup_show_pending("delta"));
    dict_popup_reset();
    CHECK(!dict_popup_is_pending(NULL));
    CHECK(!dict_popup_pending_expired(1));
    draw_card();
    CHECK(g_text_count == 0);
}

static void test_ai_entry(void) {
    // gen26.4：词典命中卡上的「AI」入口。/ gen26.4: the "AI" entry on a dict-hit card.
    g_dict_ready = true;
    g_dict_hit = true;
    snprintf(g_dict_def, sizeof(g_dict_def), "偶然发现珍宝的运气");
    CHECK(dict_popup_open("serendipity"));
    // 真机路径复现（gen26.4 真机 defect 回归断言）：open 成功后 word() 必须保持该词
    // ——app_book 的 TAP_AI 分支靠 dict_popup_word() 取词传给 force_ai，历史上这里
    // 返回空导致 0x17 发空词、AI 释义全丢。/ Real-path repro (gen26.4 device
    // defect regression): after a successful open, word() must keep the word -
    // app_book's TAP_AI branch takes the word from dict_popup_word() for
    // force_ai, and an empty return here once burned 0x17 with an empty word
    // and lost every AI definition.
    CHECK(strcmp(dict_popup_word(), "serendipity") == 0);
    // 默认（AI 开关未置）：无 AI chip，AI 区点击不吞（MISSED）。/ Default (AI switch
    // unset): no AI chip; a tap on the AI zone does not swallow (MISSED).
    EpdRect ai = dict_popup_ai_rect();
    EpdRect save = dict_popup_save_rect();
    draw_card();
    CHECK(!drew_text("AI"));
    CHECK(dict_popup_tap((uint16_t)(ai.x + ai.width / 2), (uint16_t)(ai.y + ai.height / 2))
          == DICT_POPUP_TAP_MISSED);
    // AI chip 与保存 chip 同在头部一行且不重叠（保存 chip 不被 AI chip 抢占）。
    // The AI and save chips share the header row and do not overlap (the save chip
    // is not shadowed by the AI chip).
    CHECK(ai.y == save.y && ai.height == save.height);
    CHECK(ai.x + ai.width <= save.x);
    // 开 AI：chip 显示 + 命中返回 TAP_AI。/ AI on: the chip draws and a hit returns TAP_AI.
    dict_popup_set_ai_enabled(true);
    draw_card();
    CHECK(drew_text("AI"));
    CHECK(dict_popup_tap((uint16_t)(ai.x + ai.width / 2), (uint16_t)(ai.y + ai.height / 2))
          == DICT_POPUP_TAP_AI);
    // 真机 defect 回归（gen26.4.1）：真机 TAP_AI 分支传的是 dict_popup_word()——
    // 返回值就是 &s_word 自身。旧 show_pending 入口先 reset 再查空，别名参数被
    // 一起清空 → 「!*word」误判、查询态没建、词典卡被清空（0x07 回来门 B 失配
    // 被门 A 丢弃 = 释义全丢）。别名路径必须成功且查询态为真。
    // Device-defect regression (gen26.4.1): the real TAP_AI branch passes
    // dict_popup_word() - the return value IS &s_word. The old show_pending
    // reset first and checked emptiness after, clearing the aliased argument
    // too, "!*word" misread and bailed - no query state, a wiped dict card
    // (when 0x07 landed gate B missed so gate A dropped it = defs all lost).
    // The aliased call must succeed and enter the query state.
    CHECK(dict_popup_force_ai(dict_popup_word()));
    CHECK(dict_popup_is_pending("serendipity"));
    CHECK(dict_popup_ai_over_dict());
    // 释义落定重开词典卡，再以别名路径验证 open 的入口 reset 同样不吃掉别名词。
    // Once the def lands reopen the dict card, then verify open's entry reset
    // also leaves an aliased word intact.
    CHECK(dict_popup_show_def("serendipity", "偶然发现珍宝的运气"));
    CHECK(dict_popup_open(dict_popup_word()));
    CHECK(strcmp(dict_popup_word(), "serendipity") == 0);
    // force_ai：切查询态 + 标记 over_dict（超时回退词典）。/ force_ai: enter the query
    // state + mark over_dict (fall back to the dict on timeout).
    CHECK(dict_popup_force_ai("serendipity"));
    CHECK(dict_popup_is_pending("serendipity"));
    CHECK(dict_popup_ai_over_dict());
    // 查询态卡不显示 AI chip。/ The querying card shows no AI chip.
    draw_card();
    CHECK(!drew_text("AI"));
    CHECK(drew_text("AI 查询中…"));
    // miss 路径的查询态不带 over_dict（超时应走失败提示而非回退词典）。/ The miss-path
    // query state carries no over_dict (a timeout should fail, not fall back to the dict).
    CHECK(dict_popup_show_pending("otherword"));
    CHECK(dict_popup_is_pending("otherword"));
    CHECK(!dict_popup_ai_over_dict());
    // 释义落定后 over_dict 清零（查询态清）。/ Once the def lands, over_dict clears.
    CHECK(dict_popup_show_def("otherword", "别的释义"));
    CHECK(!dict_popup_ai_over_dict());
    // word getter 回当前词头；reset 后清空。/ The word getter returns the current head;
    // reset clears it.
    CHECK(strcmp(dict_popup_word(), "otherword") == 0);
    dict_popup_reset();
    CHECK(dict_popup_word()[0] == 0);
    dict_popup_set_ai_enabled(false); // 复位开关，避免影响后续用例 / reset the switch for later cases
}

int main(void) {
    test_open_miss_contract();
    test_pending_card();
    test_in_place_update();
    test_word_match_edges();
    test_capacity();
    test_wrap_bounds();
    test_pending_timeout();
    test_reset_clears_pending();
    test_ai_entry();
    if (g_failures) {
        printf("dict_popup: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    puts("dict_popup: PASS (querying card + double-gate predicate + capacity 200/400 + wrap bounds + timeout + AI entry)");
    return 0;
}
