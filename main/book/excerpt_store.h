/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：摘抄落盘（S26-1）。**纯逻辑 + 一个极薄的 VFS 尾巴**，宿主可全量测试。
 * English: excerpt storage (S26-1). Pure logic plus a very thin VFS tail, fully host-testable.
 *
 * 纪律（26.9.29 审计终版，逐条对应答辩书）：
 * Discipline (26.9.29 final audit, matching the design answers one by one):
 *   1. **先写后亮**：UI 只在 fsync 返回 0 之后才许亮「已存 ✓」。本模块提供
 *      excerpt_store_pending() 与 excerpt_store_take_saved() 两个只读真值，UI 不许猜。
 *      Write first, tick second: the UI may show "saved" only after fsync returned 0.
 *      This module exposes two read-only truths - pending() and take_saved() - so the
 *      UI never has to guess.
 *   2. **不撒谎**：队列满 = 拒新（EXCERPT_FULL），不丢旧；PSRAM 申请失败 = 拒新
 *      （EXCERPT_NO_MEM）。「待落盘 N」永远是队列深度本身，不是估算。
 *      No lying: a full queue REFUSES the new entry (never drops the oldest); a PSRAM
 *      failure refuses too. "Pending N" is always the queue depth itself, not an estimate.
 *   3. **零新任务、零内部堆**：缓冲全走 PSRAM，落盘由调用方的家务片驱动（
 *      excerpt_store_tick），本模块不起任务、不起定时器、不加 .bss 大数组。
 *      No new task, no internal heap: buffers come from PSRAM and the caller's
 *      housekeeping slot drives excerpt_store_tick(). No task, no timer, no big .bss.
 *   4. **一个家务片至多落一条**：tick() 一次只处理队首一条，书架扫描永远插得进来。
 *      One entry per housekeeping slot, so a shelf scan always interleaves.
 *   5. **重试第一步恒为 mkdir**：目录被删后第一次冲账即自愈（细化账一.5）。
 *      Every retry starts with mkdir, so a deleted directory self-heals on the
 *      first flush (design item one point five).
 *   6. **路径只有一套词汇（26.9.29）**：本模块一律走 VFS/POSIX（`/sdcard/...`），
 *      与全仓 42 处 fopen 同源，**不碰裸 FatFs**。26.9.28 的现场事故就是这里把 VFS
 *      挂载点路径喂给了裸 `f_open`（只认 `"0:"`）——路径错层 + 栈上 `FIL` 未清零，
 *      点一次摘抄重启一次。这条由结构牙 B38 咬住，不靠记性。
 *      One path vocabulary only (26.9.29): this module goes through the VFS and never
 *      touches bare FatFs. The 26.9.28 field crash was exactly a VFS mount-point path
 *      handed to `f_open`, which only understands "0:" - plus an uninitialised stack
 *      FIL, whose error cleanup freed a wild pointer. Held by tooth B38.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 落盘路径。/ Where excerpts land.
/// #ifndef 与 vocab_log 同款：宿主测试用 -D 注入临时目录，设备侧默认 /sdcard。
/// The #ifndef matches vocab_log: host tests inject a temp dir, the device keeps /sdcard.
#ifndef EXCERPT_PATH
#define EXCERPT_PATH "/sdcard/.readpico/excerpts.txt"
#endif
#ifndef EXCERPT_DIR
#define EXCERPT_DIR  "/sdcard/.readpico"
#endif

/// P0-5（26.11.2）：虚拟摘抄本。渲染走 tmp → rename，成品路径不变式与生词本同族。
/// P0-5 (26.11.2): the virtual excerpt book. Render goes through tmp then rename;
/// the finished-file invariant matches the vocabulary book.
#ifndef EXCERPT_VIEW
#define EXCERPT_VIEW     "/sdcard/.readpico/excerpt_view.txt"
#endif
#ifndef EXCERPT_VIEW_TMP
#define EXCERPT_VIEW_TMP "/sdcard/.readpico/.excerpt_view.tmp"
#endif
/// 渲染上限（送审定案：超出只渲染最近 N 条并在首行注明，不做无界 PSRAM 承诺）。
/// Render cap (design ruling): beyond N only the most recent are rendered, noted on
/// the first line; no unbounded PSRAM promise.
#define EXCERPT_VIEW_MAX_RECORDS 4000u

