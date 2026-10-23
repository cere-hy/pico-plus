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

    /** 释义的 UTF-8 字节上限：设备侧 PUSH_AI_DEF_MAX=170 留 1 字节 NUL。 */
    public static final int DEF_MAX_BYTES = 169;

    /** 连接与读取超时（毫秒）：LLM 秒级返回，15s 足够且不让用户干等。 */
    public static final int DEFAULT_TIMEOUT_MS = 15000;

    /** 连接配置。/ Connection config (all vendor-specific bits live here). */
    public static final class Config {
        public String baseUrl = DEFAULT_BASE_URL;
        public String model = DEFAULT_MODEL;
        public String apiKey = "";
        public int timeoutMs = DEFAULT_TIMEOUT_MS;
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

    /** 构造 OpenAI 兼容的 chat/completions 请求体。/ Build the chat/completions body. */
    static String buildRequest(Config cfg, String word) {
        String model = (cfg.model == null || cfg.model.trim().isEmpty())
                ? DEFAULT_MODEL : cfg.model.trim();
        return "{\"model\":" + jsonString(model)
                + ",\"temperature\":0,\"stream\":false,\"messages\":["
                + "{\"role\":\"system\",\"content\":" + jsonString(systemPrompt()) + "},"
                + "{\"role\":\"user\",\"content\":" + jsonString("word: " + word) + "}]}";
    }

    /**
     * 系统提示：约束输出形态，让释义短到能塞进 169 字节的帧（设备侧单帧上限）。
     * 这层「让模型自己说短」是第一道防线，fitUtf8 是第二道。
     *
     * Keep the model terse so the answer fits one 169-byte frame. This is the
     * first line of defense; fitUtf8 is the second.
     */
    static String systemPrompt() {
        return "你是电子墨水屏阅读器的词典助手。用户给出英文单词，你只返回该词在语境中最常用的"
                + "中文释义，格式为「词性. 中文释义」。不要例句、不要音标、不要解释、不要换行，"
                + "总长不超过 50 个汉字。";
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
