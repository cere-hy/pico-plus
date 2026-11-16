#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 26.11.4 切节函数宿主测试（在仓库根目录运行）。
# 纯函数、零依赖：不桩 FS、不桩时钟，输入的字节就是全部上下文。
# Host tests for the section cutter. Pure function, no stubs: the input bytes are
# the whole context.
set -euo pipefail
mkdir -p build/excerpt-tests
flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g
       -fsanitize=address,undefined -fno-omit-frame-pointer -Imain/book)
gcc "${flags[@]}" tools/excerpt_section_host_test.c main/book/excerpt_section.c \
    -o build/excerpt-tests/excerpt-section
build/excerpt-tests/excerpt-section
