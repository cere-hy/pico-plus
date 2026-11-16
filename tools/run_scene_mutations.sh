#!/bin/bash
# M3 情景 0x1A 分帧变异测试：五条变异，每条跑一次宿主测试，期望被抓。
# Mutation tests for the M3 scene 0x1A segments: five mutations, each must fail
# the host test. The source is restored (byte-compared) after every case.
set -u
cd "$(dirname "$0")/.."

SRC=components/push_sink/src/push_proto.c
BAK=$(mktemp)
cp "$SRC" "$BAK"
trap 'cp "$BAK" "$SRC"; rm -f "$BAK"' EXIT

survived=0
killed=0

mutate() {
    local name="$1" old="$2" new="$3"
    cp "$BAK" "$SRC"
    python3 - "$SRC" "$old" "$new" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path, encoding='utf-8').read()
if old not in s:
    print("PATTERN NOT FOUND", file=sys.stderr)
    sys.exit(2)
open(path, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PY
    if [ $? -ne 0 ]; then
        echo "SKIP (pattern missing): $name"
        survived=$((survived + 1))
        return
    fi
    if ./tools/run_ai_scene_host_tests.sh >/dev/null 2>&1; then
        echo "SURVIVED: $name"
        survived=$((survived + 1))
    else
        echo "killed:   $name"
        killed=$((killed + 1))
    fi
    cp "$BAK" "$SRC"
}

# M1 首帧预算不减元数据：首帧塞满 234，encode 的 payload 校验必炸。
mutate "M1 first-frame budget ignores metadata" \
       "return PUSH_ST_SCENE_SLOT - (4u + scene_word_len(word));" \
       "return PUSH_ST_SCENE_SLOT;"

# M2 UTF-8 切分退化为按字节：中文用例的重组/边界断言必炸。
mutate "M2 utf8 slice degrades to raw bytes" \
       "        if (n + char_len > len) break;
        n += char_len;" \
       "        if (n + 1 > len) break;
        n += 1;"

# M3 词偏移恒写 0：首帧元数据断言必炸。
mutate "M3 word offset forced to zero" \
       "put_u16(p + 1 + wl, word_off);" \
       "put_u16(p + 1 + wl, 0);"

# M4 flags 丢弃：截断/回退标志位断言必炸。
mutate "M4 flags dropped from first frame" \
       "p[3 + wl] = flags;" \
       "p[3 + wl] = 0;"

# M5 帧数多算一帧：末帧空片，重组长度断言必炸。
mutate "M5 frame count overshoots by one" \
       "return count ? count : 1;" \
       "return count ? (uint16_t)(count + 1) : 1;"

if ! cmp -s "$SRC" "$BAK"; then
    echo "RESTORE FAILED"
    exit 1
fi

echo "SCENE_MUTATIONS killed=$killed survived=$survived"
[ "$survived" -eq 0 ]
