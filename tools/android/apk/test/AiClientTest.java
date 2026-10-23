/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：AiClient 的宿主机测试（不发真实网络请求）。覆盖四件最容易出错的事：
 * ① 释义裁剪必须按字符边界（不许切出半个汉字）② JSON 转义 ③ 响应解析
 * ④ 失败路径一律返回 Result.fail 而不抛异常。
 *
 * English: host test for AiClient (no real network). Covers the four things
 * most likely to break: ① trimming on a character boundary (never half a CJK
 * char) ② JSON escaping ③ response parsing ④ failures coming back as
 * Result.fail instead of throwing.
 */
package dev.readpico.push;

import dev.readpico.push.AiClient.Config;
import dev.readpico.push.AiClient.Result;

public final class AiClientTest {
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

    private static int bytes(String s) {
        return s.getBytes(java.nio.charset.Charset.forName("UTF-8")).length;
    }

    public static void main(String[] args) {
        System.out.println("AiClientTest（纯逻辑，不发网络请求）:");

        // 1) 裁剪：不截断时原样返回
        String shortDef = "adj. 短暂的";
        check(shortDef.equals(AiClient.fitUtf8(shortDef, AiClient.DEF_MAX_BYTES)),
                "短释义原样返回（不画蛇添足加省略号）");

        // 2) 裁剪：中文按字符边界切，绝不半字
        StringBuilder cjk = new StringBuilder();
        for (int i = 0; i < 100; i++) {
            cjk.append("汉"); // 每个 3 字节
        }
        String cut = AiClient.fitUtf8(cjk.toString(), AiClient.DEF_MAX_BYTES);
        check(bytes(cut) <= AiClient.DEF_MAX_BYTES,
                "中文裁剪后 <= 169 字节（实测 " + bytes(cut) + "）");
        check(cut.endsWith("…"), "被截断时末尾加「…」");
        // 去掉省略号后必须是完整汉字序列（长度 * 3 == 字节数）
        String body = cut.substring(0, cut.length() - 1);
        check(bytes(body) == body.length() * 3, "裁剪结果全是完整汉字（无半字）");

        // 3) 裁剪：ASCII 长串
        StringBuilder ascii = new StringBuilder();
        for (int i = 0; i < 300; i++) {
            ascii.append('x');
        }
        String cutA = AiClient.fitUtf8(ascii.toString(), AiClient.DEF_MAX_BYTES);
        check(bytes(cutA) <= AiClient.DEF_MAX_BYTES, "ASCII 裁剪后 <= 169 字节");
        check(cutA.endsWith("…"), "ASCII 截断也加「…」");

        // 4) 裁剪：混排（中英混排不越界）
        StringBuilder mix = new StringBuilder();
        for (int i = 0; i < 40; i++) {
            mix.append("word 释义");
        }
        String cutM = AiClient.fitUtf8(mix.toString(), AiClient.DEF_MAX_BYTES);
        check(bytes(cutM) <= AiClient.DEF_MAX_BYTES, "中英混排裁剪后 <= 169 字节");

        // 5) JSON 转义
        check(AiClient.jsonString("a\"b").equals("\"a\\\"b\""), "转义双引号");
        check(AiClient.jsonString("a\\b").equals("\"a\\\\b\""), "转义反斜杠");
        check(AiClient.jsonString("a\nb").equals("\"a\\nb\""), "转义换行");
        check(AiClient.jsonString(null).equals("null"), "null -> null 字面量");

        // 6) 响应解析：标准 OpenAI/DeepSeek 形状
        String resp = "{\"id\":\"1\",\"choices\":[{\"index\":0,\"message\":"
                + "{\"role\":\"assistant\",\"content\":\"adj. 短暂的；朝生暮死的\"}"
                + ",\"finish_reason\":\"stop\"}],\"usage\":{\"total_tokens\":42}}";
        check("adj. 短暂的；朝生暮死的".equals(AiClient.extractContent(resp)),
                "解析 choices[0].message.content");

        // 7) 响应解析：content 里含转义字符
        String respEsc = "{\"choices\":[{\"message\":{\"content\":\"a\\\"b\\\\c\\nd\"}}]}";
        check("a\"b\\c\nd".equals(AiClient.extractContent(respEsc)), "解析含转义的 content");

        // 8) 响应解析：unicode 转义
        String respU = "{\"choices\":[{\"message\":{\"content\":\"\\u6c49\\u5b57\"}}]}";
        check("汉字".equals(AiClient.extractContent(respU)), "解析 \\uXXXX 转义");

        // 9) 响应解析：缺失 content / 非 JSON -> null（不崩）
        check(AiClient.extractContent("{\"choices\":[]}") == null, "无 content 返回 null");
        check(AiClient.extractContent("<html>502</html>") == null, "HTML 错误页返回 null");
        check(AiClient.extractContent(null) == null, "null 输入返回 null");

        // 10) cleanup：压空白、去 Markdown 标记
        check("adj. 短暂的".equals(AiClient.cleanup("  adj. **短暂的**  \n ")),
                "cleanup 压空白 + 去 Markdown 粗体");
        check(AiClient.cleanup(null).isEmpty(), "cleanup(null) -> 空串");
        check(AiClient.cleanup("a\nb").equals("a b"), "cleanup 把换行压成空格");

        // 11) buildRequest：模型名与词都进了请求体，且词被转义
        Config cfg = new Config();
        String req = AiClient.buildRequest(cfg, "serendipity");
        check(req.contains("\"model\":\"deepseek-chat\""), "请求体带默认模型 deepseek-chat");
        check(req.contains("serendipity"), "请求体带单词");
        check(req.contains("\"temperature\":0"), "temperature=0（释义要确定性）");
        check(req.contains("\"stream\":false"), "stream=false（一次性取回）");
        String reqEsc = AiClient.buildRequest(cfg, "a\"b");
        check(reqEsc.contains("a\\\"b"), "单词里的引号被转义（不破坏 JSON）");

        // 12) 失败路径：不抛异常，返回 fail
        Config noKey = new Config();
        Result r1 = AiClient.define(noKey, "word");
        check(!r1.ok && "未配置 API key".equals(r1.error), "无 key -> fail(未配置 API key)");
        Result r2 = AiClient.define(cfg, "");
        check(!r2.ok && "词为空".equals(r2.error), "空词 -> fail(词为空)");
        Result r3 = AiClient.define(null, "word");
        check(!r3.ok, "cfg=null -> fail 而非抛异常");

        // 13) 默认值：服务商可替换
        check(AiClient.DEFAULT_BASE_URL.equals("https://api.deepseek.com"), "默认 base_url = DeepSeek");
        check(AiClient.DEFAULT_MODEL.equals("deepseek-chat"), "默认 model = deepseek-chat");
        check(AiClient.DEF_MAX_BYTES == 169, "释义上限 169 字节（设备侧 170 留 NUL）");

        System.out.println("结果 / result: " + pass + " passed, " + fail + " failed — "
                + (fail == 0 ? "ALL PASS" : "HAS FAILURES"));
        System.exit(fail == 0 ? 0 : 1);
    }
}
