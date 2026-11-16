#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 26.11.3-C2「门梯 / 提示词预算 / 产出比」三颗牙的先红验证。
# First-red sweep for the three teeth added in 26.11.3-C2 (gate ladder, prompt budget,
# output-ratio receipt).
#
# 为什么单独一个脚本：这三颗牙咬的是**跨端常量与提示词**，靶点分布在 C 头、C 源和
# Java 源三处；塞进摘抄变异脚本（只备份 excerpt_store.*）会把备份机制撑破，且
# 「哪个文件被改脏了」读不出来。所以按文件清单单独隔离。
# Why its own script: these teeth bite cross-end constants and prompts spread over C
# headers, C sources and Java sources. Folding them into the excerpt sweep (which only
# snapshots excerpt_store.*) would break its backup contract and blur the blast radius.
#
# 判定：**每刀都必须被 check_excerpt.py 单独擒住**。存活 = 牙有洞；靶点漂移（锚点
# 不存在或不唯一）= 直接判失败，因为「变异没植进去、检查照过」是最危险的假绿。
# Every knife must be caught by check_excerpt.py alone. A survivor is a hole; an anchor
# that drifted is an outright failure, because "the mutation never landed and the check
# still passed" is the most dangerous green there is.
#
# 串行纪律：与其他变异脚本共用工作区，不得并发。
set -uo pipefail
cd "$(dirname "$0")/.."

FILES=(
    components/push_sink/include/push_proto.h
    components/push_sink/include/ai_segasm.h
    components/push_sink/src/push_sink.c
    main/app/app_loop.c
    main/apps/ai_reader.c
    main/apps/dict_popup.c
    main/apps/app_book.c
    tools/android/apk/src/dev/readpico/push/AiClient.java
    tools/android/apk/src/dev/readpico/push/MainActivity.java
)

BACKUPS=()
snapshot() {
    BACKUPS=()
    for f in "${FILES[@]}"; do
        b=$(mktemp); cp "$f" "$b"; BACKUPS+=("$b")
    done
}
restore() {
    local i=0
    for f in "${FILES[@]}"; do cp "${BACKUPS[$i]}" "$f"; rm -f "${BACKUPS[$i]}"; i=$((i + 1)); done
}
trap restore EXIT
snapshot

FAILED=0
CAUGHT=0

mutate() {
    local name="$1" file="$2" old="$3" new="$4" rc=0
    OLD="$old" NEW="$new" FILE="$file" python3 - <<'EOF' || rc=$?
import os, pathlib
p = pathlib.Path(os.environ['FILE'])
s = p.read_text(encoding='utf-8')
old, new = os.environ['OLD'], os.environ['NEW']
n = s.count(old)
if n == 0:
    raise SystemExit(3)
if n > 1:
    raise SystemExit(4)
p.write_text(s.replace(old, new, 1), encoding='utf-8')
EOF
    if [ "$rc" -eq 3 ]; then echo "TARGET MISSING: $name"; FAILED=1; return 1
    elif [ "$rc" -eq 4 ]; then echo "TARGET AMBIGUOUS: $name"; FAILED=1; return 1
    elif [ "$rc" -ne 0 ]; then echo "MUTATE ERROR: $name ($rc)"; FAILED=1; return 1
    fi
    return 0
}

run_mutation() {
    local name="$1" desc="$2"
    if python3 tools/check_excerpt.py >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"; FAILED=1
    else
        echo "caught: $name $desc"; CAUGHT=$((CAUGHT + 1))
    fi
    restore; snapshot
}

if ! python3 tools/check_excerpt.py >/dev/null 2>&1; then
    echo "BASELINE RED: check_excerpt.py fails before any mutation"
    exit 1
fi

# ---- L1：传输槽缩回 4096（8192 变成假门）------------------------------------
mutate "L1" components/push_sink/include/push_proto.h \
    '#define PUSH_AI_DEF_MAX  8192u' '#define PUSH_AI_DEF_MAX  4096u' || true
[ "$FAILED" -eq 0 ] && run_mutation "L1" "slot falls back to 4096: 8192 was a fake ceiling"

# ---- L2：重组器留在 4096（答案在句中无声截断）--------------------------------
mutate "L2" components/push_sink/include/ai_segasm.h \
    '#define AI_SEGASM_MAX 8192u' '#define AI_SEGASM_MAX 4096u' || true
[ "$FAILED" -eq 0 ] && run_mutation "L2" "reassembler stays at 4096: silent mid-sentence cut"

# ---- L3：APK 帽与设备槽不同数（一端砍另一端）--------------------------------
mutate "L3" tools/android/apk/src/dev/readpico/push/AiClient.java \
    'public static final int EXCERPT_DEF_MAX_BYTES = 8192;' \
    'public static final int EXCERPT_DEF_MAX_BYTES = 6144;' || true
[ "$FAILED" -eq 0 ] && run_mutation "L3" "cross-end mismatch: APK 6144 vs device 8192"

# ---- L4：会话缓冲忘了搬 PSRAM（真机上内部 RAM 当场爆）-----------------------
mutate "L4" main/apps/ai_reader.c \
    'static EXT_RAM_BSS_ATTR ai_reader_session_t s_session;' \
    'static ai_reader_session_t s_session;' || true
[ "$FAILED" -eq 0 ] && run_mutation "L4" "session back in internal RAM: blows the 112 B left"

# ---- L5：段落提示词装回字数帽（本批要治的病）--------------------------------
mutate "L5" tools/android/apk/src/dev/readpico/push/AiClient.java \
    '                + "篇幅：不设总字数上限。内容多就写长，读者翻页不是问题。\n"' \
    '                + "篇幅：总字数不超过 340 字，每节不超过 3 行。\n"' || true
[ "$FAILED" -eq 0 ] && run_mutation "L5" "the 340-char cap returns: the exact 26.11.3-C disease"

# ---- L6：删掉「不设总字数上限」而不装回帽（要求消失，形状与删帽相同）---------
mutate "L6" tools/android/apk/src/dev/readpico/push/AiClient.java \
    '                + "篇幅：不设总字数上限。内容多就写长，读者翻页不是问题。\n"' \
    '                + "篇幅：内容多就写长，读者翻页不是问题。\n"' || true
[ "$FAILED" -eq 0 ] && run_mutation "L6" "the no-limit clause vanishes: a missing cap reads as forgotten"

# ---- L7：设备等待窗长过 APK 超时（僵尸请求永久停泊）-------------------------
mutate "L7" main/apps/app_book.c \
    '#define EXCERPT_REQ_TIMEOUT_MS 90000' '#define EXCERPT_REQ_TIMEOUT_MS 200000' || true
[ "$FAILED" -eq 0 ] && run_mutation "L7" "device wait (200 s) outlives the APK timeout (120 s)"

# ---- L8：日志不再报 truncated（砍没砍看不出来）-----------------------------
mutate "L8" tools/android/apk/src/dev/readpico/push/MainActivity.java \
    '+ "（truncated=" + (r.truncated ? 1 : 0) + "）");' \
    '+ "）");' || true
[ "$FAILED" -eq 0 ] && run_mutation "L8" "truncated= dropped: a cut answer looks like a short one"

echo "-- caught: $CAUGHT / 8 --"
if [ "$FAILED" -ne 0 ]; then
    echo "ladder mutations: HOLE FOUND (see SURVIVED / TARGET lines above)"
    exit 1
fi
echo "ladder mutations: 8/8 killed"
