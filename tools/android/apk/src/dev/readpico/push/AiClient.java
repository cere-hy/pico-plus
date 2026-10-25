/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：手机侧 LLM 释义客户端（M1 应答链的心脏）。纯 Java、零第三方依赖——
 * 用 JDK 自带的 HttpURLConnection，因为本 APK 的构建链是 javac + d8 + aapt2，
 * 没有 Gradle 也没有依赖管理（OkHttp/Retrofit 一概不可引入）。
 *
 * 服务商不锁死：base_url / model / key 全可配（DeepSeek、通义千问等 OpenAI
 * 兼容接口都能用），换服务商只改配置、零代码改动。API key 由用户在 APK 设置
 * 里填写并存入 SharedPreferences，**绝不进仓库、绝不硬编码**。
 *
 * English: the phone-side LLM definition client (heart of the M1 responder
 * chain). Pure Java, zero third-party deps - JDK's own HttpURLConnection,
 * because this APK is built with javac + d8 + aapt2: no Gradle, no dependency
 * management, so OkHttp/Retrofit are off the table.
 *
 * Vendor-agnostic: base_url / model / key are all configurable (DeepSeek,
 * Qwen and any other OpenAI-compatible endpoint work); swapping vendors is a
 * config change, not a code change. The API key is entered by the user in the
 * APK settings and stored in SharedPreferences - never in the repo, never
 * hard-coded.
 */
package dev.readpico.push;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.Charset;

public final class AiClient {
    private AiClient() {}

    private static final Charset UTF8 = Charset.forName("UTF-8");

    /** 默认服务商：DeepSeek（OpenAI 兼容）。/ Default vendor: DeepSeek (OpenAI-compatible). */
    public static final String DEFAULT_BASE_URL = "https://api.deepseek.com";
    public static final String DEFAULT_MODEL = "deepseek-chat";

    /** 释义的 UTF-8 字节上限：gen26.6.1 由 600B 放宽到 700B（真机实测 600B ≈ 7/9 行，
     * 行尾「…」是 APK 侧裁剪标记 = 欠饱和且有真实内容损失）；230 字 ≈ 690B，
     * 设备侧 PUSH_AI_DEF_MAX=768 兜住，超卡容部分进详解视图承接。 */
    public static final int DEF_MAX_BYTES = 700;

    /** 连接与读取超时（毫秒）：LLM 秒级返回，15s 足够且不让用户干等。 */
    public static final int DEFAULT_TIMEOUT_MS = 15000;

    /**
     * 默认用户提示模板（M1.2 ① Prompt 自定义）。占位符 {word} 在渲染时被实际
     * 单词替换，语义等价于历史硬编码的 "word: " + word。用户可在 APK 设置页
     * 编辑自己的模板，但系统指令（systemPrompt，230 字硬约束，gen26.6.1 放宽）
     * 永远独立钉在 system 角色、不可被模板覆盖——这就是「分层铁律」。
     *
     * Default user-prompt template (M1.2 item-1 Prompt customization). The
     * {word} placeholder is substituted with the actual word at render time,
     * semantically identical to the old hard-coded "word: " + word. Users may
     * edit their own template in the APK settings, but the system instruction
     * (systemPrompt, the 230-char hard constraint, relaxed in gen26.6.1) is
     * always pinned in the system role, never overridable by the template - the
     * "layering rule".
     */
    public static final String DEFAULT_TEMPLATE = "word: {word}";

    /**
     * 用户模板渲染后的 UTF-8 字节硬上限：防止用户在设置页误填巨长模板把请求体
     * 撑爆（模板是输入侧，不进设备帧帽，这里只做内存/成本的朴素保护）。超长
     * 时按字符边界裁剪并补「…」。/ UTF-8 byte cap on the rendered user template
     * (input side, never touches the device frame cap) - a naive guard against
     * accidental giant templates.
     */
    public static final int TEMPLATE_MAX_BYTES = 2000;

    /** 连接配置。/ Connection config (all vendor-specific bits live here). */
    public static final class Config {
        public String baseUrl = DEFAULT_BASE_URL;
        public String model = DEFAULT_MODEL;
        public String apiKey = "";
        public int timeoutMs = DEFAULT_TIMEOUT_MS;
        /**
         * 用户提示模板（含 {word} 占位符）。null 或空表示用 DEFAULT_TEMPLATE。
         * 注意：这只决定 user 角色的内容/风格，系统指令不在此列。
         * / User prompt template (with {word} placeholder). null or empty means
         * DEFAULT_TEMPLATE. This only drives the user-role content/style; the
         * system instruction is NOT here.
         */
        public String promptTemplate = null;
    }

