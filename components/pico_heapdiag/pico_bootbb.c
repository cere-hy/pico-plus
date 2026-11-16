/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：开机黑匣子的实现。设计理由见 include/pico_bootbb.h 的三条硬约束。
 * English: the boot blackbox. See the header for the three hard constraints.
 *
 * 26.9.16：整个实现由 `CONFIG_PICO_BOOTBB` 包住，默认 n 时本翻译单元只剩一个
 * `typedef`（ISO C 要求翻译单元非空），零代码零 RAM。
 * 26.9.16: the whole implementation sits behind `CONFIG_PICO_BOOTBB`. With the switch off
 * this translation unit keeps one typedef (ISO C wants a non-empty TU) and nothing else.
 */
#include "pico_bootbb.h"
#include "pico_heapdiag.h"

#if CONFIG_PICO_BOOTBB

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bootbb";

// 事件码定义在头文件里（`PICO_BOOTBB_NOTE_*`），调用点不必自己包 `#if`。
// Event codes live in the header so call sites do not need their own `#if`.

// 单点 16 B：t / free / largest / blocks。刻意不存文本——一次 heap summary 的文本
// 约 800 B，64 点就是 50 KB，而这只为了回答「在哪一秒掉了多少」。
// 16 B per point: t / free / largest / blocks. Deliberately no text: one heap summary
// formatted as text is ~800 B, so 64 points would be 50 KB - all to answer "what fell,
// in which second".
typedef struct {
    uint32_t t_ms;
    uint32_t free_bytes;
    uint32_t largest;
    uint16_t blocks;
    uint16_t pad;
} bb_point_t;

// 事件记录 8 B：t / code / a / b。
//
// 审计 26.9.16 裁定写的是「4 B/条 × 16 条」。这里用 8 B，多出来的 4 B 是时间戳，
// 理由：没有时间戳就只能说「探测失败过」，说不出「第几秒失败」——而这恰恰是本次
// 事故的分辨点（0～4 s 的失明窗口 vs 4 s 之后的正常时序）。总量 128 B **全部在
// PSRAM**，池 A 一个字节都没占，所以「测量工具不能污染被测对象」这条纪律并未破。
// 8 B instead of the ruled 4 B: the extra 4 bytes are a timestamp. Without it we could
// only say "a probe failed", never "it failed at second N", which is exactly the
// distinction this incident turns on. All 128 bytes live in PSRAM; pool A is untouched,
// so the "the instrument must not sit inside the patient" rule still holds.
typedef struct {
    uint32_t t_ms;
    uint8_t code;
    uint8_t a;
    uint16_t b;
} bb_note_t;

// 点和事件共用一次分配：避免两次 malloc，也保证「要么全有、要么全无」。
// Points and notes share one allocation: one malloc, and all-or-nothing.
typedef struct {
    bb_point_t pts[PICO_BOOTBB_POINTS];
    bb_note_t notes[PICO_BOOTBB_NOTES];
} bb_blob_t;

static bb_blob_t *s_blob;      // PSRAM，绝不在内部 RAM / Never in internal RAM
static uint32_t s_n;           // 已存的点数 / stored points
static uint32_t s_notes;       // 已存的事件数 / stored notes
static uint32_t s_note_drops;  // 写满后丢弃的事件数 / notes dropped after overflow
static uint32_t s_samples;     // 采样次数（含被「没变化」跳过的）/ samples taken, incl. dropped
static uint32_t s_min_free;    // 堆自己的历史最低水位 / heap's own low-water mark
// 26.9.31：DMA 子池的两笔账。`s_min_dma` 是采样期内 `MALLOC_CAP_DMA` 最大空闲块的
// 最低点，`s_min_dma_ms` 是它出现的时刻（单位 100 ms，uint16 覆盖 6,553 s）。
// 这两笔只回答「启动段 DMA 池最低剩多少」，覆盖不到 20 s 窗外——窗外的账由
// `s_dma_floor`（派生 note 用）记。
// 26.9.31: two DMA sub-pool numbers. `s_min_dma` is the low point of the largest free
// block tagged MALLOC_CAP_DMA during sampling; `s_min_dma_ms` is when (100 ms units,
// uint16 covers 6,553 s). They answer "how low did the DMA pool get during boot" and
// cannot see past the 20 s window - `s_dma_floor` covers the rest via derived notes.
static uint32_t s_min_dma;
static uint16_t s_min_dma_ms;
static uint32_t s_dma_floor;   // 已记过的最低点，只在创新低时派生 note / recorded low
static uint32_t s_printed;     // 已经吐出去几个点 / how many points were spilled
static int64_t s_t0;           // 起点 / origin
static esp_timer_handle_t s_timer;
static bool s_active;
static bool s_gave_up;

