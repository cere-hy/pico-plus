/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：内部堆取证的唯一实现。见 include/pico_heapdiag.h 的来由。
 * English: the single implementation of the internal-heap dump. See the header
 * for why it had to leave ble_page_turner.c.
 */
#include "pico_heapdiag.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "heapdiag";

// 站点表：按**文本**匹配，不按指针——两个 `"push-connect"` 字面量是否同一地址由
// 编译器决定，拿指针当身份迟早会分裂成两条计数。
// Site table matched by TEXT, never by pointer: whether two `"push-connect"` literals
// share an address is the compiler's business, and using the pointer as identity
// eventually splits one site into two counters.
typedef struct {
    const char *site;
    unsigned n;
} site_slot_t;

// 26.9.15 E0：8 → 16。上一版实测有 9 个不同站点（boot-pre / boot-post /
// transfer-enter / push-precheck / push-bypass / push-adv / push-connect / push-disc /
// ble-start），却只有 8 格——第 9 个撞上满表，`slot` 取 −1，dump 被**静默**丢弃。
// 为了取证而引入的新风险，而且失败无声，必须在它咬人之前修掉。
// 8 -> 16. The previous build had nine distinct sites and eight slots; the ninth hit a
// full table, got slot = -1, and was dropped SILENTLY. A risk introduced by the evidence
// tooling itself, failing without a word - fixed before it bites.
#define SITE_SLOTS 16

static site_slot_t s_sites[SITE_SLOTS];
static unsigned s_total;
static bool s_budget_warned;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void pico_heap_dump(const char *site, const char *who) {
    if (site == NULL) site = "?";
    if (who == NULL) who = "?";

    // 只在临界区里**记账**，打印一律在外面做：heap_caps_print_heap_info 是几十行
    // printf，塞进临界区等于开中断关几十毫秒，得不偿失。
    // Book-keeping only inside the critical section; all printing happens outside.
    // heap_caps_print_heap_info is dozens of printf lines, and holding interrupts off
    // for that long buys nothing.
    bool take = false;
    bool blocks = false;
    bool table_full = false;
    bool budget_out = false;
    bool warn_budget = false;
    unsigned seq = 0;
    unsigned nth = 0;
    unsigned used = 0;
    portENTER_CRITICAL(&s_lock);
    {
        int slot = -1;
        for (int i = 0; i < SITE_SLOTS; ++i) {
            if (s_sites[i].site == NULL) {
                s_sites[i].site = site;
                slot = i;
                break;
            }
            if (strcmp(s_sites[i].site, site) == 0) {
                slot = i;
                break;
            }
        }
        for (int i = 0; i < SITE_SLOTS; ++i) {
            if (s_sites[i].site != NULL) used++;
        }
        if (slot < 0) {
            table_full = true;
        } else if (s_total >= PICO_HEAP_DUMP_TOTAL) {
            budget_out = true;
            if (!s_budget_warned) {
                s_budget_warned = true;
                warn_budget = true;
            }
        } else if (s_sites[slot].n < PICO_HEAP_DUMP_PER_SITE) {
            nth = ++s_sites[slot].n;
            seq = ++s_total;
            take = true;
            blocks = (nth == 1); // 逐块清单只随每站第一份走 / block list rides the first per site
        }
        // 剩下的情况是「该站点已打满 PICO_HEAP_DUMP_PER_SITE 份」——这是设计行为，
        // 静默返回，不算丢弃。
        // The remaining case is "this site already used its budget" - by design, silent.
    }
    portEXIT_CRITICAL(&s_lock);

    if (table_full) {
        // 满表必须吼出来：这是配置错，不是策略。26.9.14 就是在这里一声不吭地丢了一个站点。
        // A full table must shout: it is a configuration bug, not a policy. 26.9.14 dropped
        // one site here without a word.
        ESP_LOGW(TAG, "DROPPED site=%s: site table full (%u/%u used) - raise SITE_SLOTS",
                 site, used, (unsigned)SITE_SLOTS);
        return;
    }
    if (budget_out) {
        if (warn_budget) {
            ESP_LOGW(TAG, "budget exhausted: %u/%u dumps handed out; further sites are dropped "
                          "(per-site cap %u still applies until then)",
                     s_total, (unsigned)PICO_HEAP_DUMP_TOTAL, (unsigned)PICO_HEAP_DUMP_PER_SITE);
        }
        return;
    }
    if (!take) return;

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 碎片率与传书页面板同一公式（1 − largest/free）。两处数字必须能逐位对上，
    // 否则「面板说红、闸门照开」那种各说各话会以另一种形式回来。
    // Fragmentation uses the transfer page's own formula (1 - largest/free). The two
    // must line up digit for digit, or "panel says red, gate opens anyway" comes back
    // in another form.
    unsigned frag = 100;
    if (info.total_free_bytes) {
        frag = info.largest_free_block >= info.total_free_bytes
                   ? 0
                   : (unsigned)(((info.total_free_bytes - info.largest_free_block) * 100u) /
                                info.total_free_bytes);
    }

    ESP_LOGW(TAG,
             "[%u/%u] site=%s who=%s internal free=%u largest=%u frag=%u%% "
             "blocks=%u (free %u / alloc %u) psram free=%u largest=%u",
             seq, (unsigned)PICO_HEAP_DUMP_TOTAL, site, who,
             (unsigned)info.total_free_bytes, (unsigned)info.largest_free_block, frag,
             (unsigned)info.total_blocks, (unsigned)info.free_blocks,
             (unsigned)info.allocated_blocks, (unsigned)psram_free, (unsigned)psram_largest);

    if (blocks) {
        // 逐块清单由 IDF 直接 printf 到控制台，不受日志级别过滤——取证包必须看得到。
        // The per-block listing is printf'd straight to the console by IDF and is not
        // filtered by the log level - the evidence build has to see it.
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    }
}

