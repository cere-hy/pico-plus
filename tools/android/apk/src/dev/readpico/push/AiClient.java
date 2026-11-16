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

    /** 释义的 UTF-8 字节上限：gen26.6.4 由 700B 放宽到 1152B。真机实测 700B 下
     * 「8 句例句 + 搭配」被切（行尾「…」= APK 侧裁剪）；8 句需求约 900–1100B。
     * 设备侧 PUSH_AI_DEF_MAX=1200 / ai_reader=1200 兜住，1152 留 48B 给 NUL。
     * 360 字 ≈ 1080B ≤ 1152B，与 systemPrompt 的字数硬约束同源。 */
    public static final int DEF_MAX_BYTES = 1152;

    /** 系统提示的字数硬约束（≈ 汉字数）。由 DEF_MAX_BYTES/3 取整并留余量得到，
     * 设置页文案也从它派生（「系统 %d 字约束」），避免三处各写一个数字再漂移。
     * gen26.6.4: 360 字（≈1080B）≤ 1152B 硬帽。 */
    public static final int SYSTEM_CHAR_LIMIT = 360;

    /**
     * M3-B 情景四节分析的字数上限：340 字 ≈ 1020B ≤ 1152B 帧帽，与释义共用
     * 同一条预算链（设备槽 1200、ai_reader 1200），**不改协议**。
     * M3-B: the four-section scene budget - 340 chars ~= 1020B, inside the same
     * 1152B frame cap as definitions and the device's 1200B slot. No protocol change.
     */
    public static final int SCENE_CHAR_LIMIT = 340;

    /**
     * 26.11.3-C2：整段解析（bit2）的输出字节上限——**独立于 DEF_MAX_BYTES**。
     *
     * 段落和词义不是一个尺度：一条词义 1 KB 就够，一篇逐句对照的段落解析 5-6 KB
     * 也装不满。用一个数管两件事，要么词义被砍，要么段落装不下。8192 = 样本上界
     * 6.5 KB + 25% 余量（≈2,700 汉字），与设备槽 PUSH_AI_DEF_MAX 同数——跨端四点
     * 一致由 check_excerpt.py 的 B72 咬牙。
     * The passage answer's byte cap, independent of DEF_MAX_BYTES. A definition needs
     * 1 KB; a sentence-by-sentence passage analysis fills 5-6 KB. 8192 = the 6.5 KB
     * sample ceiling plus 25% headroom (~2,700 CJK chars), equal to the device slot
     * PUSH_AI_DEF_MAX; the four-point cross-end agreement is bitten by B72.
     */
    public static final int EXCERPT_DEF_MAX_BYTES = 8192;

    /** 整段解析请求的 max_tokens。显式传，不吃服务端默认——默认值是别人的常量，
     * 换服务商就换了一个上限，而我们的链路是按 8192 B 建的。 */
    public static final int EXCERPT_MAX_TOKENS = 8192;

    /** max_tokens 被端点拒绝（400）时的回退档：有些 OpenAI 兼容端点默认模型上限
     * 就是 4096，硬要 8192 会被整条拒掉。回退一次，并在日志里说清是哪一档。 */
    public static final int EXCERPT_MAX_TOKENS_FALLBACK = 4096;

    /** 连接与读取超时（毫秒）：LLM 秒级返回，15s 足够且不让用户干等。 */
    public static final int DEFAULT_TIMEOUT_MS = 15000;

    /**
     * 26.11.3-C2（审计第一刀：HTTP 超时）：整段解析的读取超时必须单独给。
     * 15 s 是给「8-10 s 生成一条词义」配的；2,000 字的段落要 40-70 s，仍用 15 s
     * 就是**随机掐死长输出**——模型还在写，客户端已经超时，用户看到「请求超时」。
     * 120 s = 生成上界 70 s + 回传余量 + 25%，且高于设备侧 90 s 等待窗（设备先放手）。
     * The passage read timeout is its own figure. 15 s was sized for a 1 KB definition
     * that takes 8-10 s; a 2,000-character analysis takes 40-70 s, and 15 s kills it at
     * random. 120 s = 70 s generation ceiling + slack, and above the device's 90 s
     * window so the device still gives up first.
     */
    public static final int EXCERPT_TIMEOUT_MS = 120000;

    /**
     * 默认用户提示模板（M1.2 ① Prompt 自定义）。占位符 {word} 在渲染时被实际
     * 单词替换，语义等价于历史硬编码的 "word: " + word。用户可在 APK 设置页
     * 编辑自己的模板，但系统指令（systemPrompt，360 字硬约束，gen26.6.4 校准）
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
        /**
         * 26.11.3-C2：本次输出是否**撞上字节帽被砍**（末尾补了「…」）。
         * 它不再是内部细节——复测卡的判据要读它：「满输入档」要求 truncated=0，
         * 一旦为 1 必须在日志里报出来，不许静默。
         * Whether this answer hit the byte cap and was cut (an ellipsis was appended).
         * No longer an internal detail: the acceptance card reads it, and truncated=1
         * must be reported in the log - never silent.
         */
        public final boolean truncated;

        private Result(boolean ok, String text, String error, boolean truncated) {
            this.ok = ok;
            this.text = text;
            this.error = error;
            this.truncated = truncated;
        }

        static Result ok(String text) {
            return new Result(true, text, null, false);
        }

        static Result ok(String text, boolean truncated) {
            return new Result(true, text, null, truncated);
        }

        static Result fail(String error) {
            return new Result(false, null, error, false);
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
        Result raw = fetch(cfg, buildRequest(cfg, word));
        if (!raw.ok) {
            return raw;
        }
        String trimmed = fitUtf8(cleanup(raw.text), DEF_MAX_BYTES);
        if (trimmed.trim().isEmpty()) {
            return Result.fail("模型返回空释义");
        }
        return Result.ok(trimmed);
    }

    /**
     * M3-B 情景解析：拿「词 + 上下文」换四节分析（词义 / 句意 / 语境 / 用法）。
     * 与 define() 走同一 HTTP 通道，但**提示词与清洗规则都另一套**：
     *   - system 是 SCENE_SYSTEM（钉死四节格式与「只引用选区内文字」），
     *     不是可被用户模板影响的释义约束；
     *   - 清洗保留换行（四节靠换行分节，压成一行设备就分不出节）；
     *   - 帧帽仍是 1152B，与设备槽 1200B 一致。
     * Scene analysis for M3-B: "word + context" in, four sections out. Same HTTP
     * path as define(), but a different prompt and a different cleanup - the
     * system role is SCENE_SYSTEM (pins the four-section shape and "quote only what
     * the selection contains"), and newlines survive because they ARE the section
     * separators. The 1152B frame cap still matches the device slot.
     */
    public static Result scene(Config cfg, String word, String context) {
        if (cfg == null || word == null || word.length() == 0) {
            return Result.fail("词为空");
        }
        if (context == null || context.trim().length() == 0) {
            return Result.fail("上下文为空");
        }
        if (cfg.apiKey == null || cfg.apiKey.trim().length() == 0) {
            return Result.fail("未配置 API key");
        }
        Result raw = fetch(cfg, buildSceneRequest(cfg, word, context));
        if (!raw.ok) {
            return raw;
        }
        String text = fitUtf8(cleanupScene(raw.text), DEF_MAX_BYTES);
        if (text.trim().isEmpty()) {
            return Result.fail("模型返回空情景");
        }
        return Result.ok(text);
    }

    /**
     * 26.11.3-C：整段选文解析（bit2）。与 scene() 走同一条 HTTP 与同一套清洗，
     * 差别只在「问什么」：scene 问「这个词在此句中的含义」，excerpt 问「这段在
     * 说什么」。分成两个方法而不是给 scene 加参数，是因为提示词与校验条件都不
     * 同（段落的 ctx 是主对象、word 只是锚点，允许空词），合在一起就会长出
     * if (excerpt) 的岔路。
     *
     * Whole-passage analysis (bit2). Same HTTP path and cleanup as scene(); the only
     * difference is the question. Kept as a separate method (not a boolean) because
     * both the prompt and the validation differ: here ctx is the subject and word is
     * only an anchor, so an empty word is fine.
     */
    public static Result excerpt(Config cfg, String context) {
        if (cfg == null) {
            return Result.fail("未配置");
        }
        if (context == null || context.trim().length() == 0) {
            return Result.fail("选文为空");
        }
        if (cfg.apiKey == null || cfg.apiKey.trim().length() == 0) {
            return Result.fail("未配置 API key");
        }
        // 26.11.3-C2 三笔，逐笔对应一条审计：
        //   ① 读取超时单独给 120 s（DEFAULT_TIMEOUT_MS 的 15 s 会把长输出掐死）；
        //   ② max_tokens 显式 8192，被 400 拒了就退到 4096 重试一次（端点上限不同）；
        //   ③ 字节帽用 EXCERPT_DEF_MAX_BYTES，并把「是否被砍」如实回报。
        // 26.11.3-C2, three items, one per audit finding: its own 120 s read timeout,
        // an explicit max_tokens (with one 4096 retry when the endpoint refuses 8192),
        // and the passage byte cap with the truncation flag reported instead of hidden.
        Result raw = fetch(cfg, buildExcerptRequest(cfg, context, EXCERPT_MAX_TOKENS),
                           EXCERPT_TIMEOUT_MS);
        if (!raw.ok && raw.error != null && raw.error.contains("HTTP 400")) {
            // 端点不认 8192（部分 OpenAI 兼容服务把 max_tokens 钉在模型上限 4096）。
            // 回退一次，日志由调用方打：「这一篇是 4096 档」比「短了」有意义。
            // The endpoint rejects 8192 (some OpenAI-compatible servers pin max_tokens
            // to a 4096 model ceiling). Retry once at 4096; the caller logs which tier
            // was used, which beats a bare "it came out short".
            raw = fetch(cfg, buildExcerptRequest(cfg, context, EXCERPT_MAX_TOKENS_FALLBACK),
                        EXCERPT_TIMEOUT_MS);
        }
        if (!raw.ok) {
            return raw;
        }
        String cleaned = cleanupScene(raw.text);
        boolean cut = utf8Len(cleaned) > EXCERPT_DEF_MAX_BYTES;
        String text = fitUtf8(cleaned, EXCERPT_DEF_MAX_BYTES);
        if (text.trim().isEmpty()) {
            return Result.fail("模型返回空解析");
        }
        return Result.ok(text, cut);
    }

    /* ---- gen30 伴读案：右栏摘要（R0 冻结件 2 + APK 任务书四模式 Prompt v1）----
     * 判据定稿 R0 §二：APK 端按 UTF-8 码点数硬截发送帽，不依赖 AI 自觉、无词边界
     * 回退；四套系统提示词见放行登记 §二。二轮：帽 56 → 192 字。三轮（07:37 篇幅
     * 扩充令）：帽 192 → 250 字、提示 180~220 字、设备端字号下限 22 px；输出按
     * 233 B 文本槽切分多帧（PushProto.encodeSummaryShards，250 汉字约 4 帧），
     * 设备端拼接后整段交付；单帧仍是 241 B，wire 帽 244 B 不动。
     * gen30 companion summary: hard codepoint clip at the send cap, deterministic.
     * Round 2 lifts the cap 56 -> 192 and shards the text into 233 B slots; the
     * device reassembles and only delivers a complete summary. ---- */

    /** 发送帽：250 字（≈750 B，四帧）。与设备侧 READER_COMPANION_SUMMARY_CAP 同值。 */
    public static final int SUMMARY_CHAR_CAP = 250;

    /** 超时 / 空回包占位（R0 §二：由设备侧右栏三态显示「本次未生成」）。 */
    public static final String SUMMARY_PLACEHOLDER = "本次未生成";

    /** 四模式公共约束（三版：100~150 字、大白话、纯文本；四模式共用）。 */
    private static final String SUMMARY_COMMON =
            "输入为当前页纯文本。输出 180~220 字，用大白话讲清楚，不要复述原文；"
            + "纯文本排版，分点只用 1. 2. 这样的序号，禁止 Markdown 符号（星号、井号、反引号）；"
            + "只输出内容本身，不要开场白和客套话。";

    /** 字数与排版硬约束（所有者令：字数解封后必须追加在提示词末尾）。 */
    static final String SUMMARY_TAIL =
            "请严格遵守字数限制，总输出不超过 220 个汉字，纯文本排版，不要使用加粗、星号等符号。";

    /**
     * 四套系统提示词 v3（公主谕令 · 伴读四大模式重构，mode 0~3 映射，逐字在案）。
     * 与 v2 的差别不是措辞微调而是角色重设：从「伴读助手」转为有性格的具体角色
     * （毒舌译者 / 幽默书评人 / 专栏主笔 / 讲书高手），目的是把机翻腔与流水账
     * 从提示层掐掉——v2 的「概括本页剧情要点」本身就是流水账的配方。
     * v3 role prompts: persona-driven rewriting, not a wording tweak. v2's
     * "summarize the plot points" is literally a recipe for a bland recital.
     */
    private static final String[] SUMMARY_MODE_PROMPTS = {
        "你是一位资深且毒舌的双语译者。请将这段外文翻译成流畅地道的中文，然后用最精炼、最接地气的一句话，概括这段话的核心主旨。不要逐字死译，要保留原作者的语气和情绪。直接给出翻译和总结，禁止废话。",
        "你是一个幽默、懂梗、极其敏锐的书评人。请阅读这段小说片段，直接给出一段接地气的“神评论”或吐槽。可以结合现代人的生活和网络热梗来解读剧情或人物心理。要求语言犀利、有趣，像朋友聊天一样，一针见血，能让人会心一笑。不要剧透后文，只评当前。",
        "你是一位逻辑极其清晰的专栏主笔。请从这段文字中，提取出最核心的论点或方法论。不要复述原文，请用大白话重新解构，总结出对读者最有用的 1 到 2 个干货观点。直接给出核心结论，排版分点，让人一秒看懂。",
        "你是一个极其擅长打比方的讲书高手。这段话可能涉及复杂的专业概念，请用大白话、用生活中常见的例子（比如买菜、开车、建房子等）给中学生讲明白。禁止使用任何晦涩的学术名词。要求通俗易懂，生动形象。",
    };

    /** 按模式取 system 提示；越界模式退到 0（外语原著，默认），防御设备侧脏编号。 */
    static String summarySystemPrompt(int mode) {
        int m = (mode < 0 || mode >= SUMMARY_MODE_PROMPTS.length) ? 0 : mode;
        return SUMMARY_MODE_PROMPTS[m] + SUMMARY_COMMON + SUMMARY_TAIL;
    }

    /**
     * 发送帽内码点硬截：按 Unicode 码点计数，绝不切半个代理对，无词边界回退，
     * 不依赖 AI 自觉。与设备侧 summary 拼接帽同口径——设备按 UTF-8 首字节判
     * 1/2/3/4 字节步长，一个码点即一「字」。帽 250 只做溢出兜底，正常输出
     * 180~220 字不会触发。
     */
    public static String clipSummaryChars(String s, int cap) {
        if (s == null || cap <= 0) {
            return "";
        }
        int cps = s.codePointCount(0, s.length());
        if (cps <= cap) {
            return s;
        }
        return s.substring(0, s.offsetByCodePoints(0, cap));
    }

    /** 构造伴读摘要请求体：system 恒为模式提示+公共约束+字数尾巴，user 为设备推来的当前页纯文本。 */
    static String buildSummaryRequest(Config cfg, String pageText, int mode) {
        String model = (cfg.model == null || cfg.model.trim().isEmpty())
                ? DEFAULT_MODEL : cfg.model.trim();
        String user = (pageText == null) ? "" : pageText;
        // 字数尾巴双写：system 末尾是主约束，user 末尾是「请求最后一句」的复述——
        // 模型对最后一条消息的末句遵从度最高。同一个常量，两处永不漂移。
        // The cap sentence is doubled on purpose: system carries the rule, the tail
        // of the last user message restates it (highest salience position).
        if (!user.isEmpty()) {
            user = user + "\n\n" + SUMMARY_TAIL;
        }
        return "{\"model\":" + jsonString(model)
                + ",\"temperature\":0,\"stream\":false,\"messages\":["
                + "{\"role\":\"system\",\"content\":" + jsonString(summarySystemPrompt(mode)) + "},"
                + "{\"role\":\"user\",\"content\":" + jsonString(user) + "}]}";
    }

    /**
     * 伴读摘要：按设备给的模式号套 Prompt v3 调 DeepSeek，cleanup 后按 250 码点
     * 硬截。同步调用，须在后台线程（Android 主线程禁网）。失败/超时/空回包一律
     * Result.fail，由调用方回「空文本」0x0A 帧（dlen=0）——设备侧右栏三态据此
     * 由「摘要生成中…」切到「本次未生成」（R0 §二）。设备侧实测无 16s 自动降级，
     * 不回帧会永久停在占位态；也不回「抱歉」之类占位文本让设备误当摘要渲染。
     */
    public static Result summarize(Config cfg, String pageText, int mode) {
        if (cfg == null || pageText == null || pageText.trim().isEmpty()) {
            return Result.fail("页文本为空");
        }
        if (cfg.apiKey == null || cfg.apiKey.trim().isEmpty()) {
            return Result.fail("未配置 API key");
        }
        Result raw = fetch(cfg, buildSummaryRequest(cfg, pageText, mode));
        if (!raw.ok) {
            return raw;
        }
        String text = clipSummaryChars(cleanupSummary(raw.text), SUMMARY_CHAR_CAP);
        if (text.trim().isEmpty()) {
            return Result.fail("模型返回空摘要");
        }
        return Result.ok(text);
    }

    /**
     * 一次 chat/completions 往返，返回**未清洗**的正文（失败时 result.ok=false）。
     * 抽出来的理由：两条业务（释义 / 情景）的差异全在提示词与清洗，HTTP 与
     * 错误归类完全一致；让它们各写一遍只会让错误文案慢慢漂开。
     * One chat/completions round trip returning the RAW content (ok=false on
     * failure). Extracted because the two callers differ only in prompt and
     * cleanup while HTTP and error mapping are identical; two copies would drift.
     */
    private static Result fetch(Config cfg, String bodyJson) {
        return fetch(cfg, bodyJson, 0);
    }

    /**
     * 同上，但**单独指定超时**（0 = 用 cfg.timeoutMs）。
     * 26.11.3-C2：整段解析要 40-70 s，词卡 8-10 s——一条链路两种时间尺度，超时
     * 必须能分开给；共用一个 15 s 就是让长输出随机死。
     * Same round trip with an explicit timeout (0 = cfg.timeoutMs). A passage takes
     * 40-70 s and a word card 8-10 s: one link, two time scales, so the timeout must
     * be passable per call - a shared 15 s kills long answers at random.
     */
    private static Result fetch(Config cfg, String bodyJson, int timeoutOverrideMs) {
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
            final int tmo = timeoutOverrideMs > 0 ? timeoutOverrideMs : cfg.timeoutMs;
            conn.setConnectTimeout(tmo);
            conn.setReadTimeout(tmo);
            conn.setDoOutput(true);
            conn.setInstanceFollowRedirects(false);

            byte[] body = bodyJson.getBytes(UTF8);
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
            return Result.ok(content);
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
     *   - system 角色恒为 systemPrompt()（360 字硬约束，gen26.6.4 校准），
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
     * 系统提示（gen26.6.4 校准：360 字硬约束，由 SYSTEM_CHAR_LIMIT 派生）：把输出形态
     * 钉死——「释义不超过 360 字」，且强制纯文本。这是第一道防线（让模型自己就范），
     * fitUtf8 的 1152 字节帧帽是第二道。360 个汉字 x 3 字节 = 1080 字节 ≤ 1152 帧帽，
     * 设备槽（PUSH_AI_DEF_MAX=1200）与 ai_reader(1200) 都放得下——预算与两端自洽。
     *
     * System prompt (gen26.6.4 calibration: 360-character hard constraint, derived
     * from SYSTEM_CHAR_LIMIT): pin the output shape - within 360 chars, plain text
     * only. First line of defence (the model behaves); fitUtf8's 1152-byte cap is the
     * second. 360 chars x 3 bytes = 1080 <= 1152, and the device slot / ai_reader
     * (both 1200) hold it - budget consistent with both ends.
     */
    static String systemPrompt() {
        return "你是电子墨水屏阅读器的词典助手。用户给出英文单词，你只返回该词在语境中最常用的"
                + "中文释义：释义不超过 " + SYSTEM_CHAR_LIMIT + " 字，一句核心词义 + 一个短语示例。"
                + "只输出纯文本：不要 Markdown、不要音标、不要解释、不要换行。";
    }

    /**
     * M3-B：情景 system 提示。**第二条钉死的 system 指令**——用户模板碰不到它
     * （v1 不进设置页），因为四节格式与「只引用选区内文字」是设备版面的前提：
     * 少一节，卡面就少一块；模型引了原文以外的东西，引用条与正文就对不上。
     * M3-B: the scene system prompt. A SECOND pinned system instruction the user
     * template cannot reach (not exposed in settings for v1), because the
     * four-section shape and "quote only the selection" are layout prerequisites:
     * a missing section leaves the card blank in one block, and invented quotes
     * would contradict the strip above the body.
     */
    static String sceneSystemPrompt() {
        return "你是阅读辅助工具。只依据用户给出的句子与上下文作答，不得引入原文没有的信息，"
                + "不得编造作者意图。输出纯文本，不用 Markdown、不用星号井号。"
                + "严格按四节输出，每节以【】标题起行："
                + "【词义】该词在此句中的确切含义（不是词典第一义，不列多个义项）"
                + "【句意】整句用现代汉语说清楚"
                + "【语境】这个词在这段里起什么作用、作者为什么用它"
                + "【用法】一个搭配或语法点，自造一个短例（不得引用原文以外内容）"
                + "总字数不超过 " + SCENE_CHAR_LIMIT + " 字，每节不超过 3 行。";
    }

    /** 情景请求体：system 恒为 sceneSystemPrompt()，user 由设备给的上下文拼装。 */
    static String buildSceneRequest(Config cfg, String word, String context) {
        String model = (cfg.model == null || cfg.model.trim().isEmpty())
                ? DEFAULT_MODEL : cfg.model.trim();
        String userContent = fitUtf8("【上下文】\n" + context + "\n【目标词】" + word,
                                     SCENE_CONTEXT_MAX_BYTES);
        return "{\"model\":" + jsonString(model)
                + ",\"temperature\":0,\"stream\":false,\"messages\":["
                + "{\"role\":\"system\",\"content\":" + jsonString(sceneSystemPrompt()) + "},"
                + "{\"role\":\"user\",\"content\":" + jsonString(userContent) + "}]}";
    }

    /** 情景 user 内容上限：设备上下文 1024B + 提示标签余量。/ Scene user cap: 1024B context plus labels. */
    static final int SCENE_CONTEXT_MAX_BYTES = 1200;

    /**
     * 26.11.3-C：整段解析的四节 system 提示。**第二条段落级提示词**，与
     * sceneSystemPrompt() 并列且互不复用——两条提示的取景对象不同（词 vs 段），
     * 共用措辞会让「该词」之类的字眼漂进段落模式，正是这次翻车的样子。
     *
     * 四节换了名字也换了含义：段意/脉络/关键表达/作用。设备端只按行渲染不认标题，
     * 所以改名不需要协议改动；但**节的个数必须仍是四节**，卡面版面按四块算。
     *
     * The passage-level system prompt, sibling to sceneSystemPrompt() and deliberately
     * not sharing wording with it - shared phrasing is how "that word" leaks into
     * passage mode, which is exactly what broke. Four sections, new names: gist /
     * flow / notable expressions / role. The device renders lines, not headers, so
     * renaming needs no protocol change - but the count must stay four.
     */
    static String excerptSystemPrompt() {
        // 26.11.5：**对照提到首节**。用户要的是「打开就先看见原文和译文」——
        // 那是这一段最值钱的东西，不该翻两页才出现。节序只是呈现约定（不是设备
        // 契约）：设备按标记定位切节，与节的先后无关，所以这一刀只动 APK 一端。
        // 26.11.5: the comparison section comes first. Section order is a presentation
        // convention, not a device contract - the device locates by marker, not by
        // position - so this cut lands on the APK alone.
        return "你是阅读辅助工具。用户给的是一段选文（多为英文小说或非虚构），不是单词。\n"
                + "只依据这段文字作答：不得引入原文以外的信息，不得编造作者意图或后续情节。\n"
                + "输出纯文本，不用 Markdown、不用星号井号。节与节之间空一行。\n"
                + "严格按下面五节输出，每节以【】标题起行；节内用 1. 2. 3. 分层。\n"
                + "\n"
                + "【原文对照】\n"
                + "仅当选文是英文时输出这一节；中文选文整节跳过。\n"
                + "逐句列出，每句两行：第一行英文原句，第二行中译；句与句之间空一行。\n"
                + "必须覆盖选文全部句子，不得挑着译、不得合并句子。译文要通顺，保留语气与停顿。\n"
                + "只有一种例外：选文长到逐句对照放不下时，改为**按段对照**（每段首句原文 + 该段大意），\n"
                + "并在本节末尾单独一行写「（长文模式：按段对照）」。\n"
                + "无论如何都不许悄悄少译几句——省略必须自己说出来，读者有权知道。\n"
                + "\n"
                + "【段意】\n"
                + "3-8 句。说清：谁、在什么处境、发生了什么、为什么、结果如何；有转折或因果要写出来。\n"
                + "不要复述原句，用你自己的话。宁可写长，不要概括成一句。\n"
                + "\n"
                + "【脉络】\n"
                + "按文本推进顺序分点，4-8 点，每点一行，格式：\n"
                + "1.〔开头几个词 …〕→ 这一层说了什么、起了什么作用\n"
                + "点与点之间要能看出推进（起因→发展→转折→收束），不要并列罗列。\n"
                + "\n"
                + "【关键表达】\n"
                + "挑 5-8 处（选文短于 300 字节则 3-4 处）。每处格式：\n"
                + "`原文片段` —— 字面义；在此处的功能（刻画了什么、暗示了什么、与上下文怎么呼应）。\n"
                + "不要只给词典释义；要说明作者为什么这么写。\n"
                + "\n"
                + "【作用】\n"
                + "2-4 句。这段在整章/整本书里承担什么：转场、铺垫、交代背景、制造悬念、收束情绪…；\n"
                + "对后文可能有什么影响（只依据本段能推出的，不猜情节）。\n"
                + "\n"
                + "篇幅：不设总字数上限。内容多就写长，读者翻页不是问题。\n"
                + "判断标准：读者读完这五节，不用回看原文也能讲清这段写了什么、怎么写的。\n"
                + "禁止：一句话概括、只列关键词、把原文抄一遍当段意。";
    }

    /** 整段解析请求体：system 恒为 excerptSystemPrompt()，user 只有选文。
     * 26.11.3-C2：**显式带 max_tokens**——不传就吃服务端默认，而默认值是别人的
     * 常量：换服务商、换模型就换了上限，我们这条按 8192 B 建出来的链路会莫名其妙
     * 短一截。传了才知道自己买的是多少。 */
    static String buildExcerptRequest(Config cfg, String context, int maxTokens) {
        String model = (cfg.model == null || cfg.model.trim().isEmpty())
                ? DEFAULT_MODEL : cfg.model.trim();
        String userContent = fitUtf8("【选文】\n" + context, SCENE_CONTEXT_MAX_BYTES);
        return "{\"model\":" + jsonString(model)
                + ",\"temperature\":0,\"stream\":false"
                + ",\"max_tokens\":" + maxTokens + ",\"messages\":["
                + "{\"role\":\"system\",\"content\":" + jsonString(excerptSystemPrompt()) + "},"
                + "{\"role\":\"user\",\"content\":" + jsonString(userContent) + "}]}";
    }

    /** 兼容旧签名（宿主测试与既有调用方）：走默认档 8192。 */
    static String buildExcerptRequest(Config cfg, String context) {
        return buildExcerptRequest(cfg, context, EXCERPT_MAX_TOKENS);
    }

    /**
     * 情景输出清洗：与 cleanup() 的关键差别是**保留换行**——四节靠换行分节，
     * 压成一行设备就只有一坨字。仍清 Markdown 星号/反引号，连续空行压成一行。
     * Scene cleanup differs from cleanup() in ONE thing that matters: newlines
     * survive, because they are the section separators. Markdown stars and
     * backticks still go, and runs of blank lines collapse to one.
     */
    static String cleanupScene(String s) {
        if (s == null) {
            return "";
        }
        String t = s.replace("\r\n", "\n").replace("\r", "\n");
        t = t.replace("**", "").replace("`", "");
        StringBuilder sb = new StringBuilder(t.length());
        boolean prevBlank = false;
        for (int i = 0; i < t.length(); i++) {
            char c = t.charAt(i);
            if (c == '\n') {
                if (!prevBlank) {
                    sb.append('\n');
                }
                prevBlank = true;
            } else if (c == ' ' || c == '\t') {
                // 行首/连续空格收敛，但不动换行。/ Collapse spaces, never newlines.
                int n = sb.length();
                if (n > 0 && sb.charAt(n - 1) != ' ' && sb.charAt(n - 1) != '\n') {
                    sb.append(' ');
                }
            } else {
                sb.append(c);
                prevBlank = false;
            }
        }
        return sb.toString().trim();
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

    /**
     * 摘要专用清洗：与 cleanup() 只差一件事——**保留换行**。
     * 右栏 ui_text_block 的断行器认 '\n' 作硬换行（换行符本身不进任何一行，
     * 见 ui_text_wrap.c），模式三「排版分点」只有保住换行才能真正分两行落版；
     * cleanup() 把换行压成空格，分点会连排成一长串，这是提示词再怎么写都补
     * 不回来的。连续空行压成一个，行首尾空白丢弃，免得白白吃掉右栏行数。
     * Summary cleanup keeps newlines: the device wrapper treats '\n' as a hard
     * break, which is the only way "1. / 2." bullets really land on separate
     * lines inside the 30% column. Collapses blank-line runs, trims line edges.
     */
    static String cleanupSummary(String s) {
        if (s == null) {
            return "";
        }
        String t = s.replace("\r\n", "\n").replace('\r', '\n').replace("\t", " ");
        t = t.replace("**", "").replace("`", "");
        StringBuilder sb = new StringBuilder(t.length());
        boolean nlSeen = false;
        for (int i = 0; i < t.length(); i++) {
            char c = t.charAt(i);
            if (c == '\n') {
                while (sb.length() > 0 && sb.charAt(sb.length() - 1) == ' ') {
                    sb.setLength(sb.length() - 1);
                }
                if (!nlSeen && sb.length() > 0) {
                    sb.append('\n');
                }
                nlSeen = true;
            } else if (c == ' ') {
                final int n = sb.length();
                if (n > 0 && sb.charAt(n - 1) != ' ' && sb.charAt(n - 1) != '\n') {
                    sb.append(' ');
                }
            } else {
                sb.append(c);
                nlSeen = false;
            }
        }
        while (sb.length() > 0) {
            final char last = sb.charAt(sb.length() - 1);
            if (last == ' ' || last == '\n') {
                sb.setLength(sb.length() - 1);
            } else {
                break;
            }
        }
        return sb.toString();
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