// ---- 26.9.19 S5：三条「一生只打一次」的游标 ------------------------------------
// 26.9.18 真机上 header/cut/delta/floor/events 全部锁在 `if (from == 0)` 里，
// 而那一次落在串口还没接稳的窗口——整块证据一次性丢光且永不补。现在改成三组游标：
// 谁没吐过就吐谁，每段只吐新增的那一截。
// 26.9.19 S5: three spill cursors. In 26.9.18 the summary block sat inside
// `if (from == 0)`, so it was emitted exactly once - in the window where the port was
// still settling - and never again. Now every cursor advances independently.
static uint32_t s_notes_printed;  // 已经吐出去几条事件 / notes already spilled
static uint32_t s_cut_seq;        // 跨段连续的 cut 编号 / global cut numbering
static uint32_t s_cut_total;      // 跨段累计跌幅 / accumulated fall in bytes
static bool s_announced;          // 「以下是实时」只打一次 / said once, then never again
static bool s_full_logged;        // 「缓冲满了」只喊一次 / shout once when the ring is full
static uint32_t s_hb_ms;          // 上次心跳的毫秒 / last heartbeat, ms since t0
static uint32_t s_note_evicted;   // 关键事件挤掉普通事件的次数 / critical notes that evicted a plain one
static unsigned s_attr_fired;     // 26.9.24 B1：归因已打几次（限 2）/ B1 attributions fired (cap 2)

// 26.9.24 B1（审计批复「B1 搭车取证」）：26.9.23 真机上 26.9.21a→23 的 largest 缺口
// −4,096 是一笔 t≈3,900/3,950 ms 的**行为分配**，cut 行只有数字没有姓名。现在在
// cut 发生的**瞬间**（采样现场，分配者还握着块）问一次「这块是谁的」——
// pico_heap_attr 遍历任务表反查栈块，与块表地址对上即点名。只认「largest 跌幅
// ≥4,096 且 t∈[3,000,5,000] ms」，每次开机最多 2 次，健康路径零输出。
// 注意：attr 行打在 cut 行**之前**（cut 行是 spill 时补的账），判读时按时刻对齐。
// 26.9.24 B1 (audit ruling "B1 rides along"): on 26.9.23 hardware the largest gap
// between 21a and 23 was a -4,096 behavioural allocation at t≈3,900/3,950 ms, and the
// cut lines carry numbers, never names. Now, at the INSTANT a cut happens (sampling
// context, the allocator still holds the block), ask "whose block is this" once -
// pico_heap_attr walks the task list and reverse-looks-up stack blocks; matching an
// address against the block table names the culprit. Fires only when largest falls
// >= 4,096 B within t∈[3,000,5,000] ms, at most twice per boot, silent when healthy.
// Note: attr lines precede their cut lines (cuts are accounted at spill time);
// align by timestamp when reading.
#define BB_ATTR_MIN_FALL 4096u
#define BB_ATTR_WINDOW_MS 3000u
#define BB_ATTR_WINDOW_END_MS 5000u
#define BB_ATTR_MAX 2u

static void bb_attr_maybe(const bb_point_t *prev, const multi_heap_info_t *info) {
    if (s_attr_fired >= BB_ATTR_MAX) return;
    const uint32_t now_ms = (uint32_t)((esp_timer_get_time() - s_t0) / 1000);
    if (now_ms < BB_ATTR_WINDOW_MS || now_ms > BB_ATTR_WINDOW_END_MS) return;
    if (prev->largest < (uint32_t)info->largest_free_block + BB_ATTR_MIN_FALL) return;
    ++s_attr_fired;
    ESP_LOGW(TAG, "[DELAYED-BOOT] cut %u B at t=%u ms - asking whose block it is (B1)",
             (unsigned)(prev->largest - (uint32_t)info->largest_free_block), (unsigned)now_ms);
    pico_heap_attr("cut-who");
}

