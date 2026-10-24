/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：ReadPico 安卓 BLE 推流的协议编解码（纯 Java，无 Android 依赖）。
 * 本文件是 components/push_sink/include/push_proto.h 与 tools/android/push_pico.py
 * 的逐字段对表：帧布局、CRC32、小端字节序全部一致，宿主 main() 测试
 * （test/PushProtoTest.java）用 Python 参考实现生成的向量逐字节断言。
 *
 * English: frame codec for the ReadPico BLE push. Pure Java, zero Android
 * imports, so it can be verified on the host JVM against the vectors emitted
 * by push_pico.py (the reference implementation).
 *
 * 冻结：帧布局与 push_proto.h 完全一致，单方面改字段会破坏已发货固件。
 * Frozen: byte-identical to push_proto.h; unilateral changes break shipped firmware.
 */
package dev.readpico.push;

import java.nio.charset.Charset;
import java.util.zip.CRC32;

public final class PushProto {
    private PushProto() {}

    /* ---- 常量（与 push_proto.h / push_pico.py 一字不差） ---- */

    public static final int MAGIC0 = 0x50; // 'P'
    public static final int MAGIC1 = 0x43; // 'C'
    public static final int VERSION = 0x01;

    public static final int TITLE_MAX = 64;          // 标题 UTF-8 上限（含结尾 0）
    public static final int DATA_PAYLOAD_MAX = 241;  // MTU 247 时每包载荷
    public static final int TOTAL_MAX = 512 * 1024;  // 正文硬上限；超了 ERR(3)

    public static final int OP_BEGIN = 0x01;
    public static final int OP_END = 0x02;
    public static final int OP_RESUME_QUERY = 0x03;
    public static final int OP_ABORT = 0x04;
    // gen22：手机下发 UTC 秒给设备校时（不受武装限制，连上即可）。
    // gen22: the phone pushes a UTC second count to set the device clock (not
    // gated on an armed window - any connection can carry it).
    public static final int OP_SET_TIME = 0x05;
    // gen26：AI 查词（蓝牙代理 + 搭车协议首个插件）。与 push_proto.h 的
    // PUSH_OP_AI_QUERY / PUSH_OP_AI_RESULT 同值同义；Java 侧此前未登记（APK
    // 只做推送），本代随 M1 应答链补齐——登记即冻结，改值会破坏已发货固件。
    // gen26: AI word lookup. Same values and meaning as push_proto.h; the Java
    // side never registered them (the APK only pushed). Added with the M1
    // responder chain - registration freezes, changing breaks shipped firmware.
    public static final int OP_AI_QUERY = 0x06;  // 手机→设备：触发回流 / 即时查询
    public static final int OP_AI_RESULT = 0x07; // 手机→设备：单词 + 释义回传
    // gen26.5（M2）：AI 释义分帧回传。seg_total==1 时语义等价 0x07（首帧即终帧）；
    // 0x07 保留不删（老 APK 单帧路），新 APK 在设备上报 CAP_AI_MULTI 时优先 0x08。
    // gen26.5 (M2): segmented AI definition. seg_total==1 is equivalent to 0x07;
    // 0x07 stays for the old path, new APKs prefer 0x08 when CAP_AI_MULTI is set.
    public static final int OP_AI_RESULT_SEG = 0x08;
    // gen26.5（M2）：AI 能力使能（手机→设备）。APK 订阅 STATUS 后发一次；设备收前
    // 0x17 一律 legacy 66B，收到且 CAP_AI_CONTEXT 置位才发 74B 上下文档——旧 APK
    // 永远不发，零感知兼容。断连清零重发。
    // gen26.5 (M2): AI capability enable (phone to device). Sent once after STATUS
    // subscribe; the device stays on legacy 66B 0x17 until it arrives. An old APK
    // never sends it -> zero-perception compatibility. Cleared on disconnect.
    public static final int OP_AI_CAPS_ENABLE = 0x09;

