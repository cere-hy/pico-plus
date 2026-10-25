/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：点词查译的底部释义卡片；内容与命中都封在本模块，由 app_book 调用。
 * English: Bottom definition card for tap-to-lookup; state stays in this module.
 *
 * 冻结：draw 为纯绘制；释义行缓冲在 PSRAM；不含手势状态。
 * Frozen: draw paints only; definition lines live in PSRAM; no gesture state.
 */
#include "dict_popup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dict_lookup.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "push_proto.h" // 释义长度上限链：段缓冲由协议槽派生 / length chain: seg derives from the protocol slot
#include "ui_kit.h"
#include "vocab_log.h"

static const char* TAG = "dict";

// 暂存行数 = 卡面可显示的正文行数。**gen26.6 36px 起 14→9**：显示循环在 9 行触底，
// 若暂存仍容 14 行，10~14 行的释义会被画成 9 行 + 省略号却**不置截断位**——不开
// 详解、尾部静默丢失。让暂存帽与显示帽对齐，store_line 一溢出即 truncated，
// app_book 的「触顶进详解」门才会真正兜住长释义。
// Staging lines = the body lines the card can actually show. **14->9 at 36px in
// gen26.6**: the draw loop bottoms out at 9 lines, so a 14-line staging cap let
// a 10-14 line definition paint 9 lines plus an ellipsis WITHOUT setting the
// truncation flag - no detail view, tail silently lost. Aligning the staging cap
// to the display cap makes store_line's overflow set truncated immediately, so
// app_book's "overflow -> detail view" gate actually holds long definitions.
#define DICT_POPUP_LINES 9
#define DICT_POPUP_PAD 36
#define DICT_POPUP_WORD_PX 46
// gen26.6 词卡可读性：释义正文 27→36px（约 1.33×；裁定所指「24→36、1.5×」的
// 24 是旧固件目测值，源码自 gen10 起即为 27）。词头 46、保存/AI chip 26px 不动。
// 行高随字号重算：42→46（行距 10px，约为字号的 28%，此前 27px/42 是 55% 的松排）。
// gen26.6 card legibility: body text 27->36px (~1.33x; the ruling's "24->36,
// 1.5x" took 24 from an older build - the source has been 27 since gen10). The
// head (46) and the save/AI chips (26) stay. Line height recomputed: 42->46
// (10px leading, ~28% of the font; the old 27/42 was a loose 55%).
#define DICT_POPUP_TEXT_PX 36
#define DICT_POPUP_LINE_H 46
#define DICT_POPUP_WORD_MAX 48
// 保存 chip 放在卡片头部右侧：不占正文行，释义仍是可显示的 9 行。
// The save chip sits in the card header on the right: it costs no body line, so the
// definition still shows its usual nine lines.
#define DICT_POPUP_SAVE_W 120
#define DICT_POPUP_SAVE_H 52
#define DICT_POPUP_SAVE_PX 26
#define DICT_POPUP_SUMMARY_LINES 3
// gen26 AI 释义段缓冲：**由协议槽派生**（PUSH_AI_DEF_MAX + 1）。段间按 \n 切分后
// 任何单段都短于协议槽，故本缓冲必容得下整段——不是「凑一个够大的数」，而是
// 长度上限链的一环（SEG > DEF_MAX）。gen26.6 D3 把 DEF_MAX 从 170 放宽到 768 时，
// 旧的硬编码 192 没有跟着动，导致 >191B 的单段被静默截断；改成派生后这类漂移
// 不再可能发生。
// 缓冲放 PSRAM（show_def 内分配），不占 8KB 主栈。
// gen26 AI def segment buffer: **derived from the protocol slot** (PUSH_AI_DEF_MAX
// + 1). Any \n-separated segment is shorter than the protocol slot, so this buffer
// always holds a whole segment - not "a number big enough", but one link of the
// length chain (SEG > DEF_MAX). When gen26.6 D3 widened DEF_MAX from 170 to 768,
// the hard-coded 192 did not follow, silently truncating segments over 191B;
// deriving it makes that drift impossible.
// The buffer lives in PSRAM (allocated inside show_def), off the 8KB main stack.
#define DICT_POPUP_AI_SEG (PUSH_AI_DEF_MAX + 1u)
_Static_assert(DICT_POPUP_AI_SEG > PUSH_AI_DEF_MAX,
               "gen26.6 length chain: the segment buffer must hold any protocol-sized definition");