/// 长度账（答辩书细化账 3，静态断言锁死在 .c）：书名 ≤160 + 前缀 ≈190 + 正文 700
/// + `[截]` 5 + 换行 → 单行 ≈895B，恒低于 1,024B 上限。
/// Length budget (design item three): title <= 160 + prefix ~ 190 + text 700 +
/// `[truncated]` 5 + newline = ~895 B per line, always under the 1,024 B cap.
#define EXCERPT_TITLE_MAX 160u
#define EXCERPT_TEXT_MAX  700u
#define EXCERPT_LINE_MAX  1024u

/// 队列上限（细化账 1：满则拒新）。/ Queue cap (item one: refuse when full).
#define EXCERPT_QUEUE_MAX 10u

/// 挂载成功后冲账的让行窗口（细化账一.4：躲开书架扫描）。
/// Yield window after a mount (item one point four: let the shelf scan run).
#define EXCERPT_MOUNT_YIELD_MS 5000u

/// E-1（19.3）：失败重试的固定档数。三档 {50,200,1000} ms，此后维持末档——**无递增律**。
/// 表本身在 .c（由牙逐值咬），这里只锁档数：加第四档必须同时改牙，否则自检当场红。
/// E-1: the retry scheme has exactly three fixed steps and then holds the last one. There
/// is no growth law. Only the step count lives here; the values live in the .c so a tooth
/// can read them one by one, and adding a fourth step forces the tooth to be updated.
#define EXCERPT_BACKOFF_STEPS 3u

typedef enum {
    EXCERPT_OK = 0,     ///< 已入队（未落盘）/ queued, not yet on the card
    EXCERPT_FULL,       ///< 队列满，拒新（不丢旧）/ full: refused the new one, kept all old ones
    EXCERPT_NO_MEM,     ///< PSRAM 申请失败，拒新 / PSRAM allocation failed, refused
    EXCERPT_EMPTY,      ///< 空文本/空书名，防御性拒绝 / empty text or title, defensive refusal
} excerpt_status_t;

typedef enum {
    EXCERPT_FLUSH_IDLE = 0, ///< 队列空，本片无事 / nothing queued
    EXCERPT_FLUSH_OK,       ///< 本片落盘成功（fsync 已回 0）/ one entry written and synced
    EXCERPT_FLUSH_FAILED,   ///< 本片落盘失败，条目仍在队列 / failed; the entry stays queued
    EXCERPT_FLUSH_YIELD,    ///< 挂载让行窗口内，本片不落 / inside the mount yield window
    EXCERPT_FLUSH_DROPPED,  ///< 卡上已有半行：弃本条并亮红（重试只会复制残行）/ half a line is
                            ///< already on the card, so the entry is dropped and the footer
                            ///< goes red - retrying would only duplicate it
} excerpt_flush_t;

/// 入队一条摘抄。book 可为空（取当前书名失败时按未知处理）；unix_now 为 0 时退化为
/// 当次开机递增序号（细化账 4：不扫文件、不承诺跨掉电连续）。truncated 出参告诉调用方
/// 正文是否被截（UI 走页脚提示，见 S26-2）。
/// Queue one excerpt. book may be NULL; unix_now == 0 degrades to a per-boot sequence
/// number (item four: no file scan, no cross-power continuity promise). truncated tells
/// the caller the text was cut (the UI reports it on the footer, see S26-2).
excerpt_status_t excerpt_store_enqueue(const char *book, size_t page, uint32_t unix_now,
                                       const char *text, bool *truncated);

/// 队列积压条数——「待落盘 N」的唯一真值。/ Queued entries: the only truth behind "pending N".
unsigned excerpt_store_pending(void);

/// 累计「弃」条数（只增不减，reset 不清）—— 主动丢数据的分支必须能自证次数。
/// Cumulative drops (never cleared): the branch that discards user data must be countable.
unsigned excerpt_store_dropped(void);