    public static final int ST_READY = 0x10;
    public static final int ST_RESUME = 0x11;
    public static final int ST_DONE = 0x12;
    public static final int ST_ERR = 0x13;
    public static final int ST_EVT_READ = 0x14;
    public static final int ST_BATT = 0x15;
    public static final int ST_PROGRESS = 0x16;
    // gen26：设备→手机的两条 AI 状态。0x17 请手机算释义，0x18 上报能力位图。
    // gen26: two device-to-phone AI statuses - 0x17 asks the phone to define a
    // word, 0x18 reports the capability bitmap.
    public static final int ST_AI_QUERY = 0x17;
    public static final int ST_CAPS = 0x18;
    // gen26.5 TTS 听书：设备→手机，当前页纯文本（STATUS notify，无 CTRL 头）。
    // 编号说明见 push_proto.h 变更记录块：STATUS 空间从 0x10 起，故取 0x19
    // （0x0A 仍属 CTRL 预留区，手机→设备方向）。
    public static final int ST_TTS_TEXT = 0x19;

    /* ---- gen26 AI 帧长与上限（与 push_proto.h 逐字一致）---- */
    public static final int AI_WORD_MAX = 64;   // 单词 UTF-8 槽（含结尾 0）
    public static final int AI_DEF_MAX = 170;   // 单帧释义槽（含结尾 0）
    public static final int AI_QUERY_MAX = 4 + 1 + AI_WORD_MAX;              // 0x06 = 69
    public static final int AI_RESULT_MAX = 4 + 1 + AI_WORD_MAX + 2 + AI_DEF_MAX; // 0x07 = 241
    public static final int AI_STATUS_QUERY_MAX = 1 + 1 + AI_WORD_MAX;       // 0x17 = 66
    /** 设备支持 AI 查词（0x06/0x07 + 0x17）的能力位。/ Device supports AI lookup. */
    public static final int CAP_AI_QUERY = 1 << 0;
    // gen26.5（M2）：0x17/0x06 带书标识+位置上下文；0x08 释义分帧。老 APK 不置位 →
    // 设备逐字节走 M1 老路（向后兼容枢轴）。gen26.5 (M2): contextual 0x17/0x06 and
    // segmented 0x08; an old APK without these keeps the byte-identical M1 path.
    public static final int CAP_AI_CONTEXT = 1 << 1;
    public static final int CAP_AI_MULTI = 1 << 2;
    // gen26.5（M2）帧长：带上下文的查询与分帧。gen26.5 (M2) frame sizes.
    public static final int AI_CTX_QUERY_MAX = 4 + 1 + AI_WORD_MAX + 4 + 4;  // 0x06 带上下文 = 77
    public static final int AI_STATUS_CTX_QUERY_LEN = 1 + 1 + AI_WORD_MAX + 4 + 4; // 0x17 带上下文体 = 74
    public static final int AI_SEG_MAX = 166;                               // 分帧释义槽：241 - 头4 - wlen1 - word64 - idx2 - total2 - dlen2
    public static final int AI_SEG_FRAME_MAX = 4 + 1 + AI_WORD_MAX + 2 + 2 + 2 + AI_SEG_MAX; // 0x08 = 241
    public static final int AI_CAPS_ENABLE_MAX = 4 + 4;                    // 0x09 = 8（头4 + bits u32）
    public static final int TTS_TEXT_MAX = 234;                            // 页文本槽（含 NUL）
    public static final int TTS_TEXT_FRAME_MAX = 1 + 2 + 2 + 2 + TTS_TEXT_MAX; // 0x19 = 241

    public static final int TYPE_TEXT = 0;
    public static final int TYPE_MARKDOWN = 1;
    public static final int TYPE_HTML = 2;
    public static final int TYPE_URL = 3;
    public static final int TYPE_WORD = 4;

    public static final int ERR_NONE = 0;
    public static final int ERR_NOT_ARMED = 1;
    public static final int ERR_BAD_FRAME = 2;
    public static final int ERR_TOO_LARGE = 3;
    public static final int ERR_CHECKSUM = 4;
    public static final int ERR_NO_CARD = 5;
    public static final int ERR_LOW_BATTERY = 6;
    public static final int ERR_BUSY = 7;
    public static final int ERR_FETCH = 8;
    public static final int ERR_NO_DICT = 9;

    public static final int BEGIN_LEN = 83;

    /* GATT 服务与特征（草案 3.1，写死；实证修正记录见 push_pico.py 常量区：
     * 固件 5f11/5f12 特性位一直是 F_WRITE（WWR 写请求），F_WRITE_NO_RSP 从未出现，
     * 本客户端同样默认 WWR：ATT 逐包回执是第一道防线，设备侧 RESUME 是第二道）。 */
    public static final String SERVICE_UUID = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10";
    public static final String DATA_UUID    = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f11"; // WWR 写 DATA 帧
    public static final String CTRL_UUID    = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f12"; // WWR 写 BEGIN/END
    public static final String STATUS_UUID  = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f13"; // read + notify
    public static final String CCCD_UUID    = "00002902-0000-1000-8000-00805f9b34fb";