unsigned pico_heap_dump_count(void) { return s_total; }

void pico_heap_dump_blocks(const char *site, unsigned only_if_largest_below) {
    if (site == NULL) site = "?";
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest >= (size_t)only_if_largest_below) {
        // 健康时沉默。这一行本身就是结论：largest 没掉到闸门以下。/ Silent when
        // healthy; this single line IS the verdict: largest never fell below the gate.
        ESP_LOGI(TAG, "blocks: %s skipped (largest=%u >= %u)", site, (unsigned)largest,
                 only_if_largest_below);
        return;
    }
    // 病发：先把判据写清楚，再让 IDF 把 431 个块逐条倒出来。逐块清单是 printf 到
    // 控制台的，不受日志级别过滤——取证包必须看得到。/ Ill: state the predicate first,
    // then let IDF pour out every block. The listing goes to the console via printf and is
    // not filtered by log level - the evidence build has to see it.
    ESP_LOGW(TAG, "blocks: %s walking (largest=%u < %u)", site, (unsigned)largest,
             only_if_largest_below);
    heap_caps_dump(MALLOC_CAP_INTERNAL);
}

// ---- 26.9.20 S10：一行路标 ------------------------------------------------------
// 只回答「这一步让内部最大块掉了多少」。相邻两行的 delta= 直接点名是谁偷的。
// 26.9.20 S10: a one-line landmark. Neighbouring `delta=` values name the thief.
static int s_mark_largest = -1;

void pico_heap_mark(const char *step) {
    if (step == NULL) step = "?";
    multi_heap_info_t info;
    memset(&info, 0, sizeof(info));
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const int largest = (int)info.largest_free_block;
    if (s_mark_largest < 0) {
        ESP_LOGW(TAG, "mark %s internal free=%u largest=%u blocks=%u/%u delta=-",
                 step, (unsigned)info.total_free_bytes, (unsigned)largest,
                 (unsigned)info.free_blocks, (unsigned)info.allocated_blocks);
    } else {
        ESP_LOGW(TAG, "mark %s internal free=%u largest=%u blocks=%u/%u delta=%+d",
                 step, (unsigned)info.total_free_bytes, (unsigned)largest,
                 (unsigned)info.free_blocks, (unsigned)info.allocated_blocks,
                 largest - s_mark_largest);
    }
    s_mark_largest = largest;
}

// ---- 26.9.20 S13：8,192 B 的块是谁的 -------------------------------------------
// 任务栈在堆里是「栈 + 紧邻的 TCB」两笔分配，块表里就是 8192 紧跟 100。反查每个任务
// 栈的**真实块大小**，对上了就等于点名。数组放 PSRAM：这个工具绝不能自己变成池 A 的
// 租户，否则它测出来的数就先被自己污染了。
// A task stack is "stack + adjacent TCB" in the heap - 8192 then 100 in the table. Reverse
// look-up each task's real block size and the match names the owner. The array lives in
// PSRAM: the instrument must never become a tenant of pool A.
#define PICO_HEAP_ATTR_TASKS 24
// 26.9.21 S17：只报 ≥4 KB 的栈，最多列 8 个——面板上能一眼看完的规模。
// 26.9.21 S17: list stacks >= 4 KB only, at most 8 of them.
#define PICO_HEAP_ATTR_MIN_BLOCK 4096
#define PICO_HEAP_ATTR_HITS 8

