/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：开机黑匣子（26.9.15 B1，26.9.17 重新启用）。把开机前 20 秒的**内部堆水位**
 * 先存进 PSRAM，等 USB 连上再吐出来——因为这台机器的日志通道会在 RST 时物理消失。
 * English: boot blackbox (26.9.15 B1, re-armed in 26.9.17). Records the internal heap
 * watermark over the first 20 seconds into PSRAM and spills it once USB is back - this
 * board's log channel physically disappears on reset.
 *
 * 为什么需要它（三条都查过，不是推测）：
 *   1. `CONFIG_ESP_CONSOLE_UART_NUM=-1` + `SECONDARY_NONE=y`：日志只有 USB-Serial-JTAG
 *      一条通道，RST 后主机侧 COM 口随之消失；
 *   2. `usb_storage.c` 里 SD 卡的 `d0 = GPIO_NUM_44`，而 GPIO44 是 U0RXD——
 *      「接个 USB-UART 桥抓 UART0」这条路被 TF 卡占死了；
 *   3. `usb_serial_jtag_vfs.c` 的 write 在未连接时直接 `errno=EIO; return -1`，
 *      **一个字节都不进 FIFO**，所以那些日志不是「丢了」，是从未写出。
 * 结果：真机上永远看不到 `app_main` 第一秒，而「吃掉 10 KB 的那个分配」恰好在那里。
 *
 * Why it exists (all three verified, none inferred):
 *   1. Console is USB-Serial-JTAG only (UART disabled); the host COM port vanishes on reset.
 *   2. The SD card owns GPIO44, which is U0RXD - an external UART bridge cannot be wired in.
 *   3. The VFS write path returns EIO without touching the FIFO when disconnected, so those
 *      lines were never emitted at all.
 *
 * 设计三条硬约束（审计 26.9.15 裁定）：
 *   A. **缓冲必须放 PSRAM**。放 .bss 就从池 A 里吃掉 1 KB——为了测 21,504 而把它变成
 *      20,480，数据当场作废。拿不到 PSRAM 就**整个关掉**，绝不降级到内部 RAM。
 *   B. **只存数字，不存文本**。20 个点的文本约 16 KB；数字每点 16 B。打印时再格式化。
 *   C. **连上就吐，不定死 4 秒**。实测 RST→COM7 回来约 4 s，正好卡在边界上；等多久
 *      取决于用户的手速，不取决于固件。
 *
 * Three hard constraints from the 26.9.15 ruling:
 *   A. The buffer MUST live in PSRAM. In .bss it would eat 1 KB out of pool A - measuring
 *      21,504 by turning it into 20,480 invalidates the measurement. No PSRAM, no blackbox;
 *      never fall back to internal RAM.
 *   B. Numbers only, never text: 20 text points cost ~16 KB, a numeric point costs 16 B.
 *      Formatting happens at print time.
 *   C. Spill when the host is heard, not at a fixed 4 s: the observed reconnect is ~4 s,
 *      right on the boundary, and the wait depends on the user's hands, not the firmware.
 *
 * ⚠️ 26.9.16 起整个模块由 `CONFIG_PICO_BOOTBB` 控制，**默认 n**。原因写在 Kconfig 的
 * help 里：26.9.15 把 `pico_bootbb_start()` 插在 `app_main` 第一条，真机上 TF 卡
 * 完全不挂载。关掉时下面全部退化成空 inline，**一个字节代码、一个字节 RAM 都不生成**。
 * ⚠️ Since 26.9.16 the whole module sits behind `CONFIG_PICO_BOOTBB`, **default n**,
 * because 26.9.15 called start() before usb_storage_phy_init() and the field unit stopped
 * mounting the TF card. With the switch off every entry point below is an empty inline:
 * zero bytes of code, zero bytes of RAM.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif
