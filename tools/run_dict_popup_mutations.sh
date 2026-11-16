#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# gen26.3 第 15 runner 变异有牙验证：对 dict_popup.c 逐个植入变异，确认
# dict_popup_host_test 擒得住（exit 1 = 擒住）。任何存活变异都说明断言有洞。
# gen26.3 runner-15 mutation check: plant each mutation in dict_popup.c and confirm
# dict_popup_host_test catches it (exit 1 = caught). A surviving mutation means the
# assertions have a hole.
set -uo pipefail
cd "$(dirname "$0")/.."
# 门禁可信度 = 脚本自身可信度（gen26.6 元测试）：起点必须干净树，否则「未还原」与
# 「本来就有改动」无法区分；每条变异后断言目标文件已回到 HEAD，让「不还原」从纪律
# 问题变成脚本会红的问题。开发回路可显式 MUTATION_ALLOW_DIRTY=1 跳过起点检查。
# Gate credibility = the script's own credibility (gen26.6 meta-test): start from a
# clean tree (otherwise an unrecovered edit is indistinguishable from a pre-existing
# one) and assert the target is back at HEAD after every mutation, so a missed
# restore turns red instead of lying. A dev loop may set MUTATION_ALLOW_DIRTY=1.
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
cp main/apps/dict_popup.c /tmp/dict_popup_orig.c
restore() {
    cp /tmp/dict_popup_orig.c main/apps/dict_popup.c
    # 先与快照比对（脏树起点也成立），干净树起点再叠「相对 HEAD 无差异」。
    # Compare with the snapshot (valid from a dirty start); when the start was
    # clean, also assert no diff vs HEAD.
    cmp -s /tmp/dict_popup_orig.c main/apps/dict_popup.c || {
        echo "FATAL: dict_popup.c did not return to its pre-mutation snapshot" >&2
        exit 3
    }
    if [ "$START_CLEAN" = "1" ]; then
        git diff --quiet -- main/apps/dict_popup.c || {
            echo "FATAL: dict_popup.c did not return to HEAD after a mutation" >&2
            exit 3
        }
    fi
}
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
old = '#define DICT_POPUP_AI_SEG (PUSH_AI_DEF_MAX + 1u)'
assert old in s, 'M5 target not found'
s = s.replace(old, '#define DICT_POPUP_AI_SEG 192 /* MUTATION M5 */')
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
    copy_word_head(own, sizeof(own), word);
    dict_popup_reset();
    copy_word_head(s_word, sizeof(s_word), own);
    if (!store_line(text, strlen(text))) {'''
new = '''    if (!word || !*word) { dict_popup_reset(); return false; }
    dict_popup_reset();
    snprintf(s_word, sizeof(s_word), "%.*s", DICT_POPUP_WORD_MAX - 1, word); /* MUTATION M7 */
    if (!store_line(text, strlen(text))) {'''
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

# M9：暂存帽与显示行数脱钩（+6）——第 9 行起「暂存了却画不出、也不置截断位」，
# 尾部静默丢失。宿主「121 字必须 truncated」断言应擒住（M3-A 操作栏让行后帽=8）。
# M9: decouple the staging cap from the visible count (+6) - lines beyond the bar
# shrink get staged but neither drawn nor flagged. The host's "121 chars must be
# truncated" assertion catches it (cap = 8 after the M3-A footer).
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = 'if (s_line_count >= visible_lines()) {'
assert old in s, 'M9 target not found'
s = s.replace(old, 'if (s_line_count >= visible_lines() + 6) { /* MUTATION M9 */', 1)
p.write_text(s)
EOF
run_mutation "M9 staging cap back to 14"

# ---- M10：详解入口热区缩回（三个点本身的宽度）——真机「按不到」缺陷复现 ----
# ---- M10: the detail entry's hit zone shrinks back to the dots themselves - the
#      on-device "cannot hit it" defect returns.
restore
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = 'return (EpdRect){mid, bar_top, card.x + card.width - DICT_POPUP_PAD - mid, DICT_POPUP_BAR_H};'
assert old in s, 'M10 target not found'
p.write_text(s.replace(old, 'return (EpdRect){card.x + card.width - DICT_POPUP_PAD - 21, bar_top, 21, DICT_POPUP_BAR_H}; /* MUTATION M10 */', 1))
EOF
run_mutation "M10 detail-entry hit zone shrinks back to the dots"

# ---- M11：无剩文也画「……」/「看全文 ›」——非触顶误画，用户会以为释义被截断 ----
# ---- M11: the marker and "看全文 ›" appear even with nothing truncated - a false
#      signal that the definition was cut off.
restore
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = '    return s_truncated || s_line_count > visible_lines();'
assert old in s, 'M11 target not found'
p.write_text(s.replace(old, '    return true; /* MUTATION M11 */', 1))
EOF
run_mutation "M11 more-entry drawn with nothing truncated"

# ---- M12：「看全文 ›」字号从 24 漂走（规格钉死：24px 纯文字）----
# ---- M12: the "看全文 ›" label drifts off 24px (spec: 24px plain text).
restore
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = '#define DICT_POPUP_MORE_LABEL_PX 24'
assert old in s, 'M12 target not found'
p.write_text(s.replace(old, '#define DICT_POPUP_MORE_LABEL_PX 20 /* MUTATION M12 */', 1))
EOF
run_mutation "M12 view-full label drops off 24px"

# ---- M13：热区退回标签级小方块（M3-A 操作栏重构后的等价回归）——半条热区
#      缩成 24 见方，手指点不中、误关卡 ----
# ---- M13: the hit zone shrinks back to a label-sized square (the M3-A bar
#      equivalent of the old regression) - unclickable, taps close the card.
restore
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = 'return (EpdRect){mid, bar_top, card.x + card.width - DICT_POPUP_PAD - mid, DICT_POPUP_BAR_H};'
assert old in s, 'M13 target not found'
p.write_text(s.replace(old, 'return (EpdRect){mid + 60, bar_top, 24, 24}; /* MUTATION M13 */', 1))
EOF
run_mutation "M13 hit zone shrinks to a label-sized square"

# ---- 26.9.5 入口卡四颗牙 + 一颗跨文件静态牙 ----
# 这五条对应本版的新逻辑：拆掉 english 总闸后，任何一条退回旧形态都会让中文读者
# 重新变成二等公民，而编译、既有用例、真机点词都不会报错——只有断言能说话。

# M14：入口卡照旧画保存 chip（「有释义才可存」被拆）——空壳能被存进生词本。
# M14: the entry card draws the save chip again (the "only a definition may be
# saved" rule torn out) - an empty shell could enter the vocabulary book.
restore
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = '    const bool save_chip = !s_entry;'
assert old in s, 'M14 target not found'
p.write_text(s.replace(old, '    const bool save_chip = true; /* MUTATION M14 */', 1))
EOF
run_mutation "M14 entry card draws the save chip"

# M15：入口卡置位 s_pending——没发过请求却被 0x07 认领、被超时降级。
# M15: the entry card sets s_pending - an unsent card gets claimed by 0x07 and
# degraded by the timeout.
restore
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = '    s_pending = false;      // reset 已置 false，这里显式再钉一次（防顺序被改坏）'
assert old in s, 'M15 target not found'
p.write_text(s.replace(old, '    s_pending = true; /* MUTATION M15 */', 1))
EOF
run_mutation "M15 entry card pins s_pending = true"

# M16：词头截断退回按字节（snprintf "%.*s"）——中文卡面切出半个字，且与 0x07
# 回流词的前缀匹配失配。
# M16: the head truncation falls back to byte-wise snprintf - a Chinese head gets
# cut mid-character and the prefix match against the 0x07 word breaks.
restore
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = '''    size_t o = 0;
    if (src) {
        while (*src && o + 1 < cap) {
            const int step = utf8_step(src);
            if (step <= 0) break;
            if (o + (size_t)step > cap - 1) break; // 放不下整字就停，不切 / stop, never split
            memcpy(dst + o, src, (size_t)step);
            o += (size_t)step;
            src += step;
        }
    }
    dst[o] = 0;'''
assert old in s, 'M16 target not found'
p.write_text(s.replace(old, '    snprintf(dst, cap, "%.*s", (int)(cap - 1), src ? src : ""); /* MUTATION M16 */', 1))
EOF
run_mutation "M16 head truncation cuts UTF-8 mid-character"

# M17：入口卡仍响应保存 chip（不落库的护栏被拆）——点一下就把状态行存成释义。
# M17: the entry card still answers the save chip (the no-save guard torn out) -
# a tap stores the status line as the definition.
restore
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c')
s = p.read_text()
old = '    if (s_entry) return DICT_POPUP_TAP_MISSED;'
assert old in s, 'M17 target not found'
p.write_text(s.replace(old, '    /* MUTATION M17: guard removed */', 1))
EOF
run_mutation "M17 entry card answers the save chip"

# M18（跨文件静态牙）：app_book 的入口分支退回 `hit && english` ——中文重新静默
# 不弹卡（本版要根治的那个缺陷）。dict_popup 宿主编不到 app_book.c，故用
# vocab_wiring_host_test.py 这颗静态牙来咬；它不编译，只读文本。
# M18 (cross-file static tooth): app_book's entry branch falls back to
# `hit && english` - Chinese goes silent again (the very defect this version
# kills). The dict_popup host test never compiles app_book.c, so the static
# vocab_wiring_host_test.py does the biting (text only, nothing compiled).
restore
cp main/apps/app_book.c /tmp/app_book_orig.c
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/app_book.c')
s = p.read_text()
old = '    if (hit && !found && push_sink_ai_enabled()) {'
assert old in s, 'M18 target not found'
p.write_text(s.replace(old,
    '    if (hit && english && !found && push_sink_ai_enabled()) { /* MUTATION M18 */', 1))
EOF
if python3 tools/vocab_wiring_host_test.py >/tmp/mut_wire.txt 2>&1; then
    echo "MUTATION SURVIVED (BAD): M18 entry branch gated by english again (static)"
else
    echo "caught: M18 entry branch gated by english again (static)"
fi
cp /tmp/app_book_orig.c main/apps/app_book.c
cmp -s /tmp/app_book_orig.c main/apps/app_book.c || {
    echo "FATAL: app_book.c did not return to its pre-mutation snapshot" >&2
    exit 3
}

echo "---- mutation sweep done ----"
