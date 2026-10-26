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

# 门禁可信度 = 脚本自身可信度（gen26.6 元测试）：干净树起点 + 每条还原断言。
# Gate credibility = the script's own credibility (gen26.6 meta-test): a clean-tree
# start plus a restore assertion after every mutation.
MUT_FILES=(components/push_sink/include/push_proto.h components/push_sink/src/push_proto.c \
           main/dict/vocab_log.c components/push_sink/src/push_sink.c)
START_CLEAN=1
require_clean_tree() {
    if [ "${MUTATION_ALLOW_DIRTY:-0}" = "1" ]; then
        echo "WARN: MUTATION_ALLOW_DIRTY=1 - clean-tree check skipped (dev only)" >&2
        START_CLEAN=0
        return
    fi
    if ! git diff --quiet || ! git diff --cached --quiet; then
        echo "REFUSE: working tree is dirty before the sweep." >&2
        echo "  Commit or stash first (or set MUTATION_ALLOW_DIRTY=1 for a dev loop)." >&2
        exit 2
    fi
}
require_clean_tree

cp components/push_sink/include/push_proto.h /tmp/d3_proto.h
cp components/push_sink/src/push_proto.c /tmp/d3_proto_c.c
cp main/dict/vocab_log.c /tmp/d3_vocab.c
cp components/push_sink/src/push_sink.c /tmp/d3_sink.c
# 还原告终：先与**快照**逐字节比对（脏树起点也成立）；起点是干净树时再叠一条
# 「相对 HEAD 无差异」断言（更强：连备份本身被写坏都能抓）。
# Restored-verify: compare byte for byte with the SNAPSHOT (valid even from a dirty
# start); when the start was clean, also assert "no diff vs HEAD" (stronger: it
# catches a corrupted backup too).
RESTORED_PAIRS=("/tmp/d3_proto.h:components/push_sink/include/push_proto.h"
                "/tmp/d3_proto_c.c:components/push_sink/src/push_proto.c"
                "/tmp/d3_vocab.c:main/dict/vocab_log.c"
                "/tmp/d3_sink.c:components/push_sink/src/push_sink.c")
