/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：步 5 更新检测（22:11 裁决 §四 最小集）的宿主机守卫断言。纯逻辑、零
 * android / 零 org.json 依赖，测的是 MainActivity 真实调用的 UpdateChecker。
 * 重点钉死失败口径：网络不可用 / 版本源不可达 / 数据无法解析一律「检查失败」，
 * 绝不误报「已是最新」——防裁决点名的「伪闭环按钮」。
 *
 * English: host-side asserts for the Step5 update check (verdict section 4).
 * Pure logic, zero android deps, exercises the real UpdateChecker. Locks down
 * the failure discipline: any fetch/parse failure is CHECK_FAILED, never "up to date".
 */
package dev.readpico.push;

public final class UpdateCheckerTest {
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
        System.out.println("UpdateCheckerTest（步 5 更新检测，纯逻辑，零 android 依赖）:");

        // ---- 当前版本常量（与 AndroidManifest versionCode=18 / 1.6.6 对齐）----
        check(UpdateChecker.CURRENT_VERSION_CODE == 18,
                "当前 versionCode=18 在案");
        check("1.6.6".equals(UpdateChecker.CURRENT_VERSION_NAME),
                "当前 versionName=1.6.6 在案");

        // ---- 版本源：自家 Pages 域独立 json，正式端点改一行常量 ----
        // 22:11 原断言钉「提案值须落在 pico-plus 单前缀」。17:54 裁决 §四：
        // 验证件期间 FEED_URL 换一次性隔离端点（独立 repo、同属 cere-hy.github.io
        // 域、不触碰 pico-plus）；交付候选件轮改回提案值。守卫不降：仍限自家
        // Pages 域 + apk-update.json，且须为「提案值前缀」或「含 throwaway 语义
        // 的验证件隔离端点」二者之一（§二命名要求固化为断言），外部源一律拒绝。
        check(UpdateChecker.FEED_URL.startsWith("https://cere-hy.github.io/")
                && (UpdateChecker.FEED_URL.startsWith("https://cere-hy.github.io/pico-plus/")
                    || UpdateChecker.FEED_URL.contains("throwaway")),
                "版本源在自家 Pages 域内，且为提案值或 17:54 验证件隔离端点");
        check(UpdateChecker.FEED_URL.endsWith("apk-update.json"),
                "独立文件名 apk-update.json，与固件 OTA update.json 区分");

        // ---- ① 三态判定：有新版本 / 已是最新 ----
        String feed18 = "{\"versionCode\":18,\"versionName\":\"1.6.6\","
                + "\"url\":\"https://example.com/releases\",\"notes\":\"修复\"}";
        check(UpdateChecker.decide(feed18, true, 17) == UpdateChecker.UPDATE_AVAILABLE,
                "latest 18 > 当前 17 → 发现新版本");
        check(UpdateChecker.decide("{\"versionCode\":17}", true, 17)
                        == UpdateChecker.UP_TO_DATE,
                "latest 17 = 当前 17 → 已是最新");
        check(UpdateChecker.decide("{\"versionCode\":16}", true, 17)
                        == UpdateChecker.UP_TO_DATE,
                "latest 16 < 当前 17 → 已是最新（不倒退判更新）");
        check(UpdateChecker.decide("{\"versionCode\": 19 }", true, 17)
                        == UpdateChecker.UPDATE_AVAILABLE,
                "字段带空格容错，latest 19 → 发现新版本");

        // ---- ② 失败口径（裁决 §四 核心）：绝不误报「已是最新」----
        check(UpdateChecker.decide(null, false, 17) == UpdateChecker.CHECK_FAILED,
                "网络层失败 fetchOk=false → 检查失败");
        check(UpdateChecker.decide(feed18, false, 17) == UpdateChecker.CHECK_FAILED,
                "即便 body 有数据，fetchOk=false 仍 → 检查失败");
        check(UpdateChecker.decide(null, true, 17) == UpdateChecker.CHECK_FAILED,
                "空响应 body=null → 检查失败");
        check(UpdateChecker.decide("", true, 17) == UpdateChecker.CHECK_FAILED,
                "空串 body → 检查失败");
        check(UpdateChecker.decide("{}", true, 17) == UpdateChecker.CHECK_FAILED,
                "缺 versionCode 字段 → 检查失败");
        check(UpdateChecker.decide("not a json", true, 17) == UpdateChecker.CHECK_FAILED,
                "非 JSON 文本 → 检查失败");
        check(UpdateChecker.decide("{\"versionCode\":\"18\"}", true, 17)
                        == UpdateChecker.CHECK_FAILED,
                "versionCode 是字符串而非整数 → 检查失败");
        check(UpdateChecker.decide("{\"versionCode\":-1}", true, 17)
                        == UpdateChecker.CHECK_FAILED,
                "versionCode 负数（非法值）→ 检查失败");
        // 反证：所有失败分支都必须 != UP_TO_DATE（防「伪闭环按钮」）
        check(UpdateChecker.decide(null, false, 17) != UpdateChecker.UP_TO_DATE
                        && UpdateChecker.decide("", true, 17) != UpdateChecker.UP_TO_DATE
                        && UpdateChecker.decide("{}", true, 17) != UpdateChecker.UP_TO_DATE,
                "三类失败零一例误判为「已是最新」");

        // ---- ③ 字段抽取（零 org.json，手写解析）----
        check(UpdateChecker.extractIntField(feed18, "versionCode") == 18,
                "整数字段抽取 versionCode=18");
        check(UpdateChecker.extractIntField("{\"versionCode\": 21 }", "versionCode") == 21,
                "整数字段带空格抽取 =21");
        check(UpdateChecker.extractIntField("{}", "versionCode") == -1,
                "缺整数字段 → -1");
        check("1.6.6".equals(UpdateChecker.extractStringField(feed18, "versionName")),
                "字符串字段抽取 versionName=1.6.6");
        check("https://example.com/releases".equals(
                        UpdateChecker.extractStringField(feed18, "url")),
                "字符串字段抽取引导 url");
        check(UpdateChecker.extractStringField(feed18, "missing") == null,
                "缺字符串字段 → null");
        check(UpdateChecker.extractStringField(null, "url") == null,
                "null body 字段抽取 → null");

        // ---- ④ 最小集边界：仅引导不装、不做应用内下载（本类不含下载/安装判定）----
        check(UpdateChecker.CHECK_FAILED != UpdateChecker.UP_TO_DATE
                        && UpdateChecker.UP_TO_DATE != UpdateChecker.UPDATE_AVAILABLE
                        && UpdateChecker.CHECK_FAILED != UpdateChecker.UPDATE_AVAILABLE,
                "三态常量互异，UI 可据态分别提示（失败/最新/可更新）");

        System.out.println("UpdateCheckerTest: " + pass + " pass / " + fail + " fail");
        if (fail > 0) {
            System.exit(1);
        }
    }
}