// gen26.3 ④ 查询态文案：一词一行的真实状态提示，不是占位假卡。/ gen26.3
// query-state text: a one-line real status, not a placeholder fake card.
#define DICT_POPUP_PENDING_TEXT "AI 查询中…"
// gen26.4「AI」入口 chip：放在保存 chip 左侧（同在卡片头部右侧），只在词典释义卡
// 上可点。/ gen26.4 "AI" entry chip: left of the save chip, both in the card
// header; tappable only on a dictionary definition card.
#define DICT_POPUP_AI_W 96
#define DICT_POPUP_AI_H 52
#define DICT_POPUP_AI_PX 26
#define DICT_POPUP_AI_LABEL "AI"

static char s_word[DICT_POPUP_WORD_MAX];
static char* s_lines[DICT_POPUP_LINES];
static int s_line_count;
static bool s_truncated;
static bool s_saved; ///< 词是否已在生词本 / whether the word is in the vocabulary book
static int s_anchor_col;  ///< 点词所在侧：0=左半，1=右半 / tapped side: 0=left half, 1=right half
static bool s_anchor_split; ///< 分半几何开关：双栏生效，单栏横屏维持铺满 / split geometry: two-column only
// gen26.3 ④ 查询态：卡面停在「AI 查询中…」等 0x07 的状态。s_pending 是双门判定的
// 唯一依据（app_book 据此区分「本词的查询卡」与「用户手开的词典卡」），reset 与
// show_def 都会清掉它。/ gen26.3 query state: the card waits on "AI querying..."
// for 0x07. s_pending is the sole basis of the double gate (app_book tells this
// word's querying card from a user-opened dict card by it); reset and show_def
// both clear it.
static bool s_pending;
static int64_t s_pending_since_us; ///< 进入查询态的时刻（esp_timer 微秒）/ when the query state began
// gen26.4：AI 入口可点性（app_book 按 push_sink_ai_enabled 设置）与「查询态由词典
// 命中卡强制触发」标记（超时回退词典释义）。/ gen26.4: the AI entry's tappability
// (set by app_book from push_sink_ai_enabled) and the "querying forced from a
// dict-hit card" mark (fall back to the dict definition on timeout).
static bool s_ai_on;
static bool s_ai_over_dict;

// 避让锚点：按点词 x 选弹出侧（tap 分界与 gutter 中心重合，单栏横屏同一谓词成立）。
// Avoidance anchor: pick the popup side by the tap x (the tap boundary coincides with
// the gutter center; the same predicate holds in single-column landscape).
void dict_popup_set_anchor(int col, bool split) {
    s_anchor_col = col > 0 ? 1 : 0;
    s_anchor_split = split;
}

EpdRect dict_popup_rect(void) {
    // 竖屏：卡片放在页脚上方，正文上半仍可见(与原布局逐像素一致)。
    // 横屏：贴屏幕下沿、宽度铺满、高度按较短边钳制，避免 528 高的竖屏卡片溢出 684 的横屏。
    // Portrait: card above the footer, body stays visible above (byte-identical to the old layout).
    // Landscape: bottom-anchored, full width, height clamped to the short axis so the 528-tall
    // portrait card cannot overflow the 684-tall landscape frame.
    if (epd_get_rotation() == EPD_ROT_LANDSCAPE) {
        int h = ui_h() - 120;         // 顶部留 ~120px 正文可见 / keep ~120px of body visible on top
        if (h > 528) h = 528;
        if (h < 240) h = 240;
        if (s_anchor_split) {
            // 双栏避让：卡片占点词对侧半宽——点左栏词的卡片在右半（右缘对齐联合区右缘），
            // 点右栏词的卡片在左半，被点的词所在栏全程可见（v2 §5.2）。
            // Two-column avoidance: the card takes the half opposite the tapped word — a
            // left-side tap puts the card on the right (right edge on the union area's
            // right edge), a right-side tap on the left, so the tapped column stays visible.
            // 第 4 步接真栏矩形时按 reader_column_width()/reader_column_offset() 精化分界。
            // Refine the boundary against the real column rects (reader_column_width()/
            // reader_column_offset()) at step 4.
            int half = (ui_w() - 48) / 2;
            int x = s_anchor_col == 0 ? ui_w() - 24 - half : 24;
            return (EpdRect){x, ui_h() - 24 - h, half, h};
        }
        return (EpdRect){24, ui_h() - 24 - h, ui_w() - 48, h};
    }
    return (EpdRect){24, 540, UI_LOCK_WIDTH - 48, 528};
}

