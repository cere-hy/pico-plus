#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 中文：M3-B 情景视图的变异验证。逐个植入变异，确认两套宿主测试（纯逻辑 +
# 详解/情景渲染）擒得住——exit 1 才算擒住。存活即断言有洞。
# English: Mutation check for the M3-B scene view. Each variant is planted and the
# two host suites (pure logic + render) must fail; a survivor means a hole.
#
# 靶点漂移保护：找不到目标串直接判 TARGET MISSING 并计失败，绝不静默跳过——
# 否则重构后会出现「变异根本没植入、测试照过 → 假 SURVIVED」（v2 踩过）。
# Target-drift guard: a missing anchor fails the sweep loudly instead of silently
# skipping (the fake-survivor trap hit in v2).
set -uo pipefail
cd "$(dirname "$0")/.."

REQ=main/apps/scene_req.c
READER=main/apps/ai_reader.c
BACKUP_REQ=$(mktemp)
BACKUP_READER=$(mktemp)
cp "$REQ" "$BACKUP_REQ"
cp "$READER" "$BACKUP_READER"
trap 'cp "$BACKUP_REQ" "$REQ"; cp "$BACKUP_READER" "$READER"; rm -f "$BACKUP_REQ" "$BACKUP_READER"' EXIT

FAILED=0
CAUGHT=0

build_scene() {
    cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
       -Imain/apps -o build/book-tests/scene_mut \
       tools/ai_scene_view_host_test.c "$REQ" 2>/dev/null || return 1
    ./build/book-tests/scene_mut >/dev/null 2>&1
}

build_reader() {
    cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
       -Itools/app_loop_stubs -Imain -Imain/app -Imain/apps -Imain/ui \
       -Icomponents/push_sink/include -Icomponents/ble_page_turner/include \
       -o build/book-tests/reader_mut tools/ai_reader_host_test.c "$REQ" 2>/dev/null || return 1
    ./build/book-tests/reader_mut >/dev/null 2>&1
}

mutate() {
    local name="$1" file="$2" old="$3" new="$4"
    if ! OLD="$old" NEW="$new" FILE="$file" python3 - <<'EOF'
import os, pathlib
p = pathlib.Path(os.environ['FILE'])
s = p.read_text()
old, new = os.environ['OLD'], os.environ['NEW']
if old not in s:
    raise SystemExit(3)
p.write_text(s.replace(old, new, 1))
EOF
    then
        echo "TARGET MISSING: $name"
        FAILED=1
        return
    fi
}

run_mutation() {
    local name="$1"
    local killed=0
    if ! build_scene; then killed=1; fi
    if ! build_reader; then killed=1; fi
    if [ "$killed" -eq 1 ]; then
        echo "caught: $name"
        CAUGHT=$((CAUGHT + 1))
    else
        echo "MUTATION SURVIVED: $name"
        FAILED=1
    fi
    cp "$BACKUP_REQ" "$REQ"
    cp "$BACKUP_READER" "$READER"
}

# ---- M1：认领不再比对词（别人的答案也会被当成自己的）----
mutate "M1" "$REQ" \
  'if (!word || strcmp(word, r->word) != 0) return false; // 不是我这单 / not mine' \
  'if (false) return false; /* MUTATION M1 */'
run_mutation "M1 claim ignores the word"

# ---- M2：认领不再比对发起页（翻页后仍把 A 页分析贴到 B 页）----
mutate "M2" "$REQ" \
  'if (page != r->page) {' \
  'if (false) { /* MUTATION M2 */'
run_mutation "M2 claim ignores the originating page"

# ---- M3：认领成功不销号（同一条答案能开视图又落卡）----
mutate "M3" "$REQ" \
  'r->armed = false; // 成功即销号：一次机会 / one shot, disarmed on success' \
  '(void)0; /* MUTATION M3 */'
run_mutation "M3 claim does not disarm on success"

# ---- M4：认领忽略期限（过期答案照样开视图）----
mutate "M4" "$REQ" \
  'if (now_ms > r->deadline_ms) {
        r->armed = false;
        return false;
    }' \
  '(void)now_ms; /* MUTATION M4 */'
run_mutation "M4 claim ignores the deadline"

# ---- M5：expired 去掉期限比较（tick 每轮都销号）----
mutate "M5" "$REQ" \
  'return r && r->armed && now_ms > r->deadline_ms;' \
  'return r && r->armed; /* MUTATION M5 */'
run_mutation "M5 expired() drops the deadline comparison"

# ---- M6：取句不再认句首（引用条从段首开始，跨句引用）----
mutate "M6" "$REQ" \
  'if (sr_is_sentence_end(cp)) s = nxt;' \
  '(void)nxt; /* MUTATION M6 */'
run_mutation "M6 quote ignores the sentence head"

# ---- M7：截断把词切掉（引用条里没有目标词）----
# 靶点放在「截断后写回」这一步：把右边界收回起点，切片变空、词随之消失。
# The anchor is the write-back after truncation: collapsing the right edge onto the
# start empties the slice and the word disappears with it.
mutate "M7" "$REQ" \
  '        s = ns;
        e = ne;' \
  '        s = ns;
        e = ns; /* MUTATION M7 */'
