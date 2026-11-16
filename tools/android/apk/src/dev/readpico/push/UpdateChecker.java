/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：步 5 更新检测——版本比对的纯逻辑层（22:11 裁决 §四 最小集）。
 * 零 android、零 org.json 依赖：绑 org.json 会让本类绑死 Android、宿主无法对表
 * 测试（与 AiClient 同一纪律）。只做「取 feed → 比 versionCode → 三态判定」，
 * 网络请求、UI 弹窗、跳转引导都在 MainActivity，本类不碰。
 *
 * 失败口径（裁决 §四 明令，防「伪闭环按钮」）：网络不可用 / 版本源不可达 /
 * 数据无法解析，一律返回 CHECK_FAILED，绝不得误报「已是最新」。
 *
 * English: Step5 update check - pure version-comparison layer (verdict section 4
 * minimal set). Zero android / zero org.json deps so host javac can test it.
 * Any fetch/parse failure yields CHECK_FAILED, never a false "up to date".
 */
package dev.readpico.push;

public final class UpdateChecker {
    private UpdateChecker() {}

    /** 检查失败：不给出任何版本结论（绝不降级成「已是最新」）。 */
    public static final int CHECK_FAILED = -1;
    /** 已是最新（仅在合法解析 latestVersionCode 后才允许得出）。 */
    public static final int UP_TO_DATE = 0;
    /** 发现新版本。 */
    public static final int UPDATE_AVAILABLE = 1;

    /** 当前 APK 版本，与 AndroidManifest.xml 对齐（versionCode=18 / versionName=1.6.6）。 */
    public static final int CURRENT_VERSION_CODE = 18;
    public static final String CURRENT_VERSION_NAME = "1.6.6";

    /**
     * 版本源（代码端提案值，22:11 裁决 §四「先提案再落码」）：
     * 依据 R0 §七 四口径「Pages 独立 json / versionCode 判据」+ 台账 R4 自家 Pages
     * 单前缀 https://cere-hy.github.io/pico-plus/ 。用独立文件名 apk-update.json，
     * 与固件 OTA 的 update.json 区分，避免同源混淆。
     *
     * 这不是既成发布端点：正式版本源由公主/审计指定后，只改这一行常量即可，
     * 本类逻辑、MainActivity UI、构建链、测试用例全部零改。
     */
    // 交付候选件（15:40 §六 / 18:40 §四）：FEED_URL 改回提案值（正式端点），
    // 与 bump 18 同轮落码；一次性隔离端点仅验证件期间使用，全绿回传后已删除。
    public static final String FEED_URL =
            "https://cere-hy.github.io/pico-plus/apk-update.json";

    /*
     * 提案 feed 字段（对齐 OTA 判据 §四 update.json 模板风格，APK 侧精简）：
     * {
     *   "versionCode": 18,                       // 判据键：int，大于当前 versionCode 即判新版本
     *   "versionName": "1.6.6",                  // 提示用展示串
     *   "url": "https://github.com/cere-hy/pico-plus/releases",  // 跳转引导页
     *   "notes": "……"                            // 可选说明
     * }
     * 仅引导不装：本应用不下载、不静默安装，只打开 url 由用户自行决定。
     */

    /**
     * 三态判定。fetchOk=false 表示网络层失败（非 2xx / 超时 / 异常 / 空响应），
     * 此时一律 CHECK_FAILED；只有合法解析出 latestVersionCode 才允许 UP_TO_DATE /
     * UPDATE_AVAILABLE，从根上杜绝「没拿到数据 → 误报已是最新」。
     */
    public static int decide(String fetchedJson, boolean fetchOk, int currentCode) {
        if (!fetchOk || fetchedJson == null) {
            return CHECK_FAILED;
        }
        int latest = extractIntField(fetchedJson, "versionCode");
        if (latest < 0) {
            return CHECK_FAILED; // 解析失败同样不误报「已是最新」
        }
        return latest > currentCode ? UPDATE_AVAILABLE : UP_TO_DATE;
    }

    /** 手写极简整数字段抽取（零 org.json）。找不到 / 是字符串 / 非整数 / 负数 → -1。 */
    public static int extractIntField(String json, String key) {
        String s = extractRawValue(json, key);
        if (s == null) {
            return -1;
        }
        s = s.trim();
        if (s.isEmpty() || s.startsWith("\"")) {
            return -1;
        }
        try {
            int v = Integer.parseInt(s);
            return v < 0 ? -1 : v;
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /** 手写极简字符串字段抽取（零 org.json）。字段值不含引号/转义，取到 null。 */
    public static String extractStringField(String json, String key) {
        String s = extractRawValue(json, key);
        if (s == null) {
            return null;
        }
        s = s.trim();
        if (s.length() >= 2 && s.startsWith("\"") && s.endsWith("\"")) {
            return s.substring(1, s.length() - 1);
        }
        return null;
    }

    /** 定位 "key" : <value>；字符串值取到后引号，数值取到逗号或右花括号。 */
    private static String extractRawValue(String json, String key) {
        if (json == null) {
            return null;
        }
        String needle = "\"" + key + "\"";
        int k = json.indexOf(needle);
        if (k < 0) {
            return null;
        }
        int colon = json.indexOf(':', k + needle.length());
        if (colon < 0) {
            return null;
        }
        int i = colon + 1;
        while (i < json.length() && Character.isWhitespace(json.charAt(i))) {
            i++;
        }
        if (i >= json.length()) {
            return null;
        }
        if (json.charAt(i) == '"') {
            int end = json.indexOf('"', i + 1);
            return end < 0 ? null : json.substring(i, end + 1);
        }
        int end = i;
        while (end < json.length() && json.charAt(end) != ',' && json.charAt(end) != '}') {
            end++;
        }
        return json.substring(i, end);
    }
}