// 采一个点。**只有数值变化才落点**：20 s 里通常只有 3～6 次变化，不会写满。
// 判据：largest 变、总块数变、或 free 变动 ≥ 512 B（512 是「不是抖一下」的量级）。
// Take one sample. A point is stored ONLY when the numbers change: 20 s usually holds
// 3-6 changes. Predicate: largest changed, block count changed, or free moved >= 512 B
// (512 is "not just noise").
static void bb_sample(void) {
    if (s_blob == NULL) return;

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    s_samples++;
    s_min_free = (uint32_t)info.minimum_free_bytes;

    // 26.9.31（审计裁决 1）：上面这一行看的是内部堆**总量**，而 SD driver 的扇区 DMA
    // 缓冲区从 `MALLOC_CAP_DMA` 子池里拿。两个 caps 不是一回事：真机上总堆还有 11 KB
    // 时 driver 已经报 0x101。这里补一笔只读观测（不动任何分配路径）。
    // `largest_free_block` 而不是 `total_free_bytes`：driver 要的是**一块连续**可 DMA 的
    // 内存，碎片化对它和对总量是两种失败。
    // 26.9.31 (audit ruling 1): the line above watches the INTERNAL total, but the SD
    // driver's sector DMA buffer comes from the MALLOC_CAP_DMA sub-pool. Those are not the
    // same thing: on the field unit the total still had 11 KB when the driver reported
    // 0x101. Read-only observation, no allocation path touched. Largest free BLOCK, not
    // total: the driver needs one contiguous DMA-able chunk, and fragmentation fails it
    // differently from a short total.
    {
        const size_t dma_l = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        if (s_min_dma == 0 || (uint32_t)dma_l < s_min_dma) {
            s_min_dma = (uint32_t)dma_l;
            s_min_dma_ms = (uint16_t)(((esp_timer_get_time() - s_t0) / 100000) & 0xFFFFu);
        }
    }

    if (s_n > 0) {
        const bb_point_t *prev = &s_blob->pts[s_n - 1];
        const uint32_t free_now = (uint32_t)info.total_free_bytes;
        const bool moved = (prev->largest != (uint32_t)info.largest_free_block) ||
                           (prev->blocks != (uint16_t)info.total_blocks) ||
                           (free_now + 512u <= prev->free_bytes) ||
                           (free_now >= prev->free_bytes + 512u);
        if (!moved) return;
        // B1：大刀落下的瞬间点名（见上）。/ B1: name the culprit the instant the
        // big knife falls (see above).
        bb_attr_maybe(prev, &info);
    }

    if (s_n >= PICO_BOOTBB_POINTS) {
        // 26.9.19 S5-d：满了必须**当场喊一声**。26.9.18 这里是无声的 `return`，
        // 于是「140 s 后停采 104 秒」到底是缓冲区满还是定时器停摆，谁也判不出来。
        // 现在满的那一刻打一行（只打一次），配合 20 s 心跳，两种原因就能分开。
        // 26.9.19 S5-d: shout ONCE when the ring fills. 26.9.18 returned silently here,
        // which is why "no samples for 104 s" could not be told apart from "timer died".
        if (!s_full_logged) {
            s_full_logged = true;
            ESP_LOGW(TAG,
                     "[DELAYED-BOOT] BUFFER FULL at %u ms: %u/%u points stored - every later "
                     "change is DROPPED (raise PICO_BOOTBB_POINTS if this window matters)",
                     (unsigned)((esp_timer_get_time() - s_t0) / 1000), (unsigned)s_n,
                     (unsigned)PICO_BOOTBB_POINTS);
        }
        return;
    }

    s_blob->pts[s_n].t_ms = (uint32_t)((esp_timer_get_time() - s_t0) / 1000);
    s_blob->pts[s_n].free_bytes = (uint32_t)info.total_free_bytes;
    s_blob->pts[s_n].largest = (uint32_t)info.largest_free_block;
    s_blob->pts[s_n].blocks = (uint16_t)info.total_blocks;
    s_blob->pts[s_n].pad = 0;
    s_n++;
}

static void bb_tick(void *arg) {
    (void)arg;
    bb_sample();
}

// 26.9.20 S11：哪些事件「永不丢弃」。拔插的三段 sd-stage 与挂载后的空间统计，是每次
// 热插拔唯一的结构化证据；26.9.19 的 16 条环在 160 s 就满了，之后三轮拔插一条都没留下。
// 现在环满时，关键事件可以**挤掉一条还没吐出去的普通事件**（不是覆盖已打印的，那会让
// 游标倒流），挤掉的次数单独计数并打印——静默丢证据和静默丢日志一样不可接受。
// 26.9.20 S11: critical events. A reinsert's stage notes and the space scan are the only
// structured evidence a hotplug leaves; the 16-note ring filled at 160 s and the next three
// reinserts left nothing at all. When the ring is full a critical note may now evict a
// plain note that has NOT been spilled yet, and the eviction count is reported.
static bool note_is_critical(uint8_t code) {
    return code == PICO_BOOTBB_NOTE_SD_STAGE || code == PICO_BOOTBB_NOTE_SD_SPACE ||
           code == PICO_BOOTBB_NOTE_EXCERPT_FAIL || // 弃掉用户数据，必须留得下来 / data loss
           code == PICO_BOOTBB_NOTE_PUSH_WRITE_FAIL || // 卡在变坏的早期信号 / an early dying card
           // 31b：冻结看门狗超时与有意保持都是「只有重启能出」的强信号，环满时必须挤进来。
           // 31b: a freeze timeout and a deliberate hold both mean "reboot is the only exit".
           code == PICO_BOOTBB_NOTE_PUSH_FREEZE ||
           code == PICO_BOOTBB_NOTE_PUSH_HOLD ||
           // 31b P1-1：CD 与 I/O 打架 = 卷即将不可信，这一条必须在环满时活下来。
           // P1-1: CD and I/O disagreeing means the volume is about to go; keep it.
           code == PICO_BOOTBB_NOTE_SD_IO_DEAD;
}

static void note_store(uint32_t idx, uint8_t code, uint8_t a, uint16_t b) {
    bb_note_t *n = &s_blob->notes[idx];
    n->t_ms = (uint32_t)((esp_timer_get_time() - s_t0) / 1000);
    n->code = code;
    n->a = a;
    n->b = b;
}

