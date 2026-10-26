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

        // 2) 裁剪：中文按字符边界切，绝不半字（400 汉字 = 1200B > 1152 帧帽，必截断）
        StringBuilder cjk = new StringBuilder();
        for (int i = 0; i < 400; i++) {
            cjk.append("汉"); // 每个 3 字节
        }
        String cut = AiClient.fitUtf8(cjk.toString(), AiClient.DEF_MAX_BYTES);
        check(bytes(cut) <= AiClient.DEF_MAX_BYTES,
                "中文裁剪后 <= 1152 字节（实测 " + bytes(cut) + "）");
        check(cut.endsWith("…"), "被截断时末尾加「…」");
        // 去掉省略号后必须是完整汉字序列（长度 * 3 == 字节数）
        String body = cut.substring(0, cut.length() - 1);
        check(bytes(body) == body.length() * 3, "裁剪结果全是完整汉字（无半字）");

        // 3) 裁剪：ASCII 长串（1300 字节 > 1152 帧帽，必截断）
        StringBuilder ascii = new StringBuilder();
        for (int i = 0; i < 1300; i++) {
            ascii.append('x');
        }
        String cutA = AiClient.fitUtf8(ascii.toString(), AiClient.DEF_MAX_BYTES);
        check(bytes(cutA) <= AiClient.DEF_MAX_BYTES, "ASCII 裁剪后 <= 1152 字节");
        check(cutA.endsWith("…"), "ASCII 截断也加「…」");

        // 4) 裁剪：混排（中英混排不越界）
        StringBuilder mix = new StringBuilder();
        for (int i = 0; i < 120; i++) {
            mix.append("word 释义");
        }
        String cutM = AiClient.fitUtf8(mix.toString(), AiClient.DEF_MAX_BYTES);
        check(bytes(cutM) <= AiClient.DEF_MAX_BYTES, "中英混排裁剪后 <= 1152 字节");

        // 4b) 边界：恰好 1152B 不截断；1153B 截断（帽即版本锚，180/230/360 三态可辨）
        StringBuilder exact = new StringBuilder();
        for (int i = 0; i < AiClient.DEF_MAX_BYTES; i++) exact.append('a');
        check(exact.toString().equals(AiClient.fitUtf8(exact.toString(), AiClient.DEF_MAX_BYTES)),
                "恰好 1152B 原样返回（不补省略号）");
        StringBuilder over = new StringBuilder(exact);
        over.append('a');
        check(AiClient.fitUtf8(over.toString(), AiClient.DEF_MAX_BYTES).endsWith("…"),
                "1153B 触发截断（帽生效）");

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
        check(AiClient.DEF_MAX_BYTES == 1152,
                "释义上限 1152 字节（gen26.6.4：700B 切掉 8 句例句；1152 留 48B 给固件槽 1200）");

        // 14) gen26.6.4 ③ Prompt 360 字硬约束：一句核心词义 + 一个短语示例 + 纯文本。
        // 与容量实算联动：360 汉字 x 3 字节 = 1080 <= 1152 帧帽，且 <= 设备侧
        // PUSH_AI_DEF_MAX=1200——预算与两端容量自洽。
        // gen26.6.4 item-3: the 360-character hard constraint (one core meaning
        // plus one phrase example, plain text). Linked to the budget: 360 chars
        // x 3 bytes = 1080 <= the 1152-byte frame cap and <= the device's
        // PUSH_AI_DEF_MAX=1200 - consistent both ends.
        String prompt = AiClient.systemPrompt();
        check(prompt.contains("360 字"), "系统提示钉死「360 字」硬约束");
        check(prompt.contains("短语示例"), "系统提示要求一个短语示例");
        check(prompt.contains("纯文本"), "系统提示强制纯文本");
        check(prompt.contains("Markdown"), "系统提示排除 Markdown");
        check(!prompt.contains("50 个汉字"), "旧的 50 字约束已移除（不是并存两个数）");
        check(!prompt.contains("40 字"), "旧的 40 字约束已退役");
        check(!prompt.contains("180 字"), "旧的 180 字约束已退役");
        check(!prompt.contains("230 字"), "旧的 230 字约束已退役（gen26.6.4 放宽到 360 字）");
        check(360 * 3 <= AiClient.DEF_MAX_BYTES,
                "360 字 x 3 字节 = 1080 <= 1152 帧帽（预算与帧帽自洽）");
        check(AiClient.buildRequest(cfg, "serendipity").contains("360 字"),
                "请求体真的带着 360 字约束（prompt 进了 system 消息）");

        // 15) M1.2 ① 提示模板渲染 + 分层铁律
        // M1.2 item-1: prompt-template rendering + the layering rule.
        check(AiClient.DEFAULT_TEMPLATE.equals("word: {word}"),
                "默认模板 = word:{word}（语义等价于历史硬编码）");

        // 渲染：null / 空 → 退化为默认模板
        check(AiClient.renderTemplate(null, "apple").equals("word: apple"),
                "renderTemplate(null) 退化为默认模板");
        check(AiClient.renderTemplate("", "apple").equals("word: apple"),
                "renderTemplate(\"\") 退化为默认模板");

        // 渲染：含 {word} → 就地替换
        check(AiClient.renderTemplate("解释一下 {word} 的意思", "apple")
                        .equals("解释一下 apple 的意思"),
                "renderTemplate 替换 {word} 占位符");
        // 多处 {word} 都替换
        check(AiClient.renderTemplate("{word}/{word}", "x").equals("x/x"),
                "renderTemplate 多处 {word} 均替换");

        // 渲染：漏写 {word} → 末尾补词，单词绝不被吞
        check(AiClient.renderTemplate("请翻译 apple", "apple").equals("请翻译 apple apple"),
                "漏写占位符时末尾补词（单词不丢失）");

        // 渲染：超长模板按字符边界裁剪并补「…」
        StringBuilder huge = new StringBuilder();
        for (int i = 0; i < 2000; i++) {
            huge.append("长"); // 每字 3 字节，远超 TEMPLATE_MAX_BYTES
        }
        String cutT = AiClient.renderTemplate(huge.toString(), "w");
        check(bytes(cutT) <= AiClient.TEMPLATE_MAX_BYTES,
                "超长模板渲染后 <= 2000 字节（实测 " + bytes(cutT) + "）");
        check(cutT.endsWith("…"), "超长模板被裁剪时补「…」");

        // 分层铁律：自定义模板只进 user 角色，system 角色恒为 systemPrompt()
        Config tplCfg = new Config();
        tplCfg.promptTemplate = "用户说：{word}";
        String tplReq = AiClient.buildRequest(tplCfg, "apple");
        check(tplReq.contains("\"role\":\"system\""), "请求体有 system 角色");
        check(tplReq.contains("\"role\":\"user\""), "请求体有 user 角色");
        check(tplReq.contains("用户说：apple"), "自定义模板进了 user 消息");
        check(!tplReq.contains("word: apple"), "user 消息用的是自定义模板而非默认");
        check(tplReq.contains("360 字"), "系统 360 字约束仍在（模板碰不到 system）");
        // 最强一条：system 消息内容恒等于 systemPrompt()——模板不可覆盖
        check(tplReq.contains(AiClient.jsonString(AiClient.systemPrompt())),
                "system 消息内容恒等于 systemPrompt()（分层铁律：模板不可覆盖）");

        System.out.println("结果 / result: " + pass + " passed, " + fail + " failed — "
                + (fail == 0 ? "ALL PASS" : "HAS FAILURES"));
        System.exit(fail == 0 ? 0 : 1);
    }
}