/// 上一条是否落盘成功——「已存 ✓」的唯一真值（先写后亮）。消费一次即清。
/// Whether the last entry landed: the only truth behind "saved". Clears on read.
bool excerpt_store_take_saved(void);

/// 家务片驱动：至多落一条。now_ms 由调用方给（不起定时器）。
/// **留队的唯一条件：卡上可见内容不含本行。** 四档失败都是这条不变式的取值——
///   ① mkdir / fopen 失败：零字节写入 → 留队重试；
///   ② 短写 / flush 失败：卡上已有半行 → 补一个 `\n` 把残行锁死在本行内 → 弃 + 亮红；
///   ③ fsync 失败：目录项长度未回写，可见内容不含本行 → 留队（拔卡主场景靠这档冲账）；
///      但若随后的 fclose 内部 f_sync 成功，这一行**已经持久**，再留队就会写重复——
///      所以第三档以 fclose 的结果收口；
///   ④ fclose 单独失败：数据已由 fsync 确认 → 算成功，只记日志（亮「已存 ✓」是诚实的）。
/// Housekeeping slot: flush at most one entry. The one invariant behind every branch is
/// "keep the entry only while the card's visible content does not contain this line".
excerpt_flush_t excerpt_store_tick(uint32_t now_ms);

/// 观测到**新的挂载代号**时调用：设定让行窗口（细化账一.4，5s 由家务片状态机计，不起
/// 软件定时器）。幂等——同一代号重复到达不重开窗口。
///
/// **为什么参数是代号而不是「挂载了」这件事**：状态位是快照，拔插快于一次轮询时
/// 「刚挂载」与「一直在」在快照上同形，边沿会整个丢失（26.9.31 现场丢了 886 ms，
/// 那一段每次家务片都在无让行状态下冲账）。代号是事件，挂载一次 +1 一次。
///
/// 调用点有两处，缺一不可：`poll_media`（最早的可知时刻）与 `excerpt_footer_tick` 首行
/// （紧邻 `excerpt_store_tick`，让观测与写卡之间无物可插，窗口归零）。
/// Call on a NEW mount epoch: start the yield window. Idempotent - the same epoch twice
/// does not restart it. The epoch (not the mounted bit) is the event; the bit is a snapshot
/// and can hide an edge entirely when a replug beats one poll interval.
void excerpt_store_mount_yield(uint32_t mount_epoch, uint32_t now_ms);

/// 清空队列（测试/切书用）。/ Drop everything (tests, book switch).
void excerpt_store_reset(void);

/// P1-2（26.11.2）：只读窥队列第 i 条（i < pending()），返回入队时格式化的整行
/// （`[书名]\tp=…\t…\t正文`）。**零拷贝、零出队**——指针指向队列槽位本身，任何
/// 冲账/reset 都会让它失效，调用方必须在同一次调用内用完。越界返回 NULL。
/// P1-2: read-only peek at queued line i (i < pending()). Zero copy, zero dequeue -
/// the pointer is the queue slot itself and any flush/reset invalidates it; use it
/// within the call. NULL beyond the end.
const char *excerpt_store_peek_line(unsigned index);

/// P0-5（26.11.2）：把 excerpts.txt 渲染成虚拟摘抄本（排序倒序、按月分章、坏行计数、
/// 待落盘节置顶）。先写 EXCERPT_VIEW_TMP 再 rename；任一步失败删 tmp、成品保持原样。
/// *out_count = 磁盘渲染条数 + 待落盘条数（0 = 写出的是空态提示书）。
/// P0-5: render excerpts.txt into the virtual excerpt book (newest first, chapters by
/// month, bad lines counted, pending section on top). tmp then rename; on any failure
/// the tmp is removed and the finished file is untouched. *out_count = rendered +
/// pending (0 = the hint-only empty book was written).
bool excerpt_store_render_view(unsigned *out_count);

/// 视图路径识别：只比文件名、精确匹配、大小写不敏感（FAT 不分大小写）——
/// 与 vocab_log 同一套三条判据。/ View path check: same three rules as vocab_log.
bool excerpt_store_is_view_path(const char *path);
const char *excerpt_store_view_path(void);

#ifdef __cplusplus
}
#endif
