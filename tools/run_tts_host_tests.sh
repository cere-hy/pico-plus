#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# TTS 听书（gen26.5 M2 Step10）宿主回归：页文本抽取 + NVS 开关 + 0x19 帧切分计划。
# 帧计划器挂在 push_sink 纯核测试里（同一次编译），本脚本跑 tts_page 独册。
# Host regression for TTS read-aloud (gen26.5 M2 Step10): page-text extraction,
# the NVS switch, and the 0x19 frame plan. The planner rides in the push_sink
# pure-core test (same compile); this script runs the tts_page booklet alone.
# 在仓库根目录运行；产物仅在 build。/ Run from the repository root; artifacts stay in build.
set -euo pipefail
mkdir -p build/book-tests
flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer)
gcc "${flags[@]}" -Imain/tts -Itools/tts_page_stubs -Itools/dict_hit_stubs \
    tools/tts_page_host_test.c -o build/book-tests/tts_page
build/book-tests/tts_page
