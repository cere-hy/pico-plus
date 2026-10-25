#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# gen26.5 M2 Step10-5 变异有牙验证：对 TTS 链路逐个植入变异，确认宿主测试擒得住
# （runner 非 0 退出 = 擒住）。任何存活变异都说明断言有洞——有洞就补断言，
# 不是把变异划掉。
# gen26.5 M2 Step10-5 mutation check: plant each mutation across the TTS chain
# and confirm the host tests catch it (non-zero runner exit = caught). A
# surviving mutation means the assertions have a hole - fill the hole, never
# drop the mutation.
#
# 覆盖三层：
#   纯逻辑  main/tts/tts_page.c      -> run_tts_host_tests.sh
#   纯逻辑  push_sink/src/push_tts.c -> run_push_sink_host_tests.sh
#   接线    main/apps/app_book.c      -> run_dict_hit_host_tests.sh（vocab_wiring 静态守门）
set -uo pipefail
cd "$(dirname "$0")/.."

cp main/tts/tts_page.c /tmp/tts_mut_page.c
cp components/push_sink/src/push_tts.c /tmp/tts_mut_plan.c
cp main/apps/app_book.c /tmp/tts_mut_book.c
restore_all() {
    cp /tmp/tts_mut_page.c main/tts/tts_page.c
    cp /tmp/tts_mut_plan.c components/push_sink/src/push_tts.c
    cp /tmp/tts_mut_book.c main/apps/app_book.c
}
trap restore_all EXIT

survived=0
run_mutation() {
    local label="$1" runner="$2"
    if bash "$runner" >/tmp/tts_mut_out.txt 2>&1; then
        echo "MUTATION SURVIVED (BAD): $label"
        survived=$((survived + 1))
    else
        echo "caught: $label"
    fi
    restore_all
}

# ---- M1：U+FFFC 图片占位不滤 —— 页文本会把占位符读出来 ----
# M1: the U+FFFC image placeholder is no longer filtered out.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/tts/tts_page.c')
s = p.read_text()
old = '        if (i + 2 < end && (unsigned char)text[i] == TTS_FFFC_B0 &&'
new = '        if (0 && (unsigned char)text[i] == TTS_FFFC_B0 &&'
assert old in s, 'M1 anchor missing'
p.write_text(s.replace(old, new, 1))
EOF
run_mutation "M1 tts_page: FFFC filter disabled" tools/run_tts_host_tests.sh

# ---- M2：空白不折叠（bl 恒 0）—— 段间多空行原样进文本 ----
# M2: blank runs are never collapsed (blank detection disabled).
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/tts/tts_page.c')
s = p.read_text()
old = '        const size_t bl = blank_len_at(text, end, i);\n        if (bl) {'
new = '        const size_t bl = 0; /* MUTATION M2 */\n        if (bl) {'
assert old in s, 'M2 anchor missing'
p.write_text(s.replace(old, new, 1))
EOF
run_mutation "M2 tts_page: blank folding disabled" tools/run_tts_host_tests.sh

# ---- M3：NVS 开关缺省开 —— 手机不发声前就代用户决定 ----
# M3: the NVS switch defaults to on (decides for the user before any sound).
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/tts/tts_page.c')
s = p.read_text()
old = '    s_switch_on = false; // 缺省关'
assert old in s, 'M3 anchor missing'
p.write_text(s.replace(old, '    s_switch_on = true; /* MUTATION M3 */', 1))
EOF
run_mutation "M3 tts_page: switch defaults on" tools/run_tts_host_tests.sh

# ---- M4：空文本帧数 1 -> 0 —— total=0 帧永发不出去 ----
# M4: empty text plans zero frames instead of one.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/src/push_tts.c')
s = p.read_text()
old = '    if (!text || len == 0) return 1; // 空文本一帧'
assert old in s, 'M4 anchor missing'
p.write_text(s.replace(old, '    if (!text || len == 0) return 0; /* MUTATION M4 */', 1))
EOF
run_mutation "M4 push_tts: empty text plans 0 frames" tools/run_push_sink_host_tests.sh

# ---- M5：frame_at 越界不拒 —— 幻页帧会进协议 ----
# M5: frame_at stops rejecting out-of-range indices.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/src/push_tts.c')
s = p.read_text()
old = '    return false; // i 越界 / out of range'
assert old in s, 'M5 anchor missing'
p.write_text(s.replace(old, '    return true; /* MUTATION M5 */', 1))
EOF
run_mutation "M5 push_tts: frame_at accepts out-of-range" tools/run_push_sink_host_tests.sh

# ---- M6：ASCII 字符长 1 -> 2 —— 纯英文页帧计划全错 ----
# M6: ASCII characters are planned as 2 bytes each.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/src/push_tts.c')
s = p.read_text()
old = '        if ((c & 0x80u) == 0x00u) char_len = 1;        // 0xxxxxxx ASCII'
assert old in s, 'M6 anchor missing'
p.write_text(s.replace(old, '        if ((c & 0x80u) == 0x00u) char_len = 2;        /* MUTATION M6 */', 1))
EOF
run_mutation "M6 push_tts: ASCII char_len 1 -> 2" tools/run_push_sink_host_tests.sh

# ---- M7：跟随轮询删掉页变哨兵 —— 每 tick 重发当前页（复读机） ----
# M7: the follow poll loses its page-change sentinel (a repeat-every-tick page).
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/app_book.c')
s = p.read_text()
old = '    if (s_chapter == s_tts_last_chapter && s_page == s_tts_last_page) return;\n'
assert old in s, 'M7 anchor missing'
p.write_text(s.replace(old, '', 1))
EOF
run_mutation "M7 app_book: follow poll sentinel removed" tools/run_dict_hit_host_tests.sh

# ---- M8：跟随轮询删掉 link_up 门 —— 没连接也调 notify 白耗配额 ----
# M8: the follow poll loses its link_up gate (notify on a dead link).
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/app_book.c')
s = p.read_text()
old = '    if (!push_sink_link_up()) return;\n'
assert old in s, 'M8 anchor missing'
p.write_text(s.replace(old, '', 1))
EOF
run_mutation "M8 app_book: follow poll link_up gate removed" tools/run_dict_hit_host_tests.sh

echo "----"
if [ "$survived" -gt 0 ]; then
    echo "TTS MUTATIONS: $survived SURVIVED (BAD) - assertions have holes"
    exit 1
fi
echo "TTS MUTATIONS: all 8 caught (M1-M3 tts_page, M4-M6 push_tts, M7-M8 app_book wiring)"
exit 0