// 正文可用宽度：卡宽减去左右两侧 PAD（DICT_POPUP_PAD 两边对称）。**右侧必须同
// 样扣**——只扣左侧会让折行宽度多出 36px，末字贴/越卡右缘（gen26.6 真机
// 「die」贴边被裁的几何根因）。所有正文折行与容量实算共用此单一真源。
// Body text width: the card width minus the PAD on BOTH sides. The right PAD must
// be subtracted too - reserving only the left adds 36px to the wrap width and
// pushes the last glyph onto/over the right edge (the geometry behind the gen26.6
// on-device "die" clipping). Every body wrap and the capacity math share this.
static int body_text_width(void) {
    return dict_popup_rect().width - 2 * DICT_POPUP_PAD;
}

EpdRect dict_popup_save_rect(void) {
    EpdRect card = dict_popup_rect();
    return (EpdRect){card.x + card.width - DICT_POPUP_PAD - DICT_POPUP_SAVE_W, card.y + 22,
                     DICT_POPUP_SAVE_W, DICT_POPUP_SAVE_H};
}

// gen26.4：「AI」入口 chip 贴保存 chip 左侧、同高同在头部一行。/ gen26.4: the "AI"
// entry chip hugs the save chip's left edge, same height, same header row.
EpdRect dict_popup_ai_rect(void) {
    EpdRect card = dict_popup_rect();
    EpdRect save = dict_popup_save_rect();
    return (EpdRect){save.x - 12 - DICT_POPUP_AI_W, card.y + 22,
                     DICT_POPUP_AI_W, DICT_POPUP_AI_H};
}

void dict_popup_set_ai_enabled(bool on) { s_ai_on = on; }

const char* dict_popup_word(void) { return s_word; }

bool dict_popup_ai_over_dict(void) { return s_pending && s_ai_over_dict; }

void dict_popup_reset(void) {
    for (int i = 0; i < s_line_count; ++i) {
        free(s_lines[i]);
        s_lines[i] = NULL;
    }
    s_line_count = 0;
    s_truncated = false;
    s_saved = false;
    s_pending = false;
    s_pending_since_us = 0;
    s_ai_over_dict = false;
    s_ai_on = false;
    s_word[0] = 0;
}

// 释义摘要：给生词本当背面用，取前几行拼起来，够读就行。
// Definition summary used as the back of a flashcard: the first few lines joined.
static void summary_text(char* out, size_t cap) {
    out[0] = 0;
    size_t o = 0;
    for (int i = 0; i < s_line_count && i < DICT_POPUP_SUMMARY_LINES; ++i) {
        if (o && o + 4 < cap) { memcpy(out + o, " / ", 3); o += 3; out[o] = 0; }
        size_t n = strlen(s_lines[i]);
        if (o + n + 1 > cap) n = cap - o - 1;
        if (!n) break;
        memcpy(out + o, s_lines[i], n);
        o += n;
        out[o] = 0;
    }
}

dict_popup_tap_t dict_popup_tap(uint16_t x, uint16_t y) {
    if (!s_word[0]) return DICT_POPUP_TAP_MISSED;
    // gen26.4：词典释义卡上的「AI」入口——优先于保存 chip 判定。查询态卡不显示 AI
    // chip（s_pending 时不命中），故只在「AI 开着且非查询态」时抢占。
    // gen26.4: the "AI" entry on a dict definition card - judged before the save
    // chip. The querying card shows no AI chip (never hits while s_pending), so it
    // only takes the tap when AI is on and the card is not querying.
    if (s_ai_on && !s_pending && ui_rect_hit(dict_popup_ai_rect(), x, y)) {
        return DICT_POPUP_TAP_AI;
    }
    if (!ui_rect_hit(dict_popup_save_rect(), x, y)) return DICT_POPUP_TAP_MISSED;
    // gen26.3 ④ 查询态卡的 chip 尚未就绪：释义没到，此刻存进去的摘要是状态行而
    // 不是释义——忽略 chip 点击、卡片保持打开，等 0x07 落地后再存。chip 之外的
    // 点击照旧走 MISSED 关闭卡片（与词典卡同口径，查询窗口不吞页面点击）。
    // gen26.3: the querying card's chip is not ready yet - the def has not landed
    // and the summary would be the status line, not a definition. Ignore the chip
    // tap and keep the card open until 0x07 lands. Taps off the chip still close
    // the card as usual (same stance as the dict card; the query window must not
    // swallow page taps).
    if (s_pending) return DICT_POPUP_TAP_NOT_READY;
    char summary[288];
    summary_text(summary, sizeof(summary));
    // 移除失败就保持「已存」，保存失败就保持「保存」：chip 永远反映真实落盘结果。
    // A failed removal keeps "saved" and a failed save keeps "unsaved": the chip always
    // reflects what actually landed on the card.
    bool removing = s_saved;
    s_saved = removing ? !vocab_log_remove(s_word) : vocab_log_save(s_word, summary);
    ESP_LOGI(TAG, "vocab %s \"%s\" -> saved=%d", s_saved ? "save" : "unsave", s_word, (int)s_saved);
    return removing && !s_saved ? DICT_POPUP_TAP_REMOVED : DICT_POPUP_TAP_KEPT;
}

