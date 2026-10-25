#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 生成「APK 真实线格式帧」fixture（gen26.6.1 D-1 收口第 2 条）。
# Generate the "APK real wire-format frame" fixture (gen26.6.1 D-1, item 2).
#
# C 侧的 0x07 往返过去用 `PUSH_AI_RESULT_MAX` 当缓冲，编/解在同一个「对端从不
# 产出的表示」上自洽（D3 后变 839B），真机 241B 冻结帧被拒的缺陷因此在宿主里
# 永远绿。故 fixture 必须由 Java 侧真实编码器现产，手写字节作废。
# The C-side 0x07 round-trip used `PUSH_AI_RESULT_MAX` as its buffer, so
# encode/decode agreed on a representation the peer never emits (839B after D3) and
# the real defect - every frozen 241B frame rejected - stayed green forever. The
# fixture must therefore come from the Java encoder; hand-written bytes are void.
#
# 在仓库根目录运行 / run from the repository root.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=build/book-tests/apk_ai_result_frame.h
mkdir -p build/book-tests
WORK=/tmp/apk_frame_fixture
rm -rf "$WORK"
mkdir -p "$WORK"

javac --release 8 -encoding UTF-8 -nowarn -d "$WORK" \
    tools/android/apk/src/dev/readpico/push/PushProto.java \
    tools/apk_frame_fixture/EmitAiResultFrame.java
java -cp "$WORK" dev.readpico.push.EmitAiResultFrame > "$OUT"

# 保底：fixture 必须带生成标记且非空——Java 不可用/产出为空时这里立刻红，
# 而不是让 C 测试悄悄退化成「没有 fixture 也过」。
# Backstop: the fixture must carry the generation marker and be non-empty, so an
# unavailable Java or an empty dump goes red right here instead of letting the C
# test silently degrade into "passes without the fixture".
grep -q 'GENERATED - DO NOT EDIT' "$OUT"
[ -s "$OUT" ]
printf 'apk frame fixture: %s (%s bytes)\n' "$OUT" "$(stat -c '%s' "$OUT")"
