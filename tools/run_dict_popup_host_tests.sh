#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# gen26.3 词卡查询态 + AI 释义容量宿主回环（第 15 runner）：编译 main/apps/dict_popup.c
# 本体（真头文件契约），ui_kit/esp_* 走 tools/dict_popup_stubs 替身。
# gen26.3 querying-card + AI capacity host round trip (runner 15): compiles the real
# main/apps/dict_popup.c against tools/dict_popup_stubs doubles for ui_kit/esp_*.
# 在仓库根目录运行；产物仅在 build。/ Run from the repository root; artifacts stay in build.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/book-tests
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
   -Itools/dict_popup_stubs -Imain/apps -Imain/dict \
   tools/dict_popup_host_test.c main/apps/dict_popup.c -o build/book-tests/dict_popup
build/book-tests/dict_popup