static int utf8_step(const char* p) {
    unsigned char c = (unsigned char)*p;
    if (!c) return 0;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 1; // 非法字节按单字节跳过 / skip invalid bytes one by one
}

static bool store_line(const char* text, size_t len) {
    if (s_line_count >= DICT_POPUP_LINES) {
        s_truncated = true;
        return false;
    }
    char* copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return false;
    memcpy(copy, text, len);
    copy[len] = 0;
    s_lines[s_line_count++] = copy;
    return true;
}

// 逐字推进折行，但**每步按「整行候选」量宽**——与 ui_text_fixed 绘制时同一
// 字符串、同一字体路径判定。逐字相加会在混合行上出错：只要有一个字形不在内置
// 字体，绘制会整行切到系统 TTF，而逐字量宽仍按各自原字体累加，两套 advance
// 混算的误差沿行累积，把末字挤出卡右缘（gen26.6「die」贴边）。
// 候选行缓冲 128 字节：行长受 max_w 约束（36px 全角约 15 字 = 45 字节）。
// Advance glyph by glyph, but measure each candidate LINE as a whole - the same
// string and the same font-path decision ui_text_fixed draws with. Per-glyph
// summing breaks on mixed lines: one glyph missing from the built-in font flips
// the whole draw to the system TTF while the per-glyph widths still mix both
// fonts' advances, and the error accumulates along the line, pushing the last
// glyph past the card's right edge (the gen26.6 "die" clipping).
// The 128-byte candidate buffer is enough: a line is bounded by max_w (about 15
// full-width glyphs = 45 bytes at 36px).
static void wrap_text(const char* text, int px, int max_w) {
    const char* line = text;
    const char* p = text;
    char probe[128];
    while (*p) {
        int n = utf8_step(p);
        if (n > 4) n = 4;
        const size_t len = (size_t)(p - line) + (size_t)n;
        if (len >= sizeof(probe)) {
            // 不可达兜底（行长 ≤ max_w）：切一行保进度，绝不空转。
            // Unreachable backstop (a line is <= max_w): cut a line to make progress.
            if (!store_line(line, (size_t)(p - line))) { s_truncated = true; return; }
            line = p;
            continue;
        }
        memcpy(probe, line, len);
        probe[len] = 0;
        const int w = ui_text_fixed_width_px(px, probe);
        if (w > max_w && p > line) {
            // 分配失败也要标记截断，否则卡片少了一段却不显示省略号。
            // Mark truncation on allocation failure too, or the card silently loses the tail.
            if (!store_line(line, p - line)) { s_truncated = true; return; }
            line = p;
            continue;
        }
        p += n;
    }
    if (*line) store_line(line, strlen(line));
}