// 26.9.31：SD 事件点顺手派生一条 DMA 池水位。**只在创新低时记**——健康会话里它
// 一条都不出现，饥饿一旦开始每一步都留痕。这就是「观测到的故障」与「故障自己报出来的
// 观测」的区别：driver 的 `0x101` 只在连 512 B 都拿不到时才喊，而它开始喊之前的那段
// 下坡本来完全看不见。
// 触发点选 STAGE 与 DONE：前者是挂载三阶段（每轮挂载必有），后者是探测结束（含失败）。
// 5 轮重挂约 30 次触发，note 环 64 条装得下；健康会话约 20 条/天，更宽松。
// 26.9.31: derive one DMA pool level at SD event points, and ONLY on a new low - a healthy
// session produces none, and once a shortage starts every step leaves a mark. That is the
// difference between an observed fault and a fault that reports itself: the driver only
// shouts 0x101 when even 512 B is gone, and the slope before that shout was invisible.
static void bb_dma_note_from(uint8_t code, uint8_t a) {
    if (code != PICO_BOOTBB_NOTE_SD_STAGE && code != PICO_BOOTBB_NOTE_SD_DONE) return;
    if (s_blob == NULL) return;
    const uint32_t cur = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    if (s_dma_floor != 0 && cur + 1024u > s_dma_floor) return;   // 没跌 1 KB：不记
    s_dma_floor = cur;
    if (s_notes >= PICO_BOOTBB_NOTES) {
        s_note_drops++;
        return;
    }
    const uint32_t q = cur / 256u;
    note_store(s_notes, PICO_BOOTBB_NOTE_SD_DMA, (uint8_t)(q > 255u ? 255u : q),
               (uint16_t)(((uint16_t)code << 8) | a));
    s_notes++;
}

void pico_bootbb_note(uint8_t code, uint8_t a, uint16_t b) {
    if (s_blob == NULL) return;   // 未武装或已禁用：静默丢弃是对的，没有更早的出口
    if (s_notes >= PICO_BOOTBB_NOTES) {
        // 26.9.20 S11：关键事件优先。从尾部往前找第一条「还没吐出去、也不关键」的，
        // 用它换掉；找不到（全是关键事件或全都已打印）才计入 dropped。
        // Critical first: walk back to the newest plain note that has not been spilled and
        // take its slot. If every unprinted note is critical, count a drop instead.
        if (note_is_critical(code)) {
            for (uint32_t i = s_notes; i > s_notes_printed; --i) {
                if (!note_is_critical(s_blob->notes[i - 1].code)) {
                    s_note_evicted++;
                    note_store(i - 1, code, a, b);
                    bb_dma_note_from(code, a);
                    return;
                }
            }
        }
        // 记「丢过几条」而不是逐条刷屏——现场要的是「有没有漏」，不是漏了什么。
        // Count drops instead of spamming: the field question is "did we lose any",
        // not "which ones".
        s_note_drops++;
        return;
    }
    note_store(s_notes, code, a, b);
    s_notes++;
    bb_dma_note_from(code, a);
}

static const char *note_name(uint8_t code) {
    switch (code) {
        case PICO_BOOTBB_NOTE_SD_STATE: return "sd-state";
        case PICO_BOOTBB_NOTE_SD_PROBE: return "sd-probe";
        case PICO_BOOTBB_NOTE_SD_DONE:  return "sd-done";
        case PICO_BOOTBB_NOTE_SD_STAGE: return "sd-stage";
        case PICO_BOOTBB_NOTE_SD_SPACE: return "sd-space";
        case PICO_BOOTBB_NOTE_EXCERPT_FAIL: return "excerpt-fail";
        case PICO_BOOTBB_NOTE_SD_DMA:   return "sd-dma";
        case PICO_BOOTBB_NOTE_PUSH_WRITE_FAIL: return "push-write-fail";
        case PICO_BOOTBB_NOTE_PUSH_FREEZE: return "push-freeze";
        case PICO_BOOTBB_NOTE_PUSH_HOLD:   return "push-hold";
        case PICO_BOOTBB_NOTE_SD_IO_DEAD:  return "sd-io-dead";
        default:                        return "?";
    }
}

