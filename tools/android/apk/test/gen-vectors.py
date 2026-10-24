#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 中文：生成 PushProtoTest.java 用的期望向量。期望值全部由本仓库的参考实现
# push_pico.py 产生（帧布局与固件 push_proto.h 逐字段对表），Java 侧只做断言，
# 不手抄任何字节。修改协议字段时必须同步重跑本脚本。
#
# English: emit the expectation vectors consumed by PushProtoTest.java. Every
# value comes from push_pico.py (the reference implementation); the Java side
# only asserts, so no bytes are ever hand-transcribed.
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import push_pico as pp  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors.txt")


def utf8_chunks(text: str, size: int):
    """按 UTF-8 字节切段，不切断多字节序列。/ chunk by UTF-8 bytes, never mid-sequence."""
    raw = text.encode("utf-8")
    out, i = [], 0
    while i < len(raw):
        j = min(i + size, len(raw))
        while j > i and j < len(raw) and (raw[j] & 0xC0) == 0x80:  # 0x80..0xBF = 续字节
            j -= 1
        out.append(raw[i:j].decode("utf-8", "replace"))
        i = j
    return out


def main() -> int:
    v = {}

    # CRC32
    v["crc_sample"] = pp.crc32(b"ReadPico BLE push reference")
    v["crc_empty"] = pp.crc32(b"")

    # CTRL 公共头
    v["end"] = pp.encode_ctrl(pp.PUSH_OP_END).hex()
    v["abort"] = pp.encode_ctrl(pp.PUSH_OP_ABORT).hex()

    # DATA 帧
    v["data_abc"] = pp.encode_data(0x1234, b"abc").hex()
    v["data_empty"] = pp.encode_data(0, b"").hex()

    # BEGIN 帧（三种典型 + 长标题截断）
    body = "墨水屏测试".encode("utf-8")
    v["begin_md"] = pp.encode_begin(
        len(body), pp.crc32(body), 0xDEADBEEF, 0x1A2B, pp.PUSH_TYPE_MARKDOWN, "标题✓").hex()
    txt = "ReadPico BLE push reference".encode("utf-8")
    v["begin_text_sid1"] = pp.encode_begin(
        len(txt), pp.crc32(txt), 1, 0x1A2B, pp.PUSH_TYPE_TEXT, "来自手机").hex()
    v["begin_word"] = pp.encode_begin(
        0, 0, 1, 0x1A2B, pp.PUSH_TYPE_WORD, "serendipity").hex()
    long_title = ("🎉" * 40) + ("汉" * 30)
    lb = pp.encode_begin(3, pp.crc32(b"abc"), 1, 0x1A2B, pp.PUSH_TYPE_TEXT, long_title)
    v["begin_long"] = lb.hex()
    # 标题字段实占字节数（63 字节截断 + 尾随续字节剥离后的结果）
    v["long_title_len"] = len(lb[19:19 + 64].rstrip(b"\x00"))

    # 广播名 -> token
    v["token_ok"] = pp.token_from_adv_name("PicoPush-1A2B")
    v["token_bad"] = "" if pp.token_from_adv_name("Other-ABCD") is None else "NOT_NULL"

    # 多分片：1230 字节 / 6 包，seq 从 0 递增（客户端协议语义）
    big = ("推流测试数据块-" * 100).encode("utf-8")[:1230]
    v["big_len"] = len(big)
    v["big_crc"] = pp.crc32(big)
    v["big_first"] = pp.encode_data(0, big[0:241]).hex()
    v["big_last"] = pp.encode_data(5, big[5 * 241:1230]).hex()
    v["chunk_247"] = min(pp.PUSH_DATA_PAYLOAD_MAX, 247 - 6)
    v["chunk_23"] = min(pp.PUSH_DATA_PAYLOAD_MAX, 23 - 6)

    # ---- gen26 AI 查词：CTRL 0x06/0x07 + STATUS 0x17/0x18 ----
    # 编码向量：Java 侧编出的帧必须与 Python 参考逐字节一致（零手抄）。
    # Encoding vectors: the Java codec must match the Python reference byte for byte.
    long_word = "a" * 70  # 70 字节 ASCII：触发 63 字节截断 / triggers the 63-byte cut
    v["ai_query_serendipity"] = pp.encode_ai_query("serendipity").hex()
    v["ai_query_empty"] = pp.encode_ai_query("").hex()
    v["ai_query_long"] = pp.encode_ai_query(long_word).hex()

    # 能力位图（设备上报 AI 查词能力）/ capability bitmap reported by the device
    caps = pp.encode_caps(pp.PUSH_CAP_AI_QUERY)
    v["caps_ai"] = caps.hex()
    v["caps_ai_bits"] = pp.decode_caps(caps)

    # STATUS 0x17：设备请手机算释义 / device asks the phone to define this word
    v["status_ai_serendipity"] = pp.encode_status_ai_query("serendipity").hex()
    v["status_ai_cjk"] = pp.encode_status_ai_query("你好").hex()
    v["status_ai_long"] = pp.encode_status_ai_query(long_word).hex()
    # 解码标量：Java 解上列 STATUS 帧应得到同样的词 / decode scalars the Java side must reproduce
    v["ai_word_serendipity"] = pp.decode_ai_query_status(pp.encode_status_ai_query("serendipity"))
    v["ai_word_cjk"] = pp.decode_ai_query_status(pp.encode_status_ai_query("你好"))
    v["ai_word_long"] = pp.decode_ai_query_status(pp.encode_status_ai_query(long_word))

    # CTRL 0x07 释义回传（241 B）/ definition reply
    v["ai_result_basic"] = pp.encode_ai_result("ephemeral", "adj. 短暂的").hex()
    v["ai_result_cjk"] = pp.encode_ai_result("ephemeral", "adj. 短暂的；朝生暮死的").hex()
    v["ai_result_long"] = pp.encode_ai_result("ephemeral", "x" * 200).hex()
    v["ai_result_empty_def"] = pp.encode_ai_result("ephemeral", "").hex()

    # ---- gen26.5 上下文 / 分帧 / 能力使能 ----
    # 0x06/0x17 带 book_id + pos；0x08 释义分帧；0x09 APK 显式使能。
    # Contextual query/status (book_id + pos), segmented definition, capability enable.
    BOOK_ID, POS = 42, 123456

    v["ai_ctx_query_len"] = 4 + 1 + pp.PUSH_AI_WORD_MAX + 8      # 0x06 ctx = 77
    v["ai_status_ctx_len"] = 1 + 1 + pp.PUSH_AI_WORD_MAX + 8     # 0x17 ctx = 74
    v["ai_seg_frame_len"] = 4 + 1 + pp.PUSH_AI_WORD_MAX + 6 + pp.PUSH_AI_SEG_MAX  # 0x08 = 241
    v["ai_seg_slot"] = pp.PUSH_AI_SEG_MAX                        # 释义槽 166（含 NUL）
    v["ai_caps_enable_len"] = 8                                  # 0x09 = 头4 + bits u32

    v["ai_ctx_query_serendipity"] = pp.encode_ai_query_ctx("serendipity", BOOK_ID, POS).hex()
    v["ai_ctx_query_cjk"] = pp.encode_ai_query_ctx("你好", 7, 0).hex()
    v["ai_ctx_query_long"] = pp.encode_ai_query_ctx(long_word, 1, 2).hex()
    # 解码标量（上下文档严格 77B）/ decode scalars the Java ctx decoder must reproduce
    w, b, p = pp.decode_ai_query_ctx(pp.encode_ai_query_ctx("serendipity", BOOK_ID, POS))
    v["ai_ctx_word_serendipity"], v["ai_ctx_book_serendipity"], v["ai_ctx_pos_serendipity"] = w, b, p

    v["status_ai_ctx_serendipity"] = pp.encode_status_ai_query_ctx("serendipity", BOOK_ID, POS).hex()
    v["status_ai_ctx_cjk"] = pp.encode_status_ai_query_ctx("你好", 7, 0).hex()
    v["status_ai_ctx_long"] = pp.encode_status_ai_query_ctx(long_word, 1, 2).hex()
    w, b, p = pp.decode_status_ai_query_ctx(pp.encode_status_ai_query_ctx("serendipity", BOOK_ID, POS))
    v["ai_ctx_status_word"], v["ai_ctx_status_book"], v["ai_ctx_status_pos"] = w, b, p
    # 新旧互连：旧解码器读 74B 上下文档仍得词（上下文静默丢失）
    v["ai_word_via_legacy_decoder"] = pp.decode_ai_query_status(
        pp.encode_status_ai_query_ctx("serendipity", BOOK_ID, POS))

    # 0x08 分帧：单帧 + 三帧首/中/末（UTF-8 边界不切断）
    v["ai_seg_single"] = pp.encode_ai_result_seg("ephemeral", "adj. 短暂的", 0, 1).hex()
    long_def = ("释义分段测试-" * 60)
    segs = utf8_chunks(long_def, pp.PUSH_AI_SEG_MAX - 1)
    v["ai_seg_count"] = len(segs)
    v["ai_seg_def_len"] = len(long_def.encode("utf-8"))
    v["ai_seg_first"] = pp.encode_ai_result_seg("serendipity", segs[0], 0, len(segs)).hex()
    v["ai_seg_last"] = pp.encode_ai_result_seg(
        "serendipity", segs[-1], len(segs) - 1, len(segs)).hex()
    mid = len(segs) // 2
    v["ai_seg_mid"] = pp.encode_ai_result_seg("serendipity", segs[mid], mid, len(segs)).hex()
    # 重组前站：三段（首/中/末）拼回原文必须与切分前逐字节相等
    assert "".join(segs) == long_def, "UTF-8 切段不可无损拼回"
    w, s, si, st = pp.decode_ai_result_seg(
        pp.encode_ai_result_seg("serendipity", segs[mid], mid, len(segs)))
    v["ai_seg_word_mid"], v["ai_seg_text_mid"] = w, s
    v["ai_seg_idx_mid"], v["ai_seg_total_mid"] = si, st

    # 0x09 使能帧 + 0x18 新能力位（设备侧上报）
    v["ai_caps_enable_ctx"] = pp.encode_ai_caps_enable(
        pp.PUSH_CAP_AI_QUERY | pp.PUSH_CAP_AI_CONTEXT).hex()
    v["ai_caps_enable_ctx_bits"] = pp.decode_ai_caps_enable(
        pp.encode_ai_caps_enable(pp.PUSH_CAP_AI_QUERY | pp.PUSH_CAP_AI_CONTEXT))
    v["ai_caps_enable_all"] = pp.encode_ai_caps_enable(
        pp.PUSH_CAP_AI_QUERY | pp.PUSH_CAP_AI_CONTEXT | pp.PUSH_CAP_AI_MULTI).hex()
    v["ai_caps_enable_all_bits"] = pp.decode_ai_caps_enable(
        pp.encode_ai_caps_enable(pp.PUSH_CAP_AI_QUERY | pp.PUSH_CAP_AI_CONTEXT | pp.PUSH_CAP_AI_MULTI))
    caps_new = pp.encode_caps(pp.PUSH_CAP_AI_QUERY | pp.PUSH_CAP_AI_CONTEXT | pp.PUSH_CAP_AI_MULTI)
    v["caps_ai_ctx_multi"] = caps_new.hex()
    v["caps_ai_ctx_multi_bits"] = pp.decode_caps(caps_new)
    # 旧 APK 不使能时的对照位图（仅 AI_QUERY）
    v["caps_ai_only_bits"] = pp.PUSH_CAP_AI_QUERY

    # ---- 书标识（跨端契约）：典型文件名 → FNV-1a32（设备/手机必须同值）----
    # Book id (cross-end contract): typical file names -> FNV-1a32 (must match on device).
    book_samples = {
        "book_id_hold": "/books/Hold On to Your Kids.epub",      # 带空格 / spaces
        "book_id_cjk": "/books/战争与和平.txt",                   # 中文书名 / CJK title
        "book_id_gatsby": "/books/The.Great.Gatsby.epub",        # 多点名 / many dots
        "book_id_targz": "/books/note.tar.gz",                   # 多扩展名 / double extension
        "book_id_readme": "/books/README",                       # 无扩展名 / no extension
        "book_id_upper": "/books/UPPER.EPUB",                    # 大写 / upper case
        "book_id_lower": "/books/upper.epub",                    # 小写（须与大写不同）
        "book_id_hidden": "/books/.hidden",                      # 隐藏文件 / hidden file
        "book_id_cjk_space": "/sd/books/中文 书名.epub",           # 中文 + 空格 / CJK + space
    }
    for k, p in book_samples.items():
        v[k] = pp.book_id_from_path(p)
    # 算法标准向量（三端同源自证）/ standard algorithm vectors (self-proof, shared)
    v["fnv_empty"] = pp.fnv1a32(b"")
    v["fnv_a"] = pp.fnv1a32(b"a")
    v["fnv_foobar"] = pp.fnv1a32(b"foobar")

    # ---- gen26.5 TTS 页文本（STATUS 0x19，241B 定长）----
    v["tts_frame_len"] = pp.PUSH_ST_TTS_TEXT_FRAME_MAX          # 241
    v["tts_slot"] = pp.PUSH_TTS_TEXT_MAX                        # 234
    v["tts_single"] = pp.encode_tts_text("It was a bright cold day in April.", 0, 1).hex()
    page = ("The clocks were striking thirteen. Winston Smith, his chin nuzzled "
            "into his breast, slipped quickly through the glass doors.")
    pt = [page[i:i + 60] for i in range(0, len(page), 60)]
    assert "".join(pt) == page, "TTS 页文本切段必须无损拼回"
    v["tts_count"] = len(pt)
    v["tts_first"] = pp.encode_tts_text(pt[0], 0, len(pt)).hex()
    v["tts_last"] = pp.encode_tts_text(pt[-1], len(pt) - 1, len(pt)).hex()
    mid_pt = len(pt) // 2
    v["tts_mid"] = pp.encode_tts_text(pt[mid_pt], mid_pt, len(pt)).hex()
    t, s, tot = pp.decode_tts_text(pp.encode_tts_text(pt[mid_pt], mid_pt, len(pt)))
    v["tts_text_mid"], v["tts_seq_mid"], v["tts_total_mid"] = t, s, tot

    # 有牙：定长帧的向量绝不能短一字节（帧帽纪律的最后一道闸）。
    # Teeth: fixed-length frame vectors must never come up one byte short.
    for key, want in (("ai_seg_first", v["ai_seg_frame_len"]),
                      ("ai_seg_last", v["ai_seg_frame_len"]),
                      ("ai_ctx_query_serendipity", v["ai_ctx_query_len"]),
                      ("status_ai_ctx_serendipity", v["ai_status_ctx_len"]),
                      ("ai_caps_enable_all", v["ai_caps_enable_len"]),
                      ("tts_first", v["tts_frame_len"]),
                      ("tts_last", v["tts_frame_len"]),
                      ("tts_single", v["tts_frame_len"])):
        assert len(v[key]) == want * 2, f"{key} 向量长度 {len(v[key]) // 2}B != {want}B"

    with open(OUT, "w", encoding="utf-8") as f:
        for k in sorted(v):
            f.write(f"{k}={v[k]}\n")
    print(f"wrote {OUT} ({len(v)} vectors)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
