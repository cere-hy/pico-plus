#!/bin/bash
# M3-B 情景视图宿主测试：纯逻辑（待命态 + 引用条取句），零桩，ASan/UBSan。
# Host test for the M3-B scene view: pure logic only (pending state + quote
# extraction), zero stubs, ASan/UBSan.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/book-tests
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
   -Imain/apps \
   -o build/book-tests/ai_scene_view tools/ai_scene_view_host_test.c main/apps/scene_req.c
./build/book-tests/ai_scene_view