run_mutation "M7 truncation drops the word"

# ---- M8：几何注入被忽略（情景卡退回详解的 236/h-72，10 行变 11 行）----
mutate "M8" "$READER" \
  'if (s_geom_set && s_geom.bottom > s_geom.top && s_geom.width > 0) return s_geom;' \
  'if (false) return s_geom; /* MUTATION M8 */'
run_mutation "M8 geometry injection ignored"

# ---- M9：情景卡不画引用条（顶替标题的那一块消失）----
mutate "M9" "$READER" \
  'const int quote_h = s_session.quote_len ? 96 : 0;' \
  'const int quote_h = 0; /* MUTATION M9 */'
run_mutation "M9 quote strip never painted"

# ---- M10：「存生词」chip 退化成零矩形（画不出也点不到）----
mutate "M10" "$READER" \
  'if (!s_session.active || s_session.mode != AI_READER_MODE_SCENE) return zero;' \
  '(void)zero; return zero; /* MUTATION M10 */'
run_mutation "M10 save chip degenerates to a zero rect"

# ---- M11：引用条底带回退成近黑（0x0A 量化后纯黑 → 黑底黑字复现）----
# 靶点就是「亮底常量」本身：改回裸 0x0A，B1 亮底守卫必须点名。
mutate "M11" "$READER" \
  '        ui_fill_rect_fast((EpdRect){0, g.top, UI_LOCK_WIDTH, quote_h},
                          UI_QUOTE_BAND_BG, fb);' \
  '        ui_fill_rect_fast((EpdRect){0, g.top, UI_LOCK_WIDTH, quote_h},
                          0x0A, fb); /* MUTATION M11 */'
run_mutation "M11 wash falls back to near-black 0x0A"

# ---- M12：目标词不再垫黑底（inverted 只画白字形 → 白字落浅底，再次隐形）----
mutate "M12" "$READER" \
  '        const int wpx = ui_text_fixed_width_px(px, seg);
        const int box_h = px + 8;
        if (wpx > 0) {
            ui_fill_rect_fast((EpdRect){x, cy - box_h / 2, wpx, box_h},
                              UI_QUOTE_WORD_INK, fb);
        }' \
  '        const int wpx = ui_text_fixed_width_px(px, seg);
        const int box_h = px + 8;
        (void)wpx; (void)box_h; /* MUTATION M12: no word box */'
run_mutation "M12 word box removed"

# ---- M13：全部反色（整句都变白字 → 普通字计数归零）----
mutate "M13" "$READER" \
  '    ui_text_fixed_vc(fb, x, cy, px, seg, EPD_DRAW_ALIGN_LEFT, inverted);' \
  '    ui_text_fixed_vc(fb, x, cy, px, seg, EPD_DRAW_ALIGN_LEFT, true); /* MUTATION M13 */'
run_mutation "M13 every run inverted"

# ---- M14：全不反色（目标词也画黑字 → 反色计数归零，黑盒黑字）----
mutate "M14" "$READER" \
  '    ui_text_fixed_vc(fb, x, cy, px, seg, EPD_DRAW_ALIGN_LEFT, inverted);' \
  '    ui_text_fixed_vc(fb, x, cy, px, seg, EPD_DRAW_ALIGN_LEFT, false); /* MUTATION M14 */'
run_mutation "M14 no run inverted"

# ---- M15：情景卡上沿退回 236（白板复现：又用详解「标题区之后」的值）----
# 靶点是 ai_reader_scene_geom 的 .top——注入规则收在 ai_reader 才命中得了；
# 若留在 app_book，宿主测试碰不到，这条变异就会假存活。
mutate "M15" "$READER" \
  '        .top = AI_READER_SCENE_TOP,' \
  '        .top = 236, /* MUTATION M15 */'
run_mutation "M15 scene top falls back to 236"

# ---- M16：详解 body_top 被误套成场景上沿（「逐像素不变」悄悄破：236→96，11→12 行）----
# 反向验证：M15 验「场景守卫能拦上沿后退」，本条目验「详解守卫能拦场景几何上提」。
# 靶点就是 render_detail 里硬编码的 body_top=236——详情路径不读 current_geom，若有人
# 把它改成 AI_READER_SCENE_TOP（误把场景上沿套进详解），test_layout_metrics 的 236 / 11
# 双点名必须红。缺这条，详解的几何不变性靠人工/真机单次验证撑着，重构后可能静默失守。
# The reverse of M15: M15 proves the scene guard catches a fallen top; this proves the
# detail guard catches a scene top lifted onto detail. The anchor is render_detail's
# hard-coded body_top=236 - the detail path never reads current_geom, so if someone
# swaps it for AI_READER_SCENE_TOP (a scene top wrongly applied to detail),
# test_layout_metrics must fail on 236 / 11.
mutate "M16" "$READER" \
  'const int body_top = 236;' \
  'const int body_top = AI_READER_SCENE_TOP; /* MUTATION M16 */'
run_mutation "M16 detail body_top falls back to scene top"

echo "scene-view mutations: $CAUGHT caught, $([[ $FAILED -eq 0 ]] && echo 'all killed' || echo 'SURVIVORS PRESENT')"
exit $FAILED
