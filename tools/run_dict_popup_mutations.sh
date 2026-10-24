#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# gen26.3 第 15 runner 变异有牙验证：对 dict_popup.c 逐个植入变异，确认
# dict_popup_host_test 擒得住（exit 1 = 擒住）。任何存活变异都说明断言有洞。
# gen26.3 runner-15 mutation check: plant each mutation in dict_popup.c and confirm
# dict_popup_host_test catches it (exit 1 = caught). A surviving mutation means the
# assertions have a hole.
set -uo pipefail
cd "$(dirname "$0")/.."
cp main/apps/dict_popup.c /tmp/dict_popup_orig.c
restore() { cp /tmp/dict_popup_orig.c main/apps/dict_popup.c; }
trap restore EXIT

run_mutation() {
    local label="$1"
    if bash tools/run_dict_popup_host_tests.sh >/tmp/mut_out.txt 2>&1; then
        echo "MUTATION SURVIVED (BAD): $label"
    else
        echo "caught: $label"
    fi
}

# M1：谓词丢词比对（pending 即真）——门 B 会对别词开卡。/ M1: the predicate drops
# the word compare - gate B would update any word's card.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
s = s.replace('    return strncmp(s_word, word, DICT_POPUP_WORD_MAX - 1) == 0;',
              '    return true; /* MUTATION M1 */')
p.write_text(s)
EOF
run_mutation "M1 is_pending ignores the word"

# M2：超时恒假——查询卡永远假活。/ M2: the timeout never fires - the querying card fakes forever.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
s = s.replace('    return s_pending && esp_timer_get_time() - s_pending_since_us >= timeout_us;',
              '    (void)timeout_us; return false; /* MUTATION M2 */')
p.write_text(s)
EOF
run_mutation "M2 pending_expired never fires"

# M3：容量行数少一行（9 行）——200 的锚漂掉。/ M3: one line fewer in the capacity - the 200 anchor drifts.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
s = s.replace('    for (int y = 26 + DICT_POPUP_WORD_PX + 18; y + DICT_POPUP_TEXT_PX <= card.height - 20; y += DICT_POPUP_LINE_H) {',
              '    for (int y = 26 + DICT_POPUP_WORD_PX + 18; y + DICT_POPUP_TEXT_PX + 1 <= card.height - 20; y += DICT_POPUP_LINE_H) { /* MUTATION M3 */')
p.write_text(s)
EOF
run_mutation "M3 capacity drops a line"

# M4：show_pending 忘记置查询态标志——双门整条失效。/ M4: show_pending forgets the query flag - the whole double gate dies.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
s = s.replace('    s_pending = true;\n    s_pending_since_us = esp_timer_get_time();',
              '    s_pending_since_us = esp_timer_get_time(); /* MUTATION M4: flag dropped */')
p.write_text(s)
EOF
run_mutation "M4 show_pending drops the flag"

# M5：折行踩 NUL 的旧伤复现（钳位不回退字符边界）——ASan 应擒。/ M5: the old
# step-over-NUL injury returns (clamp without the boundary back-off) - ASan should catch it.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
s = s.replace('        while (len > 0 && ((unsigned char)p[len] & 0xC0) == 0x80) --len; // 续字节回退 / back off continuation bytes\n', '')
p.write_text(s)
EOF
run_mutation "M5 clamp cuts a multibyte sequence"

# M6：AI 入口门禁失效（命中判定忽略 s_ai_on）——AI 关时点 AI 区也会触发查询。
# M6: the AI entry gate dies (hit test ignores s_ai_on) - tapping AI with AI off would query.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
s = s.replace('    if (s_ai_on && !s_pending && ui_rect_hit(dict_popup_ai_rect(), x, y)) {',
              '    if (!s_pending && ui_rect_hit(dict_popup_ai_rect(), x, y)) { /* MUTATION M6 */')
p.write_text(s)
EOF
run_mutation "M6 AI entry gate ignores s_ai_on"

# M7：别名防御被拆（show_pending 恢复「先 reset 后查空」的原始 bug 形态）——
# 传 dict_popup_word()（别名 s_word）的调用会被 reset 清掉参数，查询态建不起来。
# M7: the aliasing defense is torn out (show_pending back to the original
# "reset first, check after" buggy shape) - a caller passing dict_popup_word()
# (aliased to s_word) gets its argument cleared by the reset and the query
# state never forms.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = '''    char own[DICT_POPUP_WORD_MAX];
    if (!word || !*word) { dict_popup_reset(); return false; }
    snprintf(own, sizeof(own), "%.*s", DICT_POPUP_WORD_MAX - 1, word);
    dict_popup_reset();
    snprintf(s_word, sizeof(s_word), "%.*s", DICT_POPUP_WORD_MAX - 1, own);'''
new = '''    if (!word || !*word) { dict_popup_reset(); return false; }
    dict_popup_reset();
    snprintf(s_word, sizeof(s_word), "%.*s", DICT_POPUP_WORD_MAX - 1, word); /* MUTATION M7 */'''
assert old in s, "M7 target not found"
s = s.replace(old, new)
p.write_text(s)
EOF
run_mutation "M7 aliasing defense torn out"

echo "---- mutation sweep done ----"
