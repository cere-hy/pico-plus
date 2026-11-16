// 状态栏差刷：纯逻辑真值表（26.11.16 电量批 / 26.11.18 节拍批）。
// 本文件只测 nav_status_watch.c，不碰任何硬件；真机行为由 app_loop 侧负责。
// Truth table for the status-strip repaint gate. Logic only - the loop owns the I/O.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "nav_status_watch.h"

static nav_watch_t w;

// 26.11.18：A–I2 十二组保持逐字不变，只把「分钟源」固定为缺席——它们断言的正是
// **没有分钟源的页面（首页 self_clock_tick=true）行为与 26.11.17 完全一致**，
// 也就是本批的零回归面。J 组才喂真实分钟。
// A–I2 keep their text: they assert the no-minute-source form (the home page), which is
// exactly this batch's zero-regression surface. Only the J groups feed a real minute.
#define STEP(p, c, v, t) nav_watch_step(&w, (p), (c), NAV_WATCH_NO_MINUTE, (v), (t))
#define COMMIT(p, c) nav_watch_commit(&w, (p), (c), NAV_WATCH_NO_MINUTE)

// 首帧登记：init 后第一次 step 只记录当前值，不刷（页面 render 已经画过）。
static void seed(int percent, bool charging) {
    nav_watch_init(&w);
    assert(STEP(percent, charging, true, 1000) == NAV_WATCH_IDLE);
}

// 书架形态的种子：带分钟。
static void seed_min(int percent, bool charging, int64_t minute) {
    nav_watch_init(&w);
    assert(nav_watch_step(&w, percent, charging, minute, true, 1000) == NAV_WATCH_IDLE);
}

