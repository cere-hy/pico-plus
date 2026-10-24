/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：PushProto 的宿主机对表测试。期望向量由 gen-vectors.py 从参考实现
 * push_pico.py 生成到 vectors.txt（不手抄字节），本测试只读取并断言。
 * 运行：python3 gen-vectors.py && javac -cp <PushProto.class 所在目录> PushProtoTest.java && java PushProtoTest
 */
import dev.readpico.push.PushProto;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.Charset;
import java.util.HashMap;
import java.util.Map;

public final class PushProtoTest {
    private static final Charset UTF8 = Charset.forName("UTF-8");
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

    private static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (int i = 0; i < b.length; i++) {
            sb.append(String.format("%02x", b[i] & 0xFF));
        }
        return sb.toString();
    }

    private static Map<String, String> vectors() throws IOException {
        Map<String, String> m = new HashMap<String, String>();
        InputStream in = PushProtoTest.class.getResourceAsStream("/vectors.txt");
        if (in == null) {
            throw new IOException("vectors.txt not found on classpath (run gen-vectors.py first)");
        }
        BufferedReader r = new BufferedReader(new InputStreamReader(in, UTF8));
        String line;
        while ((line = r.readLine()) != null) {
            int eq = line.indexOf('=');
            if (eq > 0) {
                m.put(line.substring(0, eq), line.substring(eq + 1));
            }
        }
        r.close();
        return m;
    }

    private static int vecInt(Map<String, String> v, String key) {
        return (int) Long.parseLong(v.get(key).trim());
    }

    private static String buildLongTitle() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 40; i++) {
            sb.append("🎉"); // U+1F389
        }
        for (int i = 0; i < 30; i++) {
            sb.append("汉");
        }
        return sb.toString();
    }

    /** hex -> bytes（把 vectors.txt 里的帧还原出来喂给解码器）。 */
    private static byte[] unhex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(s.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }

    /** 70 字节 ASCII 词：触发 63 字节截断。/ 70 ASCII bytes: triggers the 63-byte cut. */
    private static String longWord() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 70; i++) {
            sb.append('a');
        }
        return sb.toString();
    }

    /** 200 字节释义：触发 169 字节截断。/ 200-byte definition: triggers the 169-byte cut. */
    private static String longDef() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 200; i++) {
            sb.append('x');
        }
        return sb.toString();
    }

    private static byte[] bigBody() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 100; i++) {
            sb.append("推流测试数据块-");
        }
        byte[] all = sb.toString().getBytes(UTF8);
        byte[] out = new byte[1230];
        System.arraycopy(all, 0, out, 0, 1230);
        return out;
    }

    public static void main(String[] args) throws Exception {
        Map<String, String> v = vectors();
        System.out.println("PushProtoTest（对表 push_pico.py，向量来自 vectors.txt）:");

        // 1) CRC32
        check(PushProto.crc32("ReadPico BLE push reference".getBytes(UTF8)) == vecInt(v, "crc_sample"),
                "crc32(sample) == " + v.get("crc_sample"));
        check(PushProto.crc32(new byte[0]) == vecInt(v, "crc_empty"), "crc32(empty) == 0");

        // 2) CTRL 公共头
        check(hex(PushProto.encodeCtrl(PushProto.OP_END)).equals(v.get("end")), "END 头");
        check(hex(PushProto.encodeCtrl(PushProto.OP_ABORT)).equals(v.get("abort")), "ABORT 头");

        // 3) DATA 帧
        check(hex(PushProto.encodeData(0x1234, "abc".getBytes(UTF8))).equals(v.get("data_abc")),
                "DATA seq=0x1234 payload=abc");
        check(hex(PushProto.encodeData(0, new byte[0])).equals(v.get("data_empty")), "DATA 空包");

        // 4) BEGIN 帧
        byte[] mdBody = "墨水屏测试".getBytes(UTF8);
        check(hex(PushProto.encodeBegin(mdBody.length, PushProto.crc32(mdBody),
                0xDEADBEEFL, 0x1A2B, PushProto.TYPE_MARKDOWN, "标题✓")).equals(v.get("begin_md")),
                "BEGIN markdown/sid=DEADBEEF 逐字节一致");

        byte[] txtBody = "ReadPico BLE push reference".getBytes(UTF8);
        check(hex(PushProto.encodeBegin(txtBody.length, PushProto.crc32(txtBody),
                1L, 0x1A2B, PushProto.TYPE_TEXT, "来自手机")).equals(v.get("begin_text_sid1")),
                "BEGIN text/sid=1 逐字节一致");

        check(hex(PushProto.encodeBegin(0, 0, 1L, 0x1A2B, PushProto.TYPE_WORD,
                "serendipity")).equals(v.get("begin_word")), "BEGIN 单词卡（空正文）逐字节一致");

        // 5) 长标题截断：40 emoji + 30 汉字 -> 63 字节截断 + 续字节剥离
        byte[] lb = PushProto.encodeBegin(3, PushProto.crc32("abc".getBytes(UTF8)),
                1L, 0x1A2B, PushProto.TYPE_TEXT, buildLongTitle());
        check(lb.length == PushProto.BEGIN_LEN, "长标题 BEGIN 长度 83");
        check(hex(lb).equals(v.get("begin_long")), "长标题 BEGIN 逐字节一致");
        int titleLen = 0;
        for (int i = 19; i < 19 + 64; i++) {
            if (lb[i] != 0) {
                titleLen++;
            } else {
                break;
            }
        }
        check(titleLen == vecInt(v, "long_title_len"),
                "标题字段实占 " + v.get("long_title_len") + " 字节（截断保 0）");

        // 6) 广播名 -> token
        Integer tk = PushProto.tokenFromAdvName("PicoPush-1A2B");
        check(tk != null && tk == vecInt(v, "token_ok"), "广播名解析 token 0x1A2B");
        check(PushProto.tokenFromAdvName("Other-ABCD") == null, "非本服务广播名返回 null");

        // 7) 多分片（1230 字节 / 6 包，seq 0..5）
        byte[] big = bigBody();
        check(big.length == vecInt(v, "big_len"), "大正文 1230 字节");
        check(PushProto.crc32(big) == (int) Long.parseLong(v.get("big_crc")), "大正文 CRC32");
        check(PushProto.chunkForMtu(247) == vecInt(v, "chunk_247"), "chunk(247) == 241");
        check(PushProto.chunkForMtu(23) == vecInt(v, "chunk_23"), "chunk(23) == 17");
        check(hex(PushProto.encodeData(0, java.util.Arrays.copyOfRange(big, 0, 241)))
                .equals(v.get("big_first")), "分片 0（seq=0 len=241）逐字节一致");
        check(hex(PushProto.encodeData(5, java.util.Arrays.copyOfRange(big, 5 * 241, 1230)))
                .equals(v.get("big_last")), "分片 5（seq=5 len=25）逐字节一致");

        // 8) STATUS 解析
        byte[] errFrame = new byte[] { (byte) PushProto.ST_ERR, (byte) PushProto.ERR_CHECKSUM };
        check(PushProto.statusOp(errFrame) == PushProto.ST_ERR, "STATUS opcode");
        byte[] errBody = PushProto.statusBody(errFrame);
        check(errBody.length == 1 && (errBody[0] & 0xFF) == PushProto.ERR_CHECKSUM,
                "STATUS body = ERR_CHECKSUM");
        byte[] readyFrame = new byte[] { (byte) PushProto.ST_READY, 1, 0, 0, 0 };
        check(PushProto.readySessionId(readyFrame) == 1L, "READY session_id 解析");

        // 9) 错误码文案
        check(PushProto.errText(PushProto.ERR_CHECKSUM).contains("CRC32"), "errText CHECKSUM");
        check(PushProto.errText(PushProto.ERR_NOT_ARMED).contains("接收窗口"), "errText NOT_ARMED");

        // 10) gen26 AI 查词帧：CTRL 0x06 / 0x07 + STATUS 0x17 / 0x18
        //     编码侧与 Python 参考逐字节一致；解码侧做往返与非目标帧拒绝。
        check(PushProto.AI_QUERY_MAX == 69, "AI_QUERY(0x06) 帧长 69");
        check(PushProto.AI_RESULT_MAX == 241, "AI_RESULT(0x07) 帧长 241（<= MTU 247-3 = 244）");
        check(PushProto.AI_STATUS_QUERY_MAX == 66, "STATUS 0x17 帧长 66");

        check(hex(PushProto.encodeAiQuery("serendipity")).equals(v.get("ai_query_serendipity")),
                "AI_QUERY(0x06) serendipity 逐字节一致");
        check(hex(PushProto.encodeAiQuery("")).equals(v.get("ai_query_empty")),
                "AI_QUERY(0x06) 空词逐字节一致（槽位零填充）");
        check(hex(PushProto.encodeAiQuery(longWord())).equals(v.get("ai_query_long")),
                "AI_QUERY(0x06) 超长词按 63 字节截断");

        check(hex(PushProto.encodeAiResult("ephemeral", "adj. 短暂的"))
                .equals(v.get("ai_result_basic")), "AI_RESULT(0x07) 中文释义逐字节一致");
        check(hex(PushProto.encodeAiResult("ephemeral", "adj. 短暂的；朝生暮死的"))
                .equals(v.get("ai_result_cjk")), "AI_RESULT(0x07) 长中文释义逐字节一致");
        check(hex(PushProto.encodeAiResult("ephemeral", longDef()))
                .equals(v.get("ai_result_long")), "AI_RESULT(0x07) 释义 200B 截断到 169");
        check(hex(PushProto.encodeAiResult("ephemeral", ""))
                .equals(v.get("ai_result_empty_def")), "AI_RESULT(0x07) 空释义逐字节一致");

        check(hex(PushProto.encodeStatusAiQuery("serendipity"))
                .equals(v.get("status_ai_serendipity")), "STATUS 0x17 serendipity 逐字节一致");
        check(hex(PushProto.encodeStatusAiQuery("你好"))
                .equals(v.get("status_ai_cjk")), "STATUS 0x17 中文词逐字节一致");
        check(hex(PushProto.encodeStatusAiQuery(longWord()))
                .equals(v.get("status_ai_long")), "STATUS 0x17 超长词按 63 字节截断");
        check(hex(PushProto.encodeCaps(PushProto.CAP_AI_QUERY)).equals(v.get("caps_ai")),
                "STATUS 0x18 CAPS 能力位图逐字节一致");

        // 解码往返 / decode round-trips
        check("serendipity".equals(PushProto.aiQueryWord(unhex(v.get("status_ai_serendipity")))),
                "解 STATUS 0x17 -> serendipity");
        check("你好".equals(PushProto.aiQueryWord(unhex(v.get("status_ai_cjk")))),
                "解 STATUS 0x17 -> 你好（中文往返）");
        check(v.get("ai_word_long").equals(PushProto.aiQueryWord(unhex(v.get("status_ai_long")))),
                "解 STATUS 0x17 -> 超长词截断结果与参考一致");
        check(PushProto.capsBits(unhex(v.get("caps_ai"))) == vecInt(v, "caps_ai_bits"),
                "解 STATUS 0x18 -> 能力位图 == 1");
        check(PushProto.aiQueryWord(new byte[] { (byte) PushProto.ST_PROGRESS, 1, 'a' }) == null,
                "非 0x17 帧解词返回 null（不误判）");
        check(PushProto.capsBits(new byte[] { (byte) PushProto.ST_CAPS, 1 }) == -1L,
                "CAPS 帧过短返回 -1（不误判）");

        // 11) gen26.5：上下文档（0x06 / 0x17 ctx）/ 分帧释义（0x08）/ 能力使能（0x09）
        //     编码与 Python 参考逐字节一致；解码做往返、互逆与拒绝非目标帧。
        check(PushProto.AI_CTX_QUERY_MAX == 77, "0x06 ctx 帧长 77（词槽后接 book_id+pos）");
        check(PushProto.AI_STATUS_CTX_QUERY_LEN == 74, "0x17 ctx 帧长 74");
        check(PushProto.AI_SEG_FRAME_MAX == 241, "0x08 分帧 241（帧帽恰好用尽）");
        check(PushProto.AI_SEG_MAX == 166, "0x08 释义槽 166（含 NUL）");
        check(PushProto.AI_CAPS_ENABLE_MAX == 8, "0x09 使能帧 8B");

        check(hex(PushProto.encodeAiQueryCtx("serendipity", 42, 123456))
                .equals(v.get("ai_ctx_query_serendipity")), "0x06 ctx serendipity 逐字节一致");
        check(hex(PushProto.encodeAiQueryCtx("你好", 7, 0))
                .equals(v.get("ai_ctx_query_cjk")), "0x06 ctx 中文词逐字节一致");
        check(hex(PushProto.encodeAiQueryCtx(longWord(), 1, 2))
                .equals(v.get("ai_ctx_query_long")), "0x06 ctx 超长词按 63 字节截断");
        check(hex(PushProto.encodeStatusAiQueryCtx("serendipity", 42, 123456))
                .equals(v.get("status_ai_ctx_serendipity")), "0x17 ctx serendipity 逐字节一致");
        check(hex(PushProto.encodeStatusAiQueryCtx("你好", 7, 0))
                .equals(v.get("status_ai_ctx_cjk")), "0x17 ctx 中文词逐字节一致");
        check(hex(PushProto.encodeStatusAiQueryCtx(longWord(), 1, 2))
                .equals(v.get("status_ai_ctx_long")), "0x17 ctx 超长词按 63 字节截断");

        byte[] ctxFrame = unhex(v.get("ai_ctx_query_serendipity"));
        check("serendipity".equals(PushProto.aiQueryCtxWord(ctxFrame)), "解 0x06 ctx -> serendipity");
        check(PushProto.aiQueryCtxBookId(ctxFrame) == vecInt(v, "ai_ctx_book_serendipity"),
                "解 0x06 ctx -> book_id == 42");
        check(PushProto.aiQueryCtxPos(ctxFrame) == vecInt(v, "ai_ctx_pos_serendipity"),
                "解 0x06 ctx -> pos == 123456");
        byte[] stCtx = unhex(v.get("status_ai_ctx_serendipity"));
        check("serendipity".equals(PushProto.aiQueryStatusCtxWord(stCtx)), "解 0x17 ctx -> serendipity");
        check(PushProto.aiQueryStatusCtxBookId(stCtx) == vecInt(v, "ai_ctx_status_book"),
                "解 0x17 ctx -> book_id == 42");
        check(PushProto.aiQueryStatusCtxPos(stCtx) == vecInt(v, "ai_ctx_status_pos"),
                "解 0x17 ctx -> pos == 123456");

        // 新旧互连两向闭环：旧解码器读 ctx 仍得词；ctx 解码器拒旧帧
        check(v.get("ai_word_via_legacy_decoder").equals(PushProto.aiQueryWord(stCtx)),
                "旧解码器读 74B ctx 仍得词（上下文静默丢失）");
        check(PushProto.aiQueryStatusCtxWord(unhex(v.get("status_ai_serendipity"))) == null,
                "ctx 解码器拒 66B 旧帧（不误判）");
        check(PushProto.aiQueryCtxWord(unhex(v.get("ai_query_serendipity"))) == null,
                "ctx 解码器拒 69B 旧 0x06 帧（不误判）");
        check(PushProto.aiQueryCtxBookId(unhex(v.get("ai_query_serendipity"))) == -1L,
                "ctx book_id 在旧帧上返回 -1");

        // 0x08 分帧：首 / 中 / 末
        check(hex(PushProto.encodeAiResultSeg("ephemeral", "adj. 短暂的", 0, 1))
                .equals(v.get("ai_seg_single")), "0x08 单帧（1/1）逐字节一致");
        byte[] segFirst = unhex(v.get("ai_seg_first"));
        byte[] segMid = unhex(v.get("ai_seg_mid"));
        byte[] segLast = unhex(v.get("ai_seg_last"));
        check(PushProto.aiResultSegIdx(segFirst) == 0, "0x08 首段 idx == 0");
        check(PushProto.aiResultSegIdx(segLast) == vecInt(v, "ai_seg_count") - 1,
                "0x08 末段 idx == total-1");
        check(PushProto.aiResultSegTotal(segFirst) == vecInt(v, "ai_seg_count"),
                "0x08 总段数与参考一致");
        check(v.get("ai_seg_word_mid").equals(PushProto.aiResultSegWord(segMid)),
                "0x08 中段取词与参考一致");
        check(v.get("ai_seg_text_mid").equals(PushProto.aiResultSegText(segMid)),
                "0x08 中段释义与参考一致（UTF-8 不切断多字节序列）");
        check(PushProto.aiResultSegIdx(segMid) == vecInt(v, "ai_seg_idx_mid"), "0x08 中段 idx 一致");
        check(PushProto.aiResultSegTotal(segMid) == vecInt(v, "ai_seg_total_mid"), "0x08 中段 total 一致");
        check(hex(PushProto.encodeAiResultSeg(PushProto.aiResultSegWord(segMid),
                PushProto.aiResultSegText(segMid), PushProto.aiResultSegIdx(segMid),
                PushProto.aiResultSegTotal(segMid))).equals(v.get("ai_seg_mid")),
                "0x08 解码后重编 == 原帧（编解码互逆）");
        // 有牙：不产非法帧 + 拒收坏帧
        check(PushProto.encodeAiResultSeg("w", "x", 3, 3) == null, "0x08 idx==total 拒绝编码");
        check(PushProto.encodeAiResultSeg("w", "x", 0, 0) == null, "0x08 total==0 拒绝编码");
        check(PushProto.aiResultSegWord(unhex(v.get("ai_result_basic"))) == null,
                "0x08 解码器拒 0x07 帧（op 不符不误收）");
        byte[] badSeg = unhex(v.get("ai_seg_single"));
        int dlenOff = 5 + PushProto.AI_WORD_MAX + 4;
        badSeg[dlenOff] = (byte) 0xFF;
        badSeg[dlenOff + 1] = (byte) 0xFF;
        check(PushProto.aiResultSegText(badSeg) == null, "0x08 dlen 越界拒收（坏帧不误收）");

        // 0x09 能力使能：APK 订阅 STATUS 后发一次，设备收前 0x17 一律 legacy
        check(hex(PushProto.encodeAiCapsEnable(PushProto.CAP_AI_QUERY | PushProto.CAP_AI_CONTEXT))
                .equals(v.get("ai_caps_enable_ctx")), "0x09 使能帧（QUERY|CONTEXT）逐字节一致");
        check(PushProto.aiCapsEnableBits(unhex(v.get("ai_caps_enable_ctx")))
                == vecInt(v, "ai_caps_enable_ctx_bits"), "0x09 解出 bits == 3");
        check(hex(PushProto.encodeAiCapsEnable(PushProto.CAP_AI_QUERY | PushProto.CAP_AI_CONTEXT
                | PushProto.CAP_AI_MULTI)).equals(v.get("ai_caps_enable_all")),
                "0x09 使能帧（全三位）逐字节一致");
        check(PushProto.aiCapsEnableBits(unhex(v.get("ai_caps_enable_all")))
                == vecInt(v, "ai_caps_enable_all_bits"), "0x09 解出 bits == 7");
        check(PushProto.aiCapsEnableBits(new byte[] { 0x50, 0x43, 0x09, 0x01, 0x01 }) == -1L,
                "0x09 帧长 5B 拒（严格 8B）");
        check(PushProto.aiCapsEnableBits(
                new byte[] { 0x50, 0x43, 0x09, 0x01, 0x01, 0, 0, 0, 0 }) == -1L,
                "0x09 帧长 9B 拒（严格 8B）");
        check(PushProto.aiCapsEnableBits(unhex(v.get("caps_ai"))) == -1L,
                "0x09 解码器拒 0x18 CAPS 帧（两条线不混）");

        // 0x18 设备上报的新能力位
        check(hex(PushProto.encodeCaps(PushProto.CAP_AI_QUERY | PushProto.CAP_AI_CONTEXT
                | PushProto.CAP_AI_MULTI)).equals(v.get("caps_ai_ctx_multi")),
                "0x18 新能力位（CONTEXT|MULTI）逐字节一致");
        check(PushProto.capsBits(unhex(v.get("caps_ai_ctx_multi")))
                == vecInt(v, "caps_ai_ctx_multi_bits"), "0x18 解出 bits == 7");
        check(vecInt(v, "caps_ai_only_bits") == PushProto.CAP_AI_QUERY,
                "旧对照位图仅 CAP_AI_QUERY（未使能时的基线）");

        // 12) gen26.5 TTS 页文本（STATUS 0x19，241B 定长，设备主动 notify）
        //     与 0x08 不同方向、不同语义：0x08 是手机→设备的释义分帧，不复用。
        check(PushProto.ST_TTS_TEXT == 0x19, "TTS_TEXT opcode 0x19（STATUS 空间，非 CTRL 预留区）");
        check(PushProto.TTS_TEXT_FRAME_MAX == 241, "TTS_TEXT 帧长 241（帧帽内）");
        check(PushProto.TTS_TEXT_FRAME_MAX <= 244, "TTS_TEXT 帧长 <= STATUS notify 帽 244");
        check(PushProto.TTS_TEXT_MAX == 234, "TTS_TEXT 文本槽 234（含 NUL）");

        check(hex(PushProto.encodeTtsText("It was a bright cold day in April.", 0, 1))
                .equals(v.get("tts_single")), "0x19 单帧（1/1）逐字节一致");
        byte[] ttsFirst = unhex(v.get("tts_first"));
        byte[] ttsMid = unhex(v.get("tts_mid"));
        byte[] ttsLast = unhex(v.get("tts_last"));
        check(PushProto.ttsTextSeq(ttsFirst) == 0, "0x19 首帧 seq == 0");
        check(PushProto.ttsTextSeq(ttsLast) == vecInt(v, "tts_count") - 1, "0x19 末帧 seq == total-1");
        check(PushProto.ttsTextTotal(ttsFirst) == vecInt(v, "tts_count"), "0x19 总帧数与参考一致");
        check(v.get("tts_text_mid").equals(PushProto.ttsText(ttsMid)), "0x19 中段文本与参考一致");
        check(PushProto.ttsTextSeq(ttsMid) == vecInt(v, "tts_seq_mid"), "0x19 中段 seq 一致");
        check(PushProto.ttsTextTotal(ttsMid) == vecInt(v, "tts_total_mid"), "0x19 中段 total 一致");
        check(hex(PushProto.encodeTtsText(PushProto.ttsText(ttsMid), PushProto.ttsTextSeq(ttsMid),
                PushProto.ttsTextTotal(ttsMid))).equals(v.get("tts_mid")),
                "0x19 解码后重编 == 原帧（编解码互逆）");
        // 有牙：不产非法帧 + 不误收别的 STATUS 帧 / 坏帧
        check(PushProto.encodeTtsText("x", 1, 1) == null, "0x19 seq==total 拒绝编码");
        check(PushProto.encodeTtsText("x", 0, 0) == null, "0x19 total==0 拒绝编码");
        check(PushProto.ttsText(unhex(v.get("caps_ai"))) == null, "0x19 解码器拒 0x18 CAPS 帧（不混线）");
        byte[] badTts = unhex(v.get("tts_single"));
        badTts[5] = (byte) 0xFF;
        badTts[6] = (byte) 0xFF;
        check(PushProto.ttsText(badTts) == null, "0x19 len 越界拒收（坏帧不误收）");

        System.out.println("结果 / result: " + pass + " passed, " + fail + " failed — "
                + (fail == 0 ? "ALL PASS" : "HAS FAILURES"));
        System.exit(fail == 0 ? 0 : 1);
    }
}
