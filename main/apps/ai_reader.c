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

#include "esp_attr.h" // 26.11.3-C2：EXT_RAM_BSS_ATTR（会话缓冲落 PSRAM）
#include "esp_log.h"
#include "push_proto.h"
#include "scene_req.h" // 释义长度上限链：详解缓冲必须容下协议槽 / length chain: detail buffer >= protocol slot
#include "ui_kit.h"
#include "ui_nav.h"

static const char* TAG __attribute__((unused)) = "ai_reader";
// 一次性日志旗标：字号参照系与点阵兜底各只打一条，避免翻页刷屏。
// One-shot log flags: the size reference and the bitmap fallback each print once.
static bool s_cap_logged;
static bool s_bitmap_fallback_logged;

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

/// 整段选文副本上限：与推送上下文槽同数（PUSH_SCENE_CTX_MAX = 1024）。多留一格
/// 给 NUL。会话结构整体在 PSRAM，这 1 KB 不占内部 RAM。
/// Same ceiling as the push context slot, plus a NUL. The session lives in PSRAM.
#define AI_READER_EXCERPT_MAX 1024u

typedef struct {
    bool active;
    ai_reader_mode_t mode;
    char word[AI_READER_WORD_MAX];
    char def[AI_READER_DEF_MAX];
    size_t def_len;
    int page;
    int page_count;
    size_t pages[AI_READER_MAX_PAGES]; // 每页首字符字节偏移 / each page's first byte
    int width_px, text_px, lines_per_page;
    ai_reader_source_t source;
    // M3-B 引用条：整句（从 M3-A 的上下文快照里裁出来）+ 词在句子内的偏移。
    // M3-B quote strip: the whole sentence (cut from the M3-A context snapshot)
    // plus the word's offset INSIDE that sentence.
    char quote[AI_READER_QUOTE_MAX];
    size_t quote_len;
    size_t quote_word_off;
    size_t quote_word_len;
    bool quote_fallback;
    bool quote_head_cut; // 左侧省略（句子被截断）/ leading ellipsis
    bool quote_tail_cut; // 右侧省略 / trailing ellipsis
    // 26.11.4：整段会话的选段全文。引用条 `quote` 只有 384B（词卡的取景尺寸），
    // 而一段英文选文 900B 起——中文兜底「存选段原文」若从引用条拿，存进去的是
    // 被砍掉三分之二的段落。1065B 与 PUSH_SCENE_CTX_MAX 同数（静态断言在 open 处）。
    // The passage's full selection. The strip is 384 B (sized for a word card); a
    // 900 B paragraph read back from it would be two-thirds gone.
    bool excerpt;
    char excerpt_quote[AI_READER_EXCERPT_MAX + 1u];
} ai_reader_session_t;

// 选段副本必须装得下推送上下文槽的**全部**内容：它是「原文兜底」的唯一来源，
// 比槽小一字节就意味着某段选文存进摘抄本时已经缺了一角。
// The copy must hold the whole context slot: it is the only source for the original
// fallback, and one byte short means a selection lands in the book already clipped.
_Static_assert(AI_READER_EXCERPT_MAX >= PUSH_SCENE_CTX_MAX,
               "26.11.4: the passage copy must hold the full context slot");

// 26.11.3-C2：整具会话落 PSRAM。它内嵌 8,192 B 的释义缓冲 + 384 B 引用条，
// 内部 RAM 只剩 112 B 装不下；且它只在「打开会话」期间有意义，放外部内存等于
// 把常驻成本从开着书就付，改成想读详解才付。memset/strlen 的语义一字未改。
// 26.11.3-C2: the whole session lives in PSRAM. It embeds an 8,192 B definition
// buffer plus a 384 B quote strip, which internal RAM (112 B free) cannot hold; and
// it only matters while a session is open, so external memory turns a cost paid for
// as long as a book is open into one paid when a detail view is asked for. memset
// and strlen semantics are unchanged.
static EXT_RAM_BSS_ATTR ai_reader_session_t s_session;

/* ---------- 版心几何（M3-B）/ Body geometry (M3-B) ---------- */

static ai_reader_geom_t s_geom;
static bool s_geom_set;

// M3-B：情景词的生词本状态（只影响 chip 文案）。会话关闭不清——下一次开卡由
// 调用方按 vocab_log_contains 重新播种，这里不做跨会话记忆。
// M3-B: the scene word's vocabulary-book state (chip label only). Not cleared on
// close - the caller re-seeds it from vocab_log_contains at open time, so no state
// leaks across sessions.
static bool s_scene_saved;

void ai_reader_set_scene_saved(bool saved) { s_scene_saved = saved; }

void ai_reader_set_geometry(const ai_reader_geom_t* g) {
    if (!g) {
        s_geom_set = false;
        return;
    }
    s_geom = *g;
    s_geom_set = true;
}