// 把尚未吐出的点和事件打出去。`full` = 从头重放（replay 用）。
//
// 26.9.19 S5：**增量**打印。26.9.18 的真机上 85 条点全打出来了，可 header / cut /
// delta / floor / events / sd-stage **一条都没有**——因为它们全锁在 `if (from == 0)`
// 里面，而 `from = full ? 0 : s_printed`、`full` 永远是 false，所以那一整块**一生只
// 打一次**；偏偏那一次落在串口还没接稳的窗口（真机日志第 2 行就是两行被拼在一起的
// 烂行），于是证据一次性丢光、永不补回。S3 的 sd-stage 更是结构上不可能被看到。
//
// 现在三组游标各自走：点 `s_printed`、事件 `s_notes_printed`、cut 编号 `s_cut_seq`
// 跨段连续。任何时刻插上串口，看到的都是「从我上次之后新增的那一截」。
// 26.9.19 S5: incremental. In 26.9.18 all 85 points came out but not one header, cut,
// delta, floor, events or sd-stage line did - all of it sat inside `if (from == 0)`, and
// since `full` is always false that block ran exactly once, in the window where the port
// was still settling. The evidence was lost in one go and never came back; S3's sd-stage
// was structurally unprintable. Now points, notes and cut numbering each keep their own
// cursor, so whatever you connect you see only the new tail.
static void bb_spill(bool full) {
    const uint32_t from_p = full ? 0u : s_printed;
    const uint32_t from_n = full ? 0u : s_notes_printed;
    if (from_p >= s_n && from_n >= s_notes) return;
    if (full) { s_cut_seq = 0; s_cut_total = 0; }

    // 26.9.20 S11：512 点一次性吐出去约 36 KB 文本，串口 FIFO 阻塞会把主循环按住好几秒，
    // 而主循环正是 poll 的调用方。分块：一次最多 64 行，剩下的等下一次 poll（500 ms）。
    // 证据一条不少，只是分几口吐；`s_printed` 因此**只推进到本块末尾**，不是 s_n。
    // 26.9.20 S11: 512 points is ~36 KB of text and would block the main loop for seconds.
    // Chunked to 64 lines per call; `s_printed` advances to the chunk end, not to s_n.
    uint32_t end = s_n;
    if (from_p < s_n && end - from_p > PICO_BOOTBB_SPILL_LINES)
        end = from_p + PICO_BOOTBB_SPILL_LINES;

    if (from_p < s_n) {
        // 段头每次 spill 都打。它回答的是「这一段覆盖哪几毫秒、采样器还活着吗」——
        // `sampled` 是**采样次数**（含被判为「没变化」而丢掉的），`stored` 是真正
        // 落下的点数。两者一起看，D3 那种「停采」立刻能定性。
        // The window header is printed on EVERY spill. `sampled` counts every tick
        // (including the ones dropped as "no change"); `stored` counts real points.
        ESP_LOGW(TAG,
                 "[DELAYED-BOOT] == pts %u..%u of %u/%u%s | sampled %u | notes %u/%u%s ==",
                 (unsigned)from_p, (unsigned)(end - 1), (unsigned)s_n,
                 (unsigned)PICO_BOOTBB_POINTS, s_n >= PICO_BOOTBB_POINTS ? " FULL" : "",
                 (unsigned)s_samples, (unsigned)s_notes, (unsigned)PICO_BOOTBB_NOTES,
                 s_notes >= PICO_BOOTBB_NOTES ? " FULL" : "");

        for (uint32_t i = from_p; i < end; ++i) {
            ESP_LOGW(TAG, "[DELAYED-BOOT] t=%5u ms  free=%u  largest=%u  blocks=%u%s",
                     (unsigned)s_blob->pts[i].t_ms, (unsigned)s_blob->pts[i].free_bytes,
                     (unsigned)s_blob->pts[i].largest, (unsigned)s_blob->pts[i].blocks,
                     i == 0 ? "" : "   <- change");
        }

        // ---- 26.9.18 S2-b：逐段列，不只报最大一次 ---------------------------
        // 26.9.15 的 delta 只报「最大的一次跌幅」。可 26.9.17 真机上那两块 1 KB 是
        // **先后两次**掉的（词典一次、开书清场一次），只报最大那块等于把另一次藏起来
        // ——正是这个盲区让 26.9.17 的账得手算才知道是 −2,048。
        // 现在列出所有 ≥ 512 B 的跌幅，再给一行合计。
        // 26.9.18 S2-b: list EVERY cut, not just the biggest. The two 1 KB drops in the
        // 26.9.17 field log were two separate events (dictionary, then the book-open
        // teardown); reporting only the largest hid the other, which is why that ledger had
        // to be reconciled by hand to see the -2,048. Now every cut >= 512 B gets a line.
        //
        // 26.9.19：起点从 `from_p` 开始（首次才从 1 开始），于是**跨段的那一步**也算
        // 得上——`pts[from_p - 1]` 早打印过，但它还在数组里，照样能当 prev。
        // 26.9.19: start at `from_p` (only 1 on the first pass) so the step ACROSS two
        // spills is still counted - pts[from_p-1] was printed long ago but is still there.
        uint32_t cuts = 0, total = 0;
        const uint32_t cut_from = from_p ? from_p : 1u;
        for (uint32_t i = cut_from; i < end; ++i) {
            const uint32_t prev_l = s_blob->pts[i - 1].largest;
            const uint32_t cur_l = s_blob->pts[i].largest;
            if (cur_l + 512u > prev_l) continue;
            const uint32_t d = prev_l - cur_l;
            cuts++;
            total += d;
            s_cut_total += d;
            ESP_LOGW(TAG,
                     "[DELAYED-BOOT] cut %u: largest %u -> %u at t=%u ms (-%u B, blocks %u -> %u)",
                     (unsigned)++s_cut_seq, (unsigned)prev_l, (unsigned)cur_l,
                     (unsigned)s_blob->pts[i].t_ms, (unsigned)d,
                     (unsigned)s_blob->pts[i - 1].blocks, (unsigned)s_blob->pts[i].blocks);
        }
        if (cuts == 0) {
            ESP_LOGI(TAG, "[DELAYED-BOOT] delta: none - largest held at %u through t=%u ms",
                     (unsigned)s_blob->pts[end - 1].largest,
                     (unsigned)s_blob->pts[end - 1].t_ms);
        } else {
            ESP_LOGW(TAG, "[DELAYED-BOOT] delta: %u cut(s) this window, largest %u -> %u, "
                          "total -%u B (accumulated -%u B)",
                     (unsigned)cuts, (unsigned)s_blob->pts[from_p ? from_p - 1 : 0].largest,
                     (unsigned)s_blob->pts[end - 1].largest, (unsigned)total,
                     (unsigned)s_cut_total);
        }
        // 50 ms 采样也会漏掉瞬时峰值；`minimum_free_bytes` 是 heap 自己记的，漏不掉。
        // 50 ms sampling can still miss a transient peak; `minimum_free_bytes` is the
        // heap's own record and cannot be missed.
        ESP_LOGI(TAG, "[DELAYED-BOOT] floor: min_free=%u (heap's own low-water mark, "
                      "immune to a missed sample)",
                 (unsigned)s_min_free);
        // 26.9.31：DMA 子池的最低点**单独一行**。并进上面那行会让人以为在说同一个堆——
        // 而 0x101 的教训正是「总堆够、DMA 子池不够」。0 表示一次都没采到，明说。
        // 26.9.31: the DMA sub-pool low point gets its OWN line. Merging it into the line
        // above reads as one heap, and the 0x101 lesson is exactly "total fine, DMA pool
        // not". A zero means no sample landed, and says so.
        if (s_min_dma > 0) {
            ESP_LOGW(TAG, "[DELAYED-BOOT] dma floor: largest=%u B at t=%u00 ms "
                          "(MALLOC_CAP_DMA - where sdmmc's sector buffer comes from)",
                     (unsigned)s_min_dma, (unsigned)s_min_dma_ms);
        } else {
            ESP_LOGW(TAG, "[DELAYED-BOOT] dma floor: MISSING (no sample; do not read as 0)");
        }
        s_printed = end;
        if (end < s_n) {
            // 本块结束但还有剩：明说一句，否则读者会以为「t=本块末尾」就是全部。
            // Say so when the chunk ends early, otherwise the last t= looks like the end.
            ESP_LOGW(TAG, "[DELAYED-BOOT] chunk: %u point(s) still queued, next spill on the "
                          "next poll (printing them all at once blocks the main loop)",
                     (unsigned)(s_n - end));
        }
    }

    // 26.9.16 R2：非堆事件。26.9.15 缺的就是这段——TF 卡的每次探测尝试都发生在
    // 0～4 s 的失明窗口里，普通 ESP_LOG 一个字节都进不了 FIFO。
    // 26.9.19 S5-b：改成**增量**。拔插发生在 100 s 之后，那三条 sd-stage 写进缓冲时，
    // 26.9.18 再没有任何代码路径会去打印它们——S3 那一版等于白做。
    // 26.9.16 R2: non-heap events. 26.9.19 S5-b: now incremental - a reinsert at t=100 s
    // used to leave its three sd-stage notes with no reader at all.
    if (from_n < s_notes || (s_note_drops > 0 && !s_announced)) {
        // 26.9.21 S16/D8：两个可选计数改成分支打印。原写法是
        // `... stored%s%u%s%u`，条件为假时前缀是空串、`%u` 照样求值，于是每一行都以
        // `stored00` 收尾——26.9.20 的日志里 9 行全是这个尾巴，读的人会以为 00 是个计数。
        // 现在没有 dropped/evicted 时句尾就是 `stored`，不做减法就看不出歧义。
        // 26.9.21 S16/D8: print the optional counters in a branch. The old format string
        // always evaluated both %u, so a clean run ended every line with "stored00".
        if (s_note_drops || s_note_evicted) {
            ESP_LOGW(TAG, "[DELAYED-BOOT] events: +%u this window, %u/%u stored, dropped %u, evicted %u",
                     (unsigned)(s_notes - from_n), (unsigned)s_notes,
                     (unsigned)PICO_BOOTBB_NOTES,
                     (unsigned)s_note_drops, (unsigned)s_note_evicted);
        } else {
            ESP_LOGW(TAG, "[DELAYED-BOOT] events: +%u this window, %u/%u stored",
                     (unsigned)(s_notes - from_n), (unsigned)s_notes,
                     (unsigned)PICO_BOOTBB_NOTES);
        }
        for (uint32_t i = from_n; i < s_notes; ++i) {
            const bb_note_t *n = &s_blob->notes[i];
            // 26.9.20 S12/D5：`a` 的位域拆开——低 3 位阶段号、0x08 进入点、0x80 成功。
            // 26.9.19 只有 stage 3 设了 0x80，于是 stage 1/2 永远印 `FAIL`，而两次挂载
            // 全都成功了。这是同族错误的第三次：把「没有结果」当成「失败」。
            // 26.9.20 S12/D5: `a` is now stage | 0x08 (entry) | 0x80 (success). In 26.9.19
            // only stage 3 set 0x80, so stages 1 and 2 always printed FAIL while both mounts
            // succeeded - the third time this family of bug appeared.
            if (n->code == PICO_BOOTBB_NOTE_SD_STAGE) {
                const unsigned st = (unsigned)(n->a & 0x07u);
                const bool entry = (n->a & 0x08u) != 0;
                const bool ok = (n->a & 0x80u) != 0;
                ESP_LOGW(TAG, "[DELAYED-BOOT] t=%5u ms  %-8s stage=%u  at=%u ms  (%s)",
                         (unsigned)n->t_ms, note_name(n->code), st, (unsigned)n->b,
                         entry ? "enter" : (ok ? "ok" : "FAIL"));
                continue;
            }
            // 26.9.20 S9(b)：空间统计也是「有结果」的点，`a` 直接是 0/1，不需要掩码。
            if (n->code == PICO_BOOTBB_NOTE_SD_SPACE) {
                ESP_LOGW(TAG, "[DELAYED-BOOT] t=%5u ms  %-8s %s  at=%u ms",
                         (unsigned)n->t_ms, note_name(n->code),
                         n->a ? "ok" : "FAIL", (unsigned)n->b);
                continue;
            }
            // 26.9.31：派生事件。`a` 是 largest/256（饱和），`b` 是「原事件码 << 8 | 原 a」。
            // 看到这一行就意味着 DMA 池创了新低——它前面那半行是当时的池底，
            // 括号里的是「谁把这件事捅出来的」。
            // 26.9.31: derived event. `a` is largest/256 (saturated); `b` is
            // "original code << 8 | original a". A line here means the DMA pool set a new
            // low: the number is that floor, the bracket names the event that revealed it.
            if (n->code == PICO_BOOTBB_NOTE_SD_DMA) {
                ESP_LOGW(TAG, "[DELAYED-BOOT] t=%5u ms  %-8s dma_largest=%u B  (%s a=%u)",
                         (unsigned)n->t_ms, note_name(n->code), (unsigned)n->a * 256u,
                         note_name((uint8_t)(n->b >> 8)), (unsigned)(n->b & 0xFFu));
                continue;
            }
            ESP_LOGW(TAG,
                     "[DELAYED-BOOT] t=%5u ms  %-8s a=%u b=0x%04x",
                     (unsigned)n->t_ms, note_name(n->code), (unsigned)n->a,
                     (unsigned)n->b);
        }
        s_notes_printed = s_notes;
    }

    if (!s_announced && s_printed > 0) {
        s_announced = true;
        ESP_LOGI(TAG, "===== end of buffered data - everything below is LIVE, not history =====");
    }

    s_hb_ms = (uint32_t)((esp_timer_get_time() - s_t0) / 1000);

    // 审计 26.9.19 点名要的那一行：缓冲用了多少、还剩多少、满了没有。
    // 26.9.18 的「140 s 后停采 104 秒」就是缺这一行才判不出来——「点满了」和
    // 「定时器停摆」在日志里长得一模一样。
    // 26.9.20：多一个 `printed`——分块吐点后，「存了几个」与「吐了几个」不再相等，
    // 只报 stored 会把「还有一批在路上」看成「已经吐完」。
    // The line the 26.9.19 audit asked for by name, plus `printed` since 26.9.20: after
    // chunking, stored != printed and reporting only stored hides the queued tail.
    ESP_LOGW(TAG, "[DELAYED-BOOT] spill: %u/%u points stored, %u printed, notes %u/%u, "
                  "sampled %u, last @%u ms%s",
             (unsigned)s_n, (unsigned)PICO_BOOTBB_POINTS, (unsigned)s_printed,
             (unsigned)s_notes, (unsigned)PICO_BOOTBB_NOTES, (unsigned)s_samples,
             s_n ? (unsigned)s_blob->pts[s_n - 1].t_ms : 0u,
             s_n >= PICO_BOOTBB_POINTS ? " FULL - later changes are LOST" : "");
}