#ifndef CONFIG_PICO_BOOTBB
#  define CONFIG_PICO_BOOTBB 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 事件码（26.9.16 R2）。放在开关外面，因为调用点不该被 `#if` 包两层——常量本身零成本。
// Event codes (26.9.16 R2). Defined outside the switch so call sites do not need their own
// `#if`: the constants themselves cost nothing.
enum {
    PICO_BOOTBB_NOTE_SD_STATE = 1,  // poll_media 看到的状态 / what poll_media observed
    PICO_BOOTBB_NOTE_SD_PROBE = 2,  // 发起探测 / a probe was started
    PICO_BOOTBB_NOTE_SD_DONE  = 3,  // 探测结束 / a probe finished
    // 26.9.18 S3：挂载内部的三段耗时。`a` 的位域（26.9.20 S12 定稿）：
    //   低 3 位 = 阶段号（1 host / 2 进 VFS 挂载 / 3 挂载返回）
    //   0x08   = **进入点**（还没有结果，不许判 ok/FAIL）
    //   0x80   = 成功（只有带结果的点才置）
    // `b` = 距本次 `read_pico_sd_start_probe()` 的毫秒数（26.9.20 S12/D4：以前存的是
    // **绝对**毫秒的低 16 位，142.6 s 处回绕成 11,557，读日志的人会把它当成 11.5 秒）。
    // 26.9.18 S3: the three stages inside a mount. `a` bit layout (finalised in 26.9.20
    // S12): low 3 bits = stage, 0x08 = entry point (no result yet, never ok/FAIL),
    // 0x80 = success (only on points that carry a result). `b` = milliseconds since THIS
    // probe started. It used to be the low 16 bits of the ABSOLUTE millisecond count, which
    // wrapped at 142.6 s into 11,557 and read like "11.5 s" to anyone doing arithmetic.
    PICO_BOOTBB_NOTE_SD_STAGE = 4,
    // 26.9.20 S9(b)：挂载后的空间统计（f_getfree）。`a` = 1 表示成功，`b` = 距本次
    // `start_probe()` 的毫秒数。它证明「4.5 s 里的那 3.5 s 已经挪出关键路径」——
    // 没有这条，读者只能看到 `Mounted` 之后安静了几秒，看不出那几秒在干什么。
    // 26.9.20 S9(b): the post-mount free-space scan. `a` = 1 on success, `b` = ms since this
    // probe started. It is the proof that the 3.5 s left the critical path.
    PICO_BOOTBB_NOTE_SD_SPACE = 5,
    // 26.9.29：摘抄落盘「弃」。`a` = errno 低 8 位，`b` = 本开机累计弃条数。留这一条是
    // 因为「弃」是唯一一种**主动丢掉用户数据**的分支：它必须在本机自证发生过几次，
    // 而不是只在串口那一行里出现过一次。
    // 26.9.29: an excerpt was dropped after a partial write. `a` = low byte of errno,
    // `b` = drops so far this boot. Dropping is the only branch that throws user data away
    // on purpose, so it has to leave a count, not just one line on a serial port.
    PICO_BOOTBB_NOTE_EXCERPT_FAIL = 6,
    // 26.9.31（审计裁决 1，DMA 盲区）：**派生**事件，调用点不用管。bootbb 采样一直只看
    // `MALLOC_CAP_INTERNAL`，而 SD driver 要的是 `MALLOC_CAP_DMA`——「总堆还剩 11 KB」
    // 说明不了 DMA 子池还剩几块。26.9.31 真机上 `allocate_dma_buf: not enough mem (0x101)`
    // 出现 34 次，其中 9 次在 166 s（20 s 采样窗之外），只能靠事件级出口看见。
    // 本码由 `pico_bootbb_note()` 在 SD 事件点自动派生：`a` = largest/256（饱和 255），
    // `b` = 原事件码 << 8 | 原 `a`。只在 DMA 最大空闲块**创新低**时记一条，
    // 健康路径零占用——要的是「观测到的饥饿」，不是「饥饿自己报出来的观测」。
    // 26.9.31 (audit ruling 1, the DMA blind spot): a DERIVED event; call sites need not
    // care. Sampling only ever watched MALLOC_CAP_INTERNAL while the SD driver asks for
    // MALLOC_CAP_DMA - "11 KB of heap left" says nothing about the DMA sub-pool. On the
    // 26.9.31 field unit `allocate_dma_buf: not enough mem (0x101)` fired 34 times, 9 of
    // them at t=166 s (outside the 20 s window), so an event-level exit is the only way to
    // see it. Derived automatically at SD event points: `a` = largest/256 (saturated),
    // `b` = original code << 8 | original `a`. Stored only on a NEW LOW, so a healthy run
    // costs nothing - we want an observed shortage, not a shortage that reports itself.
    PICO_BOOTBB_NOTE_SD_DMA = 7,
    // 26.9.30 commit 5（v36 §四的悬空项，审计 2026-10-31 要求出包前落纸）：推送写包
    // 的首写失败。`a` = errno 低 8 位，`b` = 本开机累计次数（含被重试或欠账救回的）。
    // 与 EXCERPT_FAIL 的区别：那条记「已经丢掉的数据」，这条记「第一次没写进去」——
    // 后者多数被救回来了，但它才是卡在变坏的早期信号：救回来 20 次的卡和 0 次的卡
    // 是两种卡，而串口只能证明「这一篇推完了」。
    // 26.9.30 commit 5 (the dangling item of v36 §四): the first write failure of a
    // push packet. `a` = low byte of errno, `b` = failures so far this boot (rescued
    // ones included). Unlike EXCERPT_FAIL (data already lost), this counts "did not
    // go in the first time" - mostly rescued, yet the earliest sign of a dying card:
    // a card rescued 20 times and one rescued never are different cards, and the
    // serial log only ever proves "this article finished".
    PICO_BOOTBB_NOTE_PUSH_WRITE_FAIL = 8,

