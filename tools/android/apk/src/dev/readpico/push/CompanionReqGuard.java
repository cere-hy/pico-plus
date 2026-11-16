/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：步 4 页号错位「张冠李戴」A 组修复（A1/A3/A4）的纯逻辑内核。
 * 零 Android 依赖，供 MainActivity 真实调用 + 宿主机 java 断言（守卫面 ①②③）。
 *
 * 背景（14:26 裁定采信）：对账三层都在、逐帧正确比对页号；漏洞是「帧里页号新而对、
 * 内容却是上一页的」——0x1C PAGE_BEGIN 用上一页正文缓存配新页号发请求，去重键又吞掉
 * 0x19 触发的正确请求。故禁令：加严页号丢弃不得做、只加延迟等待不得做。
 *
 * 本类只承载「发起请求的准入 / 去重键 / 回执内容比对」三段判定，全部为请求级纯函数，
 * 不读写任何全局可变字段——基准一律由调用方以局部 final 引用捕获后传入。
 *
 * English: pure-logic core for the step-4 page/content mismatch A-group fix.
 * Zero android deps; called by MainActivity and asserted by the java host test.
 * The reconciliation compared page numbers correctly; the bug was a fresh page
 * number paired with the previous page's text. Verdict bans tightening page-no
 * drops and mere delays. This class holds only request-scope decisions.
 */
package dev.readpico.push;

public final class CompanionReqGuard {
    private CompanionReqGuard() { }

    /** 发令来源。A1：摘要请求的唯一合法发令点是 0x19 页文本组装完成。 */
    public static final int SRC_PAGE_BEGIN_CACHED = 0;   // 0x1C PAGE_BEGIN 用缓存重请求（旧 P1 源）
    public static final int SRC_PAGE_TEXT = 1;           // 0x19 正文组装完成（唯一合法源）

    /**
     * A1 准入（0x1C 不发请求的回归护栏）：只有 0x19 正文组装完成才允许发起摘要请求。
     * onCompanionPageBegin 的 cached 重请求已删除；本判定留在 requestCompanionSummary
     * 入口作第二道闸——即便将来有人误把 0x1C-cached 调用加回来，也会被这里挡掉。
     */
    public static boolean mayIssueFrom(int source) {
        return source == SRC_PAGE_TEXT;
    }

    /** 发令来源的可读标签（供取证日志「来源=…」打印，现场只应见 0x19）。 */
    public static String sourceLabel(int source) {
        return source == SRC_PAGE_TEXT ? "0x19" : "0x1C-cached";
    }

    /**
     * A3 去重键：pageNo:mode:正文指纹。追加正文维度后，「同页同模式但不同正文」
     * （bandu4 已证同页 0x19 存在异文重推）不再被误判为重复而吞掉；「同页同模式
     * 同正文」仍命中去重，防止无谓重复请求。指纹取全量内容 hash + 长度，不用前 32B
     * （前缀相同、后段不同的正文会漏判）。
     */
    public static String reqKey(int pageNo, int mode, String pageText) {
        return pageNo + ":" + mode + ":" + fingerprint(pageText);
    }

    /** 全量正文指纹（长度 + 全串滚动 hash）。null 归一化为空串、与 "" 同指纹，
     *  使「无正文」在去重键里一视同仁，绝不抛异常。 */
    public static String fingerprint(String s) {
        if (s == null) s = "";
        long h = 0xcbf29ce484222325L;   // FNV-1a 64 位偏移基
        for (int i = 0; i < s.length(); i++) {
            h ^= s.charAt(i);
            h *= 0x100000001b3L;
        }
        return s.length() + ":" + Long.toHexString(h);
    }

    /**
     * A4 回执内容比对。capturedSnapshot = 发起请求时捕获的正文快照（局部 final 引用，
     * 不可变）；currentSnapshot = 回执时刻「当前登记的、最近一次合法请求」的正文快照
     * （在发起请求时写入，非回执时现读 companionLastPage——后者会被翻页覆写，裁定明确
     * 不得作基准）。二者相同 → 期间没有新请求顶掉本次，正文未变 → 发帧；不同 → 生成
     * 期间已翻页 / 同页异文重推 → 丢弃迟到回执，绝不把旧内容贴到新页号。
     * null 一律判失效（断链 / 会话复位后旧回执不得上屏）。
     */
    public static boolean receiptCurrent(String capturedSnapshot, String currentSnapshot) {
        if (capturedSnapshot == null || currentSnapshot == null) return false;
        return capturedSnapshot.equals(currentSnapshot);
    }

    /** 取证日志用：取正文前 n 个 char（日志用途，代理对切半无害）；null → ""。 */
    public static String head(String s, int n) {
        if (s == null || n <= 0) return "";
        return s.substring(0, Math.min(n, s.length()));
    }
}
