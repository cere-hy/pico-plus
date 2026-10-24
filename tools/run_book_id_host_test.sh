#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 中文：书标识（跨端契约）的宿主回归：FNV-1a32 标准向量 + basename/扩展名/大小写/
# UTF-8 口径用例。book_id.c 是纯逻辑（无 ESP-IDF 依赖），故可直接 gcc 编译。
# English: Host regression for the book id (cross-end contract): standard FNV-1a32
# vectors plus the basename/extension/case/UTF-8 rule cases. book_id.c is pure logic
# (no ESP-IDF dependency), so it compiles with plain gcc.
set -euo pipefail
cd "$(dirname "$0")/.."
gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Imain/book \
    tools/book_id_host_test.c \
    main/book/book_id.c \
    -o /tmp/book_id_test
/tmp/book_id_test