    public static final String ADV_NAME_PREFIX = "PicoPush-";

    private static final Charset UTF8 = Charset.forName("UTF-8");

    /* ---- CRC32（与 zlib.crc32 / C push_crc32 完全一致） ---- */

    /** 整段 CRC32；返回值按无符号 32 位解释（Java int 可能为负，比较时注意）。 */
    public static int crc32(byte[] data) {
        CRC32 c = new CRC32();
        c.update(data);
        return (int) c.getValue();
    }

    public static int crc32(byte[] data, int off, int len) {
        CRC32 c = new CRC32();
        c.update(data, off, len);
        return (int) c.getValue();
    }

    /* ---- BEGIN 帧（83 字节） ---- */

    /**
     * 序列化 BEGIN：magic(2) + op(1) + ver(1) + type(1) + token(u16 LE) +
     * total_len(u32 LE) + crc32(u32 LE) + session_id(u32 LE) + title[64]。
     * title 按 UTF-8 字节截断到 63 并剥离尾随续字节（与 C 的 strnlen 行为一致）。
     */
    /**
     * gen22：SET_TIME 帧（8 字节）= 帧头 4B + UTC 秒 u32 LE。
     * BEGIN 帧布局保持冻结，故校时走独立操作码；老固件收到未知 op 会拒绝，
     * 不影响既有推送链。/ BEGIN stays frozen, so time sync uses its own opcode;
     * older firmware rejects the unknown op without disturbing the push chain.
     */
    public static byte[] encodeSetTime(long unixSeconds) {
        byte[] out = new byte[8];
        out[0] = (byte) MAGIC0;
        out[1] = (byte) MAGIC1;
        out[2] = (byte) OP_SET_TIME;
        out[3] = (byte) VERSION;
        putU32(out, 4, unixSeconds);
        return out;
    }

    /* ---- gen26 AI 帧（0x06 / 0x07 / STATUS 0x17 / 0x18）----
     * 布局与 push_proto.c 的 push_ai_*_encode、push_pico.py 的 encode_ai_*
     * 逐字节一致；截断一律按「UTF-8 字节上限」切（与参考实现同款：word 63 字节、
     * def 169 字节），槽位零填充。宿主对表由 test/PushProtoTest.java 用 Python
     * 生成的向量断言。
     * Byte-identical to push_proto.c and push_pico.py; truncation is always by
     * UTF-8 byte cap (63 for word, 169 for def) with zero padding, matching the
     * references. Asserted against Python-generated vectors in the host test.
     */

    /** 按 UTF-8 字节上限截断（不做字符边界回退——与 C/Python 参考同款行为）。 */
    private static byte[] fitUtf8(String s, int maxBytes) {
        byte[] raw = (s == null) ? new byte[0] : s.getBytes(UTF8);
        if (raw.length > maxBytes) {
            raw = copyOf(raw, maxBytes);
        }
        return raw;
    }

    /** CTRL 0x06 AI_QUERY（69 B）：帧头 + wlen(1) + word[64]。 */
    public static byte[] encodeAiQuery(String word) {
        byte[] raw = fitUtf8(word, AI_WORD_MAX - 1);
        byte[] out = new byte[AI_QUERY_MAX];
        out[0] = (byte) MAGIC0;
        out[1] = (byte) MAGIC1;
        out[2] = (byte) OP_AI_QUERY;
        out[3] = (byte) VERSION;
        out[4] = (byte) raw.length;
        System.arraycopy(raw, 0, out, 5, raw.length);
        return out;
    }

    /**
     * CTRL 0x07 AI_RESULT（241 B）：帧头 + wlen(1) + word[64] + dlen(u16 LE) +
     * def[170]。释义超 169 字节按字节截断（APK 侧应先自行裁到 UTF-8 安全边界，
     * 这里只是兜底，防半个汉字进帧）。
     */
    public static byte[] encodeAiResult(String word, String definition) {
        byte[] wraw = fitUtf8(word, AI_WORD_MAX - 1);
        byte[] draw = fitUtf8(definition, AI_DEF_MAX - 1);
        byte[] out = new byte[AI_RESULT_MAX];
        out[0] = (byte) MAGIC0;
        out[1] = (byte) MAGIC1;
        out[2] = (byte) OP_AI_RESULT;
        out[3] = (byte) VERSION;
        out[4] = (byte) wraw.length;
        System.arraycopy(wraw, 0, out, 5, wraw.length);
        int off = 5 + AI_WORD_MAX;
        putU16(out, off, draw.length);
        System.arraycopy(draw, 0, out, off + 2, draw.length);
        return out;
    }

