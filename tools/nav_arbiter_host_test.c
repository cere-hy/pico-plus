/* SPDX-License-Identifier: Apache-2.0
 * M 批导航仲裁「只认最后一指」· 宿主守卫。
 * 编同一份 main/app/nav_arbiter.h（产品码与守卫不复制判据，见头注）。
 *
 * 断言（对齐交付区《M批导航仲裁-判据定稿》§6）：
 *   1. 混序快速连点(快于稳定窗口) → 仅一次切换；
 *   2. 终点 = 最后一键；
 *   3. 合并/丢弃计数与现象一致，且在收敛(消费)之前就已在仲裁层产生、消费点不补记；
 *   4. 回归：单击正常切一次；两次慢点击(跨窗口)各切一次；
 *   5. 变异：以 -DNAV_ARB_MUTANT_NO_ARBITRATION 编译(删仲裁层)时，
 *      「仅一次切换」断言必须失败 → 守卫返回非 0，证明守卫真在守。
 *
 * 模拟的是主循环的「事件脚本」，不是真设备：
 *   HIT(tab) = 一次底栏 pressed 边沿；TICK(n) = 推进 n 个 5ms 主循环 tick。
 *   仲裁只在导航命中路径被调用；页面内手势 / 设置 / OTA 命中不喂给本守卫(爆炸半径外)。
 *
 * 窗定义附录（M 批对账基准，窗长由真机日志对齐，守卫只验不变式）：
 *   A   收敛窗        40 ms   = NAV_ARB_STABLE_TICKS(8) × LOOP_TICK_MS(5)
 *   B1  封面批次窗    ≈2.9 s   书架封面批次一轮；期间主循环停转、不 tick，
 *                             命中只在这条链的行边界插点上被折叠(D 层)
 *   B2  Files 头部窗  3.66 s   Files 进入时同步目录扫描(count_root×3 + scan_folder)，
 *                             逐条 readdir 采样(C① 层)
 *   主页面刷屏段      0.82 s   书架首帧/切页推屏段，段间采样(C② 层)
 *   三者共同的强不变式只有一条：窗内多次命中，窗后仍只切一次。
 *
 * 用法:
 *   cc -std=c11 -Wall -Wextra -Werror -I main/app tools/nav_arbiter_host_test.c -o guard && ./guard
 *   cc ... -DNAV_ARB_MUTANT_NO_ARBITRATION ... -o guard_mutant && ./guard_mutant   # 期望 RC!=0
 */
#include <stdio.h>
#include "nav_arbiter.h"

/* 事件脚本：kind 0 = HIT(tab)，1 = TICK(n)。 */
typedef struct { int kind; int val; } ev_t;
#define HIT(t)  {0, (t)}
#define TICK(n) {1, (n)}

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } } while (0)

/* 模拟一次「连点序列」→ 收敛，返回统计。
 * request_app 是唯一的家：仅收敛(或变异每指)时写入；switches 计实际切页次数。 */
typedef struct {
    int      final_tab;      /* 最后一次写入 request_app 的 tab */
    int      switches;       /* 消费切换次数 */
    unsigned merged;         /* 收敛前仲裁层合并计数快照 */
    unsigned dropped;        /* 收敛前仲裁层丢弃计数快照 */
    unsigned merged_after;   /* 收敛后(消费点)合并计数——应不变，证明计数不靠消费点补记 */
    unsigned dropped_after;
    unsigned commits;        /* 仲裁层执行计数 */
} sim_result_t;

static int simulate(const ev_t *evs, int n, sim_result_t *out) {
    nav_arbiter_t a;
    nav_arb_reset(&a);
    int request_app = -1;
    int switches = 0;
    int mutant_seen = 0;
    unsigned merged_before_settle = 0, dropped_before_settle = 0;
    int snap_taken = 0;

    for (int i = 0; i < n; ++i) {
        if (evs[i].kind == 0) {
            nav_arb_action_t act = nav_arb_on_hit(&a, evs[i].val);
            if (act == NAV_ARB_COMMIT_EACH) {
                /* 变异构建：删仲裁层 → 每指各自立即切一次(旧「连跳」行为)。 */
                request_app = evs[i].val;
                switches++;
                mutant_seen = 1;
            }
            /* 正常构建：OPEN/MERGE/DROP 都只动单槽/计数，不写 request_app(延迟到收敛)。 */
        } else {
            for (int t = 0; t < evs[i].val; ++t) {
                if (nav_arb_tick(&a)) {
                    /* 收敛前快照仲裁层计数(证明计数在此已产生)。 */
                    if (!snap_taken) {
                        merged_before_settle = nav_arb_merged(&a);
                        dropped_before_settle = nav_arb_dropped(&a);
                        snap_taken = 1;
                    }
                    int adopt = nav_arb_settle(&a);
                    if (!mutant_seen && adopt >= 0) {
                        request_app = adopt;   /* 一次性写入唯一的家 */
                        switches++;            /* 消费点随后只切一次 */
                    }
                }
            }
        }
    }
    out->final_tab = request_app;
    out->switches = switches;
    out->merged = merged_before_settle;
    out->dropped = dropped_before_settle;
    out->merged_after = nav_arb_merged(&a);
    out->dropped_after = nav_arb_dropped(&a);
    out->commits = nav_arb_commits(&a);
    return 0;
}

