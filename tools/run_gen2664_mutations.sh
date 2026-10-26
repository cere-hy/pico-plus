#!/bin/bash
# gen26.6.4 专项变异：字号漂移 / APK 帽回退 / 帽>槽。
# gen26.6.4 mutations: font drift, APK cap regression, cap > firmware slot.
set -e
cd "$(dirname "$0")/.."
if [ "${MUTATION_ALLOW_DIRTY:-0}" != "1" ] && [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    echo "REFUSE: working tree is dirty before the sweep."
    exit 1
fi
[ "${MUTATION_ALLOW_DIRTY:-0}" = "1" ] && \
    echo "WARN: MUTATION_ALLOW_DIRTY=1 - clean-tree check skipped (dev only)"

AI=tools/android/apk/src/dev/readpico/push/AiClient.java
AR=main/apps/ai_reader.c
BK=$(mktemp -d)

restore_all() {
    [ -f "$BK/AiClient.java" ] && cp "$BK/AiClient.java" "$AI"
    [ -f "$BK/ai_reader.c" ] && cp "$BK/ai_reader.c" "$AR"
}
trap restore_all EXIT

cp "$AI" "$BK/AiClient.java"
cp "$AR" "$BK/ai_reader.c"

survived=0
run_expect_fail() {
    desc="$1"; shift
    if "$@" >/tmp/gen2664_mut.log 2>&1; then
        echo "SURVIVED: $desc (BAD)"
        survived=$((survived + 1))
    else
        echo "caught: $desc"
    fi
}

# ---- M8：正文字号漂回 36/46 —— 宿主断言 rec_body_px/页数应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("main/apps/ai_reader.c")
s = p.read_text()
old = "const int line_h = 92, text_px = 72;"
assert old in s, "M8 anchor missing"
p.write_text(s.replace(old, "const int line_h = 46, text_px = 36; /* M8 */", 1))
PY
run_expect_fail "M8 body font drifts back to 36/46" bash tools/run_ai_reader_host_tests.sh
cp "$BK/ai_reader.c" "$AR"

# ---- M9：APK 帽退回 700 —— AiClientTest 的 DEF_MAX_BYTES==1152 应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("tools/android/apk/src/dev/readpico/push/AiClient.java")
s = p.read_text()
old = "DEF_MAX_BYTES = 1152"
assert old in s, "M9 anchor missing"
p.write_text(s.replace(old, "DEF_MAX_BYTES = 700 /* M9 */", 1))
PY
run_expect_fail "M9 APK cap regresses to 700" bash tools/run_apk_host_tests.sh
cp "$BK/AiClient.java" "$AI"

# ---- M10：APK 帽超过固件槽（1300 > 1200-48）—— 一致性守门应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("tools/android/apk/src/dev/readpico/push/AiClient.java")
s = p.read_text()
old = "DEF_MAX_BYTES = 1152"
assert old in s, "M10 anchor missing"
p.write_text(s.replace(old, "DEF_MAX_BYTES = 1300 /* M10 */", 1))
PY
run_expect_fail "M10 APK cap exceeds the firmware slot (1300 > 1200)" \
    python3 tools/apk_cap_consistency_check.py
cp "$BK/AiClient.java" "$AI"

if [ "$survived" -ne 0 ]; then
    echo "GEN26.6.4 MUTATIONS: $survived SURVIVED (BAD) - gates have holes"
    exit 1
fi
echo "GEN26.6.4 MUTATIONS: all 3 caught (M8 font drift, M9 cap regression, M10 cap>slot)"
