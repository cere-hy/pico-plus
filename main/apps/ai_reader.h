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

/// 词与释义的截断上限。释义按字节存：0x08 重组上限 1024B，留余量给截断保护。
/// Truncation caps. The definition is stored in bytes: the 0x08 reassembly
/// cap is 1024B, with headroom for the truncation guard.
#define AI_READER_WORD_MAX 48
#define AI_READER_DEF_MAX 1200
#define AI_READER_TITLE_MAX 96

/// 页表上限。1024B 释义按最坏情况（全 ASCII、每行 20 字）也就几十页。
/// Page-map cap. A 1024B definition in the worst case (all ASCII, 20 chars
/// per line) is a few dozen pages.
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
