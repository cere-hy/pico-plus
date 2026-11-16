#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 26.11.4「节标记契约 / 保存分派」两颗牙（B75 / B76）的先红验证。
# First-red sweep for the two teeth added in 26.11.4 (section contract, save dispatch).
#
# 为什么单独一个脚本：靶点横跨设备 C 头、设备 C 源、APK Java 源三处，而这两颗牙
# 咬的正是「跨端字符串」与「同位置的两套语义」。塞进别的变异脚本会把备份清单撑破，
# 也让「哪一侧被改脏」读不出来。
# Why its own script: the targets span a C header, a C source and a Java source, and the
# teeth bite a cross-end string and one slot serving two meanings.
#
# 判定：**每刀都必须被 check_excerpt.py 单独擒住**。存活 = 牙有洞；锚点漂移（不存在
# 或不唯一）= 直接判失败——「变异没植进去、检查照过」是最危险的假绿。
# Every knife must be caught; a drifted anchor is an outright failure.
#
# 串行纪律：与其他变异脚本共用工作区，不得并发。
set -uo pipefail
cd "$(dirname "$0")/.."

FILES=(
    main/book/excerpt_section.h
    main/book/excerpt_section.c
    main/apps/app_book.c
    tools/android/apk/src/dev/readpico/push/AiClient.java
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

# ---- S1：APK 模板把标记改一个字（设备静默断链）------------------------------
mutate "S1" tools/android/apk/src/dev/readpico/push/AiClient.java \
    '+ "【原文对照】\n"' '+ "【原文對照】\n"' || true
[ "$FAILED" -eq 0 ] && run_mutation "S1" "APK renames the marker: every English passage silently unsavable"

# ---- S2：节序被改回去（对照又跑到第三屏，功能不坏、没人会发现）--------------
mutate "S2" tools/android/apk/src/dev/readpico/push/AiClient.java \
    'return "你是阅读辅助工具。用户给的是一段选文' 'return "【段意】\n你是阅读辅助工具。用户给的是一段选文' || true
[ "$FAILED" -eq 0 ] && run_mutation "S2" "section order reverted: the comparison drops behind 段意"

# ---- S3：模板里提到两次标记（设备取第一个，取到的是要求不是正文）------------
mutate "S3" tools/android/apk/src/dev/readpico/push/AiClient.java \
    '+ "禁止：一句话概括、只列关键词、把原文抄一遍当段意。";' \
    '+ "禁止：一句话概括、只列关键词、把原文抄一遍当段意。不要漏掉【原文对照】。";' || true
[ "$FAILED" -eq 0 ] && run_mutation "S3" "marker mentioned twice: the device would cut at the wrong line"

# ---- S4：设备侧标记常量改字（协议常量被当普通字符串）------------------------
mutate "S4" main/book/excerpt_section.h \
    '#define EXCERPT_SECTION_MARKER "【原文对照】"' \
    '#define EXCERPT_SECTION_MARKER "【原文对比】"' || true
[ "$FAILED" -eq 0 ] && run_mutation "S4" "device marker renamed: a protocol constant treated as a string"

# ---- S5：分派退化成一个入口（死键复活）--------------------------------------
mutate "S5" main/apps/app_book.c \
    'return ai_reader_is_excerpt() ? scene_save_passage() : scene_save_word();' \
    'return scene_save_word();' || true
[ "$FAILED" -eq 0 ] && run_mutation "S5" "dispatcher collapsed: the passage saves a word again (dead key)"

# ---- S6：段落路径不再切节（整篇进摘抄，瞬间触顶）----------------------------
mutate "S6" main/apps/app_book.c \
    'excerpt_section_pick(def, strlen(def), s_passage_line, sizeof(s_passage_line), &n, &cut);' \
    'excerpt_section_pick_unused(def, strlen(def), s_passage_line, sizeof(s_passage_line), &n, &cut);' || true
[ "$FAILED" -eq 0 ] && run_mutation "S6" "passage path stops cutting: the whole answer would be saved"

# ---- S7：诚实失败被删（无对照时一声不响）------------------------------------
mutate "S7" main/apps/app_book.c \
    'reader_notice_set("无对照可存", 2000);
            ESP_LOGW(TAG, "scene save: no %s in an essay-length answer' \
    'ESP_LOGW(TAG, "scene save: no %s in an essay-length answer' || true
[ "$FAILED" -eq 0 ] && run_mutation "S7" "silent failure: a missing section reports nothing"

# ---- S8：切节引入位置判据（下次 APK 改序就得重铸固件）------------------------
mutate "S8" main/book/excerpt_section.c \
    '    const char* marker = NULL;' \
    '    const char* marker = NULL;
    const int section_index = 0;
    (void)section_index;' || true
[ "$FAILED" -eq 0 ] && run_mutation "S8" "position coupling appears: the cut starts counting sections"

echo
echo "section mutations: ${CAUGHT}/8 caught, FAILED=${FAILED}"
[ "$FAILED" -eq 0 ] && [ "$CAUGHT" -eq 8 ]
