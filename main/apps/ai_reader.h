/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * gen26.5 M2 Step9：AI 释义「详解阅读视图」会话层。
 * The gen26.5 M2 Step9 AI-definition detail reading view: the session layer.
 *
 * 形态决断（设计记录，违反即回退）：详解是 app_loop 输入链上的**覆盖会话**，
 * 不是一本书。换书路线（app_book 的 open_book_impl）会在 load_chapter_at 里
 * free_book 释放原书的 UTF-8 文本与页表（s_text = loaded.utf8），直接违反
 * 立项令 §3.4 铁律②「原书排版资源不卸载（章节缓冲保留），关闭详解 = 原书
 * 当前页重呈现，阅读位置零丢失」。覆盖层里原书的 turn_page 根本不会被触发，
 * 铁律①「按键语义隔离」天然成立。
 * Shape decision (design record - violating it is a rollback): the detail view
 * is an overlay session on app_loop's input chain, not a book. The book-swap
 * route (app_book's open_book_impl) frees the source book's UTF-8 text and page
 * map in load_chapter_at (s_text = loaded.utf8), breaking §3.4 law 2 - the
 * chapter buffer must survive, and closing the view must re-present the source
 * page with zero reading-position loss. In an overlay the source book's
 * turn_page is never reached, so law 1 (key isolation) holds by construction.
 *
 * 「复用 vocab_log render 虚拟书机制」在产品语义层兑现：超容量长文由阅读视图
 * 承载（词卡管快、阅读视图管全，与生词本视图同构），不在查词热路径上开书。
 * Reusing vocab_log render's virtual-book mechanism lands at the product
 * semantics level: oversized long text is carried by a reading view (the card
 * is quick, the view is complete - same split as the vocabulary book), and no
 * book is opened on the lookup hot path.
 *
 * 五设计点落点：
 * Five design points and where they land:
 * 1. 键隔离 / key isolation - app_loop 在 owns_keys 直达与手势分发前拦截；
 *    turn() 在首/末页停滞（APP_REDRAW_NONE），不自动退出。
 *    app_loop intercepts before the owns_keys direct route and gesture
 *    dispatch; turn() stalls at both ends without auto-exiting.
 * 2. 回原位 / return in place - 会话不开书，原书章节缓冲原地保留；关闭即重呈现。
 *    The session opens no book; the source chapter buffer stays put.
 * 3. 双 GC16 / double GC16 - open/close 各由调用方 present APP_REDRAW_FULL
 *    （均衡配置下即整屏 GC16，见 app_config.h）。
 *    Callers present APP_REDRAW_FULL for open and close (GC16 under the
 *    balanced profile).
 * 4. 息屏唤醒回详解 / wake back into the view - 会话态是模块级 static，锁屏
 *    不碰它；唤醒后 current->render 走详解分支（app_book.render 的钩子）。
 *    The session is module-static; locking never touches it, and wake
 *    re-renders through app_book.render's hook.
 * 5. 深睡不保留 / not kept across deep sleep - 无任何持久化 API 调用（宿主
 *    测试断言 nvs/sd 桩零调用）；深睡丢 static，开机恢复记录始终是原书。
 *    No persistence API is ever called (the host test asserts zero nvs/sd
 *    stub calls); deep sleep drops the statics and boot resume stays the
 *    source book.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app.h"

/// 词与释义的截断上限。释义按字节存，**按协议传输槽同数**——详解/情景/选段
/// 三个视图共用这一具会话缓冲，段落解析（bit2）也要完整落进来，任何低于传输槽
/// 的取值都会让答案在最后一跳缩水。
/// Truncation caps. The definition is stored in bytes and matches the transport
/// slot: the detail, scene and passage views share this one session buffer, and a
/// passage answer must land whole - anything under the slot shrinks it at the last hop.
#define AI_READER_WORD_MAX 48
#define AI_READER_DEF_MAX 8192
#define AI_READER_TITLE_MAX 96

/// 页表上限。8,192B 释义按最坏情况（全 ASCII、每行 20 字）约 400 行 ≈ 25 页，
/// 64 页仍有一倍余量。/ Page map: 8,192B worst case is ~400 lines ~= 25 pages;
/// 64 still doubles it.
#define AI_READER_MAX_PAGES 64

/// 来源书页快照句柄：关闭详解时校验并重呈现原书当前页；会话期间原书状态
/// 不动，快照同时用于页脚标注（读的是哪本书哪一页的详解）。
/// Source page snapshot handle: validated on close to re-present the source
/// page; the source state never moves during the session, and the snapshot
/// doubles as the footer caption (which book and page this detail belongs to).
typedef struct {
    size_t chapter;
    size_t page;
    size_t page_start_offset;
    char title[AI_READER_TITLE_MAX];
} ai_reader_source_t;

/// 打开详解会话。空词或空释义不开（返回 false，调用方照旧走词卡）。
/// Open a detail session. An empty word or definition opens nothing (false;
/// the caller keeps the card path).
bool ai_reader_open(const char* word, const char* def, const ai_reader_source_t* source);

/* ---------------- M3-B 情景视图 / M3-B scene view ---------------- */

/// 会话形态。DEF = 详解（整屏、词标题、来源页脚）；SCENE = 情景卡（上延、
/// 引用条代替标题、页脚标「情景」）。**同一套分页与装行**，只有几何与外框不同。
/// Session form. DEF = detail (full screen, word title, source footer); SCENE =
/// the scene card (extended upward, a quote strip instead of the title, a scene
/// footer). One pagination/wrap engine, only the geometry and chrome differ.
typedef enum {
    AI_READER_MODE_DEF = 0,
    AI_READER_MODE_SCENE = 1,
} ai_reader_mode_t;

/// 版心几何（设备像素）。top/bottom 是正文可用区间，left/width 是行盒。
/// 详解用缺省值（236 .. h-72）；情景卡由 app_book 注入（上沿 236、下沿扣动作栏）。
/// Body geometry in device pixels: top/bottom bound the text band, left/width the
/// line box. DEF uses the built-in default (236 .. h-72); the scene card is
/// injected by app_book (top 236, bottom above the action bar). NULL restores the
/// default - the detail path stays pixel-identical.
typedef struct {
    int top;
    int bottom;
    int left;
    int width;
} ai_reader_geom_t;

/// 情景卡正文上沿（设备像素）。情景卡是**整屏视图**（自己清屏 + 画状态栏），
/// 等价于「全屏非沉浸」阅读页——其正文顶就是 `body_rect_for_tracking()` 的 96
/// （状态栏下）。**不是**详解的 236：236 的语义是「标题区之后」，情景卡没有标题区，
/// 照抄就会留下 0..236 的白板。命名常量把值与它的语义锚绑在一起。
/// Scene card body top (device px). The card is a full-screen view (it clears the
/// panel and draws the status bar), equivalent to a "fullscreen, non-immersive"
/// reading page, whose body top is 96 (the reader's own anchor, under the status
/// bar). NOT the detail view's 236 - 236 means "after the title zone", and the
/// scene card has no title zone; copying it left 0..236 blank white. The named
/// constant binds the value to its anchor.
#define AI_READER_SCENE_TOP 96

/// 注入版心几何；NULL 恢复详解缺省。只影响绘制与分页，不影响会话内容。
/// Inject the body geometry; NULL restores the detail default. Affects painting
/// and pagination only, never the session content.
void ai_reader_set_geometry(const ai_reader_geom_t* g);

/// 引用条整句上限（UTF-8 字节）。M3-A 的上下文是 1024B，引用条只取「词所在这
/// 一句」，超过上限时保词居中截断。
/// Quote-sentence cap in UTF-8 bytes. The M3-A context is 1024B; the strip only
/// shows the sentence around the word, truncating around the word past the cap.
#define AI_READER_QUOTE_MAX 384

/// 引用条浅底与目标词墨色（8 位灰、仅高 4 位有效：值越大越亮）。0xE0 量化后为
/// nibble 14（≈93% 白），是仍非纯白的最高档（0xF0+ 归纯白）；既标出「来自书里」
/// 的分区，又不与正文黑字抢对比度。曾被误当「极浅底」的 0x0A 量化后其实是纯黑
/// ——「黑底黑字」事故的直接来源。宿主守卫与绘制共用这两个常量，避免裸数值漂移。
/// Quote-strip wash and target-word ink (8-bit gray, high nibble only: larger is
/// lighter). 0xE0 quantises to nibble 14 (~93% white), the lightest value still
/// not pure white (0xF0+ collapses to white). A light provenance zone that never
/// fights the black body text. The 0x0A once mistaken for a light wash actually
/// quantises to pure black - the direct cause of the black-on-black incident. The
/// host guard and the painter share these two, so no bare value can drift.
#define UI_QUOTE_BAND_BG 0xE0
#define UI_QUOTE_WORD_INK 0x00

/// 打开情景会话。text 是四节分析正文（0x08 回流），quote/off/len 是引用条来源
/// （M3-A 的上下文快照与词偏移——**与 0x1A 发出时同一份数据**，不重新取）。
/// 空词或空正文不开（false，调用方保留词卡）。
/// Open a scene session. text is the four-section analysis (the 0x08 return);
/// quote/off/len feed the quote strip from the M3-A context snapshot and word
/// offset - the very same data that was sent in 0x1A, never re-derived. Empty
/// word or text opens nothing (false; the caller keeps the card).
bool ai_reader_open_scene(const char* word, const char* text,
                          const char* quote, size_t quote_word_off, size_t quote_word_len,
                          const ai_reader_source_t* source);

/// 打开**整段选文**会话（bit2）：title 是「选段」，quote 是整段选文（不做句界
/// 取景、不高亮词——引用条就从选段抬头开始铺，装不下的尾部由 tail_cut 标出）。
/// 与词条共用同一套会话/版面，差别只在取景与标题：段落分析也是四节正文。
/// Open a whole-passage session (bit2): the title is 选段 and the strip holds the
/// selection itself - no sentence extraction, no word highlight, with the tail cut
/// flagged. Same session and layout as the word case; only framing and title differ.
bool ai_reader_open_excerpt(const char* title, const char* text, const char* quote,
                            const ai_reader_source_t* source);

/// 当前形态；无会话时为 DEF。/ Current form; DEF when no session is active.
ai_reader_mode_t ai_reader_mode(void);

/// 引用条纯函数：在 ctx 里以 [word_off, word_off+word_len) 为锚向两侧扩到句界
/// （。！？；!?;\n 或 ctx 边界），超过 max_chars 时保词居中截断；返回句长，
/// out_off/out_len 是 ctx 内字节区间。句界找不到时退回「词前后各 N 字」并把
/// *fallback 置 true（NULL 允许）。
/// Quote-sentence pure function: anchor [word_off, word_off+word_len) inside ctx,
/// expand both ways to a sentence boundary (。！？；!?;\n or the ctx edge), and
/// truncate around the word past max_chars. Returns the sentence length;
/// out_off/out_len are the byte span inside ctx. When no boundary is found it
/// falls back to N chars on each side and sets *fallback (NULL allowed).
size_t ai_reader_quote_sentence(const char* ctx, size_t ctx_len,
                                size_t word_off, size_t word_len,
                                size_t max_chars, size_t* out_off, size_t* out_len,
                                bool* fallback);

/// 情景卡底部「存生词 ›」chip 的矩形（绘制与命中共用）；非情景会话返回零矩形。
/// The scene card's bottom "save word" chip rect (shared by paint and hit-test);
/// a zero rect outside a scene session.
EpdRect ai_reader_scene_save_rect(void);

/// 标记当前情景词的生词本状态（开卡时由调用方按 vocab_log_contains 置初值，
/// 存成功后置 true）。只改 chip 文案，不写任何记录——写库是调用方的事。
/// Mark whether the scene word is in the vocabulary book (the caller seeds it from
/// vocab_log_contains when opening and sets true after a successful save). Only the
/// chip label changes; nothing is written here - persisting is the caller's job.
void ai_reader_set_scene_saved(bool saved);

/// 关闭会话并清空状态。/ Close the session and clear its state.
void ai_reader_close(void);

/// 会话是否活跃。/ Whether a session is active.
bool ai_reader_active(void);

/// 绘制详解页到帧缓冲（不推屏）。app_book.render 在 READING 态 + 会话活跃时
/// 整分支早退交给它：版面数值（居中/留白/间距）每次按当前方向实算，分页与
/// 所见一致。返回 void——推屏档位由调用方（app_loop 的 present）决定。
/// Paint the detail page into the framebuffer (no push). app_book.render's
/// READING + active branch early-returns into it: layout numbers (centring,
/// margins, spacing) are computed per current rotation on every paint, so
/// pagination matches what is on screen. Returns void - the present tier is
/// the caller's (app_loop's) decision.
void ai_reader_render(uint8_t* fb);

/// 翻页：dir<0 上一页、dir>0 下一页。首/末页停滞返回 APP_REDRAW_NONE，
/// 页码不变——铁律①「末页再按下一页停在末页不自动退出」。
/// Turn: dir<0 previous, dir>0 next. Both ends stall with APP_REDRAW_NONE and
/// no page change - law 1's "stay on the last page, never auto-exit".
app_redraw_t ai_reader_turn(int dir);

/// 当前页码（0 起）与总页数。/ Current page (0-based) and total page count.
int ai_reader_page(void);
int ai_reader_page_count(void);

/// 会话内的词与来源快照（只读）。/ The session's word and source snapshot.
const char* ai_reader_word(void);
const ai_reader_source_t* ai_reader_source(void);

/// 26.11.4：本会话是不是**整段选文**（bit2）会话。/ Whether this is a passage session.
///
/// 为什么需要它：段落视图的保存动作与词卡不是一回事——词卡存「词 + 词典释义」进
/// 生词本，段落存「对照那一节」进摘抄本。同一个 chip 位置，两种语义；不区分就会
/// 让第二个死键长出来（第一版的「存生词」在段落页点了 6 次、0 次落库）。
/// Why it exists: saving a card and saving a passage are different acts in the same
/// chip slot. Not distinguishing them is exactly how the dead key was born.
bool ai_reader_is_excerpt(void);

/// 会话里的解析正文全文（详解/情景/选段通用，NUL 结尾）。整段视图的「存摘抄」
/// 要在这里面找【原文对照】一节，所以它必须整篇给出，不是分页后的可见片段。
/// The whole answer text of the session. The passage view's save looks for the
/// 原文对照 section inside it, so it needs the full text, not the current page.
const char* ai_reader_def(void);

/// 整段会话的**选段原文全文**（非整段会话返回空串）。引用条只有 384B，装不下
/// 一段 963B 的英文；摘抄的中文兜底要存原文，读的就是这里。
/// The full selection of a passage session (empty otherwise). The strip buffer is
/// 384 B and cannot hold a 963 B English paragraph; the Chinese fallback saves the
/// original, and this is where it reads from.
const char* ai_reader_excerpt_text(void);

/// 详解页的排版样式：**全部取自当前阅读正文**，不另立一套度量。
/// 字号 = 阅读设置字号；行距 = 字号 × 行距% / 100（与 book_layout 同一公式）；
/// left/width = 正文版心（body_rect，已按整字格收齐）。app_book 每次绘制前注入。
/// The detail view's type style - every field comes from the LIVE reader body, so
/// there is no second set of metrics: px is the reading size, line_h is the same
/// px*spacing%/100 the book layout uses, and left/width are the body rect (already
/// snapped to whole glyph cells). app_book injects it before each paint.
typedef struct {
    int px;     ///< 正文字号 / reading font px
    int line_h; ///< 正文行距 / reading line pitch
    int left;   ///< 版心左缘 / body left edge
    int width;  ///< 版心宽 / body measure
} ai_reader_style_t;

/// 注入排版样式；NULL 取消注入（回落到内置安全默认，仅调试/宿主场景）。
/// Inject the style; NULL clears it (falls back to a safe default, debug/host only).
void ai_reader_set_style(const ai_reader_style_t* style);

/// 情景卡版心：上沿 AI_READER_SCENE_TOP、下沿扣 48px 动作栏，左右与版心取注入
/// 样式。app_book 用它构造注入几何；宿主测试直接调它——注入规则因此可被变异命中
/// （若把规则留在 app_book 里，宿主测试根本碰不到，变异就是假门禁）。
/// Scene geometry: top AI_READER_SCENE_TOP, bottom above the 48px action bar,
/// left/measure from the injected style. app_book builds the injected geometry
/// from it, and the host test calls it directly - so the rule is mutation-visible.
/// (Kept in app_book it would never be reached by the host test: a fake gate.)
ai_reader_geom_t ai_reader_scene_geom(const ai_reader_style_t* style, int screen_h);

/// 版面参数（正文版心像素宽、正文字号、每页行数）。render 每次绘制前按当前
/// 方向实算后设置；turn/页码查询用同一组参数保证分页与所见一致。参数变化才重排。
/// 装行按「字号推出来的真实字宽」累加（见 ai_reader_advance_fn），不再用
/// 「每行几个全角」的理想化单位——度量与绘制必须同源。
/// Layout metrics (body measure in px, body font px, lines per page). render
/// computes them for the current rotation before every paint; turns and page
/// queries reuse them so pagination matches the screen. Repaginate only on
/// change. Line breaking sums real glyph advances (see ai_reader_advance_fn)
/// instead of an idealised "full-width slots per line" - measure must equal paint.
void ai_reader_set_layout(int width_px, int text_px, int lines_per_page);

/// 逐字前进宽度回调（宿主测试注入假宽表；设备侧默认走 ui_text_reader_advance_px）。
/// 传 NULL 恢复默认。px 为该次排版的字号。
/// Per-code-point advance callback (host tests inject a width table; the device
/// defaults to ui_text_reader_advance_px). NULL restores the default.
typedef int (*ai_reader_advance_fn)(void* ctx, uint32_t codepoint, int px);
void ai_reader_set_advance(ai_reader_advance_fn fn, void* ctx);

/// 分页纯函数：返回总页数；out_starts[i] = 第 i 页首字符在 text 里的字节偏移。
/// 规则——按真实字宽装行（宽度不够时优先退回最近空格/连字符，其次服从避头尾
/// 标点）；显式 '\n' 强制断行；空行保留占位。不写 out_starts 也能安全调用。
/// Pagination pure function: returns the page count; out_starts[i] is the byte
/// offset of page i's first character. Rules - fill by real glyph advances,
/// backing off to the nearest space/hyphen first and then to the punctuation
/// no-start/no-end rules; an explicit '\n' forces a break; empty lines keep
/// their slot. Safe with NULL/0 to count only.
int ai_reader_paginate(const char* text, int width_px, int text_px, int lines_per_page,
                       size_t* out_starts, int max_pages);

/// 取某一页的各行（指向 text 内部的偏移/长度对，不复制、不改写）。
/// Fetch one page's lines as offset/length pairs into the text (no copy, no
/// mutation). Returns the line count.
typedef struct {
    size_t off;
    size_t len;
} ai_reader_line_t;
int ai_reader_page_lines(int page, ai_reader_line_t* out, int max);
