#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 中文：M5-A 框选变异扫描——打掉九条关键判定（M1-M9），必须条条被擒。
#       靶点清单与各自擒拿者冻结在 docs/EXCERPT_GATE_BASELINE.md §八。
# English: M5-A mutation sweep - break nine load-bearing decisions (M1-M9), all must be caught.
#       The target list and each one's catcher live in EXCERPT_GATE_BASELINE.md §8.
set -u
cd "$(dirname "$0")/.."

SEL_C=main/book/book_select.c
APP_C=main/apps/app_book.c
LAY_C=main/book/book_layout.c
TMP=$(mktemp -d)
trap 'cp "$TMP/book_select.c" '"$SEL_C"' 2>/dev/null; cp "$TMP/app_book.c" '"$APP_C"' 2>/dev/null; cp "$TMP/book_layout.c" '"$LAY_C"' 2>/dev/null; rm -rf "$TMP"' EXIT
cp "$SEL_C" "$TMP/book_select.c"
cp "$APP_C" "$TMP/app_book.c"
cp "$LAY_C" "$TMP/book_layout.c"

fail=0
caught=0

run_case() {
    local name="$1"
    local ok=0
    if python3 tools/check_boxsel.py >/dev/null 2>&1; then
        if gcc -std=c11 -Wall -Wextra -I main/book tools/boxsel_host_test.c main/book/book_select.c \
               -o "$TMP/t" 2>/dev/null; then
            "$TMP/t" >/dev/null 2>&1 && ok=1
        fi
    fi
    if [ "$ok" -eq 0 ]; then
        echo "caught: $name"
        caught=$((caught + 1))
    else
        echo "SURVIVED: $name"
        fail=$((fail + 1))
    fi
    cp "$TMP/book_select.c" "$SEL_C"
    cp "$TMP/app_book.c" "$APP_C"
    cp "$TMP/book_layout.c" "$LAY_C"
}

echo "== M5-A mutation sweep =="

# M1：框选态不再立即吞事件 → 结构牙 E-S2 必须报
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/apps/app_book.c'); s = p.read_text()
s = s.replace("    if (boxsel_active()) return boxsel_touch(ctx, ev);\n", "", 1)
p.write_text(s)
PY
run_case "M1 去掉单指早退（事件漏到翻页/点词）"

# M2：反向选不再交换 → 宿主测试 E-R1 必须报
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/book/book_select.c'); s = p.read_text()
s = s.replace("""        if (we <= sel->anchor_ws) {
            sel->off_start = ws;
            sel->off_end = sel->anchor_we;
        } else {""", """        if (0) {
            sel->off_start = ws;
            sel->off_end = sel->anchor_we;
        } else {""", 1)
p.write_text(s)
PY
run_case "M2 反向选不交换（先点后行再点前行）"

# M3：超时边界错成 <= → 宿主测试的「到点即过期」必须报
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/book/book_select.c'); s = p.read_text()
s = s.replace("if (!sel->deadline_ms || now_ms < sel->deadline_ms) return false;",
              "if (!sel->deadline_ms || now_ms <= sel->deadline_ms) return false;", 1)
p.write_text(s)
PY
run_case "M3 超时边界错一格"

# M4：容量检查被删 → 宿主测试的「装不下就一个字不写」必须报
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/book/book_select.c'); s = p.read_text()
s = s.replace("    if (n + 1 > cap) return 0;                        // 装不下（含 NUL）：不写半截 / need room for NUL\n", "", 1)
p.write_text(s)
PY
run_case "M4 去掉容量检查（允许截断拷贝）"

# ---- M5：句首不跳过行首缩进（全角空格被吞进选区）----
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/book/book_select.c'); s = p.read_text()
s = s.replace("            if (!is_indent_cp(cp)) break;",
              "            if (!is_indent_cp(cp) || 1) break; /* MUTATION M5 */", 1)
p.write_text(s)
PY
run_case "M5 句首不跳缩进（全角空格被选进句子）"

# ---- M6：尾点不含句末标点（选区在标点前断掉）----
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/book/book_select.c'); s = p.read_text()
s = s.replace("""                if (text[i] == '\\n') { e = i; break; }
                e = nx;""",
              """                if (text[i] == '\\n') { e = i; break; }
                e = i; /* MUTATION M6 */""", 1)
p.write_text(s)
PY
run_case "M6 尾点不含标点（句子在标点前被截断）"

# ---- M7：跨页不截断（句子跑出本页）----
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/book/book_select.c'); s = p.read_text()
s = s.replace("        if (!hit) e = page_end;", "        (void)hit; /* MUTATION M7 */", 1)
p.write_text(s)
PY
run_case "M7 跨页不截断（选区越过页尾）"

# ---- M8：英文句号不判后跟空白（3.14 与 Mr. 被切成两句）----
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/book/book_select.c'); s = p.read_text()
s = s.replace("""            if (c == ' ' || c == '\\t' || c == '\\r' || c == '\\n') { *next = j; return true; }
            return false;""",
              """            (void)c; *next = j; return true; /* MUTATION M8 */""", 1)
p.write_text(s)
PY
run_case "M8 英文句号不判后跟空白（小数点/缩写被误切）"

# ---- M9：反色底退回裸 line_width（右侧少盖半个字，26.9.11 的原样）----
python3 - <<'PY'
import pathlib
p = pathlib.Path('main/book/book_layout.c'); s = p.read_text()
s = s.replace("EpdRect line_rect = {x, rect.y + (int)used, ink, line_height};",
              "EpdRect line_rect = {x, rect.y + (int)used, line_width, line_height}; /* MUTATION M9 */", 1)
p.write_text(s)
PY
run_case "M9 反色底按自然行宽铺（两端对齐后右侧短一截）"

echo "-- caught: $caught / $((caught + fail)) --"
[ "$fail" -eq 0 ] || { echo "FAIL: $fail mutation(s) survived"; exit 1; }
echo "all mutations caught"
