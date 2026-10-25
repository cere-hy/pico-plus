/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：M1.3 保活 + 事件驱动重试的宿主机测试（纯逻辑，零 android.*）。覆盖：
 * ① 保活组合判定四态 ② 快速重试的时序与额度一次性 ③ 事件重试的窗口资格与
 * 额度抢占 ④ 14s 窗口 cap 与终态封闭。
 *
 * English: host test for the M1.3 keep-alive + event-driven retry logic (pure
 * logic, zero android.*). Covers: 1. the four keep-alive combinations 2. the
 * quick retry's timing and one-shot budget 3. the event retry's window
 * eligibility and budget preemption 4. the 14s cap and the finished seal.
 */
package dev.readpico.push;

import dev.readpico.push.AiKeepAlive.KeepAlive;

public final class AiKeepAliveTest {
    private static int pass = 0;
    private static int fail = 0;

    private static void check(boolean cond, String msg) {
        if (cond) {
            pass++;
            System.out.println("  [PASS] " + msg);
        } else {
            fail++;
            System.out.println("  [FAIL] " + msg);
        }
    }

    public static void main(String[] args) {
        System.out.println("AiKeepAliveTest（纯逻辑，零 android 依赖）:");

        // 1) 保活组合判定：FGS × 白名单 四态
        check(AiKeepAlive.keepAliveState(true, true) == KeepAlive.FULL, "FGS+白名单=完整");
        check(AiKeepAlive.keepAliveState(true, false) == KeepAlive.RESTRICTED, "仅 FGS=受限（深度 Doze 网络仍挂起）");
        check(AiKeepAlive.keepAliveState(false, true) == KeepAlive.RESTRICTED, "仅白名单=受限（进程可能被回收）");
        check(AiKeepAlive.keepAliveState(false, false) == KeepAlive.RESTRICTED, "两者皆无=受限");

        // 2) 快速重试时序：3s 前不到期、3s 到期、额度一次性
        AiKeepAlive a = new AiKeepAlive("hunger", 0x1234L, 5678L, 10000L);
        check(!a.quickRetryDue(10000L), "t+0 不排快速重试");
        check(!a.quickRetryDue(12999L), "t+2.999s 不到期");
        check(a.quickRetryDue(13000L), "t+3s 到期");
        a.consumeQuickRetry();
        check(!a.quickRetryDue(14000L), "额度用掉后不再排（一次性）");
        check(a.quickRetryUsed(), "额度已用");

        // 3) 事件重试：窗口内随时可触发；事件早到抢先消耗额度，不与快速重试双发
        AiKeepAlive b = new AiKeepAlive("word", 0L, 0L, 0L);
        check(b.eventRetryDue(1500L), "事件早到（t+1.5s）也有资格——不等 3s 保底");
        b.consumeQuickRetry(); // 调用方语义：事件触发即消耗额度
        check(!b.quickRetryDue(3000L), "事件抢先消耗额度，3s 保底不再排（不双发）");
        check(b.eventRetryDue(13999L), "t+13.999s 事件仍可触发");
        check(!b.eventRetryDue(14000L), "t+14s 窗口关闭");
        check(b.windowExpired(14000L), "t+14s 窗口耗尽");

        // 4) 终态封闭：markFinished 后一切资格关闭（成功回帧 / 超窗放弃 / 断开）
        AiKeepAlive c = new AiKeepAlive("word", 0L, 0L, 0L);
        c.markFinished();
        check(!c.eventRetryDue(1L) && !c.quickRetryDue(99999L) && !c.windowExpired(99999L),
                "终态后事件/快速/窗口全部封闭");
        check(c.isFinished(), "isFinished 反映终态");

        // 5) 载荷透传：词 / 书标识 / 位置原样带在计划里（重发时要关连同一词）
        AiKeepAlive d = new AiKeepAlive("hunger", 0xABCDL, 99L, 500L);
        check(d.word().equals("hunger") && d.bookId() == 0xABCDL && d.pos() == 99L
                && d.startMs() == 500L, "重试计划透传词/书标识/位置/起始时刻");
        check(new AiKeepAlive(null, 0L, 0L, 0L).word().isEmpty(), "null 词归一为空串不抛");

        // 6) 窗口 cap 与设备 15s 超时的关系（常量值即设计决定，防呆）
        check(AiKeepAlive.QUICK_RETRY_DELAY_MS == 3000, "快速重试 3s（防瞬时 DNS 抖动）");
        check(AiKeepAlive.RETRY_WINDOW_MS == 14000, "窗口 cap 14s（贴设备 15s 超时留 1s）");

        if (fail > 0) {
            System.out.println("AiKeepAliveTest FAILED: pass=" + pass + " fail=" + fail);
            System.exit(1);
        }
        System.out.println("AiKeepAliveTest passed: " + pass + " checks");
    }
}