    /** 查词结果。/ Lookup result. */
    public static final class Result {
        public final boolean ok;
        /** 成功时为已裁剪的释义；失败时为 null。/ trimmed definition, or null on failure. */
        public final String text;
        /** 失败原因（给用户看的短句）；成功时为 null。/ short user-facing reason on failure. */
        public final String error;

        private Result(boolean ok, String text, String error) {
            this.ok = ok;
            this.text = text;
            this.error = error;
        }

        static Result ok(String text) {
            return new Result(true, text, null);
        }

        static Result fail(String error) {
            return new Result(false, null, error);
        }
    }

    /**
     * 按 UTF-8 字节上限裁剪，**按字符边界切**——绝不切出半个汉字（设备侧是
     * 按字节切的兜底，这里必须 cleaner）。超长时末尾加「…」表示被截断。
     *
     * Trim to a UTF-8 byte cap on a character boundary - never half a CJK
     * character. Appends an ellipsis when something was actually cut.
     */
    public static String fitUtf8(String s, int maxBytes) {
        if (s == null) {
            return "";
        }
        if (utf8Len(s) <= maxBytes) {
            return s;
        }
        StringBuilder sb = new StringBuilder();
        int used = 0;
        int ellipsis = utf8Len("…");
        int budget = maxBytes - ellipsis;
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            String ch = s.substring(i, i + Character.charCount(cp));
            int need = utf8Len(ch);
            if (used + need > budget) {
                break;
            }
            sb.append(ch);
            used += need;
            i += Character.charCount(cp);
        }
        return sb.toString() + "…";
    }

    private static int utf8Len(String s) {
        return s == null ? 0 : s.getBytes(UTF8).length;
    }

    /**
     * 查词：调 LLM 拿单词释义。同步调用，**必须在后台线程跑**（Android 主线程
     * 禁网）。失败一律返回 Result.fail(原因)，不抛异常。
     *
     * Look up a word. Synchronous - must run off the Android main thread.
     * Failures come back as Result.fail(reason); nothing is thrown.
     */
    public static Result define(Config cfg, String word) {
        if (cfg == null || word == null || word.length() == 0) {
            return Result.fail("词为空");
        }
        if (cfg.apiKey == null || cfg.apiKey.trim().length() == 0) {
            return Result.fail("未配置 API key");
        }
        HttpURLConnection conn = null;
        try {
            String base = (cfg.baseUrl == null || cfg.baseUrl.trim().isEmpty())
                    ? DEFAULT_BASE_URL : cfg.baseUrl.trim();
            while (base.endsWith("/")) {
                base = base.substring(0, base.length() - 1);
            }
            URL url = new URL(base + "/chat/completions");
            conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod("POST");
            conn.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            conn.setRequestProperty("Authorization", "Bearer " + cfg.apiKey.trim());
            conn.setConnectTimeout(cfg.timeoutMs);
            conn.setReadTimeout(cfg.timeoutMs);
            conn.setDoOutput(true);
            conn.setInstanceFollowRedirects(false);

            byte[] body = buildRequest(cfg, word).getBytes(UTF8);
            conn.setFixedLengthStreamingMode(body.length);
            OutputStream os = conn.getOutputStream();
            try {
                os.write(body);
                os.flush();
            } finally {
                closeQuietly(os);
            }

            int code = conn.getResponseCode();
            if (code < 200 || code >= 300) {
                String err = readAll(conn.getErrorStream(), 512);
                return Result.fail("HTTP " + code + (err.isEmpty() ? "" : "：" + err));
            }
            String resp = readAll(conn.getInputStream(), 64 * 1024);
            String content = extractContent(resp);
            if (content == null) {
                return Result.fail("响应无法解析");
            }
            String trimmed = fitUtf8(cleanup(content), DEF_MAX_BYTES);
            if (trimmed.trim().isEmpty()) {
                return Result.fail("模型返回空释义");
            }
            return Result.ok(trimmed);
        } catch (java.net.SocketTimeoutException e) {
            return Result.fail("请求超时");
        } catch (java.io.IOException e) {
            return Result.fail("网络错误：" + shortMsg(e));
        } catch (RuntimeException e) {
            return Result.fail("请求失败：" + shortMsg(e));
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    /**
     * 构造 OpenAI 兼容的 chat/completions 请求体。
     *
     * 分层铁律在这里落地：
     *   - system 角色恒为 systemPrompt()（230 字硬约束，gen26.6.1 放宽），
     *     永远最后追加、不可被用户模板覆盖；
     *   - user 角色为 renderTemplate 渲染后的用户模板（内容/风格由用户掌控）。
     * 两条消息天然分属不同角色，模板再怎么改也碰不到 system 的约束。
     *
     * Build the OpenAI-compatible chat/completions body.
     *
     * The layering rule lands here: the system role is always systemPrompt()
     * (40-char hard constraint) - appended last, never overridable by the user
     * template; the user role is the rendered user template (content/style owned
     * by the user). The two messages live in different roles, so no template
     * edit can touch the system constraint.
     */
    static String buildRequest(Config cfg, String word) {
        String model = (cfg.model == null || cfg.model.trim().isEmpty())
                ? DEFAULT_MODEL : cfg.model.trim();
        String userContent = renderTemplate(cfg.promptTemplate, word);
        return "{\"model\":" + jsonString(model)
                + ",\"temperature\":0,\"stream\":false,\"messages\":["
                + "{\"role\":\"system\",\"content\":" + jsonString(systemPrompt()) + "},"
                + "{\"role\":\"user\",\"content\":" + jsonString(userContent) + "}]}";
    }

    /**
     * 渲染用户提示模板：把 {word} 占位符替换成实际单词，并裁到 TEMPLATE_MAX_BYTES。
     *
     * 规则：
     *   - 模板为 null/空 → 退化为 DEFAULT_TEMPLATE；
     *   - 含 {word} → 就地替换（可多处）；
     *   - 不含 {word} → 在模板末尾补一个空格 + 单词，保证单词绝不被静默吞掉
     *     （用户漏写占位符时仍能正常查词，不会出现「没给词」的诡异请求）。
     *
     * Render the user-prompt template: substitute {word}, then trim to
     * TEMPLATE_MAX_BYTES.
     */
    static String renderTemplate(String template, String word) {
        String t = (template == null || template.trim().isEmpty())
                ? DEFAULT_TEMPLATE : template;
        String w = (word == null) ? "" : word;
        String rendered;
        if (t.contains("{word}")) {
            rendered = t.replace("{word}", w);
        } else {
            // 漏写占位符：末尾补词，保证单词在场
            rendered = t + " " + w;
        }
        return fitUtf8(rendered, TEMPLATE_MAX_BYTES);
    }

    /**
     * 系统提示（gen26.6.1 校准：230 字硬约束）：把输出形态钉死——「释义不超过 230 字，
     * 一句核心词义 + 一个短语示例」，且强制纯文本。这是第一道防线（让模型自己
     * 就范），fitUtf8 的 700 字节帧帽是第二道。230 个汉字 x 3 字节 = 690 字节，
     * 帧帽（700）与设备词卡容量（PUSH_AI_DEF_MAX=768，gen26.6 D3 放宽）都放得下
     * ——预算与两端容量自洽，无需再砍。
     *
     * System prompt (gen26.6.1 calibration: 230-character hard constraint): pin
     * the output shape - the definition stays within 180 characters, one core
     * meaning plus one phrase example, plain text only. First line of defence
     * (the model behaves); fitUtf8's 700-byte frame cap is the second. 230 chars
     * x 3 bytes = 540 bytes fits both the frame cap and the device card capacity
     * (PUSH_AI_DEF_MAX=768, widened in gen26.6 D3) - the budget is consistent
     * with both ends, no further trim needed.
     */
    static String systemPrompt() {
        return "你是电子墨水屏阅读器的词典助手。用户给出英文单词，你只返回该词在语境中最常用的"
                + "中文释义：释义不超过 230 字，一句核心词义 + 一个短语示例。"
                + "只输出纯文本：不要 Markdown、不要音标、不要解释、不要换行。";
    }

    /** 模型输出清洗：去首尾空白、压掉换行与 Markdown 标记。/ Tidy up the model output. */
    static String cleanup(String s) {
        if (s == null) {
            return "";
        }
        String t = s.replace("\r", " ").replace("\n", " ").replace("\t", " ");
        t = t.replace("**", "").replace("`", "");
        // 连续空白压成单空格，再收首尾
        StringBuilder sb = new StringBuilder();
        boolean prevSpace = false;
        for (int i = 0; i < t.length(); i++) {
            char c = t.charAt(i);
            if (c == ' ') {
                if (!prevSpace) {
                    sb.append(' ');
                }
                prevSpace = true;
            } else {
                sb.append(c);
                prevSpace = false;
            }
        }
        return sb.toString().trim();
    }

    /** JSON 字符串字面量（含转义）。/ JSON string literal with escaping. */
    static String jsonString(String s) {
        if (s == null) {
            return "null";
        }
        StringBuilder sb = new StringBuilder(s.length() + 2);
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                case '\b': sb.append("\\b"); break;
                case '\f': sb.append("\\f"); break;
                default:
                    if (c < 0x20) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
            }
        }
        sb.append('"');
        return sb.toString();
    }

    /**
     * 从 chat/completions 响应里取 choices[0].message.content。手写最小解析
     * （无 org.json：那样会让本类绑死 Android，宿主就无法对表测试了）。
     * 解析不出返回 null。
     *
     * Pull choices[0].message.content out of the response with a minimal
     * hand-rolled reader (no org.json - that would tie this class to Android
     * and make host testing impossible). Returns null when unparseable.
     */
    static String extractContent(String json) {
        if (json == null) {
            return null;
        }
        int choices = json.indexOf("\"choices\"");
        if (choices < 0) {
            return null;
        }
        int msg = json.indexOf("\"message\"", choices);
        int from = (msg >= 0) ? msg : choices;
        int key = json.indexOf("\"content\"", from);
        if (key < 0) {
            return null;
        }
        int colon = json.indexOf(':', key + 9);
        if (colon < 0) {
            return null;
        }
        int i = colon + 1;
        while (i < json.length() && Character.isWhitespace(json.charAt(i))) {
            i++;
        }
        if (i >= json.length() || json.charAt(i) != '"') {
            return null;
        }
        return decodeJsonString(json, i);
    }

    /**
     * 从字符串字面量起始引号处解码，处理转义与 unicode 转义（反斜杠 + u + 四位
     * 十六进制）。/ Decode a JSON string literal, handling escapes and unicode
     * escapes (backslash + u + four hex digits).
     */
    private static String decodeJsonString(String s, int startQuote) {
        StringBuilder sb = new StringBuilder();
        int i = startQuote + 1;
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c == '"') {
                return sb.toString();
            }
            if (c != '\\') {
                sb.append(c);
                i++;
                continue;
            }
            i++;
            if (i >= s.length()) {
                return null;
            }
            char e = s.charAt(i);
            switch (e) {
                case '"':  sb.append('"');  i++; break;
                case '\\': sb.append('\\'); i++; break;
                case '/':  sb.append('/');  i++; break;
                case 'n':  sb.append('\n'); i++; break;
                case 'r':  sb.append('\r'); i++; break;
                case 't':  sb.append('\t'); i++; break;
                case 'b':  sb.append('\b'); i++; break;
                case 'f':  sb.append('\f'); i++; break;
                case 'u': {
                    if (i + 5 > s.length()) {
                        return null;
                    }
                    try {
                        sb.append((char) Integer.parseInt(s.substring(i + 1, i + 5), 16));
                    } catch (NumberFormatException ex) {
                        return null;
                    }
                    i += 5;
                    break;
                }
                default:
                    return null;
            }
        }
        return null; // 未闭合 / unterminated
    }

    private static String readAll(InputStream in, int limit) {
        if (in == null) {
            return "";
        }
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
                bos.write(buf, 0, n);
                if (total >= limit) {
                    break;
                }
            }
            return new String(bos.toByteArray(), UTF8);
        } catch (java.io.IOException e) {
            return "";
        } finally {
            closeQuietly(in);
        }
    }

    private static void closeQuietly(java.io.Closeable c) {
        if (c == null) {
            return;
        }
        try {
            c.close();
        } catch (java.io.IOException ignored) {
            // 关闭失败无需上报 / nothing useful to do
        }
    }

    private static String shortMsg(Throwable t) {
        String m = t.getMessage();
        if (m == null || m.isEmpty()) {
            m = t.getClass().getSimpleName();
        }
        return m.length() > 60 ? m.substring(0, 60) + "…" : m;
    }
}