void pico_bootbb_start(void) {
    if (s_active || s_blob != NULL) return;

    // t0 取在函数入口。**注意它现在不是 app_main 的第一条语句**——26.9.16 R1 把它挪到
    // `usb_storage_phy_init()` 之后。这几十毫秒里没有堆分配（PHY 交还只动寄存器），
    // 所以丢掉的只是时间轴起点，不是任何一个采样点。
    // t0 is taken at entry. NOTE it is no longer the first statement of app_main - R1 moved
    // it after usb_storage_phy_init(). Those milliseconds hold no heap allocation (the PHY
    // hand-back only touches registers), so all we lose is the time origin, not a sample.
    s_t0 = esp_timer_get_time();

    const size_t bytes = sizeof(bb_blob_t);
    s_blob = (bb_blob_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_blob == NULL) {
        // 拿不到 PSRAM 就**整个关掉**。降级到内部 RAM 会直接从池 A 里吃掉 1 KB，把
        // 「21,504」测成「20,480」——宁可没有数据，也不给一份污染过的数据。
        // No PSRAM means NO blackbox. Falling back to internal RAM would eat 1 KB out of
        // pool A and measure 21,504 as 20,480 - better no data than poisoned data.
        ESP_LOGW(TAG, "DISABLED: no PSRAM for %u B - refusing to steal internal RAM "
                      "(that would corrupt the very number being measured)",
                 (unsigned)bytes);
        return;
    }
    memset(s_blob, 0, bytes);

    // 先建 timer 再采第 0 点：timer 对象本身要几十字节内部 RAM，让它落在基线里，
    // 不污染 delta。
    // Create the timer BEFORE point 0: the timer object itself costs a few dozen bytes of
    // internal RAM, and taking point 0 afterwards keeps it inside the baseline.
    const esp_timer_create_args_t args = {
        .callback = bb_tick,
        .arg = NULL,
        .name = "bootbb",
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        heap_caps_free(s_blob);
        s_blob = NULL;
        ESP_LOGW(TAG, "DISABLED: esp_timer_create failed");
        return;
    }

    bb_sample();   // 第 0 点 / point 0
    s_active = true;
    esp_timer_start_periodic(s_timer, PICO_BOOTBB_PERIOD_US);

    ESP_LOGW(TAG, "armed: %u pts + %u notes in PSRAM (%u B), %u ms period, "
                  "spill at >=%u ms or on host",
             (unsigned)PICO_BOOTBB_POINTS, (unsigned)PICO_BOOTBB_NOTES, (unsigned)bytes,
             (unsigned)(PICO_BOOTBB_PERIOD_US / 1000),
             (unsigned)(PICO_BOOTBB_FIRST_US / 1000));
}

