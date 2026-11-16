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

        // 16) gen30 伴读摘要：码点硬截 + 四模式 Prompt + 请求体构造
        //    （R0 冻结件 2 / 放行登记 §二；三轮帽 250，帧分片见 PushProtoTest）
        check(AiClient.SUMMARY_CHAR_CAP == 250, "发送帽 = 250 码点（三轮篇幅扩充令）");
        check("本次未生成".equals(AiClient.SUMMARY_PLACEHOLDER),
                "占位文案 = 本次未生成（R0 §二）");

        // 码点硬截：确定性，无省略号、无词边界回退、不切半个汉字。
        // 与 fitUtf8（释义，按字节 + 省略号）刻意不同口径，两者互不复用。
        StringBuilder c260 = new StringBuilder();
        for (int i = 0; i < 260; i++) {
            c260.append('伴');
        }
        String clip250 = AiClient.clipSummaryChars(c260.toString(), AiClient.SUMMARY_CHAR_CAP);
        check(clip250.codePointCount(0, clip250.length()) == 250, "260 汉字硬截到 250 码点");
        check(bytes(clip250) == 250 * 3, "250 汉字 = 750 字节（全字，无半字）");
        check(clip250.length() == 250, "硬截不追加省略号（与 fitUtf8 不同）");
        check(c260.toString().startsWith(clip250), "硬截取前缀，不做提炼 / 词边界回退");
        check(250 * 3 <= 1024, "250 字 x 3 字节 = 750B <= 1024B 拼装帽");
        check((250 * 3 + 232) / 233 == 4, "750B 需 4 帧 233B 槽（帧号 0..3 落在 u8）");

        // 不超帽 / null / 非正帽：原样或空串，不抛异常
        check("短摘要".equals(AiClient.clipSummaryChars("短摘要", 250)), "不超帽原样返回");
        check(AiClient.clipSummaryChars(null, 250).isEmpty(), "null -> 空串");
        check(AiClient.clipSummaryChars("abc", 0).isEmpty(), "cap<=0 -> 空串");

        // 代理对（emoji U+1F600）按码点计数，绝不切出孤立高代理。
        // 249 汉字 + 1 emoji + 1 汉字 = 251 码点；截到 250 => 保留完整 emoji、丢末字。
        StringBuilder emoji = new StringBuilder();
        for (int i = 0; i < 249; i++) {
            emoji.append('字');
        }
        emoji.append("\uD83D\uDE00");
        emoji.append('尾');
        String clipE = AiClient.clipSummaryChars(emoji.toString(), 250);
        check(clipE.codePointCount(0, clipE.length()) == 250, "含代理对时按码点计数（251→250）");
        check(clipE.endsWith("\uD83D\uDE00"), "第 250 码点是完整 emoji（不切半代理对）");
        check(!clipE.endsWith("尾"), "第 251 码点被截掉");

        // 四套系统提示词 v3 逐字在案（公主谕令 · 四大模式重构），公共约束 + 字数尾巴
        // 四模式共用。角色重设是本版唯一目的：v2「概括本页剧情要点」本身就是
        // 流水账配方，v3 换成有性格的具体角色，从提示层掐掉机翻腔。
        String[] expect = {
            "你是一位资深且毒舌的双语译者。请将这段外文翻译成流畅地道的中文，然后用最精炼、最接地气的一句话，概括这段话的核心主旨。不要逐字死译，要保留原作者的语气和情绪。直接给出翻译和总结，禁止废话。",
            "你是一个幽默、懂梗、极其敏锐的书评人。请阅读这段小说片段，直接给出一段接地气的“神评论”或吐槽。可以结合现代人的生活和网络热梗来解读剧情或人物心理。要求语言犀利、有趣，像朋友聊天一样，一针见血，能让人会心一笑。不要剧透后文，只评当前。",
            "你是一位逻辑极其清晰的专栏主笔。请从这段文字中，提取出最核心的论点或方法论。不要复述原文，请用大白话重新解构，总结出对读者最有用的 1 到 2 个干货观点。直接给出核心结论，排版分点，让人一秒看懂。",
            "你是一个极其擅长打比方的讲书高手。这段话可能涉及复杂的专业概念，请用大白话、用生活中常见的例子（比如买菜、开车、建房子等）给中学生讲明白。禁止使用任何晦涩的学术名词。要求通俗易懂，生动形象。",
        };
        for (int m = 0; m < expect.length; m++) {
            check(AiClient.summarySystemPrompt(m).startsWith(expect[m]),
                    "模式 " + m + " 系统提示 v3 逐字在案");
        }

        // 角色重设的回归护栏：v2 的四条「伴读助手」开场必须绝迹，防误回滚。
        String[] v2Heads = {"你是外语原著伴读助手", "你是小说伴读助手",
                            "你是社科阅读伴读助手", "你是古文诗词伴读助手"};
        for (String h : v2Heads) {
            check(!AiClient.summarySystemPrompt(0).contains(h), "v2 旧角色已清除：" + h);
        }

        // 公共约束（100~150 字、大白话、纯文本、序号分点、禁 Markdown）。
        String common = AiClient.summarySystemPrompt(0);
        check(common.contains("180~220 字"), "公共约束：180~220 字（三轮扩充）");
        check(common.contains("大白话") && common.contains("不要复述原文"),
                "公共约束：大白话 + 不复述原文");
        check(common.contains("纯文本") && common.contains("禁止 Markdown 符号"),
                "公共约束：纯文本 + 禁 Markdown 符号");
        check(common.contains("1. 2."), "公共约束：分点只用 1. 2. 序号（墨水屏可排）");

        // 所有者令：字数解封后，尾巴必须逐字追加在提示词末尾，四模式一致。
        check(AiClient.SUMMARY_TAIL.equals(
                        "请严格遵守字数限制，总输出不超过 220 个汉字，纯文本排版，不要使用加粗、星号等符号。"),
                "字数尾巴逐字在案（三轮 220）");
        for (int m = 0; m < expect.length; m++) {
            check(AiClient.summarySystemPrompt(m).endsWith(AiClient.SUMMARY_TAIL),
                    "模式 " + m + " 以字数尾巴收尾（请求末尾硬约束）");
        }

        // 提示词自身不得带 Markdown 星号 / 井号 / 反引号——否则模型会照抄符号。
        for (int m = 0; m < expect.length; m++) {
            String p = AiClient.summarySystemPrompt(m);
            check(!p.contains("*") && !p.contains("#") && !p.contains("`"),
                    "模式 " + m + " 提示词不含 Markdown 符号");
        }

        // 篇幅锚点：输出 180~220 字 = 540~660 B，落在 1024B 拼装帽内（三帧足够）。
        check(220 * 3 <= 1024, "220 汉字 x 3B = 660B <= 1024B 拼装帽（三帧 233B 槽即可）");

        check(AiClient.summarySystemPrompt(-1).equals(AiClient.summarySystemPrompt(0))
                        && AiClient.summarySystemPrompt(99).equals(AiClient.summarySystemPrompt(0)),
                "越界模式退到 0（外语原著），防御设备侧脏编号");

        // 请求体构造：system 恒为模式提示 + 公共约束 + 尾巴，user 恒为页文本 + 尾巴。
        Config sumCfg = new Config();
        String sumReq = AiClient.buildSummaryRequest(sumCfg, "第一页文本", 2);
        check(sumReq.contains("\"model\":\"deepseek-chat\""), "摘要请求带默认模型 deepseek-chat");
        check(sumReq.contains("\"temperature\":0"), "摘要请求 temperature=0（确定性）");
        check(sumReq.contains("\"stream\":false"), "摘要请求 stream=false（一次性取回）");
        check(sumReq.contains("\"role\":\"system\"") && sumReq.contains("\"role\":\"user\""),
                "摘要请求含 system + user 双角色");
        check(sumReq.contains(expect[2]), "摘要请求 system 用模式 2 的提示词");
        check(sumReq.contains("第一页文本"), "摘要请求 user 为设备推来的页文本");
        check(sumReq.contains(AiClient.jsonString("第一页文本\n\n" + AiClient.SUMMARY_TAIL)),
                "user 末句为字数尾巴（请求最后一句复述，双写不漂移）");
        String sumReqEmpty = AiClient.buildSummaryRequest(sumCfg, "", 0);
        check(sumReqEmpty.contains("\"content\":\"\""), "空页文本不追加尾巴（保持空 user）");
        String sumReqEsc = AiClient.buildSummaryRequest(sumCfg, "a\"b\\c", 0);
        check(sumReqEsc.contains("a\\\"b\\\\c"), "页文本里的引号 / 反斜杠被转义（不破 JSON）");

        // 摘要清洗保留换行：模式三「排版分点」只有保住 '\n' 才能真分行
        // （设备侧 ui_text_wrap 认 '\n' 作硬换行）。老 cleanup 保持压空格，释义链零影响。
        check("1. 甲\n2. 乙".equals(AiClient.cleanupSummary("1. 甲\n2. 乙")),
                "cleanupSummary 保留硬换行（分点真分行）");
        check("a\nb".equals(AiClient.cleanupSummary("a\n\n\nb")), "cleanupSummary 连续空行压成一个");
        check("粗体 code".equals(AiClient.cleanupSummary("**粗体** `code`")),
                "cleanupSummary 去 ** 与反引号");
        check("x\ny".equals(AiClient.cleanupSummary("  x  \n  y  ")),
                "cleanupSummary 行首尾空白丢弃");
        check("a b".equals(AiClient.cleanupSummary("a\r\nb").replace("\n", " ")),
                "cleanupSummary CRLF 归一（\\r 不残留）");
        check("1. 甲 2. 乙".equals(AiClient.cleanup("1. 甲\n2. 乙")),
                "对照：老 cleanup 仍压空格（释义 / 词卡 / 情景链零影响）");

        System.out.println("结果 / result: " + pass + " passed, " + fail + " failed — "
                + (fail == 0 ? "ALL PASS" : "HAS FAILURES"));
        System.exit(fail == 0 ? 0 : 1);
    }
}
