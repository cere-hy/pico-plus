#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 手机 BLE 推流纯核回归：帧编解码 + 会话状态机。/ Host regression for the phone BLE push pure core: frame codec + session machine.
set -euo pipefail
cd "$(dirname "$0")/.."
# 纯核不依赖 ESP-IDF：-std=c11 严格模式不暴露 strnlen，宿主显式开 POSIX 层级。
# The pure core has no ESP-IDF dependency: -std=c11 hides strnlen, so the host opts into POSIX.
# gen26.6.1 D-1：先由 Java 侧现产「APK 真实线格式帧」fixture，再编译 C 宿主——
# 协议往返必须用对端的真实帧，不能用本端缓冲大小自洽（那正是 D-1 的宿主盲区）。
# gen26.6.1 D-1: generate the APK's real wire-frame fixture from the Java side
# first, then build the C host - a protocol round-trip must use the peer's real
# frame, never this side's buffer size (that was D-1's blind spot).
bash tools/gen_apk_frame_fixture.sh
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Icomponents/push_sink/include -Ibuild/book-tests \
    tools/push_sink_host_test.c \
    components/push_sink/src/push_proto.c \
    components/push_sink/src/push_session.c \
    components/push_sink/src/ai_ridealong.c \
    components/push_sink/src/ai_segasm.c \
    components/push_sink/src/push_tts.c \
    -o /tmp/push_sink_test
/tmp/push_sink_test
