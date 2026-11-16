#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# S26-3 0x1A bit2 黄金向量（手推字节 vs 固件编码器）。/ S26-3 golden vectors (ASan/UBSan).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/excerpt-tests
flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer
       -Icomponents/push_sink/include -Icomponents/push_sink/src)
gcc "${flags[@]}" tools/excerpt_ai_golden_test.c components/push_sink/src/push_proto.c \
    -o build/excerpt-tests/excerpt_ai_golden
build/excerpt-tests/excerpt_ai_golden
