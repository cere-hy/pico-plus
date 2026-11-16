#!/bin/bash
# M3 情景释义 0x1A 分帧宿主测试：编译 + 运行（ASan/UBSan）。
# Host test for the M3 scene 0x1A segments: build + run (ASan/UBSan).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/book-tests
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined \
   -Icomponents/push_sink/include -Icomponents/push_sink/src \
   -o build/book-tests/ai_scene tools/ai_scene_host_test.c
./build/book-tests/ai_scene