    /** STATUS 0x17（66 B，设备→手机请算释义）：op + wlen(1) + word[64]。 */
    public static byte[] encodeStatusAiQuery(String word) {
        byte[] raw = fitUtf8(word, AI_WORD_MAX - 1);
        byte[] out = new byte[AI_STATUS_QUERY_MAX];
        out[0] = (byte) ST_AI_QUERY;
        out[1] = (byte) raw.length;
        System.arraycopy(raw, 0, out, 2, raw.length);
        return out;
    }

    /** 解 STATUS 0x17 → 单词；帧不是 0x17 或过短返回 null。 */
    public static String aiQueryWord(byte[] frame) {
        if (frame == null || frame.length < 2 || (frame[0] & 0xFF) != ST_AI_QUERY) {
            return null;
        }
        int n = frame[1] & 0xFF;
        if (n > AI_WORD_MAX - 1) {
            n = AI_WORD_MAX - 1;
        }
        if (frame.length < 2 + n) {
            n = frame.length - 2;
        }
        return new String(frame, 2, n, UTF8);
    }

    /** STATUS 0x18 CAPS（5 B）：op + 能力位图 u32 LE。 */
    public static byte[] encodeCaps(int bits) {
        byte[] out = new byte[5];
        out[0] = (byte) ST_CAPS;
        putU32(out, 1, bits & 0xFFFFFFFFL);
        return out;
    }

    /** 解 STATUS 0x18 → 能力位图；帧不是 0x18 或不足 5 字节返回 -1。 */
    public static long capsBits(byte[] frame) {
        if (frame == null || frame.length < 5 || (frame[0] & 0xFF) != ST_CAPS) {
            return -1L;
        }
        return getU32(frame, 1) & 0xFFFFFFFFL;
    }

    /* ---- gen26.5：上下文档（0x06 / STATUS 0x17）/ 分帧释义（0x08）/ 能力使能（0x09）----
     * 与 push_proto.c、push_pico.py 逐字节一致：ctx 帧 = 词槽 64 后接 book_id u32 +
     * pos u32（u32 一律 LE）；分帧释义槽 166（含 NUL）；使能帧严格 8B。
     * 长度分流即协议边界：ctx 解码严格等长（旧 69B / 66B 一律 null），而旧解码器
     * 读 ctx 上下文档仍得词（上下文静默丢失）——新旧互连两向都不断。
     * Byte-identical to push_proto.c / push_pico.py. Length is the protocol
     * boundary: ctx decoders are strict (old frames -> null) while the legacy
     * decoder still reads a ctx frame's word, so both interop directions hold.
     */

    /** CTRL 0x06 上下文档（77 B）：头4 + wlen1 + word[64] + book_id u32 + pos u32。 */
    public static byte[] encodeAiQueryCtx(String word, long bookId, long pos) {
        byte[] raw = fitUtf8(word, AI_WORD_MAX - 1);
        byte[] out = new byte[AI_CTX_QUERY_MAX];
        out[0] = (byte) MAGIC0;
        out[1] = (byte) MAGIC1;
        out[2] = (byte) OP_AI_QUERY;
        out[3] = (byte) VERSION;
        out[4] = (byte) raw.length;
        System.arraycopy(raw, 0, out, 5, raw.length);
        int off = 5 + AI_WORD_MAX;
        putU32(out, off, bookId);
        putU32(out, off + 4, pos);
        return out;
    }

    /** 0x06 ctx 校验（严格 77B）：合法返回词长，否则 -1。/ strict check; -1 = not a ctx frame. */
    private static int aiCtxWordLen(byte[] frame) {
        if (frame == null || frame.length != AI_CTX_QUERY_MAX) {
            return -1;
        }
        if ((frame[0] & 0xFF) != MAGIC0 || (frame[1] & 0xFF) != MAGIC1) {
            return -1;
        }
        if ((frame[2] & 0xFF) != OP_AI_QUERY || (frame[3] & 0xFF) != VERSION) {
            return -1;
        }
        int n = frame[4] & 0xFF;
        return (n < AI_WORD_MAX) ? n : -1;
    }