int main(void) {
    // A. 值与充电位都不变：不许刷。跑 7 秒，每 200 ms 一拍。
    seed(84, false);
    for (uint32_t t = 1200; t < 8000; t += 200)
        assert(STEP(84, false, true, t) == NAV_WATCH_IDLE);

    // B. charging 置位：挂起，2000 ms 到点才刷一次；刷完落定，再不动。
    seed(84, false);
    assert(STEP(84, true, true, 2000) == NAV_WATCH_IDLE);
    assert(STEP(84, true, true, 3999) == NAV_WATCH_IDLE);
    assert(STEP(84, true, true, 4000) == NAV_WATCH_REPAINT);
    COMMIT(84, true);
    for (uint32_t t = 4200; t < 8000; t += 200)
        assert(STEP(84, true, true, t) == NAV_WATCH_IDLE);

    // C. 去抖窗内第二次变化（回到原值）：吞掉，永远不刷。
    seed(84, false);
    assert(STEP(84, true, true, 2000) == NAV_WATCH_IDLE);
    assert(STEP(84, false, true, 2500) == NAV_WATCH_IDLE);
    for (uint32_t t = 2600; t < 10000; t += 200)
        assert(STEP(84, false, true, t) == NAV_WATCH_IDLE);

    // D. 不可见不刷；恢复可见后首拍只登记（页面已重绘），不补刷。
    seed(84, false);
    assert(STEP(84, true, false, 2000) == NAV_WATCH_IDLE);
    assert(STEP(84, true, true, 2100) == NAV_WATCH_IDLE);
    for (uint32_t t = 2200; t < 7000; t += 200)
        assert(STEP(84, true, true, t) == NAV_WATCH_IDLE);

    // E. 百分比变化：立即刷（分钟级，不走去抖）。
    seed(84, false);
    assert(STEP(83, false, true, 2000) == NAV_WATCH_REPAINT);

    // E2. 百分比变化时 charging 也在挂起：一次刷把它一起落定，不留第二刷。
    seed(84, false);
    assert(STEP(84, true, true, 2000) == NAV_WATCH_IDLE);
    assert(STEP(83, true, true, 2200) == NAV_WATCH_REPAINT);
    COMMIT(83, true);
    assert(STEP(83, true, true, 2400) == NAV_WATCH_IDLE);

    // F. 首帧未知（-1 = 未测到）：只登记，不拿一次未知值去刷屏。
    nav_watch_init(&w);
    assert(STEP(-1, false, true, 10) == NAV_WATCH_IDLE);
    assert(STEP(-1, false, true, 210) == NAV_WATCH_IDLE);

    // G. ms 回绕：无符号相减，跨 0xFFFFFFFF 仍算得出 2000。
    nav_watch_init(&w);
    assert(STEP(84, false, true, 0xFFFFFF00u) == NAV_WATCH_IDLE);
    assert(STEP(84, true, true, 0xFFFFFF10u) == NAV_WATCH_IDLE);
    assert(STEP(84, true, true, 0x000006E0u) == NAV_WATCH_REPAINT);

    // H. 上屏失败不落定：窗到点 REPAINT 了，但调用方没 commit（err != SUCCESS）；
    //    下一拍必须继续要求重试，直到 commit 成功才安静。模块不碰 I/O，所以这条
    //    只能用「不调 commit」来模拟——它测的正是「落定权只在 commit」。
    //    H. A failed push must not settle: keep asking every tick until commit succeeds.
    seed(84, false);
    assert(STEP(84, true, true, 2000) == NAV_WATCH_IDLE);    // 挂起
    assert(STEP(84, true, true, 4000) == NAV_WATCH_REPAINT); // 窗到
    for (uint32_t t = 4200; t < 8000; t += 200)              // 上屏一直失败
        assert(STEP(84, true, true, t) == NAV_WATCH_REPAINT);
    COMMIT(84, true);                                        // 这一次成功
    for (uint32_t t = 8200; t < 12000; t += 200)
        assert(STEP(84, true, true, t) == NAV_WATCH_IDLE);

    // H2. 百分比路径同理：不 commit 就每拍重试，commit 后立刻安静。
    //     H2. Same for the percent path; retry until commit, then go quiet.
    seed(84, false);
    assert(STEP(83, false, true, 2000) == NAV_WATCH_REPAINT);
    for (uint32_t t = 2200; t < 4000; t += 200)
        assert(STEP(83, false, true, t) == NAV_WATCH_REPAINT);
    COMMIT(83, false);
    assert(STEP(83, false, true, 4200) == NAV_WATCH_IDLE);

    // I. 清除方向对称：已上屏 charging=true，拔线后同样走满 2000 ms，不是立即刷。
    //    若实现只给置位去抖、清除放行，本组当场红。
    //    I. Clearing debounces like plugging in; a one-way debounce fails here.
    seed(84, true);
    assert(STEP(84, false, true, 2000) == NAV_WATCH_IDLE);
    assert(STEP(84, false, true, 3999) == NAV_WATCH_IDLE);
    assert(STEP(84, false, true, 4000) == NAV_WATCH_REPAINT);
    COMMIT(84, false);
    for (uint32_t t = 4200; t < 8000; t += 200)
        assert(STEP(84, false, true, t) == NAV_WATCH_IDLE);

    // I2. 拔一次线只刷一下：拔线抖回、窗内回原值即吞，全程零刷。
    //     The "flash twice per unplug" failure mode, asserted from the clear side.
    seed(84, true);
    assert(STEP(84, false, true, 2000) == NAV_WATCH_IDLE); // 拔线挂起
    assert(STEP(84, true, true, 2500) == NAV_WATCH_IDLE);  // 窗内插回
    for (uint32_t t = 2600; t < 10000; t += 200)
        assert(STEP(84, true, true, t) == NAV_WATCH_IDLE);

    // ---- 26.11.18 节拍批：J 组（分钟源）------------------------------------------
    const int64_t M0 = 29600000; // 一个真实的 epoch 分钟量级（≈2026-11）

    // J1. 书架形态：同一分钟不刷，前进一格刷**整条**（不是电量小矩形）。
    seed_min(84, false, M0);
    for (uint32_t t = 1200; t < 60000; t += 200)
        assert(nav_watch_step(&w, 84, false, M0, true, t) == NAV_WATCH_IDLE);
    assert(nav_watch_step(&w, 84, false, M0 + 1, true, 60000) == NAV_WATCH_REPAINT_STRIP);

    // J2. 分钟不变、电量变：只刷电量区。两种矩形不能混——“反正都是整条”是错的。
    seed_min(84, false, M0);
    assert(nav_watch_step(&w, 83, false, M0, true, 2000) == NAV_WATCH_REPAINT);

    // J3. 对时跳变单独立组：-1 → N 恰好一刷；-1 → -1 永不刷。
    //     对时那一瞬是哨兵语义的核心时刻，不并进别的组。
    //    这里用**字面 -1**，不用宏：`ui_nav_clock_minute()` 返回的就是 -1，断言要锚真实值。
    //    宏只是配置层的别名，若有人把宏改成 0，这一组必须红（V2 刀咬的正是它）。
    //    Literal -1, not the macro: the time source returns -1, and the assertion must anchor
    //    the real value. If someone folds the sentinel to 0, this group has to fail.
    nav_watch_init(&w);
    assert(nav_watch_step(&w, 84, false, -1, true, 1000) == NAV_WATCH_IDLE);
    for (uint32_t t = 1200; t < 20000; t += 200) // 仍未校时：100 拍零刷
        assert(nav_watch_step(&w, 84, false, -1, true, t) == NAV_WATCH_IDLE);
    assert(nav_watch_step(&w, 84, false, M0, true, 20200) == NAV_WATCH_REPAINT_STRIP);
    nav_watch_commit(&w, 84, false, M0);
    assert(nav_watch_step(&w, 84, false, M0, true, 20400) == NAV_WATCH_IDLE);

    // J4. 同拍双变（分钟 + 百分比 + charging）：一次整条吞掉，commit 三个源后不补第二刷。
    //     若 commit 只提交触发源，这里会在下一拍补一次电量小矩形——正是要防的“双刷”。
    seed_min(84, false, M0);
    assert(nav_watch_step(&w, 83, true, M0 + 1, true, 60000) == NAV_WATCH_REPAINT_STRIP);
    nav_watch_commit(&w, 83, true, M0 + 1);
    for (uint32_t t = 60200; t < 80000; t += 200)
        assert(nav_watch_step(&w, 83, true, M0 + 1, true, t) == NAV_WATCH_IDLE);

    // J5. 回绕组：+65,536 分钟（int16 的低 16 位回绕）。int16 实现会判「没变」，
    //     这一组就是发现 2b 的可测断言——45.5 天不用等，宿主造 epoch 零成本。
    seed_min(84, false, 29600000);
    assert(nav_watch_step(&w, 84, false, 29600000 + 65536, true, 2000) == NAV_WATCH_REPAINT_STRIP);

    // J6. 整条刷失败不落定（H 组的分钟版）：不 commit 就每拍重试，commit 后安静。
    seed_min(84, false, M0);
    assert(nav_watch_step(&w, 84, false, M0 + 1, true, 60000) == NAV_WATCH_REPAINT_STRIP);
    for (uint32_t t = 60200; t < 70000; t += 200)
        assert(nav_watch_step(&w, 84, false, M0 + 1, true, t) == NAV_WATCH_REPAINT_STRIP);
    nav_watch_commit(&w, 84, false, M0 + 1);
    assert(nav_watch_step(&w, 84, false, M0 + 1, true, 70200) == NAV_WATCH_IDLE);

    // J7. 不可见清分钟账：隐藏期间分钟前进，回来后首拍只登记（页面已重绘），不补刷。
    seed_min(84, false, M0);
    assert(nav_watch_step(&w, 84, false, M0, true, 1200) == NAV_WATCH_IDLE);
    assert(nav_watch_step(&w, 84, false, M0 + 5, false, 1300) == NAV_WATCH_IDLE); // 隐藏
    assert(nav_watch_step(&w, 84, false, M0 + 5, true, 1400) == NAV_WATCH_IDLE);  // 回来：只登记
    for (uint32_t t = 1600; t < 60000; t += 200)
        assert(nav_watch_step(&w, 84, false, M0 + 5, true, t) == NAV_WATCH_IDLE);

    // J8. 首页形态（self_clock_tick=true → 分钟缺席）跑满三分钟：零整条刷。
    //     这是批的零回归断言：本批在首页上必须是可观测的空操作。
    seed(84, false);
    for (uint32_t t = 1200; t < 180000; t += 200)
        assert(STEP(84, false, true, t) == NAV_WATCH_IDLE);

    printf("nav_watch: 20 groups PASS\n");
    return 0;
}
