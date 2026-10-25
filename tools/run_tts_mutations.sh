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
# 覆盖四层：
#   纯逻辑  main/tts/tts_page.c      -> run_tts_host_tests.sh
#   纯逻辑  push_sink/src/push_tts.c -> run_push_sink_host_tests.sh
#   接线    main/apps/app_book.c      -> run_dict_hit_host_tests.sh（vocab_wiring 静态守门）
#   声明/构建 push_sink.h / tts_page.h / main/CMakeLists.txt
#           -> run_dict_hit_host_tests.sh（Step10-5 补的 P7 同型头同步网 + CMake 登记网）
set -uo pipefail
cd "$(dirname "$0")/.."

cp main/tts/tts_page.c /tmp/tts_mut_page.c
cp components/push_sink/src/push_tts.c /tmp/tts_mut_plan.c
cp main/apps/app_book.c /tmp/tts_mut_book.c
cp components/push_sink/include/push_sink.h /tmp/tts_mut_sink_h.h
cp main/tts/tts_page.h /tmp/tts_mut_tts_h.h
cp main/CMakeLists.txt /tmp/tts_mut_main_cmake.txt
restore_all() {
    cp /tmp/tts_mut_page.c main/tts/tts_page.c
    cp /tmp/tts_mut_plan.c components/push_sink/src/push_tts.c
    cp /tmp/tts_mut_book.c main/apps/app_book.c
    cp /tmp/tts_mut_sink_h.h components/push_sink/include/push_sink.h
    cp /tmp/tts_mut_tts_h.h main/tts/tts_page.h
    cp /tmp/tts_mut_main_cmake.txt main/CMakeLists.txt
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
old = '    if (!up) return;\n'
assert old in s, 'M8 anchor missing'
p.write_text(s.replace(old, '', 1))
EOF
run_mutation "M8 app_book: follow poll link_up gate removed" tools/run_dict_hit_host_tests.sh

# ---- M9：push_sink.h 漏 push_sink_tts_send_page 声明 ----
# M9: push_sink.h loses the push_sink_tts_send_page declaration. P7 同型缺口：
# 宿测不编 push_sink.c（定义在，编译不需要声明），vocab 守门只读文本——
# 只有 Step10-5 补的头同步网看得见，固件 fullclean 也会炸（隐式声明）。
# M9: push_sink.h loses the push_sink_tts_send_page declaration. The P7 gap
# class: the host build never compiles push_sink.c (the definition needs no
# declaration) and the wiring guard only reads text - only the Step10-5
# header-sync net sees it; a firmware fullclean would also fail.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('components/push_sink/include/push_sink.h')
s = p.read_text()
old = 'bool push_sink_tts_send_page(const char *text, size_t len);\n'
assert old in s, 'M9 anchor missing'
p.write_text(s.replace(old, '', 1))
EOF
run_mutation "M9 push_sink.h: tts_send_page declaration removed" tools/run_dict_hit_host_tests.sh

# ---- M10：tts_page.h 漏 tts_switch_on 声明 ----
# M10: tts_page.h loses the tts_switch_on declaration. tts_page 独册直接
# #include "tts_page.c"，静态定义全可见，编译一声不吭——同 P7，只有头同步
# 网擒得住。
# M10: tts_page.h loses the tts_switch_on declaration. The tts_page booklet
# #includes the .c directly so every static definition is visible and the
# compile stays silent - same P7 class, only the header-sync net catches it.
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/tts/tts_page.h')
s = p.read_text()
old = 'bool tts_switch_on(void);\n'
assert old in s, 'M10 anchor missing'
p.write_text(s.replace(old, '', 1))
EOF
run_mutation "M10 tts_page.h: switch_on declaration removed" tools/run_dict_hit_host_tests.sh

# ---- M11：main/CMakeLists.txt 漏登记 tts/tts_page.c ----
# M11: main/CMakeLists.txt loses the tts/tts_page.c entry. 宿主测试不读
# CMake，链接期才炸——CMake 登记网（Step10-5 同批补）擒。
# M11: main/CMakeLists.txt loses the tts/tts_page.c entry. The host tests
# never read CMake; the failure would only surface at link time - caught by
# the CMake registration net (added in the same Step10-5 batch).
restore_all
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/CMakeLists.txt')
s = p.read_text()
old = '        "tts/tts_page.c"\n'
assert old in s, 'M11 anchor missing'
p.write_text(s.replace(old, '        /* MUTATION M11 */\n', 1))
EOF
run_mutation "M11 CMake: tts/tts_page.c dropped from the source list" tools/run_dict_hit_host_tests.sh

echo "----"
if [ "$survived" -gt 0 ]; then
    echo "TTS MUTATIONS: $survived SURVIVED (BAD) - assertions have holes"
    exit 1
fi
echo "TTS MUTATIONS: all 11 caught (M1-M3 tts_page, M4-M6 push_tts, M7-M8 app_book wiring, M9-M10 header sync, M11 CMake)"
exit 0