    /** 解 0x06 ctx → 词；非 ctx 帧（含旧 69B）返回 null。 */
    public static String aiQueryCtxWord(byte[] frame) {
        int n = aiCtxWordLen(frame);
        return n < 0 ? null : new String(frame, 5, n, UTF8);
    }

    /** 解 0x06 ctx → book_id；非 ctx 帧返回 -1。 */
    public static long aiQueryCtxBookId(byte[] frame) {
        if (aiCtxWordLen(frame) < 0) {
            return -1L;
        }
        return getU32(frame, 5 + AI_WORD_MAX) & 0xFFFFFFFFL;
    }

    /** 解 0x06 ctx → pos（书内位置）；非 ctx 帧返回 -1。 */
    public static long aiQueryCtxPos(byte[] frame) {
        if (aiCtxWordLen(frame) < 0) {
            return -1L;
        }
        return getU32(frame, 5 + AI_WORD_MAX + 4) & 0xFFFFFFFFL;
    }

    /** STATUS 0x17 上下文档（74 B）：op + wlen1 + word[64] + book_id u32 + pos u32。 */
    public static byte[] encodeStatusAiQueryCtx(String word, long bookId, long pos) {
        byte[] raw = fitUtf8(word, AI_WORD_MAX - 1);
        byte[] out = new byte[AI_STATUS_CTX_QUERY_LEN];
        out[0] = (byte) ST_AI_QUERY;
        out[1] = (byte) raw.length;
        System.arraycopy(raw, 0, out, 2, raw.length);
        putU32(out, 2 + AI_WORD_MAX, bookId);
        putU32(out, 2 + AI_WORD_MAX + 4, pos);
        return out;
    }

    /** 0x17 ctx 校验（严格 74B）：合法返回词长，否则 -1。 */
    private static int aiStatusCtxWordLen(byte[] frame) {
        if (frame == null || frame.length != AI_STATUS_CTX_QUERY_LEN) {
            return -1;
        }
        if ((frame[0] & 0xFF) != ST_AI_QUERY) {
            return -1;
        }
        int n = frame[1] & 0xFF;
        return (n < AI_WORD_MAX) ? n : -1;
    }

    /** 解 STATUS 0x17 ctx → 词；非 ctx 帧（含旧 66B）返回 null。 */
    public static String aiQueryStatusCtxWord(byte[] frame) {
        int n = aiStatusCtxWordLen(frame);
        return n < 0 ? null : new String(frame, 2, n, UTF8);
    }

    /** 解 STATUS 0x17 ctx → book_id；非 ctx 帧返回 -1。 */
    public static long aiQueryStatusCtxBookId(byte[] frame) {
        if (aiStatusCtxWordLen(frame) < 0) {
            return -1L;
        }
        return getU32(frame, 2 + AI_WORD_MAX) & 0xFFFFFFFFL;
    }

    /** 解 STATUS 0x17 ctx → pos；非 ctx 帧返回 -1。 */
    public static long aiQueryStatusCtxPos(byte[] frame) {
        if (aiStatusCtxWordLen(frame) < 0) {
            return -1L;
        }
        return getU32(frame, 2 + AI_WORD_MAX + 4) & 0xFFFFFFFFL;
    }

    /**
     * CTRL 0x08 分帧释义（241 B）：头4 + wlen1 + word[64] + idx u16 + total u16 +
     * dlen u16 + seg[166]。idx/total 越界返回 null（不产非法帧）。
     */
    public static byte[] encodeAiResultSeg(String word, String seg, int segIdx, int segTotal) {
        if (segTotal < 1 || segIdx < 0 || segIdx >= segTotal) {
            return null; // 序号须在界内 / index must be in range
        }
        byte[] wraw = fitUtf8(word, AI_WORD_MAX - 1);
        byte[] sraw = fitUtf8(seg, AI_SEG_MAX - 1);
        byte[] out = new byte[AI_SEG_FRAME_MAX];
        out[0] = (byte) MAGIC0;
        out[1] = (byte) MAGIC1;
        out[2] = (byte) OP_AI_RESULT_SEG;
        out[3] = (byte) VERSION;
        out[4] = (byte) wraw.length;
        System.arraycopy(wraw, 0, out, 5, wraw.length);
        int off = 5 + AI_WORD_MAX;
        putU16(out, off, segIdx);
        putU16(out, off + 2, segTotal);
        putU16(out, off + 4, sraw.length);
        System.arraycopy(sraw, 0, out, off + 6, sraw.length);
        return out;
    }

