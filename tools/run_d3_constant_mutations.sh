#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# gen26.6 D3 常量族变异有牙验证：把释义槽缩回旧值，确认编译期静态断言擒住
# （runner 非 0 退出 = 擒住）。任何存活变异说明护栏有洞——补断言，不划掉变异。
# gen26.6 D3 constant-family mutation check: shrink the definition slots back to
# their old values and confirm the compile-time static assertions catch it
# (non-zero runner exit = caught). A surviving mutation means a hole in the
# guard - add the assertion, never drop the mutation.
#
# 覆盖：
#   设备协议槽  components/push_sink/include/push_proto.h  PUSH_AI_DEF_MAX
#               -> run_push_sink_host_tests.sh（push_proto.c 编译该头）
#   批内 def 槽 main/dict/vocab_log.c                      VOCAB_AI_DEF_MAX
#               -> run_dict_hit_host_tests.sh（vocab_log.c 直接编译）
#   APK 裁剪帽  AiClient.DEF_MAX_BYTES==600 由 AiClientTest 直接断言（每次
#               build-apk.sh 都会跑；本轮 169→600 的旧断言即因此转红），故不在此重复。
set -uo pipefail
cd "$(dirname "$0")/.."

cp components/push_sink/include/push_proto.h /tmp/d3_proto.h
cp main/dict/vocab_log.c /tmp/d3_vocab.c
restore_all() {
    cp /tmp/d3_proto.h components/push_sink/include/push_proto.h
    cp /tmp/d3_vocab.c main/dict/vocab_log.c
}
trap restore_all EXIT

survived=0
run_mutation() {
    local label="$1" runner="$2"
    if bash "$runner" >/tmp/d3_mut_out.txt 2>&1; then
        echo "MUTATION SURVIVED (BAD): $label"
        survived=$((survived + 1))
    else
        echo "caught: $label"
    fi
    restore_all
}

# ---- M1：协议槽缩回 170 ---- / the protocol slot shrinks back to 170
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/include/push_proto.h')
s = p.read_text()
old = '#define PUSH_AI_DEF_MAX  768u'
assert old in s, 'M1 anchor missing'
p.write_text(s.replace(old, '#define PUSH_AI_DEF_MAX  170u /* MUTATION M1 */', 1))
EOF
run_mutation "M1 PUSH_AI_DEF_MAX shrinks to 170" tools/run_push_sink_host_tests.sh

# ---- M2：批内 def 槽缩回 288（D3 之前的同源值）----
# ---- M2: the batch def slot shrinks back to 288 (the pre-D3 twin value)
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/dict/vocab_log.c')
s = p.read_text()
old = '#define VOCAB_AI_DEF_MAX 768'
assert old in s, 'M2 anchor missing'
p.write_text(s.replace(old, '#define VOCAB_AI_DEF_MAX 288 /* MUTATION M2 */', 1))
EOF
run_mutation "M2 VOCAB_AI_DEF_MAX shrinks to 288" tools/run_dict_hit_host_tests.sh

if [ "$survived" -ne 0 ]; then
    echo "D3 CONSTANT MUTATIONS: $survived SURVIVED (BAD) - assertions have holes"
    exit 1
fi
echo "D3 CONSTANT MUTATIONS: all 2 caught (M1 protocol slot, M2 batch slot)"
