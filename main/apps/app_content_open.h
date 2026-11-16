/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：文件管理与首页请求内容页打开指定文件。
 * English: File manager and home request a specific file in the content pages.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

bool app_book_request_open(const char *path);
/// gen24.4：带一次整屏 GC16 的开书（推送收官专用，第一页无残影）。/ With one full-screen GC16 underneath (push finale only, ghost-free first page).
bool app_book_request_open_clean(const char *path);
bool app_book_request_open_from_home(const char *path);
/// 一次性深睡恢复；保持保存的正文位置与全屏状态。/ One-shot deep-sleep resume with saved position and fullscreen state.
bool app_book_request_resume(const char *path, bool fullscreen);
/// gen25 让路链：预检不过时临时卸下当前书（归还 12K prep 栈与解析缓冲），推送收官后自动重开原位置。位置走既有 book_progress，不引入第二套机制。
/// Gen25 yield chain: unload the current book when the precheck fails (returns the 12K prep stack and parse buffers), then reopen it at the saved position after the push. Position uses the existing book_progress - no second mechanism.
/// 返回值 / return: true = 已就绪（无书可卸，或已卸）；false = 无法干净卸下（prep 未停 / 正在渲染）——让路链按失败处理。
/// true = ready (nothing to yield, or yielded); false = cannot unload cleanly (prep still alive / rendering) - the yield chain treats it as a failure.
bool app_book_request_yield_for_push(void);
/// 让路后的恢复：重开被卸下的书。已让路且能重开 → true；用户已开别的书或重开失败 → false（调用方必须给提示，绝不静默）。
/// Restore after the yield: reopen the unloaded book. true when it was yielded and can be reopened; false when the user opened something else or the reopen failed (the caller must speak up, never stay silent).
bool app_book_restore_after_push(void);
/// 当前是否处于「已让路、待恢复」状态。/ Whether a yield is outstanding.
bool app_book_yield_active(void);
/// gen25 接线裁定 #1：停机的**非阻塞**一半——只置标志并唤醒任务，不等它退出。调用方据此「先出提示、再分拍等」。
/// Gen25 wiring ruling #1: the **non-blocking** half of stopping - set the flag and wake the task, never wait for it to exit. The caller shows a hint first and then waits across ticks.
void app_book_request_prep_stop(void);
/// 停机的查询一半：prep 任务是否已退出（12K 栈是否已归还）。/ The query half: whether the prep task has exited (the 12K stack is back).
bool app_book_prep_stopped(void);
/// 让路触发面收窄用：当前是否真有一本书开着（无书就不必让路）。/ Narrows the yield trigger: is a book actually open (no book means no yield needed).
bool app_book_has_open_book(void);
/// gen25：让路收官把推送内容留在书架时置位——它的**首次手动打开**也要一次整屏 GC16 打底（否则首开带残影）。消费一次即止。
/// Gen25: set by the yield finale when the pushed text is left on the shelf - its **first manual open** also gets one screen-wide GC16 base (otherwise the first open ghosts). Consumed once.
void app_book_mark_push_first_open(void);
/// 仅导出已打开阅读上下文。/ Export only an already open reader context.
bool app_book_resume_context(char *path, size_t capacity, bool *fullscreen);

/* ---- 26.9.30 commit 3：读者句柄释放（机制二）/ releasing the reader's handle ---- */
// 机制二：读者正读着 push.txt 时，book_txt_open() 留在 s_book.file 的 FILE* 仍指着
// 那条簇链；提交侧的 rename/remove 把簇链还给 FAT 之后，任何 fread 都在读已释放的
// 簇（现场：乱码页 / 0x101 / 崩）。修法只有一条——提交前把书**关干净**。
// Mechanism #2: a reader sitting on push.txt still holds the FILE* left in
// s_book.file. Once the commit side renames/removes that file, the clusters are
// back in FAT and every later fread reads freed clusters (garbage pages / 0x101 /
// crash). There is one fix: close the book cleanly before the commit.
//
// 七项占用位：每位一条判据，送审与日志逐项对表（位值不重排，只许追加）。
// Seven occupancy bits, one predicate each, tabulated in review and logs.
#define BOOK_HOLD_PREP   (1u << 0)  ///< 预排版任务活着：行循环正在读 s_text / prep task alive, its row loop reads s_text
#define BOOK_HOLD_DRAW   (1u << 1)  ///< 绘制锁被占：有渲染在飞 / draw lock held, a render is in flight
#define BOOK_HOLD_TURN   (1u << 2)  ///< 正文翻页或正文帧排期中 / a body turn or body frame is queued
#define BOOK_HOLD_WATER  (1u << 3)  ///< 水墨翻页在飞 / a water-mode turn is in flight
#define BOOK_HOLD_FOOTER (1u << 4)  ///< 页脚刷新排期中 / the footer repaint is queued
#define BOOK_HOLD_BOXSEL (1u << 5)  ///< 框选进行中：选区偏移直接索引 s_text / selection active, its offsets index s_text
#define BOOK_HOLD_PANEL  (1u << 6)  ///< 阅读面板（词典/详解）开着 / a reader panel (dict/detail) is open