    /** 0x08 校验（严格 241B + 序号在界内）：合法返回词长，否则 -1。 */
    private static int aiSegWordLen(byte[] frame) {
        if (frame == null || frame.length != AI_SEG_FRAME_MAX) {
            return -1;
        }
        if ((frame[0] & 0xFF) != MAGIC0 || (frame[1] & 0xFF) != MAGIC1) {
            return -1;
        }
        if ((frame[2] & 0xFF) != OP_AI_RESULT_SEG || (frame[3] & 0xFF) != VERSION) {
            return -1;
        }
        int n = frame[4] & 0xFF;
        if (n >= AI_WORD_MAX) {
            return -1;
        }
        int off = 5 + AI_WORD_MAX;
        int idx = getU16(frame, off);
        int total = getU16(frame, off + 2);
        int dlen = getU16(frame, off + 4);
        if (total < 1 || idx >= total || dlen >= AI_SEG_MAX) {
            return -1;
        }
        return n;
    }

    /** 解 0x08 → 词；非法帧返回 null。 */
    public static String aiResultSegWord(byte[] frame) {
        int n = aiSegWordLen(frame);
        return n < 0 ? null : new String(frame, 5, n, UTF8);
    }

    /** 解 0x08 → 本段释义文本；非法帧返回 null。 */
    public static String aiResultSegText(byte[] frame) {
        if (aiSegWordLen(frame) < 0) {
            return null;
        }
        int off = 5 + AI_WORD_MAX;
        int dlen = getU16(frame, off + 4);
        return new String(frame, off + 6, dlen, UTF8);
    }

    /** 解 0x08 → 段序号；非法帧返回 -1。 */
    public static int aiResultSegIdx(byte[] frame) {
        if (aiSegWordLen(frame) < 0) {
            return -1;
        }
        return getU16(frame, 5 + AI_WORD_MAX);
    }

    /** 解 0x08 → 总段数；非法帧返回 -1。 */
    public static int aiResultSegTotal(byte[] frame) {
        if (aiSegWordLen(frame) < 0) {
            return -1;
        }
        return getU16(frame, 5 + AI_WORD_MAX + 2);
    }

    /** CTRL 0x09 能力使能（8 B）：头4 + bits u32 LE；APK 订阅 STATUS 后发一次。 */
    public static byte[] encodeAiCapsEnable(long bits) {
        byte[] out = new byte[AI_CAPS_ENABLE_MAX];
        out[0] = (byte) MAGIC0;
        out[1] = (byte) MAGIC1;
        out[2] = (byte) OP_AI_CAPS_ENABLE;
        out[3] = (byte) VERSION;
        putU32(out, 4, bits & 0xFFFFFFFFL);
        return out;
    }

    /** 解 0x09 → 能力位图；非使能帧或长度 ≠ 8 返回 -1（严格等长）。 */
    public static long aiCapsEnableBits(byte[] frame) {
        if (frame == null || frame.length != AI_CAPS_ENABLE_MAX) {
            return -1L;
        }
        if ((frame[0] & 0xFF) != MAGIC0 || (frame[1] & 0xFF) != MAGIC1) {
            return -1L;
        }
        if ((frame[2] & 0xFF) != OP_AI_CAPS_ENABLE || (frame[3] & 0xFF) != VERSION) {
            return -1L;
        }
        return getU32(frame, 4) & 0xFFFFFFFFL;
    }

    /* ---- gen26.5 TTS 页文本（STATUS 0x19，241 B 定长）----
     * op + seq(u16 LE) + total(u16 LE) + len(u16 LE) + text[234]；与 push_proto.c
     * 的 push_st_tts_text_encode、push_pico.py 的 encode_tts_text 逐字节一致。
     * 由设备主动 notify，旧 APK 对未知 STATUS 帧本就丢弃，无需能力协商。
     */

