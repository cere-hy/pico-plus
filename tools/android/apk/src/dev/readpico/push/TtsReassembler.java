/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：gen26.5 M2 Step10 TTS 听书的页文本重组器（纯逻辑，零 android 依赖，
 * 宿主先行：test/TtsReassemblerTest.java）。把设备 STATUS 0x19 的多帧页文本
 * 按冻结约定（plan step10-recon.md §二）重组回整页：
 *
 *   1. seq==0 = 新页开始：清空累积缓冲（无论上一页是否攒齐——设备只在「新页
 *      落定」时发 seq==0，页内不会重发 0）；
 *   2. 拼接：按 seq 顺序追加各帧 text，攒够 total 帧（seq 0..total-1 各一）
 *      即得整页；
 *   3. 乱序/缺帧：seq≠下一期望序号 → 本页作废（丢弃已攒），等下一个 seq==0
 *      自然重置——设备侧「翻页重发」提供收敛；
 *   4. total==1 = 整页一帧（页文本 ≤233B 的常见情形）。
 *
 * 防御：单页累积超 PAGE_CAP（设备侧一页 ≤2048B 抽取，cap 取双倍余量）即作废
 * 本页——错帧/恶意大 total 不得吃内存。收齐后返回的文本可以是 ""（空页）：
 * 设备侧本不推空页，但重组器如实交付，由调用方决定静默。
 *
 * English: the gen26.5 M2 Step10 TTS page-text reassembler (pure logic, zero
 * android dependencies, host-first: test/TtsReassemblerTest.java). It folds
 * the device's multi-frame STATUS 0x19 page text back into whole pages per the
 * frozen contract (plan step10-recon.md section 2):
 *
 *   1. seq==0 starts a new page: clear the accumulation buffer (whether or not
 *      the previous page ever completed - the device only sends seq==0 when a
 *      new page settles, never re-0 inside a page);
 *   2. append each frame's text in seq order; once all total frames (seq
 *      0..total-1, one each) are in, the page is complete;
 *   3. out-of-order/missing: a seq that is not the expected next one drops the
 *      whole page (accumulated text included) and waits for the next seq==0
 *      to reset - the device's "resend on turn" provides convergence;
 *   4. total==1 means the whole page in one frame (the common case, <=233B).
 *
 * Defence: accumulating past PAGE_CAP (a device page extracts to <=2048B; the
 * cap doubles that) drops the page - a corrupt frame or a hostile large total
 * must never eat memory. A completed page may be "" (empty): the device never
 * pushes empty pages, but the reassembler delivers honestly and the caller
 * decides the silence.
 */

package dev.readpico.push;

import java.nio.charset.StandardCharsets;

public final class TtsReassembler {

    /** 单页累积上限（**字节**）。设备侧一页抽取 ≤2048B；双倍余量只防错帧。 */
    public static final int PAGE_CAP = 4096;

    private final StringBuilder page = new StringBuilder(256);
    /** 已累积字节数（UTF-8）——cap 按字节算，与设备侧字节世界同口径。 */
    private int pageBytes = 0;
    private int expectedSeq = -1; // 下一帧应有 seq；-1 = 空闲（等 seq==0）
    private int total = 0;        // 本页总帧数；0 = 未知

