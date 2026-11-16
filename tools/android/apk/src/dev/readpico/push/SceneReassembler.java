/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：M3 情景释义 0x1A 的上下文重组器（纯逻辑、零 android 依赖，宿主可测）。
 * 设备把「词 + 上下文 + 词偏移 + flags」切成 241B 定长帧（首帧带元数据，后续帧
 * 纯文本，最多 5 帧）。本类按 seq/total 折叠回一条完整请求：
 *
 *   1. seq==0 开新请求：清空累积（上一请求无论是否攒齐——设备只在发起时发 0）；
 *   2. 顺序追加；攒够 total 帧即完成；
 *   3. 乱序/缺帧：seq≠期望值 → 整个请求作废，等下一个 seq==0 自然重置；
 *   4. 累积超过 1024B（SCENE_CTX_MAX）→ 作废，错帧不得吃内存。
 *
 * English: the M3 scene 0x1A context reassembler (pure logic, no android
 * dependencies, host-testable). The device splits "word + context + offset +
 * flags" into fixed 241B frames (metadata on the first, plain text after, at
 * most 5). This folds them back per seq/total: seq==0 starts a request,
 * in-order appends accumulate, a gap drops the whole request and waits for the
 * next seq==0, and anything past SCENE_CTX_MAX is dropped.
 */

package dev.readpico.push;

import java.nio.charset.StandardCharsets;

public final class SceneReassembler {

    /** 一条重组完成的情景请求。/ One reassembled scene request. */
    public static final class Scene {
        public final String word;
        public final String context;
        public final int wordOffset;
        public final int flags;

        Scene(String word, String context, int wordOffset, int flags) {
            this.word = word;
            this.context = context;
            this.wordOffset = wordOffset;
            this.flags = flags;
        }

        public boolean truncated() {
            return (flags & PushProto.SCENE_FLAG_CTX_TRUNCATED) != 0;
        }

        public boolean sentenceFallback() {
            return (flags & PushProto.SCENE_FLAG_SENTENCE_FALLBACK) != 0;
        }

        /** 整段选文（bit2）：word 只是锚点，回答要走段落四节。
         *  Whole passage (bit2): word is only the anchor; answer with four sections. */
        public boolean excerpt() {
            return (flags & PushProto.SCENE_FLAG_EXCERPT) != 0;
        }
    }

    /**
     * 未知 flags 位的喊话口。本类保持零 android 依赖（宿主可测），所以不直接
     * Log；App 启动时把 android.util.Log 挂进来，宿主上保持 null = 静默。
     * 26.11.3-C 的病根就是「对端收到未知位却一声不吭」，这一行 WARN 是防复发。
     *
     * Where unknown flag bits get shouted about. This class stays android-free so it
     * can run on the host, so it does not call Log directly: the app installs
     * android.util.Log at startup and the host leaves it null. The 26.11.3-C bug was
     * exactly "the peer received unknown bits and said nothing".
     */
    public interface FlagSink {
        void onUnknownFlags(int flags);
    }

    /** 由 App 安装；null = 静默（宿主）。/ Installed by the app; null = silent (host). */
    public static volatile FlagSink flagSink;

    private final byte[] buf = new byte[PushProto.SCENE_CTX_MAX];
    private int len;
    private int expectedSeq = -1; // 下一帧应有 seq；-1 = 空闲 / next expected seq; -1 = idle
    private int total;
    private String word = "";
    private int wordOffset;
    private int flags;

    /** 清空（断连/关闭监听时调用）。/ Reset (on disconnect or listener stop). */
    public void reset() {
        len = 0;
        expectedSeq = -1;
        total = 0;
        word = "";
        wordOffset = 0;
        flags = 0;
    }

    /**
     * 喂入一帧 0x1A 原始 notify 字节；攒齐返回 Scene，否则 null。
     * Feed one raw 0x1A notify frame; returns the Scene once complete, else null.
     */
    public Scene accept(byte[] frame) {
        if (frame == null || frame.length < PushProto.SCENE_FRAME_LEN) return null;
        if ((frame[0] & 0xFF) != PushProto.ST_AI_SCENE) return null;

        final int seq = getU16(frame, 1);
        final int frames = getU16(frame, 3);
        final int payload = getU16(frame, 5);
        if (frames <= 0 || frames > PushProto.SCENE_MAX_FRAMES) { reset(); return null; }
        if (seq >= frames || payload > PushProto.SCENE_SLOT) { reset(); return null; }

        if (seq == 0) {
            len = 0;
            total = frames;
            expectedSeq = 0;
            word = "";
            wordOffset = 0;
            flags = 0;
        } else if (expectedSeq < 0 || seq != expectedSeq || frames != total) {
            // 乱序 / 缺帧 / 跨请求串帧：整条作废，等下一个 seq==0。
            reset();
            return null;
        }

        int ctxOff = 7;
        int ctxLen = payload;
        if (seq == 0) {
            if (payload < 4) { reset(); return null; }
            final int wl = frame[7] & 0xFF;
            if (wl >= PushProto.SCENE_WORD_MAX || 4 + wl > payload) { reset(); return null; }
            word = new String(frame, 8, wl, StandardCharsets.UTF_8);
            wordOffset = getU16(frame, 8 + wl);
            flags = frame[10 + wl] & 0xFF;
            // 未知位当面喊：对端发了本版本不认识的语义，别等到真机结果不对才发现。
            // Shout about unknown bits now: the peer asked for semantics this build
            // does not know, and silence is what made 26.11.3-C hard to see.
            final int unknown = flags & ~PushProto.SCENE_FLAG_KNOWN;
            if (unknown != 0) {
                final FlagSink sink = flagSink;
                if (sink != null) sink.onUnknownFlags(unknown);
            }
            ctxOff = 11 + wl;
            ctxLen = payload - (4 + wl);
        }
        if (ctxLen < 0 || len + ctxLen > PushProto.SCENE_CTX_MAX) { reset(); return null; }
        System.arraycopy(frame, ctxOff, buf, len, ctxLen);
        len += ctxLen;
        expectedSeq = seq + 1;

        if (expectedSeq < total) return null;
        final String context = new String(buf, 0, len, StandardCharsets.UTF_8);
        final Scene done = new Scene(word, context, wordOffset, flags);
        reset();
        return done;
    }

    private static int getU16(byte[] in, int off) {
        return (in[off] & 0xFF) | ((in[off + 1] & 0xFF) << 8);
    }
}