restore_all() {
    for pair in "${RESTORED_PAIRS[@]}"; do
        cp "${pair%%:*}" "${pair#*:}"
    done
    for pair in "${RESTORED_PAIRS[@]}"; do
        cmp -s "${pair%%:*}" "${pair#*:}" || {
            echo "FATAL: ${pair#*:} did not return to its pre-mutation snapshot" >&2
            exit 3
        }
    done
    if [ "$START_CLEAN" = "1" ]; then
        git diff --quiet -- "${MUT_FILES[@]}" || {
            echo "FATAL: a mutated file did not return to HEAD" >&2
            exit 3
        }
    fi
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
old = '#define PUSH_AI_DEF_MAX  1200u'
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
old = '#define VOCAB_AI_DEF_MAX 1200'
assert old in s, 'M2 anchor missing'
p.write_text(s.replace(old, '#define VOCAB_AI_DEF_MAX 288 /* MUTATION M2 */', 1))
EOF
run_mutation "M2 VOCAB_AI_DEF_MAX shrinks to 288" tools/run_dict_hit_host_tests.sh

# ---- M3：decode 守卫重新被容量拖拽（把最小长度锚回 PUSH_AI_DEF_MAX 派生的 839）
#      —— 真机 D-1 的原始形态：APK 恒发的 241B 冻结帧被拒，短释义全灭。
# ---- M3: the decode guard is dragged by the capacity constant again (minimum
#      length anchored to the PUSH_AI_DEF_MAX-derived 839) - the original shape of
#      the on-device D-1: every 241B frozen frame is rejected.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/src/push_proto.c')
s = p.read_text()
old = 'if (!in || !r || len < PUSH_AI_RESULT_WIRE_LEN) return false;'
assert old in s, 'M3 target not found'
new = ('if (!in || !r || len < (PUSH_CTRL_HDR_LEN + 1u + PUSH_AI_WORD_MAX + 2u '
       '+ PUSH_AI_DEF_MAX)) return false; /* MUTATION M3 */')
p.write_text(s.replace(old, new, 1))
EOF
run_mutation "M3 decode guard dragged back to the capacity-derived minimum" \
    tools/run_push_sink_host_tests.sh

# ---- M4：encode 重新产出随容量膨胀的帧长（839B，偏离冻结线格式）----
# ---- M4: encode emits the capacity-derived frame length again (839B).
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/src/push_proto.c')
s = p.read_text()
old = '    memcpy(out + off + 2u, r->def, dn);\n    return PUSH_AI_RESULT_WIRE_LEN;'
assert old in s, 'M4 target not found'
new = ('    memcpy(out + off + 2u, r->def, dn);\n'
       '    return PUSH_CTRL_HDR_LEN + 1u + PUSH_AI_WORD_MAX + 2u + PUSH_AI_DEF_MAX;'
       ' /* MUTATION M4 */')
p.write_text(s.replace(old, new, 1))
EOF
run_mutation "M4 encode emits the capacity-derived frame length" tools/run_push_sink_host_tests.sh

# ---- M5：冻结线格式槽被容量同化（170 -> 768）——静态断言应擒 ----
# ---- M5: the frozen wire slot is assimilated by the capacity (170 -> 768).
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/include/push_proto.h')
s = p.read_text()
old = '#define PUSH_AI_DEF_WIRE_MAX 170u'
assert old in s, 'M5 target not found'
p.write_text(s.replace(old, '#define PUSH_AI_DEF_WIRE_MAX 768u /* MUTATION M5 */', 1))
EOF
run_mutation "M5 frozen wire def slot follows the capacity constant" \
    tools/run_push_sink_host_tests.sh

# ---- M6：dn 上限守卫被容量放宽（170 -> 768）——170B 越界侧应被拒 ----
# ---- M6: the dn guard is relaxed to the capacity - the 170B side must be refused.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/src/push_proto.c')
s = p.read_text()
old = '    if (dn >= PUSH_AI_DEF_WIRE_MAX) return false;'
assert old in s, 'M6 target not found'
p.write_text(s.replace(old, '    if (dn >= PUSH_AI_DEF_MAX) return false; /* MUTATION M6 */', 1))
EOF
run_mutation "M6 dn guard relaxed to the capacity constant" tools/run_push_sink_host_tests.sh

# ---- M7：缓存查询被挪到 live 门之后 —— 离线缓存命中失效（gen26.6.2 修的那条）----
# ---- M7: the cache lookup moves after the live gate - offline cache hits die.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/src/push_sink.c')
s = p.read_text()
# 正确形态：cache 查在一个独立块里，排在 `live =` 赋值之前（与连接态解耦）。
# Correct shape: the cache lookup sits in its own block, BEFORE the `live =` assignment.
cache = ('    {\n'
         '        const char *cached = ai_cache_find(word);\n'
         '        if (cached) { ai_surface(word, cached); return; } // 命中预取缓存：秒出卡\n'
         '    }\n')
assert cache in s, 'M7 target not found'
live_open = '    if (live) {\n'
assert live_open in s, 'M7 live-open anchor not found'
# 真·回归：把 cache 查搬进 `if (live)` 内——离线 + 已缓存词不再出卡（命中被连接态吞掉）。
# Real regression: move the cache lookup INSIDE `if (live)` - an offline-but-cached word
# no longer surfaces (the hit is swallowed by the link gate).
indented = ('        {\n'
            '            const char *cached = ai_cache_find(word);\n'
            '            if (cached) { ai_surface(word, cached); return; } // 命中预取缓存：秒出卡\n'
            '        }\n')
s = s.replace(cache, '', 1)
s = s.replace(live_open, live_open + indented, 1)
p.write_text(s)
EOF
# 守门在 wiring 静态检查（push_sink.c 不进宿主编译），故跑 run_dict_hit_host_tests.sh
# （内含 vocab_wiring_host_test.py）。/ The guard is the static wiring check
# (push_sink.c is not host-compiled), so run run_dict_hit_host_tests.sh, which
# includes vocab_wiring_host_test.py.
run_mutation "M7 prefetch-cache lookup moved inside the live gate (offline hits die)" \
    tools/run_dict_hit_host_tests.sh

if [ "$survived" -ne 0 ]; then
    echo "D3 CONSTANT MUTATIONS: $survived SURVIVED (BAD) - assertions have holes"
    exit 1
fi
echo "D3 CONSTANT MUTATIONS: all 7 caught (M1 protocol slot, M2 batch slot, "
echo "  M3-M6 gen26.6.1 D-1 wire-format chain: guard/encode/wire slot/dn guard)"