// 详解缺省：上沿 236（状态栏下）、下沿 h-72（页脚上方）、版心取注入样式。
// M3-B 的情景卡由 app_book 注入（上沿 236、下沿扣掉动作栏），详解路径不注入，
// 因此**逐像素不变**（宿主断言钉住）。
// Detail default: top 236 (under the status bar), bottom h-72 (above the footer),
// measure from the injected style. The M3-B scene card is injected by app_book
// (top 236, bottom above the action bar); the detail path injects nothing, so it
// stays pixel-identical (asserted on the host).
static ai_reader_geom_t current_geom(const ai_reader_style_t* st, int screen_h) {
    if (s_geom_set && s_geom.bottom > s_geom.top && s_geom.width > 0) return s_geom;
    return (ai_reader_geom_t){.top = 236, .bottom = screen_h - 72, .left = st->left, .width = st->width};
}

ai_reader_geom_t ai_reader_scene_geom(const ai_reader_style_t* style, int screen_h) {
    return (ai_reader_geom_t){
        .top = AI_READER_SCENE_TOP,
        .bottom = screen_h - 48,
        .left = style->left,
        .width = style->width,
    };
}


/* ---------- 排版样式（正文唯一事实源）/ Type style (the body is the only source) ---------- */

static ai_reader_style_t s_style;
static bool s_style_set;

void ai_reader_set_style(const ai_reader_style_t* style) {
    if (!style) {
        s_style_set = false;
        return;
    }
    s_style = *style;
    s_style_set = true;
}

// 生产路径由 app_book 每帧注入；没注入时的默认值只为宿主/异常兜底，并喊一声。
// Production injects it every paint; the fallback exists for hosts and for the
// pathological "someone forgot to inject" case, which is logged once.
static ai_reader_style_t current_style(void) {
    if (s_style_set && s_style.px > 0 && s_style.line_h > 0 && s_style.width > 0) return s_style;
    ESP_LOGW(TAG, "detail: style not injected, using fallback 48px");
    return (ai_reader_style_t){.px = 48, .line_h = 72, .left = 36, .width = 612};
}

/* ---------- 逐字宽度（度量与绘制同源）/ Per-glyph advance (measure == paint) ---------- */

static ai_reader_advance_fn s_advance;
static void* s_advance_ctx;

void ai_reader_set_advance(ai_reader_advance_fn fn, void* ctx) {
    s_advance = fn;
    s_advance_ctx = ctx;
}

// 设备默认：阅读字体的真实 advance；该字体没有这个字时回退内建点阵的 advance
// （与 ui_text_reader 的整行回退同一套判据）。始终返回 >0，装行不会原地打转。
// Device default: the reader face's real advance, falling back to the built-in
// bitmap advance for glyphs it lacks (same predicate as ui_text_reader's
// whole-line fallback). Always > 0 so line breaking can never stall.
static int advance_of(uint32_t cp, int px) {
    if (s_advance) {
        const int w = s_advance(s_advance_ctx, cp, px);
        return w > 0 ? w : px;
    }
    const int w = ui_text_reader_advance_px(px, cp);
    return w > 0 ? w : px;
}

static int utf8_step(const char* s, int avail, uint32_t* out) {
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char c = p[0];
    int n = 1;
    uint32_t cp = c;
    if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07u; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0Fu; }
    else if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1Fu; }
    else if (c >= 0x80) { *out = 0xFFFD; return 1; } // 非法前导 / stray continuation
    if (n > avail) { *out = 0xFFFD; return 1; }
    for (int i = 1; i < n; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *out = cp;
    return n;
}

// 避头：不能出现在行首的标点。/ No-start: punctuation that may not begin a line.
static bool no_start(uint32_t cp) {
    switch (cp) {
        case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF1B: case 0xFF1A:
        case 0xFF1F: case 0xFF01: case 0xFF09: case 0x3011: case 0x3009: case 0x300B:
        case 0x300D: case 0x300F: case 0x3015: case 0x3017: case 0x2026: case 0x2014:
        case ',': case '.': case ';': case ':': case '?': case '!': case ')': case ']':
        case '}': case '%':
            return true;
        default:
            return false;
    }
}

// 避尾：不能留在行尾的开标点。/ No-end: opening punctuation that may not end a line.
static bool no_end(uint32_t cp) {
    switch (cp) {
        case 0xFF08: case 0x3010: case 0x300A: case 0x300C: case 0x300E: case 0x3014:
        case 0x3016: case 0x201C: case 0x2018: case '(': case '[': case '{': case '$':
            return true;
        default:
            return false;
    }
}

// 从 i 往前退一个 UTF-8 字符的起点（i > floor 时保证前进至少 1 字节）。
// Start of the UTF-8 character before i (always moves at least one byte when i > floor).
static size_t prev_char_start(const char* text, size_t i, size_t floor) {
    size_t k = i - 1;
    while (k > floor && ((unsigned char)text[k] & 0xC0) == 0x80) --k;
    return k;
}

/* ---------- 断行与分页（纯逻辑，宿主可测）/ Wrapping and pagination ---------- */