int main(void) {
    /* ---- C1 混序快速连点：6 指、快于稳定窗口，终点应为最后一键 2 ---- */
    {
        static const ev_t evs[] = {
            HIT(2), TICK(2), HIT(0), TICK(2), HIT(3), TICK(2),
            HIT(1), TICK(2), HIT(3), TICK(2), HIT(2), TICK(2),
            TICK(12),   /* 停手 → 稳定 → 收敛一次 */
        };
        sim_result_t r;
        simulate(evs, (int)(sizeof(evs)/sizeof(evs[0])), &r);
        /* 命中序列 [2,0,3,1,3,2]：1 次 OPEN + 5 次 MERGE(每次换键) + 0 次 DROP。 */
        CHECK(r.switches == 1, "C1 混序连点应仅一次切换, got %d", r.switches);
        CHECK(r.final_tab == 2, "C1 终点应为最后一键 2, got %d", r.final_tab);
        CHECK(r.merged == 5, "C1 合并计数应为 5, got %u", r.merged);
        CHECK(r.dropped == 0, "C1 丢弃计数应为 0, got %u", r.dropped);
        CHECK(r.merged == r.merged_after, "C1 合并计数不得靠消费点补记(收敛前后应相等)");
        CHECK(r.dropped == r.dropped_after, "C1 丢弃计数不得靠消费点补记(收敛前后应相等)");
        CHECK(r.commits == 1, "C1 仲裁层执行计数应为 1, got %u", r.commits);
    }

    /* ---- C2 带重复键的连点：[1,1,3,1]，同键重复被 DROP，终点最后一键 1 ---- */
    {
        static const ev_t evs[] = {
            HIT(1), TICK(2), HIT(1), TICK(2), HIT(3), TICK(2), HIT(1), TICK(2),
            TICK(12),
        };
        sim_result_t r;
        simulate(evs, (int)(sizeof(evs)/sizeof(evs[0])), &r);
        /* [1,1,3,1]：OPEN(1) + DROP(1 重复) + MERGE(3) + MERGE(1) = merged 2 / dropped 1。 */
        CHECK(r.switches == 1, "C2 应仅一次切换, got %d", r.switches);
        CHECK(r.final_tab == 1, "C2 终点应为最后一键 1, got %d", r.final_tab);
        CHECK(r.merged == 2, "C2 合并计数应为 2, got %u", r.merged);
        CHECK(r.dropped == 1, "C2 丢弃计数应为 1, got %u", r.dropped);
        CHECK(r.merged + r.dropped + 1 == 4, "C2 计数与命中数(4)应对齐(OPEN 计 1)");
        CHECK(r.commits == 1, "C2 仲裁层执行计数应为 1, got %u", r.commits);
    }

    /* ---- C3 回归：单击一次 → 正常切一次，不因仲裁丢失 ---- */
    {
        static const ev_t evs[] = { HIT(3), TICK(12) };
        sim_result_t r;
        simulate(evs, (int)(sizeof(evs)/sizeof(evs[0])), &r);
        CHECK(r.switches == 1, "C3 单击应正常切一次, got %d", r.switches);
        CHECK(r.final_tab == 3, "C3 单击终点应为 3, got %d", r.final_tab);
        CHECK(r.merged == 0 && r.dropped == 0, "C3 单击不应有合并/丢弃");
    }

    /* ---- C4 回归：两次慢点击(各自跨稳定窗口) → 各切一次，共两次 ---- */
    {
        static const ev_t evs[] = {
            HIT(0), TICK(12),   /* 单击 0 → 收敛切一次 */
            HIT(3), TICK(12),   /* 停手后新一次单击 3 → 再收敛切一次 */
        };
        sim_result_t r;
        simulate(evs, (int)(sizeof(evs)/sizeof(evs[0])), &r);
        CHECK(r.switches == 2, "C4 两次慢点击应各切一次(共 2), got %d", r.switches);
        CHECK(r.final_tab == 3, "C4 终点应为最后一次单击 3, got %d", r.final_tab);
    }

    /* ---- B1 D 层封面批次窗（≈2.9s）：窗内不 tick，命中只在行边界折叠 ---- */
    {
        static const ev_t evs[] = {
            HIT(3),      /* 批次第 1 行边界上的一指 → OPEN */
            HIT(2),      /* 同窗换键 → MERGE，只认最后一指 */
            HIT(2),      /* 同键重复 → DROP */
            TICK(12),    /* 批次结束、主循环恢复 → 一个收敛窗后只切一次 */
        };
        sim_result_t r;
        simulate(evs, (int)(sizeof(evs)/sizeof(evs[0])), &r);
        /* 窗内没有 tick 就没有收敛：若插点让主循环逐指收敛，这里会变成 3 次切换。 */
        CHECK(r.switches == 1, "B1 批次窗内 3 指应仅一次切换, got %d", r.switches);
        CHECK(r.final_tab == 2, "B1 终点应为最后一指 2, got %d", r.final_tab);
        CHECK(r.merged == 1, "B1 合并计数应为 1, got %u", r.merged);
        CHECK(r.dropped == 1, "B1 丢弃计数应为 1, got %u", r.dropped);
        CHECK(r.commits == 1, "B1 仲裁层执行计数应为 1, got %u", r.commits);
        CHECK(r.merged == r.merged_after && r.dropped == r.dropped_after,
              "B1 计数不得靠消费点补记(收敛前后应相等)");
    }

    /* ---- B2 C① Files 头部窗（3.66s）：按住不灌水 / 灌水也安全但计数会暴露 ---- */
    {
        /* 正常采样：边沿自建，一只按住的手只在首拍产生一条命中。 */
        static const ev_t held[] = { HIT(1), TICK(12) };
        sim_result_t r;
        simulate(held, (int)(sizeof(held)/sizeof(held[0])), &r);
        CHECK(r.switches == 1, "B2 按住一只手指应切一次, got %d", r.switches);
        CHECK(r.merged == 0 && r.dropped == 0, "B2 按住不得产生合并/丢弃(边沿只认首拍)");

        /* 退化采样（“按下即算”）：同一只按住的手在每个插点各喂一次，7 指全同键。
         * 安全性仍是 1 次切换；计数把灌水当场写进日志（dropped=6），真机对账据此定位。 */
        static const ev_t flooded[] = {
            HIT(1), HIT(1), HIT(1), HIT(1), HIT(1), HIT(1), HIT(1), TICK(12),
        };
        simulate(flooded, (int)(sizeof(flooded)/sizeof(flooded[0])), &r);
        CHECK(r.switches == 1, "B2 灌水下仍只切一次, got %d", r.switches);
        CHECK(r.dropped == 6, "B2 灌水应被计数暴露(dropped=6), got %u", r.dropped);
        CHECK(r.merged == 0, "B2 同键灌水不得记合并, got %u", r.merged);
    }

    /* ---- B3 D+C 共槽：批次窗先折 3，主循环恢复后换 0，仍只切一次（同一个家）---- */
    {
        static const ev_t evs[] = {
            HIT(3), TICK(3),   /* D 层插点折进单槽；没到 8 拍，尚未收敛 */
            HIT(0), TICK(12),  /* 主循环自己的命中换槽 → 仍只认最后一指 */
        };
        sim_result_t r;
        simulate(evs, (int)(sizeof(evs)/sizeof(evs[0])), &r);
        /* 若 D 与 C 各持一份状态，这里会切成 2 次——共槽是「不造第二个家」的守卫。 */
        CHECK(r.switches == 1, "B3 D+C 共槽应仅一次切换, got %d", r.switches);
        CHECK(r.final_tab == 0, "B3 终点应为最后一指 0, got %d", r.final_tab);
        CHECK(r.merged == 1, "B3 跨层换键应记一次合并, got %u", r.merged);
        CHECK(r.commits == 1, "B3 仲裁层执行计数应为 1, got %u", r.commits);
    }

    printf("ALL_PASS: 仲裁「只认最后一指」守卫全绿（连点仅一次切换/终点最后一键/计数与现象一致/单击与慢点击回归）\n");
    return 0;
}
