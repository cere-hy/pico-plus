#!/bin/bash
# 详解页宿主测试：编译 + 运行（ASan/UBSan）。
# Host test for the detail reading view: build + run (ASan/UBSan).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/book-tests
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
   -Itools/app_loop_stubs -Imain -Imain/app -Imain/apps -Imain/ui \
   -Icomponents/push_sink/include -Icomponents/ble_page_turner/include \
   -o build/book-tests/ai_reader tools/ai_reader_host_test.c
./build/book-tests/ai_reader
