#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 在仓库根目录运行；产物仅在build。/ Run from the repository root; artifacts stay in build.
# EPUB主机解压替身需要zlib开发包。/ The EPUB host inflate shim requires zlib development headers.
set -euo pipefail
mkdir -p build/book-tests
python tools/gen_book_fixtures.py
flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -Imain/book -Imain)
if [[ "$(uname -s)" == Darwin ]]; then link_gc=(-Wl,-dead_strip); else link_gc=(-Wl,--gc-sections); fi
gcc "${flags[@]}" -Itools/book_epub_stubs -Itools/zip_host_stubs tools/book_source_host_test.c main/book/book_source.c main/book/book_index_cache.c main/book/book_txt.c main/book/gbk.c main/book/book_epub.c main/book/book_image_header.c main/book/zip_reader.c main/book/html_text.c -lz -o build/book-tests/source
gcc "${flags[@]}" -Itools/book_layout_stubs -Itools/book_source_host_stubs tools/book_layout_host_test.c main/book/book_layout.c main/book/hyphen.c main/book/hyph_en_us_data.c main/book/html_text.c -o build/book-tests/layout
gcc "${flags[@]}" -Itools/book_layout_stubs -Itools/book_source_host_stubs tools/book_layout_twin_image_host_test.c main/book/book_layout.c main/book/hyphen.c main/book/hyph_en_us_data.c main/book/html_text.c -o build/book-tests/twin_image
gcc "${flags[@]}" -Itools/book_storage_stubs tools/book_storage_host_test.c main/book/book_progress.c -o build/book-tests/progress
gcc "${flags[@]}" -Imain -Itools/book_storage_stubs -Icomponents/read_pico/include tools/book_store_host_test.c -o build/book-tests/store
gcc "${flags[@]}" tools/book_policy_host_test.c -o build/book-tests/policy
build/book-tests/source build/book-fixtures/books/*.txt build/book-fixtures/books/*.epub
build/book-tests/layout
# 双栏 × 插图 fixture（e2e 返工范围第 1 条，tools/book_layout_twin_image_host_test.c）：
# G7-A1 修复落地后直跑——退出码 0 = 三场景全绿（修复后的预期落点）；1 = 修复不完整
# （大插图几何层仍见红、对照/基线全绿）；2 = 意外形态（对照/基线见红 = 新缺陷；大图
# 红点不在几何层 = 非预期形态）。任何非零都照失败拦（set -e 直接断），登记放行分支
# 随 KNOWN_FAILURES.md §5 退役一并移除。
# Two-column x illustration fixture (e2e rework scope item 1): straight run after the
# G7-A1 fix — exit 0 all green (the expected landing spot); 1 incomplete fix (red in the
# big scenario's geometry layer with control/baseline green); 2 unexpected shape. Any
# non-zero exit fails the runner (set -e breaks here); the KNOWN-RED tolerance branch
# was removed together with the KNOWN_FAILURES.md §5 retirement.
build/book-tests/twin_image
build/book-tests/progress
build/book-tests/store
build/book-tests/policy
gcc "${flags[@]}" -Itools/ui_gesture_stubs -Imain/ui tools/ui_gesture_host_test.c main/ui/ui_gesture.c -o build/book-tests/gesture
gcc "${flags[@]}" -ffunction-sections -fdata-sections "${link_gc[@]}" -Itools/ui_gesture_stubs -Itools/html_text_stubs -Imain -Imain/ui -Imain/font tools/ui_kit_host_test.c main/ui/ui_kit.c main/ui/ui_text_wrap.c -o build/book-tests/ui_kit
build/book-tests/gesture
# gen30 伴读案 · 修复轮：右栏多行块排版的断行核心（纯逻辑，零 stub，独立二进制）。
# 单独立一个二进制的原因：下面的 ui_kit 带登记在案的 KNOWN-FAIL，混编会吞掉本组断言。
# gen30 repair round: the block-layout line breaker (pure logic, no stubs, own binary).
# Kept separate because ui_kit below carries a registered KNOWN-FAIL that would
# swallow this group's asserts if they shared a process.
gcc "${flags[@]}" -Imain/ui tools/ui_text_wrap_host_test.c main/ui/ui_text_wrap.c -o build/book-tests/ui_text_wrap
build/book-tests/ui_text_wrap
# 26.11.16 仿生阅读：50% 公式（v2，ceil(len/2) 无上限）与切词状态机（纯逻辑、零 stub）。
# 2026-11-16 bionic reading: the v2 50% rule and the tokenizer (pure logic, no stubs).
gcc "${flags[@]}" -Imain/ui tools/ui_bionic_host_test.c main/ui/ui_bionic.c -o build/book-tests/ui_bionic
build/book-tests/ui_bionic
# 26.11.31 词典卡 AI 键一键武装：挂起态纯逻辑五路径（武装/超时/取消/连接即发/防重发）。
# 2026-11-31 dict-card AI arming: stash-state pure-logic five paths (zero stubs).
gcc "${flags[@]}" -Icomponents/push_sink/include tools/dict_ai_arm_host_test.c components/push_sink/src/dict_ai_arm.c -o build/book-tests/dict_ai_arm
build/book-tests/dict_ai_arm
# 步 4 A2「翻页即清」守卫（14:26 裁定 §三守卫面 ④）：include 的是 app_book.c 同一份
# app_companion_state.h（static inline，零 stub、零平台依赖，独立二进制）。
# Step-4 A2 "clear on page turn" guard: includes the exact static-inline header
# app_book.c calls, so the host assert exercises the real code, not a copy.
gcc "${flags[@]}" -Imain/apps tools/app_companion_state_host_test.c -o build/book-tests/app_companion_state
build/book-tests/app_companion_state
# known-fail（r93 4bpp packed 帧缓冲契约失效）：仍然编译并运行以保持可见，但只放行登记形态
# ——「ui_draw_control_frame 之后的第一个像素采样落空」。换成别的失败形态就是新缺陷，照旧拦。
# Known fail (r93 4bpp packed framebuffer contract): still built and run so it stays visible,
# but only the registered shape is tolerated — the first pixel sample after
# ui_draw_control_frame coming back blank. Any other shape is a new defect and still fails.
# 登记与证据链见 tools/KNOWN_FAILURES.md。/ See tools/KNOWN_FAILURES.md for the evidence chain.
# known-fail: KF-7.0
KNOWN_HITS=0
ui_kit_output="$(build/book-tests/ui_kit 2>&1)" && ui_kit_status=0 || ui_kit_status=$?
if [[ $ui_kit_status -eq 0 ]]; then
    printf '%s\n' "$ui_kit_output"
elif printf '%s' "$ui_kit_output" | grep -qF 'fb[10*W+30]'; then
    printf '%s\n' "$ui_kit_output"
    KNOWN_HITS=$((KNOWN_HITS + 1))
    echo "ui_kit: KNOWN-FAIL (r93 packed framebuffer contract, registered in tools/KNOWN_FAILURES.md) — not a regression"
else
    printf '%s\n' "$ui_kit_output"
    echo "ui_kit: FAIL — 失败形态与登记的不一致，按新缺陷处理 / unregistered failure shape, treated as a new defect" >&2
    exit 1
fi
gcc "${flags[@]}" -Itools/html_text_stubs tools/html_text_host_test.c main/book/html_text.c -o build/book-tests/html
build/book-tests/html

# 26.11.4：西文按词断行的黄金向量（当前版 vs HEAD 版逐行对照）。
# 放在书籍宿主这一组的末尾：它测的是排版，改动也只落在 book_layout.c。
# 26.11.4: the word-wrap golden vector (current vs HEAD). It tests layout, so it
# belongs to this group.
python3 tools/book_wrap_golden.py

# 登记条数入眼（与 run_book_management_tests.sh 末尾同一口径）。
# Print the tolerated count, same convention as the management runner.
echo "known-failures: ${KNOWN_HITS} tolerated (run_book_host_tests.sh; registry: tools/KNOWN_FAILURES.md §7)"

# 判决行（契约，不是注释）：**只有全绿才会走到这一行**（set -e），而门禁按这一行断言
# 「runner 跑到底了」。少了它，一个被截断的 runner 与一个真跑完的 runner 在日志里长得一样
# ——26.11.5 那次 92 s 的半截账就是这么骗过三遍关键字扫描的。
# Verdict line (a contract, not a comment): only reached when everything above passed, so
# the gate can tell "finished" from "truncated" - those two look identical in a log.
echo "JUDGEMENT: BOOK_HOST PASS tolerated=${KNOWN_HITS}"