void pico_bootbb_poll(void) {
    if (!s_active || s_gave_up) return;

    // 26.9.19 S5-c：判据从「点」扩到「点或事件」。26.9.18 只盯点，于是拔插写进去的
    // sd-stage 在没有新堆点的那几秒里**永远不会被吐出来**——S3 白做。
    // 26.9.19 S5-c: gate on points OR notes. 26.9.18 watched points only, so a reinsert
    // note written during a heap-quiet second had no path to the port at all.
    const bool fresh = (s_printed < s_n) || (s_notes_printed < s_notes);

    const int64_t dt = esp_timer_get_time() - s_t0;
    if (dt < PICO_BOOTBB_FIRST_US) return;

    // 未连接时 VFS 的 write 直接返回 EIO，打也是白打（一个字节都不进 FIFO）。
    // When disconnected the VFS write returns EIO without touching the FIFO, so printing
    // would be writing into a void.
    if (!usb_serial_jtag_is_connected()) {
        if (dt > PICO_BOOTBB_GIVEUP_US) {
            s_gave_up = true;
            ESP_LOGW(TAG, "GAVE UP at %u ms: host never heard; %u pts still buffered",
                     (unsigned)(dt / 1000), (unsigned)s_n);
        }
        return;
    }

    if (fresh) {
        bb_spill(false);
        return;
    }

    // 26.9.19 S5-d：没新数据也要**定期报个平安**。这是 D3 的定因手段：
    //   - `sampled` 在涨、`stored` 不动 → 定时器活着，只是这段时间堆真的没变；
    //   - `sampled` 也不动了           → 采样定时器停摆（这才是缺陷）；
    //   - `FULL`                      → 128 点写满，后面的变化确实丢了。
    // 没有这一行，26.9.18 的「140 s 后安静 104 秒」三种原因长得一模一样。
    // 26.9.19 S5-d: a heartbeat even when nothing is new. This is what pins D3 down:
    // sampled rising + stored flat = timer alive, heap simply did not move; sampled flat
    // too = the timer died; FULL = the ring filled and later changes are gone.
    const uint32_t now_ms = (uint32_t)(dt / 1000);
    if (now_ms - s_hb_ms >= PICO_BOOTBB_HB_MS) {
        s_hb_ms = now_ms;
        ESP_LOGI(TAG,
                 "[DELAYED-BOOT] hb: t=%u ms, sampled %u, stored %u/%u%s, notes %u/%u, "
                 "last change @%u ms",
                 (unsigned)now_ms, (unsigned)s_samples, (unsigned)s_n,
                 (unsigned)PICO_BOOTBB_POINTS, s_n >= PICO_BOOTBB_POINTS ? " FULL" : "",
                 (unsigned)s_notes, (unsigned)PICO_BOOTBB_NOTES,
                 s_n ? (unsigned)s_blob->pts[s_n - 1].t_ms : 0u);
    }
}

void pico_bootbb_replay(void) {
    // 只在「一次都没吐出去」时兜底：已经吐过就不重复刷屏。
    // Backstop ONLY when nothing was ever spilled: no repeat spam otherwise.
    if (!s_active || s_printed != 0 || s_n == 0) return;
    if (!usb_serial_jtag_is_connected()) return;
    bb_spill(true);
}

bool pico_bootbb_active(void) { return s_active; }

unsigned pico_bootbb_count(void) { return s_n; }

#else  // !CONFIG_PICO_BOOTBB

// ISO C 要求翻译单元至少有一个声明；开关关掉时本文件只留这一行。
// ISO C requires a non-empty translation unit; with the switch off this is all there is.
typedef int pico_bootbb_disabled_t;

#endif  // CONFIG_PICO_BOOTBB
