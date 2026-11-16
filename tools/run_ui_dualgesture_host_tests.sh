#!/usr/bin/env bash
# 中文：双指识别器主机测试（四态 + 三条硬语义 + 冻结映射）。
# English: Two-finger recognizer host tests (four states, three hard rules, frozen mapping).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/book-tests
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
   -Itools/ui_dualgesture_stubs -Imain/ui \
   -o build/book-tests/ui_dualgesture \
   tools/ui_dualgesture_host_test.c main/ui/ui_dualgesture.c
./build/book-tests/ui_dualgesture
