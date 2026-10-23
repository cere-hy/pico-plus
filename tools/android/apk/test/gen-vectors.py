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

    with open(OUT, "w", encoding="utf-8") as f:
        for k in sorted(v):
            f.write(f"{k}={v[k]}\n")
    print(f"wrote {OUT} ({len(v)} vectors)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
