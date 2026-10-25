/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TtsReassembler 的宿主机测试（纯逻辑，零 android 依赖）。逐条钉死
 * plan step10-recon.md §二 的冻结约定：seq==0 重置 / 顺序拼接 / total 收齐 /
 * 缺帧整页丢弃 / total==1 单帧整页，外加 cap 防御与非法帧拒收。中文与 emoji
 * 走 UTF-8 整颗切分（与设备侧 push_tts 同规），拼回必须逐字节等于原文。
 *
 * English: host test for TtsReassembler (pure logic, zero android deps). It
 * pins the frozen contract (plan step10-recon.md section 2) item by item:
 * seq==0 reset / ordered append / total completion / whole-page drop on a
 * missing frame / total==1 single-frame pages, plus the cap defence and
 * invalid-frame rejection. Chinese and emoji ride whole-character UTF-8 cuts
 * (same rule as the device's push_tts) and must reassemble byte-identical.
 */

package dev.readpico.push;

public final class TtsReassemblerTest {
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

    public static void main(String[] args) {
        System.out.println("TtsReassemblerTest（纯逻辑，零 android 依赖）:");

        // 0) 对表 PushProto 编码器：0x19 帧定长 241、零填充、头字段就位。
        byte[] f = TtsReassembler.encodeFrame("hi", 0, 1);
        check(f.length == PushProto.TTS_TEXT_FRAME_MAX,
                "0x19 帧定长 241（op1+seq2+total2+len2+text234）");
        check((f[0] & 0xFF) == PushProto.ST_TTS_TEXT, "op = 0x19");
        check(PushProto.ttsTextSeq(f) == 0 && PushProto.ttsTextTotal(f) == 1,
                "seq/total 头字段就位");
        boolean zeroTail = true;
        for (int i = 7 + 2; i < f.length; ++i) {
            zeroTail &= f[i] == 0;
        }
        check(zeroTail, "槽余部零填充");

        // 1) total==1 = 整页一帧（页文本 ≤233B 的常见情形）。
        TtsReassembler r1 = new TtsReassembler();
        String shortPage = "从前有座山，山里有座庙。";
        check(shortPage.getBytes(java.nio.charset.StandardCharsets.UTF_8).length
                <= PushProto.TTS_TEXT_MAX - 1, "短页一帧装得下");
        String got1 = r1.accept(TtsReassembler.encodeFrame(shortPage, 0, 1));
        check(shortPage.equals(got1), "单帧整页原文返回");
        check(!r1.pending(), "收齐后回到空闲");

        // 2) 多帧顺序拼接：中文页（每帧恒 UTF-8 整颗），拼回逐字节等于原文。
        StringBuilder cn = new StringBuilder();
        for (int i = 0; i < 60; ++i) {
            cn.append("一轮孤月骤然翻过山坡，把谷底的雾气照成薄纱。");
        }
        String cnText = cn.toString();
        check(cnText.length() > PushProto.TTS_TEXT_MAX, "长页必然多帧");
        String got2 = TtsReassembler.roundTrip(cnText);
        check(cnText.equals(got2), "中文多帧拼回与原文逐字节一致");

        // 2b) 纯 ASCII 长页（无回退，每帧恰好 233B）：另一条边界路径。
        // 尺寸贴设备侧真实页（≤2048B 抽取上限），不造超过 PAGE_CAP 的幻页。
        StringBuilder en = new StringBuilder();
        for (int i = 0; i < 30; ++i) {
            en.append("The moon climbed over the ridge and turned the fog to gauze. ");
        }
        String enText = en.toString();
        check(enText.length() > 3 * PushProto.TTS_TEXT_MAX, "ASCII 长页必然多帧");
        check(enText.getBytes(java.nio.charset.StandardCharsets.UTF_8).length
                        < 2048, "ASCII 测试页在设备侧页上限内");
        check(enText.equals(TtsReassembler.roundTrip(enText)),
                "ASCII 多帧拼回与原文逐字节一致（无回退路径，帧数模拟步进）");

        // 3) emoji（4 字节序列）同样整颗切分拼回。
        String emoji = "月色真美🌸🌸🌸，晚风也温柔🌙。";
        for (int i = 0; i < 40; ++i) {
            emoji += "思念像星星一样多✨，一颗接着一颗亮起来。";
        }
        String got3 = TtsReassembler.roundTrip(emoji);
        check(emoji.equals(got3), "emoji 多帧拼回与原文逐字节一致");

        // 4) seq==0 无条件重置：半页在途时新页 seq==0 到达，旧半页作废。
        TtsReassembler r4 = new TtsReassembler();
        check(r4.accept(TtsReassembler.encodeFrame("第一页前半", 0, 3)) == null,
                "多帧第一帧（seq0/3）未收齐");
        check(r4.pending(), "半页在途 pending=true");
        String got4 = r4.accept(TtsReassembler.encodeFrame("新页全文", 0, 1));
        check("新页全文".equals(got4),
                "半页在途时 seq==0 重置并立即收齐新页（旧半页不留）");
        check(!r4.pending(), "新页收齐后空闲");

        // 5) 缺帧：seq 0,2（跳过 1）→ 本页作废，返回 null。
        TtsReassembler r5 = new TtsReassembler();
        check(r5.accept(TtsReassembler.encodeFrame("甲", 0, 3)) == null, "缺帧页 seq0 入栈");
        check(r5.accept(TtsReassembler.encodeFrame("丙", 2, 3)) == null,
                "seq 跳号（0→2）：整页作废返回 null");
        check(!r5.pending(), "作废后回到空闲（等下一个 seq==0）");
        // 作废后设备重发（seq==0 起）能正常收齐——收敛条款。
        check("甲乙丙".equals(
                r5.accept(TtsReassembler.encodeFrame("甲乙丙", 0, 1))),
                "作废后重发可正常收齐（翻页重发收敛）");

        // 6) 乱序：seq 1 先到（没有 seq0 开局）→ 丢弃。
        TtsReassembler r6 = new TtsReassembler();
        check(r6.accept(TtsReassembler.encodeFrame("乙", 1, 2)) == null,
                "没有 seq==0 开局的帧丢弃");
        check(!r6.pending(), "丢弃后空闲");

        // 7) 迟到旧页帧两种防御语义（BLE 单连接 notify 保序，跨页乱序实际
        //    不可达；此处按冻结约定 §二.3 的序号哲学记录在案）。
        // 7a) 序号对得上：按序号吞入（拼上就是本页流）。
        TtsReassembler r7a = new TtsReassembler();
        check(r7a.accept(TtsReassembler.encodeFrame("新页第一块", 0, 2)) == null,
                "7a 新页第一帧（seq0/2）未收齐");
        check("新页第一块旧页残芯".equals(
                        r7a.accept(TtsReassembler.encodeFrame("旧页残芯", 1, 2))),
                "7a 序号对得上的迟到帧按序号吞入（不可达，防御语义在案）");
        // 7b) 序号对不上：整页作废等下一个 seq==0；设备重发收敛。
        TtsReassembler r7b = new TtsReassembler();
        check(r7b.accept(TtsReassembler.encodeFrame("新页第一块", 0, 2)) == null,
                "7b 新页第一帧（seq0/2）未收齐");
        check(r7b.accept(TtsReassembler.encodeFrame("旧页残留", 2, 4)) == null,
                "7b 迟到帧序号对不上（期望 seq1 来得 seq2）：整页作废");
        check(!r7b.pending(), "7b 作废后空闲，等下一个 seq==0");
        check("甲乙丙".equals(
                        r7b.accept(TtsReassembler.encodeFrame("甲乙丙", 0, 1))),
                "7b 设备重发（seq==0 起）正常收齐（翻页重发收敛）");

        // 8) 非法帧：长度错 / op 错 / seq 越界 → null，且不污染状态。
        TtsReassembler r8 = new TtsReassembler();
        check(r8.accept(new byte[100]) == null, "长度非 241 拒收");
        byte[] wrongOp = TtsReassembler.encodeFrame("x", 0, 1);
        wrongOp[0] = (byte) 0x18;
        check(r8.accept(wrongOp) == null, "op 非 0x19 拒收");
        // 编码器本就拒绝产 seq≥total 的帧（返回 null），这里手改帧头字节造非法
        // 帧验解码层校验：seq=3（LE）≥ total=2 → ttsTextLen 拒。
        // The encoder refuses to produce seq>=total at all, so hand-patch the
        // header to forge the illegal frame and prove the decoder rejects it.
        byte[] badSeq = TtsReassembler.encodeFrame("x", 0, 2);
        badSeq[1] = 3;
        badSeq[2] = 0;
        check(r8.accept(badSeq) == null, "seq≥total 拒收（解码层校验）");
        check(r8.accept(null) == null, "null 帧拒收");
        check(!r8.pending(), "非法帧不污染状态");

        // 9) 空页：total==1、len==0 → 收齐为 ""（设备侧不推，重组器如实交付）。
        TtsReassembler r9 = new TtsReassembler();
        String got9 = r9.accept(TtsReassembler.encodeFrame("", 0, 1));
        check(got9 != null && got9.isEmpty(), "空页收齐为 \"\"（调用方决定静默）");

        // 10) cap 防御：30 帧 × 233B = 6990B > PAGE_CAP → 中途作废不吃内存。
        TtsReassembler r10 = new TtsReassembler();
        StringBuilder chunk = new StringBuilder();
        for (int i = 0; i < 73; ++i) {
            chunk.append("啊"); // 3×73 = 219 字节
        }
        chunk.append("abcd"); // +4 = 223，加中文标点补到 233 内
        String frameText = chunk.toString();
        boolean dropped = false;
        String out10 = null;
        for (int i = 0; i < 30; ++i) {
            out10 = r10.accept(TtsReassembler.encodeFrame(frameText, i, 30));
            if (out10 == null && !r10.pending()) {
                dropped = true; // 被 cap 作废并回到空闲
                break;
            }
        }
        check(dropped, "超 cap 的巨页在中途作废（错帧/恶意大 total 不吃内存）");
        check(TtsReassembler.PAGE_CAP > 9 * (PushProto.TTS_TEXT_MAX - 1),
                "合法页永远够用：设备侧一页 ≤2048B → ≤9 帧 < cap");

        // 11) reset() 显式丢弃半页（停读/断开路径）。
        TtsReassembler r11 = new TtsReassembler();
        r11.accept(TtsReassembler.encodeFrame("半页", 0, 4));
        check(r11.pending(), "reset 前半页在途");
        r11.reset();
        check(!r11.pending(), "reset 后半页丢弃");

        // 12) 收齐后再来 seq==0：永远开新页（冻结约定 §二.1）——设备不会重发
        //     seq0，此处记录幻影半页在途态与它的自愈（下页 seq==0 自然重置）。
        //     A repeated seq==0 after completion always starts a new page: the
        //     device never re-0s a page, so this records the phantom half-page
        //     pending state and its self-heal (the next page's seq==0 resets).
        TtsReassembler r12 = new TtsReassembler();
        check("整页".equals(r12.accept(TtsReassembler.encodeFrame("整页", 0, 1))),
                "整页一帧收齐");
        check(r12.accept(TtsReassembler.encodeFrame("重复seq0", 0, 3)) == null,
                "收齐后再来 seq==0：开新页（未收齐返回 null）");
        check(r12.pending(), "幻影半页在途（设备不会重发 seq0；下页 seq==0 重置）");
        check("新页".equals(r12.accept(TtsReassembler.encodeFrame("新页", 0, 1))),
                "下一个 seq==0 重置幻影半页并收齐新页（自愈）");

        if (fail > 0) {
            System.out.println("TtsReassemblerTest FAILED: pass=" + pass + " fail=" + fail);
            System.exit(1);
        }
        System.out.println("TtsReassemblerTest passed: " + pass + " checks");
    }
}