void pico_heap_attr(const char *site) {
    if (site == NULL) site = "?";
    ESP_LOGW(TAG, "attr %s: internal largest=%u", site,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    TaskStatus_t *st = (TaskStatus_t *)heap_caps_malloc(
        sizeof(TaskStatus_t) * PICO_HEAP_ATTR_TASKS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (st == NULL) {
        ESP_LOGW(TAG, "attr: no PSRAM for %u task records - attribute by hand from the table",
                 (unsigned)(sizeof(TaskStatus_t) * PICO_HEAP_ATTR_TASKS));
        return;
    }
    const UBaseType_t n = uxTaskGetSystemState(st, PICO_HEAP_ATTR_TASKS, NULL);
    typedef struct { uintptr_t base; unsigned size; const char *name; } stack_hit_t;
    stack_hit_t *hits = (stack_hit_t *)heap_caps_malloc(
        sizeof(stack_hit_t) * PICO_HEAP_ATTR_HITS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned n_hits = 0;
    for (UBaseType_t i = 0; i < n; ++i) {
        // 空闲任务的栈是静态数组（.bss），不在任何堆区里；`heap_caps_get_allocated_size()`
        // 对这类指针会直接 `assert(heap)` 把固件打死。所以先用公开的
        // `heap_caps_check_integrity_addr(addr, false)` 问一句「这个地址在堆里吗」——
        // 它在区间外只返回 false，不打印、不 assert。
        // Idle-task stacks are static arrays in .bss, outside every heap region, and
        // heap_caps_get_allocated_size() would `assert(heap)` and kill the firmware. Ask the
        // public integrity probe first: outside every region it just returns false.
        const bool in_heap =
            heap_caps_check_integrity_addr((intptr_t)st[i].pxStackBase, false);
        if (!in_heap) {
            ESP_LOGW(TAG, "attr task %-14s stack=0x%08x static (not heap-backed)",
                     st[i].pcTaskName, (unsigned)(uintptr_t)st[i].pxStackBase);
            continue;
        }
        const unsigned sz = (unsigned)heap_caps_get_allocated_size(st[i].pxStackBase);
        const unsigned hwm = (unsigned)(st[i].usStackHighWaterMark * sizeof(StackType_t));
        ESP_LOGW(TAG, "attr task %-14s stack=0x%08x alloc=%u hwm=%u",
                 st[i].pcTaskName, (unsigned)(uintptr_t)st[i].pxStackBase, sz, hwm);
        if (sz < PICO_HEAP_ATTR_MIN_BLOCK || hits == NULL) continue;
        // 插入排序，只留最大的 8 个：块表里值得对地址的就是这几行。
        // 满了以后先比最小者，小了就直接丢——否则会把第 8 名顶掉。
        // Insertion sort keeping the 8 biggest; once full, drop anything smaller than
        // the current 8th instead of overwriting it.
        if (n_hits == PICO_HEAP_ATTR_HITS && sz <= hits[PICO_HEAP_ATTR_HITS - 1].size) continue;
        unsigned pos = n_hits < PICO_HEAP_ATTR_HITS ? n_hits : PICO_HEAP_ATTR_HITS - 1;
        while (pos > 0 && hits[pos - 1].size < sz) {
            hits[pos] = hits[pos - 1];
            --pos;
        }
        hits[pos].base = (uintptr_t)st[i].pxStackBase;
        hits[pos].size = sz;
        hits[pos].name = st[i].pcTaskName;
        if (n_hits < PICO_HEAP_ATTR_HITS) ++n_hits;
    }
    // 26.9.21 S17/D9：结论行从「最大的任务栈是谁」改成**按块反查**。
    // 26.9.20 打的是 `biggest task stack = 12800 B (main @0x3fcc8b74)`——这句话本身没错，
    // 但它回答的不是我们问的问题。真正要问的是「块表里那个 8,192 是谁」，而答案
    // （nimble_host）当时藏在上面十几行的任务列表里，只能靠人眼对地址。
    // 现在把 ≥4 KB 的栈按大小降序逐条点名，格式直接对齐块表那行 `Block 0x… size: N bytes`。
    // 26.9.21 S17/D9: the verdict used to name the biggest task stack, which is not the
    // question. What we ask is "who owns the 8,192 B block in the table" - the answer was
    // buried in the task lines above. Emit every stack >= 4 KB in descending size, in the
    // same shape as the table's `Block 0x... size: N bytes` line.
    ESP_LOGW(TAG, "attr %s: %u tasks, %u heap-backed stack(s) >= %u B - match each address "
                  "against the table's 'Block 0x... size: N bytes, Free: No'",
             site, (unsigned)n, n_hits, (unsigned)PICO_HEAP_ATTR_MIN_BLOCK);
    for (unsigned i = 0; i < n_hits; ++i) {
        ESP_LOGW(TAG, "attr %s: block 0x%08x %5u B -> %s", site,
                 (unsigned)hits[i].base, hits[i].size, hits[i].name);
    }
    if (hits == NULL) {
        ESP_LOGW(TAG, "attr %s: no PSRAM for the block list - read the sizes from the task lines",
                 site);
    }
    heap_caps_free(hits);
    heap_caps_free(st);
#else
    ESP_LOGW(TAG, "attr %s: CONFIG_FREERTOS_USE_TRACE_FACILITY is off, so the task list is "
                  "not walkable - enable it (read-only) to name the 8,192 B block", site);
#endif
}