    /** STATUS 0x19 页文本（241 B）；seq/total 越界返回 null（不产非法帧）。 */
    public static byte[] encodeTtsText(String text, int seq, int total) {
        if (total < 1 || seq < 0 || seq >= total) {
            return null; // 序号须在界内 / index must be in range
        }
        byte[] raw = fitUtf8(text, TTS_TEXT_MAX - 1);
        byte[] out = new byte[TTS_TEXT_FRAME_MAX];
        out[0] = (byte) ST_TTS_TEXT;
        putU16(out, 1, seq);
        putU16(out, 3, total);
        putU16(out, 5, raw.length);
        System.arraycopy(raw, 0, out, 7, raw.length);
        return out;
    }

    /** 0x19 校验（严格 241B + 序号在界内）：合法返回文本长度，否则 -1。 */
    private static int ttsTextLen(byte[] frame) {
        if (frame == null || frame.length != TTS_TEXT_FRAME_MAX) {
            return -1;
        }
        if ((frame[0] & 0xFF) != ST_TTS_TEXT) {
            return -1;
        }
        int n = getU16(frame, 5);
        int seq = getU16(frame, 1);
        int total = getU16(frame, 3);
        if (total < 1 || seq >= total || n >= TTS_TEXT_MAX) {
            return -1;
        }
        return n;
    }

    /** 解 0x19 → 本帧文本；非法帧返回 null。 */
    public static String ttsText(byte[] frame) {
        int n = ttsTextLen(frame);
        return n < 0 ? null : new String(frame, 7, n, UTF8);
    }

    /** 解 0x19 → 本帧序号；非法帧返回 -1。 */
    public static int ttsTextSeq(byte[] frame) {
        return ttsTextLen(frame) < 0 ? -1 : getU16(frame, 1);
    }

    /** 解 0x19 → 总帧数；非法帧返回 -1。 */
    public static int ttsTextTotal(byte[] frame) {
        return ttsTextLen(frame) < 0 ? -1 : getU16(frame, 3);
    }

    public static byte[] encodeBegin(long totalLen, long crc, long sessionId,
                                     int token, int ptype, String title) {
        byte[] out = new byte[ BEGIN_LEN ];
        out[0] = (byte) MAGIC0;
        out[1] = (byte) MAGIC1;
        out[2] = (byte) OP_BEGIN;
        out[3] = (byte) VERSION;
        out[4] = (byte) ptype;
        putU16(out, 5, token);
        putU32(out, 7, totalLen);
        putU32(out, 11, crc);
        putU32(out, 15, sessionId);
        byte[] raw = title == null ? new byte[0] : title.getBytes(UTF8);
        if (raw.length > TITLE_MAX - 1) {
            raw = copyOf(raw, TITLE_MAX - 1);
            while (raw.length > 0 && (raw[raw.length - 1] & 0xC0) == 0x80) {
                raw = copyOf(raw, raw.length - 1); // 删除尾随 UTF-8 续字节
            }
        }
        System.arraycopy(raw, 0, out, 19, raw.length);
        return out;
    }

    /* ---- CTRL 帧（END / RESUME_QUERY / ABORT：4 字节公共头） ---- */

    public static byte[] encodeCtrl(int op) {
        return new byte[] { (byte) MAGIC0, (byte) MAGIC1, (byte) op, (byte) VERSION };
    }

    /* ---- DATA 帧：seq(u16 LE) + len(1) + payload ---- */

    public static byte[] encodeData(int seq, byte[] payload) {
        if (payload.length > DATA_PAYLOAD_MAX) {
            throw new IllegalArgumentException("payload too large for one DATA frame");
        }
        byte[] out = new byte[3 + payload.length];
        putU16(out, 0, seq);
        out[2] = (byte) payload.length;
        System.arraycopy(payload, 0, out, 3, payload.length);
        return out;
    }

    /* ---- STATUS 帧 ---- */

    /** 取 opcode；空帧返回 0。 */
    public static int statusOp(byte[] frame) {
        return (frame == null || frame.length == 0) ? 0 : frame[0] & 0xFF;
    }

    /** 取 body（opcode 之后的字节）。 */
    public static byte[] statusBody(byte[] frame) {
        if (frame == null || frame.length <= 1) {
            return new byte[0];
        }
        return copyOf(frame, frame.length - 1, 1);
    }

    /** READY 帧 body 前 4 字节是设备侧 session_id（小端）；不足 4 字节返回 -1。 */
    public static long readySessionId(byte[] frame) {
        byte[] body = statusBody(frame);
        if (body.length < 4) {
            return -1L;
        }
        return (getU32(body, 0)) & 0xFFFFFFFFL;
    }