bool dict_popup_open(const char* word) {
    // 计时：真机上想知道「打开 / 查词 / 折行」各占多少，串口直接读。
    // Timing: report how the cost splits across open, lookup and wrapping.
    const int64_t t0 = esp_timer_get_time();
    // 别名防御（与 show_pending 同一隐患，gen26.4.1 对称修复）：word 可能指向
    // s_word 自身（dict_popup_word() 的返回值），入口 reset 会把它一起清空。
    // 先拷贝到局部缓冲再 reset。/ Aliasing defense (same hazard as
    // show_pending, fixed symmetrically in gen26.4.1): word may point at
    // s_word itself (dict_popup_word()'s return), which the entry reset would
    // clear along with the card. Copy to a local buffer before the reset.
    char own[DICT_POPUP_WORD_MAX];
    if (!word || !*word) { dict_popup_reset(); return false; }
    snprintf(own, sizeof(own), "%.*s", DICT_POPUP_WORD_MAX - 1, word);
    dict_popup_reset();
    word = own;
    // 释义缓冲放 PSRAM：主任务栈只有 8KB，不能在这里开大数组。
    // The definition buffer lives in PSRAM; the main task stack is only 8KB.
    char* out = heap_caps_malloc(DICT_ENTRY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return false;
    bool ok = false;
    int64_t open_us = 0, look_us = 0, wrap_us = 0;
    bool ready = dict_ready();
    if (!ready) {
        const int64_t a = esp_timer_get_time();
        ready = dict_open();
        open_us = esp_timer_get_time() - a;
    }
    if (ready) {
        const int64_t a = esp_timer_get_time();
        const bool found = dict_lookup_word(word, out, DICT_ENTRY_MAX);
        look_us = esp_timer_get_time() - a;
        if (found) {
            const int64_t b = esp_timer_get_time();
            snprintf(s_word, sizeof(s_word), "%.*s", DICT_POPUP_WORD_MAX - 1, word);
            int text_w = body_text_width();
            char* p = out;
            while (*p) {
                char* nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                char saved = p[len];
                p[len] = 0;
                if (len) wrap_text(p, DICT_POPUP_TEXT_PX, text_w);
                p[len] = saved;
                if (s_truncated) break;
                if (!nl) break;
                p = nl + 1;
            }
            wrap_us = esp_timer_get_time() - b;
            ok = s_line_count > 0;
            // 索引未就绪时返回 false，chip 显示「保存」——绝不在这里读卡。
            // Returns false when the index is unavailable and the chip shows "unsaved";
            // the card is never read here.
            if (ok) s_saved = vocab_log_contains(s_word);
        }
    }
    ESP_LOGI(TAG, "lookup \"%s\" ok=%d open=%lldms io=%lldms wrap=%lldms total=%lldms",
             word, (int)ok, (long long)(open_us / 1000), (long long)(look_us / 1000),
             (long long)(wrap_us / 1000), (long long)((esp_timer_get_time() - t0) / 1000));
    free(out);
    if (!ok) dict_popup_reset();
    return ok;
}

// gen26.3 ④ 「AI 查询中…」卡：词典 miss 且 AI 开着时立即进入查询态。卡面是真实
// 状态（状态行 + 保存 chip 初值），0x07 到达后由 dict_popup_show_def 原地更新
// 为释义。词为空或暂存失败返回 false 且不留半张卡，调用方照旧走提示路径。
// gen26.3 "AI querying..." card: enter the query state at once on a dictionary
// miss with AI on. The card is a real state (status line plus the save chip's
// initial value); dict_popup_show_def updates it in place when 0x07 lands.
// False on an empty word or a staging failure, leaving no half card, so the
// caller takes its notice path as before.
bool dict_popup_show_pending(const char* word) {
    // 别名防御（gen26.4 真机 defect 根因）：调用方可能直接传 dict_popup_word()——
    // 返回值就是 &s_word 自身。旧代码先 reset 再查空，reset 把别名参数一起清空，
    // 「!*word」误判空词提前返回 → 查询态没建、词典卡被清空，0x07 回来门 B 失配
    // 被门 A 丢弃 = 释义全丢。先拷贝到局部缓冲再 reset。
    // Aliasing defense (direct root cause of the gen26.4 device defect): a caller
    // may pass dict_popup_word() - the return value IS &s_word. The old code
    // reset first and checked emptiness after, so the reset cleared the aliased
    // argument too, "!*word" misread an empty word and bailed - no query state,
    // a wiped dict card, and when 0x07 landed gate B missed so gate A dropped
    // it = every definition lost. Copy to a local buffer before the reset.
    char own[DICT_POPUP_WORD_MAX];
    if (!word || !*word) { dict_popup_reset(); return false; }
    snprintf(own, sizeof(own), "%.*s", DICT_POPUP_WORD_MAX - 1, word);
    dict_popup_reset();
    snprintf(s_word, sizeof(s_word), "%.*s", DICT_POPUP_WORD_MAX - 1, own);
    if (!store_line(DICT_POPUP_PENDING_TEXT, strlen(DICT_POPUP_PENDING_TEXT))) {
        dict_popup_reset(); // 不留半张卡 / leave no half card
        return false;
    }
    s_pending = true;
    s_pending_since_us = esp_timer_get_time();
    // 保存 chip 初值与另两条开卡路径同一谓词：反映生词本真实落盘状态。/ The chip's
    // initial value uses the same predicate as the other two open paths: the real
    // vocabulary-book state.
    s_saved = vocab_log_contains(s_word);
    ESP_LOGI(TAG, "ai pending \"%s\"", s_word);
    return true;
}

// gen26.4：词典命中卡上的「AI」入口——强制发起 AI 查询。复用 show_pending 的查询态
// 状态机（词头 + 状态行 + 保存 chip 初值），唯一区别是置位 s_ai_over_dict：查询失败
// 超时时调用方据此回退重开词典释义卡，不把词典内容弄丢。/ gen26.4: the "AI" entry
// on a dict-hit card forces an AI query. Reuses show_pending's querying state
// machine (head + status line + save chip initial value); the only difference is
// setting s_ai_over_dict so a timeout falls back to re-opening the dictionary
// definition card instead of losing it.
bool dict_popup_force_ai(const char* word) {
    if (!dict_popup_show_pending(word)) return false;
    // show_pending 的入口 reset 已清 s_ai_over_dict；此处置位标记「词典命中卡强制触发」。
    // show_pending's entry reset cleared s_ai_over_dict; set the "forced from a
    // dict-hit card" mark here.
    s_ai_over_dict = true;
    ESP_LOGI(TAG, "ai force over dict \"%s\"", s_word);
    return true;
}

// gen26.3 ④ 谓词：查询态且查的是本词——app_book 双门判定的核心。word=NULL 只问
// 查询态；词长超过词头缓冲时按截断词头匹配（卡面显示的就是这个词的截断形）。
// gen26.3 predicate: the query state for THIS word - the core of app_book's
// double gate. A NULL word asks only about the state; an overlong word matches
// on its truncated head (the card shows that word's truncated form anyway).
bool dict_popup_is_pending(const char* word) {
    if (!s_pending) return false;
    if (!word) return true;
    return strncmp(s_word, word, DICT_POPUP_WORD_MAX - 1) == 0;
}

// gen26.3 ④ 查询态超时：非查询态恒假；查询态按 esp_timer 时钟判 timeout_us。
// 手机迟迟不回帧（查询失败/链路断）时调用方把卡片降级为失败提示。/ gen26.3
// query-state timeout: always false outside the query state; inside it the
// esp_timer clock decides. The caller degrades the card to a failure notice
// when the phone never answers (query failed / link down).
bool dict_popup_pending_expired(int64_t timeout_us) {
    return s_pending && esp_timer_get_time() - s_pending_since_us >= timeout_us;
}

// gen26.3 ⑤ AI 释义显示容量（全角字符数）：按当前方向的卡片布局实算。行数用与
// dict_popup_draw 收行条件完全相同的谓词（行顶+字号 ≤ 卡底-20），每行字数为正文
// 宽整除 36px 全角字宽。**gen26.6 乘字号 36px/行高 46 重算**（旧 27px/42 的 200/400
// 作废）：竖屏卡 {24,540,636,528}，正文宽 text_w=636-72=564px；头部 26+46+18=90px，
// 底留 20px，行高 46：46*i ≤ 528-20-36-90=382 → i≤8.30 → 9 行；564/36=15.67 → 15
// 字/行；容量 = 9 x 15 = 135 全角字符。横屏同公式：text_w=1216-48-72=1096 →
// 30 字/行，9 行 → 270。D3 的 180 字释义预算超过词卡容量，溢出部分进详解视图承接
// （可读性优先的 trade-off，非缺陷）。
// gen26.3 item-5 AI definition capacity in full-width chars, computed from the
// card layout in the current rotation. The line count uses the exact predicate
// dict_popup_draw's stop uses (line top + font size <= card bottom - 20) and the
// per-line count is the body width divided by the 36px full-width advance.
// **Recomputed for gen26.6's 36px font / 46px line height** (the old 27/42
// 200/400 is void): portrait card {24,540,636,528}, text_w=636-72=564px; header
// 26+46+18=90px, 20px bottom margin, line height 46: 46*i <= 528-20-36-90=382 ->
// i<=8.30 -> 9 lines; 564/36=15.67 -> 15 chars per line; capacity = 9 x 15 = 135
// full-width chars. Landscape, same formula: text_w=1216-48-72=1096 -> 30 chars
// per line, 9 lines -> 270. D3's 180-char budget exceeds the card capacity and
// the overflow goes to the detail view (a readability-first trade-off, not a bug).
int dict_popup_ai_capacity_chars(void) {
    const EpdRect card = dict_popup_rect();
    const int text_w = body_text_width();
    int lines = 0;
    for (int y = 26 + DICT_POPUP_WORD_PX + 18; y + DICT_POPUP_TEXT_PX <= card.height - 20; y += DICT_POPUP_LINE_H) {
        ++lines;
    }
    return lines * (text_w / DICT_POPUP_TEXT_PX);
}

// M2 Step9：释义截断标记的只读出口。s_truncated 由 wrap_text/store_line 在
// 行数触顶（或行缓冲分配失败）时置位——正是「卡片装不下」的事实本身。
// reset 清零，故只在一次暂存成功后到下一次开卡/reset 之间有意义。
// M2 Step9: the read-only outlet of the truncation flag. s_truncated is set by
// wrap_text/store_line when the lines fill up (or a line buffer fails) - the
// very fact of "the card cannot hold it". reset clears it, so it is meaningful
// only between a successful staging and the next open/reset.
bool dict_popup_ai_truncated(void) { return s_truncated; }

// gen26 AI 释义开卡：外部释义（手机 0x07 回流 / 预取缓存命中）直接暂存。与
// dict_popup_open 共用 reset/wrap/store 链路与保存 chip 谓词，唯一区别是不做
// 本地词典查找。gen26.3 ④：查询态卡到达时走的正是原地更新——入口的 reset 已清
// 掉查询态标志，落定的就是释义态（差分局部刷新，不二次全刷打底）。
// gen26 AI definition staging: an external definition (phone 0x07 return /
// prefetch-cache hit) is staged directly. It shares the reset/wrap/store chain
// and the save-chip predicate with dict_popup_open; the only difference is that
// no local dictionary lookup happens. gen26.3: a querying card's arrival is
// exactly this in-place update - the entry reset already cleared the query
// state, so what lands is the definition state (a differential local refresh,
// no second full repaint).
bool dict_popup_show_def(const char* word, const char* def) {
    const int64_t t0 = esp_timer_get_time();
    dict_popup_reset();
    if (!word || !*word || !*def) return false;
    snprintf(s_word, sizeof(s_word), "%.*s", DICT_POPUP_WORD_MAX - 1, word);
    const int text_w = body_text_width();
    // 段缓冲放 PSRAM：gen26.6 D3 起可容 768B 整段，不能压 8KB 主栈。
    // The segment buffer lives in PSRAM: since gen26.6 D3 it holds a full 768-byte
    // segment and must not sit on the 8KB main stack.
    char* seg = heap_caps_malloc(DICT_POPUP_AI_SEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!seg) { dict_popup_reset(); return false; } // 不留半张卡 / leave no half card
    const char* p = def;
    while (*p && !s_truncated) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        // 协议上限（PUSH_AI_DEF_MAX=768）保证单段必小于 seg；此处钳制纯防御。
        // 钳位必须落在字符边界上：从中间砍断一个多字节序列，折行器的 utf8_step
        // 会信引导字节踩着 NUL 走过去（ASan 在 300 字段用例上实证过这个越界）。
        // The protocol cap (PUSH_AI_DEF_MAX=768) guarantees every segment fits in
        // seg; the clamp is pure defence. It must land on a character boundary:
        // cutting a multibyte sequence in half makes the wrapper's utf8_step step
        // over the NUL (ASan proved this overflow on a 300-char segment).
        if (len > DICT_POPUP_AI_SEG - 1) len = DICT_POPUP_AI_SEG - 1;
        memcpy(seg, p, len);
        while (len > 0 && ((unsigned char)p[len] & 0xC0) == 0x80) --len; // 续字节回退 / back off continuation bytes
        seg[len] = 0;
        if (len) wrap_text(seg, DICT_POPUP_TEXT_PX, text_w);
        if (s_truncated || !nl) break;
        p = nl + 1;
    }
    free(seg);
    const bool ok = s_line_count > 0;
    // 保存 chip 反映生词本真实落盘状态，与本地词典卡同一谓词。/ The save chip
    // reflects the real vocabulary-book state, same predicate as the dict card.
    if (ok) s_saved = vocab_log_contains(s_word);
    ESP_LOGI(TAG, "ai def \"%s\" lines=%d truncated=%d wrap=%lldms",
             s_word, s_line_count, (int)s_truncated,
             (long long)((esp_timer_get_time() - t0) / 1000));
    if (!ok) dict_popup_reset();
    return ok;
}

