/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：M1.3（v1.4.3）息屏查词保活 + 事件驱动重试的**纯逻辑核心**——零 android.*
 * 依赖，宿主机可测（见 test/AiKeepAliveTest.java）。框架胶水（FGS 服务、
 * BroadcastReceiver、NetworkCallback）在 MainActivity 与 ListenKeepAliveService
 * 里，只调本类的判定。
 *
 * 背景（用户真机账）：手机息屏后 APK 查词 DNS 失败——BLE notify 照常到达（不走
 * 网络栈），但 LLM 查询的后台网络被 Doze + 国产 ROM 省电策略挂起；解锁瞬间恢复。
 * 设备侧「失败不回帧」兜底正确（15s 超时回退词典），本类负责让 APK 侧在挂起期间
 * 不空转、恢复瞬间补得上。
 *
 * English: the M1.3 (v1.4.3) screen-off keep-alive and event-driven retry logic
 * core - zero android.* imports, host-testable. Framework glue (FGS service,
 * receivers, network callbacks) lives in MainActivity / ListenKeepAliveService
 * and only calls the decisions here.
 */

package dev.readpico.push;

/**
 * 保活与重试的纯逻辑。两个关注点分开：
 * ① 保活状态组合判定（FGS 运行中 × 电池白名单）——状态行「保活：完整/受限」的真相源；
 * ② 一次点词查询的重试计划——失败后一次 ~3s 快速重试（防瞬时 DNS 抖动），之后只由
 *    事件（网络可用 / 亮屏）触发，窗口 cap 14s（贴设备 15s 超时：窗口内补发的 0x07
 *    仍能落到 pending 卡原地更新；超窗放弃，迟到释义进设备侧预取缓存走门 A 语义，
 *    用户下次点 AI chip 秒出，不浪费）。
 */
public final class AiKeepAlive {

    /** 保活档位：完整 = FGS 运行中且已入电池白名单；受限 = 缺任一。 */
    public enum KeepAlive {
        FULL,
        RESTRICTED
    }

    /** 组合判定（顺序无关，纯函数）。 */
    public static KeepAlive keepAliveState(boolean fgsRunning, boolean batteryWhitelisted) {
        return fgsRunning && batteryWhitelisted ? KeepAlive.FULL : KeepAlive.RESTRICTED;
    }

    // ---- 重试计划参数 ----

    /** 失败后的快速重试延迟：防瞬时 DNS 抖动，不给系统事件当唯一依赖。 */
    public static final int QUICK_RETRY_DELAY_MS = 3000;
    /**
     * 重试窗口 cap：贴设备 15s 超时留 1s 余量。窗口内补发成功，0x07 仍能落到
     * pending 卡原地更新（门 B 语义）；超窗放弃——迟到 0x07 进预取缓存（门 A），
     * 下次点同一词秒出。Doze 期定时重试是空转（网络仍挂起、必败、白耗电），所以
     * 快速重试之后只挂事件，不排定时轮询。
     */
    public static final int RETRY_WINDOW_MS = 14000;

    // ---- 一次点词的重试计划 ----

    private final String word;
    private final long bookId;
    private final long pos;
    private final long startMs;
    private boolean quickRetryUsed;
    private boolean finished;

    /**
     * @param nowMs 查询发起的时刻（调用方时钟，宿测可直接推进）。
     */
    public AiKeepAlive(String word, long bookId, long pos, long nowMs) {
        this.word = word == null ? "" : word;
        this.bookId = bookId;
        this.pos = pos;
        this.startMs = nowMs;
    }

    public String word() { return word; }
    public long bookId() { return bookId; }
    public long pos() { return pos; }
    public long startMs() { return startMs; }
    public boolean isFinished() { return finished; }
    public boolean quickRetryUsed() { return quickRetryUsed; }

    /** 标记终态（重试成功已回帧 / 超窗放弃 / 监听断开），此后一切重试资格关闭。 */
    public void markFinished() { finished = true; }

    /**
     * 快速重试到期？未完成、额度未用、且已过 QUICK_RETRY_DELAY_MS。
     * 事件若早到，走 {@link #consumeEventRetry} 并消耗本额度——不重复发。
     */
    public boolean quickRetryDue(long nowMs) {
        return !finished && !quickRetryUsed && nowMs - startMs >= QUICK_RETRY_DELAY_MS;
    }

    /** 消耗快速重试额度（快速重试真的发了，或事件重试抢先发了）。 */
    public void consumeQuickRetry() { quickRetryUsed = true; }

    /**
     * 事件（ConnectivityManager.onAvailable / ACTION_SCREEN_ON）触发的重试资格：
     * 未完成、在窗口内。事件触发时调用方应同时 {@link #consumeQuickRetry()}——
     * 抢在快速重试前面就把额度用掉，避免同一次查询两条路各发一遍。
     */
    public boolean eventRetryDue(long nowMs) {
        return !finished && nowMs - startMs < RETRY_WINDOW_MS;
    }

    /** 窗口耗尽（快速重试已用、仍无事件救回）：放弃重试，查询进入终态。 */
    public boolean windowExpired(long nowMs) {
        return !finished && quickRetryUsed && nowMs - startMs >= RETRY_WINDOW_MS;
    }
}