// 行尾避尾：cut 之前若是开标点，就把整行往前收一个字符（至少要留一个字）。
// No-end fix-up: if the character before cut is an opening punctuation, pull the
// line back one character (always keeping at least one).
static size_t fix_tail(const char* text, size_t cut, size_t off) {
    while (cut > off) {
        const size_t last = prev_char_start(text, cut, off);
        if (last <= off) break;
        uint32_t cp = 0;
        (void)utf8_step(text + last, (int)(cut - last), &cp);
        if (!no_end(cp)) break;
        cut = last;
    }
    return cut;
}

// 断行规则**只有这一份**：词边界优先（该行已用掉 ≥60% 版心才认）→ 避头 → 避尾 →
// 最后才在字符边界硬断。正文用阅读字体的 advance，引用条用固定字模的 advance——
// 差别只在度量回调，规则不复制。2026-11-03 审计点名「禁第二套折行」正是为此：
// `scene_draw_quote` 曾自己写了一个逐码位累加、超了就地断的循环，把 `Isosceles`
// 断成 `I` / `sosceles`、`appreciate` 断成 `apprecia`。
//
// The wrapping rule lives here once: word boundary first (only when the line is at least
// 60% full), then no-start, then no-end, then a hard break on a code point boundary.
// Body text and the quote strip differ only in the advance callback, never in the rule.
typedef int (*line_advance_fn)(uint32_t cp, void* ctx);

static size_t wrap_line_len(const char* text, size_t len, size_t off, int width_px,
                            line_advance_fn advance, void* ctx) {
    if (off >= len || width_px <= 0) return 0;
    size_t i = off, last_space = 0;
    int used = 0, used_at_space = 0;
    while (i < len && text[i] != '\n') {
        uint32_t cp = 0;
        const int n = utf8_step(text + i, (int)(len - i), &cp);
        const int adv = advance(cp, ctx);
        if (used + adv > width_px) {
            if (i == off) return (size_t)n; // 单字比版心还宽：至少放一个，避免死循环
            if (last_space > off && used_at_space * 5 >= width_px * 3)
                return fix_tail(text, last_space, off) - off; // 词边界优先 / prefer the word boundary
            if (no_start(cp)) {             // 避头：连前一个字一起下移
                const size_t back = prev_char_start(text, i, off);
                if (back > off) return fix_tail(text, back, off) - off;
            }
            return fix_tail(text, i, off) - off;
        }
        used += adv;
        i += (size_t)n;
        if (cp == ' ' || cp == '-') { last_space = i; used_at_space = used; }
    }
    // 段尾同样要避尾：行末是开标点则把它推到下一行（整行至少留一个字）。
    // The paragraph tail obeys no-end too: an opening punctuation moves down.
    return fix_tail(text, i, off) - off;
}

// 正文度量：阅读字体的真实 advance。
// Body metric: the reader face's real advance.
static int body_advance(uint32_t cp, void* ctx) { return advance_of(cp, *(int*)ctx); }

static size_t line_len_at(const char* text, size_t len, size_t off, int width_px, int text_px) {
    return wrap_line_len(text, len, off, width_px, body_advance, &text_px);
}

// 跳过行尾 '\n'。/ Skip the line-ending '\n'.
static size_t next_line_off(const char* text, size_t len, size_t off) {
    return off < len && text[off] == '\n' ? off + 1 : off;
}