    /// 31b 第 1 条：三拍封送的冻结看门狗强制解冻。`a` = 冻结毫秒数低 8 位，
    /// `b` = 高 16 位。三拍区间正常 ≤ 9 s，20 s 还没到终态说明终态枚举漏了项或
    /// 流程卡死——这是「只有重启能出」那一族问题的早期信号（31a 五环死锁的教训）。
    /// The freeze watchdog's forced thaw. Normally the three-beat ends within 9 s;
    /// 20 s without a terminal state means a missing branch or a stuck flow.
    PICO_BOOTBB_NOTE_PUSH_FREEZE = 9,

    /// 31b 第 1 条补账（审计 2026-11-01）：**进入 hold 的留痕**。
    /// 三拍封送的「全败」终态是有意保持冻结（毒卷不放开），但此前只有一行 ESP_LOGW——
    /// 那次会话的串口日志一旦丢了，下次开机查「push 为什么死了」就完全没有依据。
    /// 「失败而不留痕」同族，所以进黑匣子：`a` = 原因码（见 push_sink.h 的
    /// push_hold_why_t），`b` = 当时仍未关闭的 FILE* 数（hold 时还剩几个句柄，最能说明
    /// 卡在哪一拍）。
    /// Entering a deliberate hold. The all-fail terminal state keeps the freeze on purpose,
    /// but only a log line recorded it - lose that session's UART capture and the next boot
    /// has no way to answer "why is push dead". `a` = reason code, `b` = FILE* still open.
    PICO_BOOTBB_NOTE_PUSH_HOLD = 10,

