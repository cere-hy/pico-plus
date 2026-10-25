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
    # 每条变异都从原件重新开始：否则前一条的致命变异会把后一条「顺带」抓掉，
    # 「caught」就失去意义（gen26.6 发现：原实现只在 EXIT 时还原）。
    # Restore before the next mutation: otherwise an earlier fatal mutation makes
    # every later one look "caught" and the sweep stops meaning anything (found in
    # gen26.6: the original only restored on EXIT).
    restore
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

# M3：容量行数少一行——135 的锚漂掉。起行下移一整行，任何几何下都成立（旧的
# 「条件 +1」在 36px/46 下不再跨行边界，会存活）。/ M3: one line fewer in the
# capacity - the 135 anchor drifts. Shifting the start down a full line is robust
# under any geometry (the old "+1 on the condition" no longer crosses a line
# boundary at 36px/46 and would survive).
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
s = s.replace('    for (int y = 26 + DICT_POPUP_WORD_PX + 18; y + DICT_POPUP_TEXT_PX <= card.height - 20; y += DICT_POPUP_LINE_H) {',
              '    for (int y = 26 + DICT_POPUP_WORD_PX + 18 + DICT_POPUP_LINE_H; y + DICT_POPUP_TEXT_PX <= card.height - 20; y += DICT_POPUP_LINE_H) { /* MUTATION M3 */')
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

# M5：AI 段缓冲回退到 192（D3 联动修正前的值）——180 字（540B）释义会被静默截到
# 63 字（5 行、无省略号），宿主「9 行 + ……」断言应擒住。gen26.6 起此条专门守
# 「放宽释义不得回收段缓冲」。/ M5: the AI segment buffer back to 192 (the pre-D3
# value) - a 180-char (540B) definition is silently cut to 63 chars (5 lines, no
# ellipsis); the host's "9 lines + ……" assertion should catch it. Since gen26.6
# this guards "widening the definition must not shrink the segment buffer".
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
assert '#define DICT_POPUP_AI_SEG 769' in s, 'M5 target not found'
s = s.replace('#define DICT_POPUP_AI_SEG 769', '#define DICT_POPUP_AI_SEG 192 /* MUTATION M5 */')
p.write_text(s)
EOF
run_mutation "M5 AI segment buffer shrinks back to 192"

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

# M8：折行宽度只扣左 PAD（右侧不扣）——正文末字贴/越卡右缘（真机「die」贴边被裁
# 的几何缺陷）。宿主「右缘 ≤ 卡右缘 - PAD」断言应擒住。
# M8: the wrap width subtracts only the LEFT pad - the body's last glyph sits on
# or over the right edge (the geometry behind the on-device "die" clipping). The
# host's "right edge <= card right - PAD" assertion should catch it.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = '    return dict_popup_rect().width - 2 * DICT_POPUP_PAD;'
assert old in s, 'M8 target not found'
s = s.replace(old, '    return dict_popup_rect().width - DICT_POPUP_PAD; /* MUTATION M8 */')
p.write_text(s)
EOF
run_mutation "M8 wrap width drops the right pad"

# M9：暂存行帽回退到 14（旧值）——10~14 行的释义被画成 9 行 + 省略号却不置截断位，
# 不开详解、尾部静默丢失。宿主「136 字必须 truncated」断言应擒住。
# M9: the staging cap back to 14 (the old value) - a 10-14 line definition paints
# 9 lines plus an ellipsis without setting the flag, so no detail view and a
# silently lost tail. The host's "136 chars must be truncated" assertion catches it.
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
assert '#define DICT_POPUP_LINES 9' in s, 'M9 target not found'
s = s.replace('#define DICT_POPUP_LINES 9', '#define DICT_POPUP_LINES 14 /* MUTATION M9 */')
p.write_text(s)
EOF
run_mutation "M9 staging cap back to 14"

echo "---- mutation sweep done ----"