/// 读者是否正持有该路径（有书打开且路径相同）。/ Whether the reader holds exactly this path.
bool app_book_holds_path(const char *path);
/// 七项占用的当前快照（位值见 BOOK_HOLD_*）。无书时返回 0。
/// Snapshot of the seven occupancies (bits above); zero when no book is open.
uint32_t app_book_hold_busy(void);
/// 提交前释放读者对 path 的持有：不持有 → true；持有且能安全关 → 关书并返回 true；
/// 持有但被硬阻（prep 未停 / 渲染在飞）→ 返回 false，**本拍什么都不改**，调用方下一帧再来。
/// 有界重试的所有权在调用方（2 s 死线外必须放弃并明确回复，不许无界重投）。
/// Release the reader's hold on `path` before a commit: true when not held, or when
/// held and closed cleanly; false when a hard blocker (prep alive / render in
/// flight) says "not this frame" - nothing is touched in that case. The caller owns
/// the bounded retry and must give up loudly past its deadline.
bool app_book_release_hold(const char *path);
bool app_image_request_open(const char *path);
void app_files_request_folder(int folder);
void app_book_request_manage(void);
bool app_book_reader_body_visible(void);
/// 模式/对比度变化时释放旧格式封面；绘制前由主循环一次调用。/ Drop obsolete cover formats once on mode/contrast changes, before drawing.
void app_book_cover_mode_changed(void);
/// gen26 AI 释义卡入口：主循环从释义 UI 槽取到内容后调用。阅读态且无面板开着时
/// 暂存释义并开卡（返回 true，调用方随即呈现）；有面板开着或不在阅读态时不抢
/// 焦点（释义留在预取缓存，下次点同一词秒出）。
/// Gen26 AI definition card entry: called by the main loop after taking content
/// from the definition UI slot. While reading with no panel open it stages the
/// definition and opens the card (returns true; the caller presents right away);
/// with a panel open or outside reading it never steals focus (the definition
/// stays in the prefetch cache and the next tap on the same word shows it
/// instantly).
bool app_book_ai_def_ready(const char* word, const char* def);
/// M3-B：情景答案回流认领。必须在 app_book_ai_def_ready **之前**调用——0x08 帧
/// 没有情景标记，待命态先认领（词 + 发起页 + 期限三重匹配）；返回 true 表示这条
/// 答案已被情景视图消费，调用方**不要**再进生词批、也不要再走释义卡双门。
/// M3-B: claim a returning scene answer. Must run BEFORE app_book_ai_def_ready -
/// the 0x08 frame carries no scene mark, so the pending state claims it first by
/// word + originating page + deadline. True means the scene view consumed it: the
/// caller must NOT feed the AI vocabulary batch nor take the card double gate.
bool app_book_scene_def_ready(const char* word, const char* def);
/// M2 Step9：app_book_ai_def_ready 刚开了一次详解会话（超容量长文改由
/// ai_reader 覆盖层承载）——主循环据此把呈现档位从 PAGE 提到 FULL（双 GC16
/// 的 open 侧）。一次性消费：读过即清零，陈旧的 FULL 请求不会滞留。
/// M2 Step9: app_book_ai_def_ready just opened a detail session (an oversized
/// long definition routed to the ai_reader overlay) - the main loop lifts the
/// present tier from PAGE to FULL for it (the open side of the double GC16).
/// One-shot: read-and-clear, so a stale FULL request never lingers.
bool app_book_ai_def_full(void);
/// M2 Step9 谓词：详解会话正活跃（READING 视图上的覆盖层）。主循环用它把
/// KEY_2 短按从「中键菜单」豁免成「直达 app 关详解」——defer_middle_short
/// 此时必须让位，否则菜单会盖住详解页。输入隔离主体在 app_book 各入口。
/// M2 Step9 predicate: a detail session is active (the overlay on READING).
/// The main loop uses it to exempt KEY_2 short-press from the middle-key menu
/// so it reaches the app to close the view - defer_middle_short must yield
/// here, or the menu would cover the detail page. The input isolation itself
/// lives in app_book's entry points.
bool app_book_ai_detail_active(void);
void app_home_cover_mode_changed(void);
/// 开书前释放可重建的表盘底图，让解析器使用这部分 PSRAM。/ Release rebuildable clock bases before opening a book so parsing can use that PSRAM.
void app_home_release_clock_cache(void);