    /// 31b P1-1：**掉盘的第二个信源**——CD 脚说「在」，I/O 说「不在」。
    /// 现场 26.9.31 的 present 在 888/892/923/927 之间来回跳，是因为「卡没了」只由
    /// 一个无去抖的 CD 采样回答。本码记录判定时刻：`a` = 连续 I/O 失败次数（饱和 255），
    /// `b` = 从第一次失败到判决的毫秒数（截断 16 位）。**为什么值得 critical**：这条
    /// 只在「CD 与 I/O 打架」时出现，而那个状态意味着卷已经不可信、马上要卸载——
    /// 环满时它必须活下来，否则下次现场只有一行 `err=ESP_ERR_INVALID_STATE`，
    /// 看不出是谁先说话。
    /// P1-1: the second source of truth for card removal. `a` = consecutive I/O failures,
    /// `b` = ms from first failure to the verdict. Critical because this only fires when
    /// CD and I/O disagree, which is exactly when the volume is about to be dropped.
    PICO_BOOTBB_NOTE_SD_IO_DEAD = 11,
};

#if CONFIG_PICO_BOOTBB

/// 26.9.20 S11：128 → **512**。26.9.19 真机上 128 点在 **172.6 s** 写满，此后 300 秒
/// 的数据全被丢弃——而第二次开书（326.6 s）、蓝牙（301 s）、第三轮拔插（173 s）全在
/// 那个窗口里，于是「18,432 是谁造成的」根本没有数据可查。512 点在同样的变化密度下
/// 覆盖约 690 s（11 分钟），代价是 PSRAM 8 KB，池 A 依旧零字节。
/// 512. On the 26.9.19 unit the 128-point ring filled at t=172.6 s and everything after it
/// was dropped - and the second book open (326.6 s), the BLE session (301 s) and the third
/// reinsert (173 s) all sat inside that blind window, so "who made it 18,432" had no data
/// at all. 512 points cover about 690 s at the same change density for 8 KB of PSRAM.
#define PICO_BOOTBB_POINTS 512

/// 26.9.20 S11：16 → **64**。26.9.19 真机上 16 条在 160 s 写满，之后每次拔插的三段
/// sd-stage 全被丢弃——S3 的打点第二次白做。
/// 64. The 16-note ring filled at 160 s, after which every reinsert's three sd-stage notes
/// were dropped - S3's instrumentation wasted a second time.
#define PICO_BOOTBB_NOTES 64

/// 26.9.20：一次 spill 最多打几行点。512 点一次性吐出去约 36 KB 文本，串口阻塞会把
/// 主循环按住好几秒（26.9.19 实测 30～90 行就能让 `I()` 时间戳漂 1.2～1.7 s）。
/// 分块后每次最多 64 行，剩下的下一个 poll（500 ms）继续——**证据不会丢，只是分几口吐**。
/// Max point lines per spill call. Dumping 512 points at once is ~36 KB of text and blocks
/// the main loop for seconds (26.9.19 measured 1.2-1.7 s of timestamp drift from only
/// 30-90 lines). Chunked to 64 lines per call, the rest follows on the next 500 ms poll.
#define PICO_BOOTBB_SPILL_LINES 64

/// 采样周期（µs）。26.9.18 由 200 ms 降到 **50 ms**：200 ms 抓不住连续分配，
/// 而 26.9.17 真机上「开书清场」那 1 KB 是在 **1 毫秒内**掉的，200 ms 只能看到
/// 「有过这么一回事」，看不到它和谁相邻。50 ms 会让 heap 锁每 50 ms 被摸一次，
/// 单次几微秒，代价可忽略。
/// Sampling period. Lowered from 200 ms to 50 ms in 26.9.18: 200 ms cannot resolve
/// consecutive allocations, and the field unit dropped its 1 KB in ONE millisecond.
/// 50 ms touches the heap lock for a few microseconds at a time - negligible.
#define PICO_BOOTBB_PERIOD_US 50000

/// 首次允许打印的时刻（µs）。实测 RST→COM7 回来约 4 s，在这之前打也是白打。
/// Earliest instant we may print. The measured RST→COM7 reconnect is about 4 s; printing
/// before that would be writing into a disconnected port.
#define PICO_BOOTBB_FIRST_US 4000000LL