    /**
     * 喂入一帧 0x19（原始 notify 字节）。
     *
     * @return null = 未收齐或本页已作废；非 null = 整页文本（"" = 空页）。
     *         Returning null means "not complete (or dropped)"; a non-null
     *         result is the whole page's text ("" for an empty page).
     */
    public String accept(byte[] frame) {
        final String text = PushProto.ttsText(frame);
        final int seq = PushProto.ttsTextSeq(frame);
        final int totalFrames = PushProto.ttsTextTotal(frame);
        if (text == null || seq < 0 || totalFrames < 1) {
            return null; // 解码层已拒（长度/序号/文本越界）：静默丢弃
        }
        if (seq == 0) {
            // 新页开始：无条件清空（迟到的上一页帧、半页残留都不留）。
            // A new page starts: unconditional clear (no late frame from the
            // previous page and no half-page residue survives).
            drop();
            total = totalFrames;
            expectedSeq = 0;
        } else if (expectedSeq < 0 || seq != expectedSeq) {
            // 缺帧/乱序/迟到旧页帧：整页作废，等下一个 seq==0 重置。
            // Missing/out-of-order/late frame from an older page: drop the
            // whole page and wait for the next seq==0 to reset.
            drop();
            return null;
        }
        final int textBytes = text.getBytes(StandardCharsets.UTF_8).length;
        if (pageBytes + textBytes > PAGE_CAP) {
            drop(); // cap 防御：错帧大 total 不吃内存 / cap defence
            return null;
        }
        page.append(text);
        pageBytes += textBytes;
        ++expectedSeq;
        if (expectedSeq >= total) {
            final String out = page.toString();
            drop();
            return out;
        }
        return null;
    }

    /** 丢弃半页状态（停读/断开时调；下一个 seq==0 本就重置，这是显式兜底）。 */
    public void reset() {
        drop();
    }

    /** 当前是否正在攒某一页（UI 状态行可用：半页在途 = true）。 */
    public boolean pending() {
        return expectedSeq >= 0;
    }

    private void drop() {
        page.setLength(0);
        pageBytes = 0;
        expectedSeq = -1;
        total = 0;
    }

    /* ---- 自测辅助：UTF-8 往返（宿主测试构造中文/emoji 帧用）---- */

    /** 把字符串按设备侧同款预算切帧（宿主测试用；产品路径只 accept）。 */
    public static byte[] encodeFrame(String text, int seq, int total) {
        byte[] f = PushProto.encodeTtsText(text, seq, total);
        if (f == null) {
            throw new IllegalArgumentException("bad seq/total");
        }
        return f;
    }

    /** 便捷：按 233B 预算切一页并逐帧 accept（宿主测试用）。 */
    public static String roundTrip(String pageText) {
        byte[] raw = pageText.getBytes(StandardCharsets.UTF_8);
        if (raw.length == 0) {
            return new TtsReassembler().accept(encodeFrame("", 0, 1)); // 空页一帧
        }
        // 帧数按设备侧 push_tts_frame_count 同款模拟步进数出来——UTF-8 边界
        // 回退让每帧装不满 233B，ceil(len/233) 会少算帧、尾部字节永远发不
        // 出去（第一版助手就栽在这：17×231=3927 < 3960）。助手必须与设备
        // 同算法，否则测的是另一个协议。
        // Frame count is simulated with the device's own stepping: UTF-8
        // backoff under-fills each frame, so ceil(len/budget) undercounts and
        // the tail would never be sent (the first helper revision hit exactly
        // that: 17x231 = 3927 < 3960). The helper must share the device's
        // algorithm or it tests a different protocol.
        int frames = 0;
        for (int off = 0; off < raw.length; off += stepAt(raw, off)) {
            ++frames;
        }
        TtsReassembler r = new TtsReassembler();
        String out = null;
        int off = 0;
        for (int i = 0; i < frames; ++i) {
            final int take = stepAt(raw, off);
            out = r.accept(encodeFrame(
                    new String(raw, off, take, StandardCharsets.UTF_8), i, frames));
            off += take;
        }
        return out;
    }

    /**
     * 与设备侧 push_tts_utf8_advance 同规的一步：预算内走到 UTF-8 字符边界
     * （切点下一字节是续字节 10xxxxxx 就回退）。恒 ≥1 保进度。
     * One device-identical step: advance to a UTF-8 character boundary within
     * the budget (back off when the byte after the cut is a continuation).
     * Always >= 1 byte of progress.
     */
    private static int stepAt(byte[] raw, int off) {
        int take = Math.min(PushProto.TTS_TEXT_MAX - 1, raw.length - off);
        while (take > 0 && off + take < raw.length
                && (raw[off + take] & 0xC0) == 0x80) {
            --take;
        }
        return take > 0 ? take : 1;
    }
}