    /**
     * RESUME 帧 body：offset(u32 LE) + next_seq(u16 LE)（gen14 新增解析）。
     * offset 是设备 .part 里已落盘的字节数；不足 4 字节返回 -1。
     * RESUME body: offset (u32 LE) + next_seq (u16 LE). The offset is the number
     * of bytes already flushed to the device's partial file.
     */
    public static long resumeOffset(byte[] frame) {
        byte[] body = statusBody(frame);
        if (body.length < 4) {
            return -1L;
        }
        return getU32(body, 0) & 0xFFFFFFFFL;
    }

    /** RESUME 帧 body 的 next_seq（u16 LE，偏移 4）；不足 6 字节返回 -1。 */
    public static long resumeNextSeq(byte[] frame) {
        byte[] body = statusBody(frame);
        if (body.length < 6) {
            return -1L;
        }
        return (body[4] & 0xFF) | ((body[5] & 0xFF) << 8);
    }

    /* ---- 广播名 -> token ---- */

    /** 从广播名 PicoPush-XXXX 解析 token（十六进制后缀）；非本服务广播名返回 null。 */
    public static Integer tokenFromAdvName(String name) {
        if (name == null || !name.startsWith(ADV_NAME_PREFIX)) {
            return null;
        }
        String suffix = name.substring(ADV_NAME_PREFIX.length());
        if (suffix.length() == 0) {
            return null;
        }
        try {
            return (int) Long.parseLong(suffix, 16) & 0xFFFF;
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /* ---- 错误码文案 ---- */

    public static String errText(int code) {
        switch (code) {
            case ERR_NONE:        return "成功";
            case ERR_NOT_ARMED:   return "设备未处于接收窗口";
            case ERR_BAD_FRAME:   return "帧魔数/版本/长度非法";
            case ERR_TOO_LARGE:   return "正文超过 512KB 上限";
            case ERR_CHECKSUM:    return "CRC32 校验不符";
            // gen16：ERR(5) 的语义不止「TF 卡不可用」——固件 do_commit 的 rename
            // 失败（旧版 FatFs 不覆盖目标名时）也回它。文案写宽，别误导用户去查卡。
            // gen16: ERR(5) means more than "card unavailable" - the firmware's
            // commit rename failure also answers it. Keep the label broad so
            // users don't chase the wrong cause.
            case ERR_NO_CARD:     return "设备存储写入失败（TF 卡缺失、写保护或提交失败）";
            case ERR_LOW_BATTERY: return "电量不足且未充电";
            case ERR_BUSY:        return "已有一次推送在处理";
            case ERR_FETCH:       return "URL 抓取失败";
            case ERR_NO_DICT:     return "设备无词典，无法显示单词卡";
            default:              return "未知错误码 " + code;
        }
    }

    /* ---- 分片大小 ---- */

    /** 每包 DATA 载荷 = min(241, mtu - 3 ATT 写头 - 3 帧头)。 */
    public static int chunkForMtu(int mtu) {
        return Math.min(DATA_PAYLOAD_MAX, mtu - 6);
    }

    /* ---- 小工具 ---- */

    private static void putU16(byte[] out, int off, int v) {
        out[off] = (byte) (v & 0xFF);
        out[off + 1] = (byte) ((v >> 8) & 0xFF);
    }

    private static void putU32(byte[] out, int off, long v) {
        out[off] = (byte) (v & 0xFF);
        out[off + 1] = (byte) ((v >> 8) & 0xFF);
        out[off + 2] = (byte) ((v >> 16) & 0xFF);
        out[off + 3] = (byte) ((v >> 24) & 0xFF);
    }

    private static int getU16(byte[] in, int off) {
        return (in[off] & 0xFF) | ((in[off + 1] & 0xFF) << 8);
    }

    private static int getU32(byte[] in, int off) {
        return (in[off] & 0xFF) | ((in[off + 1] & 0xFF) << 8)
                | ((in[off + 2] & 0xFF) << 16) | ((in[off + 3] & 0xFF) << 24);
    }

    private static byte[] copyOf(byte[] src, int len) {
        byte[] out = new byte[len];
        System.arraycopy(src, 0, out, 0, Math.min(len, src.length));
        return out;
    }

    private static byte[] copyOf(byte[] src, int len, int off) {
        byte[] out = new byte[len];
        System.arraycopy(src, off, out, 0, Math.min(len, src.length - off));
        return out;
    }
}
