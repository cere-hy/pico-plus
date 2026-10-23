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
void app_home_cover_mode_changed(void);
/// 开书前释放可重建的表盘底图，让解析器使用这部分 PSRAM。/ Release rebuildable clock bases before opening a book so parsing can use that PSRAM.
void app_home_release_clock_cache(void);
