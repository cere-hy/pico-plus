#!/bin/bash
# gen26.6.5 专项变异：字体路径回退点阵 / 装行退理想常量 / 避头尾删除 / 标题小于正文。
# gen26.6.5 mutations: body falls back to the bitmap system, line breaking regresses to
# an idealised constant, punctuation rules removed, title smaller than the body.
set -e
cd "$(dirname "$0")/.."
if [ "${MUTATION_ALLOW_DIRTY:-0}" != "1" ] && [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    echo "REFUSE: working tree is dirty before the sweep."
    exit 1
fi
[ "${MUTATION_ALLOW_DIRTY:-0}" = "1" ] && \
    echo "WARN: MUTATION_ALLOW_DIRTY=1 - clean-tree check skipped (dev only)"

AR=main/apps/ai_reader.c
BK=$(mktemp -d)
restore_all() {
    [ -f "$BK/ai_reader.c" ] && cp "$BK/ai_reader.c" "$AR"
}
trap restore_all EXIT
cp "$AR" "$BK/ai_reader.c"

survived=0
run_expect_fail() {
    desc="$1"; shift
    if "$@" >/tmp/gen2665_mut.log 2>&1; then
        echo "SURVIVED: $desc (BAD)"
        survived=$((survived + 1))
    else
        echo "caught: $desc"
    fi
}

# ---- N1：正文绘制退回 ui_text_fixed（点阵系统）—— 接线静态守门应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("main/apps/ai_reader.c")
s = p.read_text()
old = "ui_text_reader(fb, pad, y, text_px, line);"
assert old in s, "N1 anchor missing"
p.write_text(s.replace(old, "ui_text_fixed(fb, pad, y, text_px, line, EPD_DRAW_ALIGN_LEFT, false); /* N1 */", 1))
PY
run_expect_fail "N1 body falls back to the bitmap system" python3 tools/ai_detail_wiring_host_test.py
cp "$BK/ai_reader.c" "$AR"

# ---- N2：装行退回「理想化常量 36」—— 宿主黄金值/词边界应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("main/apps/ai_reader.c")
s = p.read_text()
old = "const int w = ui_text_reader_advance_px(px, cp);"
assert old in s, "N2 anchor missing"
p.write_text(s.replace(old, "(void)cp; (void)px; const int w = 36; /* N2 */", 1))
PY
run_expect_fail "N2 line breaking uses an idealised 36px constant" bash tools/run_ai_reader_host_tests.sh
cp "$BK/ai_reader.c" "$AR"

# ---- N3：删除避头尾 —— 标点用例应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("main/apps/ai_reader.c")
s = p.read_text()
old = "if (no_start(cp)) {"
assert old in s, "N3 anchor missing"
s = s.replace(old, "if (false) { /* N3 */", 1)
old2 = "if (!no_end(cp)) break;"
assert old2 in s, "N3 second anchor missing"
s = s.replace(old2, "break; /* N3 */", 1)
p.write_text(s)
PY
run_expect_fail "N3 punctuation no-start/no-end rules removed" bash tools/run_ai_reader_host_tests.sh
cp "$BK/ai_reader.c" "$AR"

# ---- N4：标题小于正文 —— 版面断言应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("main/apps/ai_reader.c")
s = p.read_text()
old = "const int title_px = text_px;"
assert old in s, "N4 anchor missing"
p.write_text(s.replace(old, "const int title_px = text_px - 8; /* N4 */", 1))
PY
run_expect_fail "N4 title becomes smaller than the body" bash tools/run_ai_reader_host_tests.sh
cp "$BK/ai_reader.c" "$AR"

# ---- N5：详解字号写死 72（不取正文 px）—— 宿主 rec_body_px==51 应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("main/apps/ai_reader.c")
s = p.read_text()
old = "const int text_px = st.px;"
assert old in s, "N5 anchor missing"
p.write_text(s.replace(old, "const int text_px = 72; /* N5 */", 1))
PY
run_expect_fail "N5 detail px hard-coded to 72 (ignores the reader style)" bash tools/run_ai_reader_host_tests.sh
cp "$BK/ai_reader.c" "$AR"

# ---- N6：行距写死 92（不跟正文行距%）—— pitch==76 应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("main/apps/ai_reader.c")
s = p.read_text()
old = "const int line_h = st.line_h;"
assert old in s, "N6 anchor missing"
p.write_text(s.replace(old, "const int line_h = 92; /* N6 */", 1))
PY
run_expect_fail "N6 line pitch hard-coded to 92 (ignores the reader spacing)" bash tools/run_ai_reader_host_tests.sh
cp "$BK/ai_reader.c" "$AR"

# ---- N7：版心左缘被忽略（pad 写死 0）—— rec_body_x==36 应擒 ----
python3 - <<'PY'
import pathlib
p = pathlib.Path("main/apps/ai_reader.c")
s = p.read_text()
old = "const int pad = st.left;"
assert old in s, "N7 anchor missing"
p.write_text(s.replace(old, "const int pad = 0; /* N7 */", 1))
PY
run_expect_fail "N7 body left edge ignores the reader measure (pad=0)" bash tools/run_ai_reader_host_tests.sh
cp "$BK/ai_reader.c" "$AR"

if [ "$survived" -ne 0 ]; then
    echo "GEN26.6.5 MUTATIONS: $survived SURVIVED (BAD) - gates have holes"
    exit 1
fi
echo "GEN26.6.5 MUTATIONS: all 7 caught (N1 bitmap path, N2 constant advance, N3 punctuation, N4 title<body, N5 px=72, N6 pitch=92, N7 pad=0)"