void dict_popup_draw(uint8_t* fb) {
    EpdRect card = dict_popup_rect();
    if (!s_word[0]) return;
    ui_fill_round_rect(fb, card, 24, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, card, 24, 0x30);
    const int inner_x = card.x + DICT_POPUP_PAD;
    const int inner_w = card.width - 2 * DICT_POPUP_PAD;
    // gen26.4：「AI」入口 chip 只在词典释义卡上显示（AI 开着且非查询态）。
    // gen26.4: the "AI" entry chip shows only on a dictionary card (AI on, not querying).
    const bool ai_chip = s_ai_on && !s_pending;
    // 词头可能超出卡片宽，按宽度从左截短；可用宽度要扣掉右侧的保存 chip（以及 AI 入口 chip）。
    // Trim overlong heads from the left; the usable width excludes the save chip (and the AI chip).
    const int head_w = ai_chip ? (inner_w - DICT_POPUP_SAVE_W - DICT_POPUP_AI_W - 24)
                               : (inner_w - DICT_POPUP_SAVE_W - 12);
    char head[DICT_POPUP_WORD_MAX];
    snprintf(head, sizeof(head), "%s", s_word);
    while (head[0] && ui_text_fixed_width_px(DICT_POPUP_WORD_PX, head) > head_w) {
        size_t len = strlen(head);
        head[len - 1] = 0; // ASCII 词头逐字节收缩 / ASCII heads shrink byte by byte
    }
    ui_text_fixed(fb, inner_x, card.y + 26, DICT_POPUP_WORD_PX, head, EPD_DRAW_ALIGN_LEFT, false);
    // gen26.4：「AI」入口 chip（词典释义卡专属）——填充深色 + 反白「AI」，与保存 chip 的
    // 描边形态区分，示意这是一个动作入口而非状态。/ gen26.4: the "AI" entry chip
    // (dictionary card only) - filled dark with an inverted "AI", visually distinct
    // from the outlined save chip so it reads as an action, not a state.
    if (ai_chip) {
        EpdRect ai = dict_popup_ai_rect();
        ui_fill_round_rect(fb, ai, 12, UI_GRAY_BLACK);
        ui_draw_round_rect(fb, ai, 12, UI_GRAY_BLACK);
        int ai_w = ui_text_fixed_width_px(DICT_POPUP_AI_PX, DICT_POPUP_AI_LABEL);
        ui_text_fixed(fb, ai.x + (ai.width - ai_w) / 2, ai.y + 12, DICT_POPUP_AI_PX,
                      DICT_POPUP_AI_LABEL, EPD_DRAW_ALIGN_LEFT, true);
    }
    // 保存 chip：未保存是描边，已保存是选中态。
    // Save chip: outlined when unsaved, selected when saved.
    EpdRect chip = dict_popup_save_rect();
    const char* label = s_saved ? "已存" : "保存";
    if (s_saved) ui_draw_choice_round_rect(fb, chip, 12, true);
    else ui_draw_round_rect(fb, chip, 12, UI_GRAY_BLACK);
    int label_w = ui_text_fixed_width_px(DICT_POPUP_SAVE_PX, label);
    ui_text_fixed(fb, chip.x + (chip.width - label_w) / 2, chip.y + 12, DICT_POPUP_SAVE_PX,
                  label, EPD_DRAW_ALIGN_LEFT, false);
    int y = card.y + 26 + DICT_POPUP_WORD_PX + 18;
    for (int i = 0; i < s_line_count; ++i) {
        const char* line = s_lines[i];
        ui_text_fixed(fb, inner_x, y, DICT_POPUP_TEXT_PX, line, EPD_DRAW_ALIGN_LEFT, false);
        y += DICT_POPUP_LINE_H;
        if (y + DICT_POPUP_TEXT_PX > card.y + card.height - 20) {
            if (i + 1 < s_line_count || s_truncated) {
                ui_text_fixed(fb, card.x + card.width - DICT_POPUP_PAD, y - DICT_POPUP_LINE_H + DICT_POPUP_TEXT_PX + 8,
                              21, "……", EPD_DRAW_ALIGN_RIGHT, false);
            }
            break;
        }
    }
}
