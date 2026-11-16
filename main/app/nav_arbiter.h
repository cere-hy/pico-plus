/* SPDX-License-Identifier: Apache-2.0
 * M 批导航仲裁「只认最后一指」· 纯逻辑状态机（header-only）。
 * 中文：产品码 main/app/app_loop.c 与宿主守卫 tools/nav_arbiter_host_test.c 编同一份，
 *       判据不复制两个家。本模块不持有导航意图队列：折叠窗口内只保留「最后一指」
 *       单槽 slot(int)，窗口收敛时由产品码一次性 ui_nav_request 写入唯一的家
 *       app_ctx_t::request_app；零 FIFO、零数组、零动态内存，静态量几十字节级。
 *       计数(合并/丢弃/执行)在仲裁层「边沿→单槽」这一步产生，不在消费点补记。
 * English: pure arbitration state machine shared by firmware and its host guard.
 * 冻结边界：cst836u 驱动 / ui_nav_hit 命中几何 / ui_nav_request / 消费点 /
 *       switch_to→app_present 刷屏链 / 菜单与开机轮询的 request_app 来源，全部零改动。
 *       cst836u_read 是当前触点快照(最多 2 点、无 FIFO)，主循环每 LOOP_TICK_MS=5ms
 *       轮询一次；「积压」是恢复轮询后逐个采到的 pressed 边沿各自触发切页的行为层
 *       现象，不是数据结构里的队列。折叠窗口合并的是「恢复轮询后实际采到的边沿」，
 *       不是窗口内全部物理点击——深度不预写，真机计数与现象对齐坐实。
 */
#ifndef PICO_NAV_ARBITER_H
#define PICO_NAV_ARBITER_H

#include <stdbool.h>

/* 折叠窗口：连续 NAV_ARB_STABLE_TICKS 个 tick(5ms each) 无导航命中即收敛。
 * 初始编排常量；真机窗口按计数对齐校准，深度不预写。刷屏期间主循环停转不 tick，
 * 恢复轮询后自然开始计稳定。 */
#define NAV_ARB_STABLE_TICKS 8

typedef enum {
    NAV_ARB_OPEN = 0,   /* 开窗首指：采纳、做单次局部回显，暂不写 request_app（延迟到收敛） */
    NAV_ARB_MERGE,      /* 窗口内不同键：覆盖单槽为最后一指，不回显、不写 request_app */
    NAV_ARB_DROP,       /* 窗口内同键重复：丢弃，不回显、不写 request_app */
    NAV_ARB_COMMIT_EACH /* 仅变异构建出现：删仲裁层后每指各自切一次（守卫须转红） */
} nav_arb_action_t;

typedef struct {
    bool     switching;  /* 折叠/切换窗口进行中标志（收敛时清位） */
    int      slot;       /* 窗口内「最后一指」单槽(tab，-1 空)；收敛时一次性写入 request_app */
    unsigned merged;     /* 合并计数：窗口内被后续命中覆盖进单槽的次数 */
    unsigned dropped;    /* 丢弃计数：窗口内被丢弃、不各自执行的命中次数 */
    unsigned commits;    /* 实际执行切换次数（收敛时 +1） */
    unsigned quiet;      /* 连续无导航命中 tick 数（状态稳定判定） */
} nav_arbiter_t;

static inline void nav_arb_reset(nav_arbiter_t *a) {
    a->switching = false;
    a->slot = -1;
    a->merged = 0;
    a->dropped = 0;
    a->commits = 0;
    a->quiet = 0;
}

static inline bool     nav_arb_switching(const nav_arbiter_t *a) { return a->switching; }
static inline int      nav_arb_slot(const nav_arbiter_t *a)      { return a->slot; }
static inline unsigned nav_arb_merged(const nav_arbiter_t *a)    { return a->merged; }
static inline unsigned nav_arb_dropped(const nav_arbiter_t *a)   { return a->dropped; }
static inline unsigned nav_arb_commits(const nav_arbiter_t *a)   { return a->commits; }

#ifndef NAV_ARB_MUTANT_NO_ARBITRATION
/* 正常产品逻辑：空闲开窗；窗口内只覆盖单槽(只认最后一指)或丢弃同键重复。 */
static inline nav_arb_action_t nav_arb_on_hit(nav_arbiter_t *a, int tab) {
    if (!a->switching) {
        a->switching = true;
        a->slot = tab;
        a->quiet = 0;
        a->merged = 0;   /* 新窗口开始：窗口计数归零，日志打印「本次连点吞了几指」 */
        a->dropped = 0;
        return NAV_ARB_OPEN;
    }
    a->quiet = 0;  /* 窗口内有命中，重置稳定计时 */
    if (a->slot == tab) {
        a->dropped++;
        return NAV_ARB_DROP;
    }
    a->slot = tab;  /* 单槽覆盖：后者胜，正是「只认最后一指」 */
    a->merged++;
    return NAV_ARB_MERGE;
}
#else
/* 变异：删除仲裁层，每个命中各自触发一次切换。守卫必须在此构建下转红。 */
static inline nav_arb_action_t nav_arb_on_hit(nav_arbiter_t *a, int tab) {
    a->slot = tab;
    a->commits++;
    return NAV_ARB_COMMIT_EACH;
}
#endif

/* 每 tick 末尾推进一次；on_hit 已把 quiet 清零，故无需入参。返回 true = 窗口应收敛。 */
static inline bool nav_arb_tick(nav_arbiter_t *a) {
    if (!a->switching) return false;
    a->quiet++;
    return a->quiet >= NAV_ARB_STABLE_TICKS;
}

/* 窗口收敛：返回最后一指 tab，清标志与单槽，commits +1。
 * 产品码据此一次性 ui_nav_request(&ctx, adopt) 写唯一的家，消费点随后切一次。 */
static inline int nav_arb_settle(nav_arbiter_t *a) {
    int adopt = a->slot;
    a->switching = false;
    a->slot = -1;
    a->quiet = 0;
    a->commits++;
    return adopt;
}

#endif /* PICO_NAV_ARBITER_H */
