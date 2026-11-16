/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：步 4 页号错位 A 组修复（A1/A3/A4）的宿主机守卫断言，14:26 裁定 §三守卫面
 * ①②③。纯逻辑、零 android 依赖，测的是 MainActivity 真实调用的 CompanionReqGuard。
 * 禁令回归：加严页号丢弃 / 只加延迟等待均不得做——本测试不含任何「页号丢弃」判定，
 * 只钉死「发令来源」「去重键含正文维度」「回执内容比对」三段请求级判定。
 *
 * English: host-side guard asserts for the A-group fix (verdict section 3,
 * guards 1-3). Pure logic, zero android deps, exercises the real guard class.
 */
package dev.readpico.push;

public final class CompanionReqGuardTest {
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
        System.out.println("CompanionReqGuardTest（步 4 A 组守卫，纯逻辑，零 android 依赖）:");

        // ---- 守卫 ①：0x1C 到达不再发起任何请求（cached 分支绝迹回归护栏） ----
        // A1：唯一合法发令点是 0x19 正文组装完成。0x1C-cached 一律在准入闸被挡。
        check(CompanionReqGuard.SRC_PAGE_TEXT == 1
                        && CompanionReqGuard.SRC_PAGE_BEGIN_CACHED == 0,
                "发令源常量在案：0x19=1 / 0x1C-cached=0");
        check(CompanionReqGuard.mayIssueFrom(CompanionReqGuard.SRC_PAGE_TEXT),
                "0x19 正文组装完成 = 唯一合法发令点（放行）");
        check(!CompanionReqGuard.mayIssueFrom(CompanionReqGuard.SRC_PAGE_BEGIN_CACHED),
                "0x1C PAGE_BEGIN cached 重请求 = 准入闸拒绝（A1 护栏）");
        check(!CompanionReqGuard.mayIssueFrom(-1)
                        && !CompanionReqGuard.mayIssueFrom(2)
                        && !CompanionReqGuard.mayIssueFrom(99),
                "任何非 0x19 来源一律拒绝（脏编号防御）");
        check("0x19".equals(CompanionReqGuard.sourceLabel(CompanionReqGuard.SRC_PAGE_TEXT)),
                "取证日志标签：0x19 来源打印为 0x19");
        check("0x1C-cached".equals(
                        CompanionReqGuard.sourceLabel(CompanionReqGuard.SRC_PAGE_BEGIN_CACHED)),
                "取证日志标签：0x1C 来源打印为 0x1C-cached（现场若见此即回归）");

        // ---- 守卫 ②：去重键含正文维度（同页同模式不同正文不误判重复） ----
        String a = "九岁的海伦剪了头发，母亲在厨房里叹气。";
        String b = "十五岁的艾米丽把自己锁在房间，窗外的雨下个不停。";
        check(!CompanionReqGuard.reqKey(5, 2, a).equals(CompanionReqGuard.reqKey(5, 2, b)),
                "同页(5)同模式(2)不同正文 → 去重键不同（异文不被误判重复）");
        check(CompanionReqGuard.reqKey(5, 2, a).equals(CompanionReqGuard.reqKey(5, 2, a)),
                "同页同模式同正文 → 去重键相同（仍去重防浪费）");
        // 前缀相同、后段不同：证明指纹是全量内容，不是前 32B 前缀。
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 40; i++) sb.append('同');
        String longPrefixA = sb.toString() + "结尾甲";
        String longPrefixB = sb.toString() + "结尾乙";
        check(!CompanionReqGuard.reqKey(5, 2, longPrefixA)
                        .equals(CompanionReqGuard.reqKey(5, 2, longPrefixB)),
                "前 40 字相同、仅末字不同 → 键不同（全量指纹，非前缀）");
        check(!CompanionReqGuard.reqKey(5, 2, a).equals(CompanionReqGuard.reqKey(6, 2, a)),
                "页号不同 → 键不同");
        check(!CompanionReqGuard.reqKey(5, 2, a).equals(CompanionReqGuard.reqKey(5, 3, a)),
                "模式不同 → 键不同");
        check(CompanionReqGuard.reqKey(5, 2, null).equals(CompanionReqGuard.reqKey(5, 2, "")),
                "null 正文归一化为空串（不抛异常）");
        check(CompanionReqGuard.fingerprint(a).equals(CompanionReqGuard.fingerprint(a))
                        && !CompanionReqGuard.fingerprint(a).equals(CompanionReqGuard.fingerprint(b)),
                "指纹确定性：同文相同 / 异文不同");
        check(CompanionReqGuard.reqKey(5, 2, a).contains(":2:"),
                "去重键仍保留 page:mode 两段（不破坏既有对账维度）");

        // ---- 守卫 ③：回执内容比对命中（未变→发帧）与不命中（已变→不发帧）两路 ----
        // 基准 = 发起请求时捕获的正文快照（局部引用）；current = 回执时登记的最新请求快照。
        check(CompanionReqGuard.receiptCurrent(a, a),
                "正文未变 → 命中 → 正常发帧");
        check(!CompanionReqGuard.receiptCurrent(a, b),
                "正文已变（翻页/异文重推）→ 不命中 → 丢弃迟到回执，不发帧");
        check(!CompanionReqGuard.receiptCurrent(a, null),
                "回执时登记快照已被清空（断链/复位）→ 不发帧");
        check(!CompanionReqGuard.receiptCurrent(null, a),
                "发起快照为空 → 不发帧");
        check(CompanionReqGuard.receiptCurrent("", ""),
                "空页占位：两空相等 → 发空帧（设备落「本次未生成」）");
        // A3 + A4 配合：同页异文重推会更新登记快照，旧回执被比掉。
        String snapOld = a;
        String registeredAfterNewRequest = b;   // 第二次 0x19 异文触发新请求，登记被覆写
        check(!CompanionReqGuard.reqKey(5, 2, snapOld)
                        .equals(CompanionReqGuard.reqKey(5, 2, registeredAfterNewRequest))
                        && !CompanionReqGuard.receiptCurrent(snapOld, registeredAfterNewRequest),
                "A3+A4：同页异文新请求登记后，旧回执既不被去重吞、也被内容比对丢弃");

        // ---- 取证工具：正文前 20 字（日志用途） ----
        check(CompanionReqGuard.head("一二三四五六七八九十一二三四五六七八九十甲乙丙", 20).length() == 20,
                "head 截取前 20 字");
        check("短".equals(CompanionReqGuard.head("短", 20)),
                "head 短于 n 原样返回");
        check("".equals(CompanionReqGuard.head(null, 20))
                        && "".equals(CompanionReqGuard.head("abc", 0)),
                "head null / n<=0 → 空串，不抛异常");

        System.out.println("CompanionReqGuardTest 结果 / result: " + pass + " checks, " + fail + " failed");
        if (fail != 0) System.exit(1);
    }
}