/// 放弃等待的时刻（µs）。60 s 还没人听就记一行 GAVE UP——但缓冲不释放，`replay()` 仍可用。
/// Give-up instant. With nobody listening for 60 s we log one GAVE UP line, but keep the
/// buffer so `replay()` still works.
#define PICO_BOOTBB_GIVEUP_US 60000000LL

/// 心跳间隔（ms）。26.9.19 S5-d 新增：没有新数据时每 20 s 报一次「采样器还活着」，
/// 带 `sampled`（采样次数）与 `stored`（落点数）。26.9.18 的「t=140 s 之后安静 104 秒」
/// 之所以定不了因，就是因为「缓冲满了」「堆没变」「定时器停摆」在日志里长得一样。
/// Heartbeat interval. Since 26.9.19 every 20 s of silence reports `sampled` (ticks) and
/// `stored` (points), which is the only way to tell "ring full" from "heap flat" from
/// "timer died" - 26.9.18 could not.
#define PICO_BOOTBB_HB_MS 20000

/// 启动黑匣子。**必须排在 `usb_storage_phy_init()` 之后**（26.9.16 R1）。
/// 26.9.15 把它放在第一条，真机 TF 卡直接不挂载；那几十毫秒里没有任何堆分配，
/// 挪后不损失任何数据。
/// Start it. MUST sit AFTER `usb_storage_phy_init()` (26.9.16 R1). 26.9.15 called it
/// first and the field unit stopped mounting the TF card; those few dozen milliseconds
/// contain no heap allocations, so moving it later costs nothing measurable.
void pico_bootbb_start(void);

/// 1 Hz 轮询：host 一连上就把还没吐的点打出去。幂等，可高频调用（内部自己判时机）。
/// 1 Hz poll: spill whatever is not yet printed as soon as the host is heard. Idempotent
/// and cheap enough to call at any rate; it gates itself.
void pico_bootbb_poll(void);

/// 无条件重放一次（`boot-post` 时兜底：用户 40 s 后才开电脑也能看到）。
/// Force one full replay (`boot-post` backstop for a user who opens the terminal late).
void pico_bootbb_replay(void);

/// 记一条非堆事件（26.9.16 R2）。`code` 见 `pico_bootbb.c` 里的 BB_NOTE_*；
/// `a` 放错误码低 8 位，`b` 放位域（present / mounted / invalidated / probe_state）。
/// 定时器还没武装时是空操作；写满则丢弃并计数（输出里打印 dropped）。
/// Record one non-heap event (26.9.16 R2). `code` is a BB_NOTE_* constant; `a` carries
/// the low byte of an error code, `b` a bitfield (present / mounted / invalidated /
/// probe_state). No-op before the timer is armed; overflow is counted and reported.
void pico_bootbb_note(uint8_t code, uint8_t a, uint16_t b);

/// 是否真的在记录（false = 没拿到 PSRAM，已自我禁用）。
/// Whether recording is actually live (false = no PSRAM, self-disabled).
bool pico_bootbb_active(void);

/// 本次已经存了几个点（含第 0 点）。用于结构牙与宿主测试。
/// How many points were stored (including point 0). For structural teeth and host tests.
unsigned pico_bootbb_count(void);

#else  // !CONFIG_PICO_BOOTBB —— 全部退化成空 inline / everything is an empty inline

static inline void pico_bootbb_start(void) {}
static inline void pico_bootbb_poll(void) {}
static inline void pico_bootbb_replay(void) {}
static inline void pico_bootbb_note(uint8_t code, uint8_t a, uint16_t b) {
    (void)code; (void)a; (void)b;
}
static inline bool pico_bootbb_active(void) { return false; }
static inline unsigned pico_bootbb_count(void) { return 0; }

#endif  // CONFIG_PICO_BOOTBB

#ifdef __cplusplus
}
#endif