int ai_reader_paginate(const char* text, int width_px, int text_px, int lines_per_page,
                       size_t* out_starts, int max_pages) {
    if (!text || !*text || width_px <= 0 || text_px <= 0 || lines_per_page <= 0) return 0;
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
        off = next_line_off(text, len, off + line_len_at(text, len, off, width_px, text_px));
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

ai_reader_mode_t ai_reader_mode(void) {
    return s_session.active ? s_session.mode : AI_READER_MODE_DEF;
}

/* ---------- M3-B：引用条取句（委托给纯逻辑模块）/ quote sentence (delegates) ---------- */

// 取句逻辑住在 scene_req.c：那里没有 UI 依赖，宿主可以零桩逐条断言「句界 /
// 词居中截断 / UTF-8 不切半字」。这里保留同名入口，调用方不必知道落点。
// The extraction logic lives in scene_req.c: no UI dependencies there, so the host
// asserts boundaries, word-centred truncation and UTF-8 safety with zero stubs.
// The same-named entry stays here so callers need not know where it landed.
size_t ai_reader_quote_sentence(const char* ctx, size_t ctx_len,
                                size_t word_off, size_t word_len,
                                size_t max_chars, size_t* out_off, size_t* out_len,
                                bool* fallback) {
    return scene_req_quote_sentence(ctx, ctx_len, word_off, word_len, max_chars,
                                    out_off, out_len, fallback);
}

// 情景会话的唯一打开路径（词条 / 整段共用）。whole_quote 决定引用条取景：
// 词条按「含该词的整句」取，整段就整段铺（头部装不下的部分用 tail_cut 标出来，
// 不假装引用完整）。
// The single scene-opening path shared by the word case and the passage case.
// whole_quote picks the framing: a word gets its sentence, a passage gets the
// passage (head-truncated when it exceeds the strip, flagged via tail_cut rather
// than pretending the quote is complete).
static bool open_scene_common(const char* title, const char* text,
                              const char* quote, size_t quote_word_off, size_t quote_word_len,
                              bool whole_quote, const ai_reader_source_t* source) {
    if (!title || !*title || !text || !*text) return false;
    memset(&s_session, 0, sizeof(s_session));
    copy_bounded(s_session.word, sizeof(s_session.word), title);
    copy_bounded(s_session.def, sizeof(s_session.def), text);
    s_session.def_len = strlen(s_session.def);
    s_session.mode = AI_READER_MODE_SCENE;
    if (source) {
        s_session.source = *source;
    } else {
        memset(&s_session.source, 0, sizeof(s_session.source));
    }
    // 引用条：只在有上下文时铺；取不到就是没有引用条，绝不编一句出来。
    // Quote strip only when a context exists; when it does not, there is no
    // strip - the view never invents a sentence.
    if (quote && *quote) {
        const size_t qn = strlen(quote);
        size_t off = 0, len = 0;
        bool fb = false;
        if (whole_quote) {
            len = qn;
            if (len > sizeof(s_session.quote) - 1) len = sizeof(s_session.quote) - 1;
        } else {
            (void)ai_reader_quote_sentence(quote, qn, quote_word_off, quote_word_len,
                                           AI_READER_QUOTE_MAX / 3, &off, &len, &fb);
        }
        if (len > sizeof(s_session.quote) - 1) len = sizeof(s_session.quote) - 1;
        memcpy(s_session.quote, quote + off, len);
        s_session.quote[len] = 0;
        s_session.quote_len = len;
        s_session.quote_fallback = whole_quote ? false : fb;
        // 词偏移换算到「句子内」；原词偏移可能落在句子之前（截断），钳到 [0,len]。
        // 整段引用不高亮单个词（word_len=0），这里锚点没有呈现意义。
        // Re-base the word offset into the sentence; clamp when the cut moved it out.
        // A whole-passage quote highlights nothing (word_len = 0): the anchor has no
        // meaning on a strip that already starts at the selection head.
        s_session.quote_word_off = whole_quote ? 0 : (quote_word_off > off ? quote_word_off - off : 0);
        if (s_session.quote_word_off > len) s_session.quote_word_off = len;
        s_session.quote_word_len = whole_quote ? 0 : quote_word_len;
        if (s_session.quote_word_len > len - s_session.quote_word_off)
            s_session.quote_word_len = len - s_session.quote_word_off;
        s_session.quote_head_cut = off > 0;
        s_session.quote_tail_cut = (off + len) < qn;
    }
    s_session.active = true;
    return true;
}

bool ai_reader_open_scene(const char* word, const char* text,
                          const char* quote, size_t quote_word_off, size_t quote_word_len,
                          const ai_reader_source_t* source) {
    return open_scene_common(word, text, quote, quote_word_off, quote_word_len,
                             false, source);
}

bool ai_reader_open_excerpt(const char* title, const char* text, const char* quote,
                            const ai_reader_source_t* source) {
    // 整段模式：标题是「选段」，引用条整段铺，不取句、不高亮词。
    // Passage mode: the title is 选段, the strip holds the selection itself, with no
    // sentence extraction and no word highlight.
    if (!open_scene_common(title, text, quote, 0, 0, true, source)) return false;
    // 全文另存一份：引用条已被截到 384B（那是词卡的取景尺寸），摘抄的中文兜底
    // 必须拿到完整选段，否则「存选段原文」存进去的是残段。
    // Keep the whole selection: the strip was cut to 384 B (a word card's framing),
    // and the Chinese fallback must save the passage, not two-thirds of it.
    s_session.excerpt = true;
    copy_bounded(s_session.excerpt_quote, sizeof(s_session.excerpt_quote), quote ? quote : "");
    return true;
}

bool ai_reader_is_excerpt(void) { return s_session.active && s_session.excerpt; }

const char* ai_reader_def(void) { return s_session.active ? s_session.def : ""; }

const char* ai_reader_excerpt_text(void) {
    return (s_session.active && s_session.excerpt) ? s_session.excerpt_quote : "";
}

EpdRect ai_reader_scene_save_rect(void) {    const EpdRect zero = {0, 0, 0, 0};
    if (!s_session.active || s_session.mode != AI_READER_MODE_SCENE) return zero;
    const ai_reader_style_t st = current_style();
    const ai_reader_geom_t g = current_geom(&st, UI_LOCK_HEIGHT);
    // 与页脚同一行：左缘版心、垂直居中于 footer 带。
    // Same row as the footer: body left edge, centred in the footer band.
    const int chip_w = ui_text_fixed_width_px(24, "存生词 ›") + 32;
    const int chip_h = 40;
    return (EpdRect){g.left - 8, g.bottom + (48 - chip_h) / 2, chip_w, chip_h};
}


// 参数变化才重排；页码钳到新页数内。/ Repaginate only on a metric change;
// clamp the page into the new count.
static void ensure_paginated(int width_px, int text_px, int lines_per_page) {
    if (!s_session.active) return;
    if (width_px == s_session.width_px && text_px == s_session.text_px &&
        lines_per_page == s_session.lines_per_page) return;
    s_session.width_px = width_px;
    s_session.text_px = text_px;
    s_session.lines_per_page = lines_per_page;
    s_session.page_count = ai_reader_paginate(s_session.def, width_px, text_px, lines_per_page,
                                               s_session.pages, AI_READER_MAX_PAGES);
    if (s_session.page >= s_session.page_count) s_session.page = s_session.page_count - 1;
    if (s_session.page < 0) s_session.page = 0;
}

void ai_reader_set_layout(int width_px, int text_px, int lines_per_page) {
    if (width_px <= 0 || text_px <= 0 || lines_per_page <= 0) return;
    ensure_paginated(width_px, text_px, lines_per_page);
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
    if (s_session.width_px <= 0 || s_session.text_px <= 0 || s_session.lines_per_page <= 0) return 0;
    size_t off = s_session.pages[page];
    int n = 0;
    while (n < s_session.lines_per_page && off < s_session.def_len) {
        const size_t ll = line_len_at(s_session.def, s_session.def_len, off,
                                       s_session.width_px, s_session.text_px);
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
/* ---------- M3-B 情景卡绘制 / M3-B scene card painting ---------- */

// 引用条一行里的三段（前 / 高亮词 / 后）分别绘制：固定字模是唯一带 inverted
// 参数的接口，因此引用条走固定字模、正文仍走阅读字体——两者各司其职。
// One quote line paints in up to three runs (before / highlighted word / after):
// the fixed face is the only interface with an `inverted` flag, so the strip uses
// it while the body keeps the reading face - each does its own job.
static int quote_run(uint8_t* fb, int x, int cy, int px,
                     const char* s, size_t off, size_t len, bool inverted) {
    if (!len) return x;
    char seg[256];
    if (len > sizeof(seg) - 1) len = sizeof(seg) - 1;
    memcpy(seg, s + off, len);
    seg[len] = 0;
    if (inverted) {
        // inverted 只画白字形、不铺实底（bg 像素被 draw_text_px 跳过），浅色引用条
        // 上会「白字落浅底」再次隐形——先自己垫一块黑底，再画白字，才是真正的
        // 局部反色。方块高 px+8、以行中心 cy 垂直居中。
        // inverted paints white glyphs only, never a solid backing (bg pixels are
        // skipped by draw_text_px); on a light strip that would vanish again. Lay a
        // black box first, then draw white on it - a true local inversion. The box
        // is px+8 tall, centred on the line's cy.
        const int wpx = ui_text_fixed_width_px(px, seg);
        const int box_h = px + 8;
        if (wpx > 0) {
            ui_fill_rect_fast((EpdRect){x, cy - box_h / 2, wpx, box_h},
                              UI_QUOTE_WORD_INK, fb);
        }
    }
    ui_text_fixed_vc(fb, x, cy, px, seg, EPD_DRAW_ALIGN_LEFT, inverted);
    return x + ui_text_fixed_width_px(px, seg);
}

// 引用条的度量：固定字模按码位取宽度。逐字累加与整串测量等价，由门 1 的零差值
// 断言（`ui_font_draw_text_px` 的返回笔位 == `ui_font_text_width_px`）。
// Quote metric: the fixed face's per-code-point advance; gate 1 asserts per-glyph sums
// equal the whole-string measure with zero tolerance.
static int fixed_advance(uint32_t cp, void* ctx) {
    char g[5];
    int n = 1;
    if (cp < 0x80) {
        g[0] = (char)cp;
    } else if (cp < 0x800) {
        g[0] = (char)(0xC0 | (cp >> 6));
        g[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        g[0] = (char)(0xE0 | (cp >> 12));
        g[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        g[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        g[0] = (char)(0xF0 | (cp >> 18));
        g[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        g[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        g[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    g[n] = 0;
    const int w = ui_text_fixed_width_px(*(int*)ctx, g);
    return w > 0 ? w : 1; // 永不返回 0：装行不许原地打转 / never 0, or the wrap stalls
}

static void scene_draw_quote(uint8_t* fb, const ai_reader_geom_t* g) {
    if (s_session.quote_len == 0) return;
    const int px = 22;
    const int x0 = g->left + 16;
    const int avail = g->width - 32;
    const int cy[2] = {g->top + 32, g->top + 70};
    const size_t ws = s_session.quote_word_off;
    const size_t we = ws + s_session.quote_word_len;

    // 折两行，走**同一份**断行规则（词边界优先 + 避头尾），不再自写逐码位循环。
    // Two lines through the same wrapping rule as the body - no second implementation.
    size_t off[2] = {0, 0}, len[2] = {0, 0};
    int nlines = 0;
    size_t pos = 0;
    while (pos < s_session.quote_len && nlines < 2) {
        size_t n = wrap_line_len(s_session.quote, s_session.quote_len, pos, avail,
                                 fixed_advance, (void*)&px);
        if (n == 0) { // 空行/换行：跳过一个字节，保证前进
            n = 1;
        }
        off[nlines] = pos;
        len[nlines] = n;
        ++nlines;
        pos += n;
    }

    for (int ln = 0; ln < nlines; ++ln) {
        int x = x0;
        if (ln == 0 && s_session.quote_head_cut) {
            x = quote_run(fb, x, cy[ln], px, "\xE2\x80\xA6", 0, 3, false); // …
        }
        const size_t a = off[ln], b = off[ln] + len[ln];
        const size_t hs = a > ws ? a : ws;
        const size_t he = b < we ? b : we;
        if (hs < he) {
            x = quote_run(fb, x, cy[ln], px, s_session.quote, a, hs - a, false);
            x = quote_run(fb, x, cy[ln], px, s_session.quote, hs, he - hs, true);
            x = quote_run(fb, x, cy[ln], px, s_session.quote, he, b - he, false);
        } else {
            (void)quote_run(fb, x, cy[ln], px, s_session.quote, a, b - a, false);
        }
        if (ln == nlines - 1 && s_session.quote_tail_cut) {
            (void)quote_run(fb, x, cy[ln], px, "\xE2\x80\xA6", 0, 3, false);
        }
    }
}

// 情景卡：引用条（代替标题）+ 正文 + 页脚（「存生词 ›」+ 页码）。上沿由注入几何
// 给出（AI_READER_SCENE_TOP=96，整屏视图 ＝ 阅读页全屏正文顶；**不是**详解的 236，
// 236 的语义是「标题区之后」），状态栏由 ui_nav_status 画；正文与详解共用同一套分页。
// The scene card: quote strip (in place of the title) + body + footer. The top
// comes from the injected geometry (AI_READER_SCENE_TOP=96, the reader's
// fullscreen body top - NOT the detail view's 236, which means "after the title
// zone"); ui_nav_status paints the status bar. Body shares the same pagination.
static void scene_render(uint8_t* fb) {
    const ai_reader_style_t st = current_style();
    const ai_reader_geom_t g = current_geom(&st, UI_LOCK_HEIGHT);
    const int pad = g.left;
    const int body_w = g.width;
    const int text_px = st.px;
    const int line_h = st.line_h;
    const int quote_h = s_session.quote_len ? 96 : 0;
    const int body_top = g.top + quote_h + 12;
    int lines = (g.bottom - body_top) / line_h;
    if (lines < 1) lines = 1;
    ai_reader_set_layout(body_w, text_px, lines);

    ui_clear_page(fb);
    ui_nav_status(fb);

    if (quote_h) {
        // 浅底 + 左侧竖条：既标出「这段来自书里」，又不与正文抢注意力。底色走
        // UI_QUOTE_BAND_BG（量化后 nibble 14 ≈93% 白），黑字在上面清晰可读。
        // A light wash plus a left bar: marks "this came from the book" without
        // competing with the body. The wash is UI_QUOTE_BAND_BG (nibble 14 ~93%
        // white after quantisation), so the black text stays legible on it.
        ui_fill_rect_fast((EpdRect){0, g.top, UI_LOCK_WIDTH, quote_h},
                          UI_QUOTE_BAND_BG, fb);
        ui_fill_rect_fast((EpdRect){0, g.top, 4, quote_h}, 0x50, fb);
        scene_draw_quote(fb, &g);
        ui_hairline(fb, g.top + quote_h, pad, body_w, 0x40);
    }

    ai_reader_line_t lns[64];
    char line[256];
    const int n = ai_reader_page_lines(s_session.page, lns, 64);
    int y = body_top;
    for (int i = 0; i < n; ++i) {
        const size_t ll = lns[i].len < sizeof(line) - 1 ? lns[i].len : sizeof(line) - 1;
        memcpy(line, s_session.def + lns[i].off, ll);
        line[ll] = 0;
        ui_text_reader(fb, pad, y, text_px, line);
        y += line_h;
    }

    // 页脚＝动作栏：左「存生词 ›」、右「情景 x/y」。只有一个动作，不做双槽——
    // 情景卡的主出口是翻页键与返回键，chip 是附加动作。
    // The footer IS the action bar: "save word" left, "scene x/y" right. One
    // action only - the card's main exits are the turn and back keys.
    ui_hairline(fb, g.bottom, pad, body_w, 0x70);
    const int foot_cy = g.bottom + 26;
    const EpdRect chip = ai_reader_scene_save_rect();
    if (chip.width > 0) {
        ui_hairline(fb, chip.y, chip.x, chip.width, 0x60);
        ui_hairline(fb, chip.y + chip.height - 1, chip.x, chip.width, 0x60);
        ui_fill_rect_fast((EpdRect){chip.x, chip.y, 1, chip.height}, 0x60, fb);
        ui_fill_rect_fast((EpdRect){chip.x + chip.width - 1, chip.y, 1, chip.height}, 0x60, fb);
        // 26.11.4：段落视图的同一个 chip 是「存摘抄」，不是「存生词」——词义进
        // 生词本，段落进摘抄本，两件事。位置与宽度都不变（两串同为 3 个汉字），
        // 只有落点不同。
        // The same chip saves a passage to the excerpt book, not a word to the
        // vocabulary book. Same slot, same width, different destination.
        const char* chip_text = s_scene_saved
                                    ? "已存 ✓"
                                    : (s_session.excerpt ? "存摘抄 ›" : "存生词 ›");
        ui_text_fixed_vc(fb, chip.x + chip.width / 2, foot_cy, 24, chip_text,
                         EPD_DRAW_ALIGN_CENTER, false);
    }
    char page_label[32];
    snprintf(page_label, sizeof(page_label), "情景 %d/%d", s_session.page + 1,
             s_session.page_count > 0 ? s_session.page_count : 1);
    ui_text_fixed_vc(fb, pad + body_w, foot_cy, 24, page_label, EPD_DRAW_ALIGN_RIGHT, false);
}

void ai_reader_render(uint8_t* fb) {
    if (!s_session.active) return;
    if (s_session.mode == AI_READER_MODE_SCENE) {
        scene_render(fb);
        return;
    }
    const ai_reader_style_t st = current_style();
    const int h = UI_LOCK_HEIGHT;
    // 版心、字号、行距**全部来自阅读正文**（app_book 每帧注入），此处不再自算：
    // 上一版写死 72px/92 行距/36 边距，而正文是 51px/76/版心整字格——真机就是这样
    // 变成「大字报」的（72 是 51 的 1.41 倍）。度量只有一套，才不会再漂。
    // Measure, size and pitch all come from the LIVE reader body (app_book injects
    // every paint) instead of being computed here: the previous build hard-coded
    // 72px / 92 pitch / 36 margin while the body ran 51px / 76 / a glyph-snapped
    // measure - which is exactly the on-device "billboard". One set of metrics only.
    const int pad = st.left;       // 版心左缘 / body left edge
    const int body_w = st.width;   // 版心宽 / body measure
    const int text_px = st.px;     // 正文字号 / reading font px
    const int line_h = st.line_h;  // 正文行距 / reading line pitch
    const int title_px = text_px;  // 口径②：标题与正文同大 / title == body
    const int gap = 24;            // GAP
    const int meta_line_gap = 8;   // META_LINE_GAP
    const int body_top = 236;
    const int footer_h = 72;
    // 装行预算 = 正文版心像素宽；不再是「每行几个全角」的理想化单位。
    // The wrap budget is the body measure in pixels, not an idealised chars-per-line.
    ai_reader_set_layout(body_w, text_px, (h - footer_h - body_top) / line_h);

    // 字号只记录不门禁：真正的门禁是「详解 px == 正文 px」（同源，见宿主断言）。
    // The size is logged, not gated: the real gate is "detail px == reader px"
    // (same source, asserted on the host); the cap is for on-device eyeballing.
    if (!s_cap_logged) {
        const int cap = ui_text_reader_cap_px(text_px);
        if (cap > 0) {
            s_cap_logged = true;
            ESP_LOGI(TAG, "detail: px=%d cap=%d lines=%d", text_px, cap,
                     (h - footer_h - body_top) / line_h);
        }
    }

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
    // 口径③：右侧元信息列随正文缩小（30→24 / 24→20），避免小字压大字失衡。
    // Ruling 3: the meta column scales down with the body (30->24 / 24->20) so the
    // small labels do not overpower the text.
    const int counter_w = ui_text_fixed_width_px(24, page_label);
    const int tag_w = ui_text_fixed_width_px(20, "AI 详解");
    const int meta_w = (counter_w > tag_w ? counter_w : tag_w);
    const int title_cy = 148;                    // 词列的视觉中心线 / the word column's centre line
    (void)ui_text_title_fit(title, title_px, body_w - meta_w - gap, title);
    ui_text_title_vc(fb, pad, title_cy, title_px, title, title, EPD_DRAW_ALIGN_LEFT);
    // 右列：列视觉中心对齐 title_cy - 4（光学上移）；列内两行按 META_LINE_GAP
    // 排——行1中心 = 列中心 - (行距 + 行2高)/2，行2 = 行1 + 行1高 + 行距。
    // Right column: its centre aligns to title_cy - 4 (the optical lift); the
    // two rows use META_LINE_GAP - row 1's centre is the column centre minus
    // (gap + row-2 height)/2, and row 2 sits a row-1 height plus gap below it.
    const int meta_col_cy = title_cy - 4;
    const int counter_cy = meta_col_cy - (meta_line_gap + 20) / 2;
    const int tag_cy = counter_cy + 24 + meta_line_gap;
    ui_text_fixed_vc(fb, pad + body_w, counter_cy, 24, page_label, EPD_DRAW_ALIGN_RIGHT, false);
    ui_text_fixed_vc(fb, pad + body_w, tag_cy, 20, "AI 详解", EPD_DRAW_ALIGN_RIGHT, false);
    ui_hairline(fb, 206, pad, body_w, 0x70);

    // 正文：按页取行，行文本落到栈缓冲（行宽 ≤ 版心 612px，最长 8 个全角也才
    // 24B + NUL，256 只是保守上界）。
    // Body: take the page's lines into a stack buffer (a line is <= 612px wide, so
    // even 8 full-width glyphs are 24B + NUL; 256 is merely a conservative bound).
    ai_reader_line_t lines[64];
    char line[256];
    const int n = ai_reader_page_lines(s_session.page, lines, 64);
    int y = body_top;
    for (int i = 0; i < n; ++i) {
        const size_t ll = lines[i].len < sizeof(line) - 1 ? lines[i].len : sizeof(line) - 1;
        memcpy(line, s_session.def + lines[i].off, ll);
        line[ll] = 0;
        // gen26.6.5：正文走**阅读字体全路径**（矢量 + 抗锯齿），与阅读正文同源；
        // 点阵只在「该字体缺字」时按段兜底。旧版因 ui_font_has_text() 被点阵兜底
        // 恒真，正文永远走内建系统：TTF 有的字 72px 平滑、没有的字 24px 点阵拉到
        // 51px（源字身仅 17px）→ 同行 3×3 台阶，真机「嫉妒、猜忌」整片锯齿。
        // gen26.6.5: the body paints through the READING face's full path (vector +
        // antialiasing), the same source as the reader body; the bitmap is only a
        // per-run fallback when that face lacks a glyph. The old path always used
        // the built-in system because ui_font_has_text() was made true by the bitmap
        // supplement: TTF-covered glyphs were 72px and smooth, the rest were a 24px
        // bitmap (17px of real ink) blown up to 51px - 3x3 stair-steps in one line.
        if (!ui_text_reader_uses_ttf(line) && !s_bitmap_fallback_logged) {
            s_bitmap_fallback_logged = true;
            ESP_LOGW(TAG, "detail: bitmap fallback (reader face lacks a glyph in this line)");
        }
        ui_text_reader(fb, pad, y, text_px, line);
        y += line_h;
    }

    // 页脚：来源书名（章节/页号）+ 键位提示，左右同 PAD、同一条视觉中心线
    // （分隔线 + footer_h/2，24px 中文光学下沉 2px）。深睡不保留，只读快照。
    // Footer: source title (chapter/page) + key hints, both at PAD margins on
    // one shared centre line (separator + footer_h/2, with a 2px optical drop
    // for 24px CJK). Nothing persists, so this only reads the snapshot.
    ui_hairline(fb, h - footer_h, pad, body_w, 0x70);
    const int foot_cy = h - footer_h + footer_h / 2 + 2;
    static const char k_hint[] = "翻页键翻页 · 返回键关闭";
    const int hint_w = ui_text_fixed_width_px(24, k_hint);
    char foot[192];
    const ai_reader_source_t* src = &s_session.source;
    if (src->title[0]) {
        snprintf(foot, sizeof(foot), "摘自：%s · 第%u章 第%u页",
                 src->title, (unsigned)(src->chapter + 1), (unsigned)(src->page + 1));
    } else {
        snprintf(foot, sizeof(foot), "摘自：未知书目");
    }
    // gen26.6.3：左段必须给右段让位——书名可以很长（96 字标题 ≈ 1500px），旧版直接
    // 整段画、与右侧键位提示叠字（真机页脚一团）。按可用宽度截短，字符边界回退。
    // gen26.6.3: the left block must yield to the right one - a title can be very long
    // (a 96-char title is ~1500px) and the old code painted it whole, over the key
    // hints (the on-device footer was an overlapping mess). Clamp to the available
    // width, backing off to a character boundary.
    const int foot_avail = body_w - hint_w - 24;
    while (foot[0] && ui_text_fixed_width_px(24, foot) > foot_avail) {
        size_t n = strlen(foot);
        if (!n) break;
        size_t cut = n - 1;
        while (cut && ((unsigned char)foot[cut] & 0xC0) == 0x80) --cut;
        foot[cut] = 0;
    }
    ui_text_fixed_vc(fb, pad, foot_cy, 24, foot, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, pad + body_w, foot_cy, 24, k_hint, EPD_DRAW_ALIGN_RIGHT, false);
}
