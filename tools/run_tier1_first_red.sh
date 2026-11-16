#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# Tier 1 接入批的「最小先红」证据（2026-11-01，§13.29 档 1）。
#
# 一条资产进门禁，只证明「它跑了」；能对产品回归报红，才证明「它咬得住」。
# 本脚本对每条资产**打掉一条关键断言**，要求：
#   ① 退出码非 0；
#   ② 红在**那条**断言上（按期望字符串匹配），不是编译错、路径错、环境错。
# 跑完无条件 `git checkout -- <file>` 回滚，用 trap 兜住中途中断。
#
# 用法：bash tools/run_tier1_first_red.sh        （0 = 全部按预期见红）
# 不进日常门禁：它要改产品源码再回滚，只适合在接入批/复验时跑（见登记簿 out-ok）。
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

pass=0
fail=0
current=""

cleanup() {
    if [ -n "$current" ]; then
        git checkout -- "$current" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

inject() {
    python3 - "$1" "$2" "$3" <<'PY'
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text(encoding="utf-8")
n = text.count(old)
if n != 1:
    print("INJECT-FAIL: pattern occurs %d time(s) in %s" % (n, path))
    sys.exit(9)
path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY
}

# red <id> <file> <old> <new> <expected-substring> <cmd...>
red() {
    local id="$1" file="$2" old="$3" new="$4" expect="$5"
    shift 5
    local out rc
    current="$file"
    if ! inject "$file" "$old" "$new"; then
        echo "FAIL $id: could not inject the bite (pattern drifted)"
        git checkout -- "$file"
        current=""
        fail=$((fail + 1))
        return
    fi
    out=$("$@" 2>&1)
    rc=$?
    git checkout -- "$file"
    current=""
    if [ "$rc" -eq 0 ]; then
        echo "FAIL $id: still green after the bite was removed"
        fail=$((fail + 1))
        return
    fi
    if ! grep -qF "$expect" <<<"$out"; then
        echo "FAIL $id: red, but not on the expected assertion ($expect)"
        echo "$out" | tail -6
        fail=$((fail + 1))
        return
    fi
    echo "ok   $id  (rc=$rc, red on: $expect)"
    pass=$((pass + 1))
}

# red_soft：给编译型宿主测试用。它们各自一套失败打印（CHECK / assert / [FAIL] / red N），
# 不强行统一格式，但**排除编译错**——`error:` 出现的红不算断言红，那是注入写坏了。
red_soft() {
    local id="$1" file="$2" old="$3" new="$4"
    shift 4
    local out rc line
    current="$file"
    if ! inject "$file" "$old" "$new"; then
        echo "FAIL $id: could not inject the bite (pattern drifted)"
        git checkout -- "$file"
        current=""
        fail=$((fail + 1))
        return
    fi
    out=$("$@" 2>&1)
    rc=$?
    git checkout -- "$file"
    current=""
    if [ "$rc" -eq 0 ]; then
        echo "FAIL $id: still green after the bite was removed"
        fail=$((fail + 1))
        return
    fi
    if grep -q "error:" <<<"$out"; then
        echo "FAIL $id: red at compile time, not on an assertion"
        echo "$out" | grep "error:" | head -3
        fail=$((fail + 1))
        return
    fi
    line=$(grep -m1 -E "FAIL|Assertion|[0-9]+ red|reds=|checks,.*red|失败 [1-9]|mismatch" <<<"$out")
    if [ -z "$line" ]; then
        echo "FAIL $id: red, but no assertion line found"
        echo "$out" | tail -6
        fail=$((fail + 1))
        return
    fi
    echo "ok   $id  (rc=$rc, red on: ${line:0:96})"
    pass=$((pass + 1))
}

echo "TIER1_FIRST_RED: every asset below must go red on its own assertion."

# ---------------------------------------------------------------------------
# 6 结构牙（纯静态）
# ---------------------------------------------------------------------------
red "S1 check_26910_vocab / G2 入口 english 总闸" \
    "main/apps/app_book.c" \
    'found = hit && dict_popup_open(word);' \
    'found = hit && english && dict_popup_open(word);' \
    "G2: entry still gated by english" \
    python3 tools/check_26910_vocab.py

red "S2 check_boot_order / B5 Kconfig 默认开" \
    "main/Kconfig.projbuild" \
    'config PICO_BOOTBB
    bool "Arm the boot blackbox (26.9.15 B1, ON by default since 26.9.18)"
    default y' \
    'config PICO_BOOTBB
    bool "Arm the boot blackbox (26.9.15 B1, ON by default since 26.9.18)"
    default n' \
    "B5 PICO_BOOTBB defaults to n" \
    python3 tools/check_boot_order.py

red "S3 check_bootbb_ready / B7 采样周期 50 ms" \
    "components/pico_heapdiag/include/pico_bootbb.h" \
    'PICO_BOOTBB_PERIOD_US 50000' \
    'PICO_BOOTBB_PERIOD_US 200000' \
    "B7 PICO_BOOTBB_PERIOD_US is 200000 us" \
    python3 tools/check_bootbb_ready.py

red "S4 check_clock / C2 临时对时 6 s 上限" \
    "components/read_pico_transfer/read_pico_transfer.c" \
    'esp_timer_get_time() + 6000000;' \
    'esp_timer_get_time() + 15000000;' \
    "C2 ip_deadline is 15000000 us" \
    python3 tools/check_clock.py

red "S5 check_heapdiag / H2 总预算 >= 站点 x 每站点" \
    "components/pico_heapdiag/include/pico_heapdiag.h" \
    '#define PICO_HEAP_DUMP_TOTAL 24' \
    '#define PICO_HEAP_DUMP_TOTAL 8' \
    "H2 budget 8 <" \
    python3 tools/check_heapdiag.py

red "S6 check_hotplug / K1 lost 分支必须 remount" \
    "main/app/app_loop.c" \
    '    s_last_release = read_pico_sd_remount();
    s_release_known = true;' \
    '    s_last_release = ESP_OK; /* bite: remount call removed */
    s_release_known = true;' \
    "K1 the lost branch never calls read_pico_sd_remount()" \
    python3 tools/check_hotplug.py

# ---------------------------------------------------------------------------
# 11 宿主 runner（裁定档 1）+ dict_hit（§13.32 本轮修绿，明示随本批）
# ---------------------------------------------------------------------------
red_soft "R1 run_ai_reader / 分页行数" \
    "main/apps/ai_reader.c" \
    '        if (lines >= lines_per_page) {' \
    '        if (lines > lines_per_page) {' \
    bash tools/run_ai_reader_host_tests.sh

red_soft "R2 run_ai_scene / 0x1A 分帧规划" \
    "components/push_sink/src/push_proto.c" \
    '        off += step ? step : 1;
        ++count;' \
    '        off += (step ? step : 1) * 2;
        ++count;' \
    bash tools/run_ai_scene_host_tests.sh

red_soft "R3 run_ai_scene_view / arm 字段打包" \
    "main/apps/scene_req.c" \
    '    r->word_off = word_off;' \
    '    r->word_off = (uint16_t)(word_off + 1);' \
    bash tools/run_ai_scene_view_host_tests.sh

red_soft "R4 run_book_id / FNV-1a 基值" \
    "main/book/book_id.c" \
    '#define BOOK_FNV_BASIS 2166136261u' \
    '#define BOOK_FNV_BASIS 2166136262u' \
    bash tools/run_book_id_host_test.sh

red_soft "R5 run_epub / CSS 选择器小写化" \
    "main/book/html_text.c" \
    "    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;" \
    "    return c; /* bite: no lowercasing */" \
    bash tools/run_epub_host_tests.sh

red_soft "R6 run_image_animation / dither 端点" \
    "main/ui/ui_image_dither.c" \
    '    const unsigned base = lifted / 17u;' \
    '    const unsigned base = lifted / 16u;' \
    bash tools/run_image_animation_tests.sh

red_soft "R7 run_reading_origin / 首页续读卡命中矩形" \
    "main/apps/app_dashboard.c" \
    '    return control.height == 378 ? (EpdRect){56,270,184,252} : control;' \
    '    return control.height == 378 ? (EpdRect){56,270,184,253} : control;' \
    bash tools/run_reading_origin_host_test.sh

red_soft "R8 run_transfer_ui / 退格真的删掉一个字符" \
    "main/ui/ui_text_edit.c" \
    '    ui_text_edit_place(edit, old - 1);' \
    '    ui_text_edit_place(edit, old); /* bite: backspace no longer steps back */' \
    bash tools/run_transfer_ui_host_tests.sh

red_soft "R9 run_tts / 空白折叠为单空格" \
    "main/tts/tts_page.c" \
    "    if (c == ' ' || c == '\\t' || c == '\\n' || c == '\\r' || c == '\\v' || c == '\\f') return 1;" \
    "    if (c == ' ' || c == '\\t' || c == '\\n' || c == '\\r' || c == '\\v' || c == '\\f') return 0;" \
    bash tools/run_tts_host_tests.sh

red_soft "R10 run_ui_dualgesture / 纵轴档位量化" \
    "main/ui/ui_dualgesture.c" \
    '                    // quantifies travel, it is not a font value.
                    dual->axis = 2;
                    dual->peak_steps = clamp_steps(dy / UI_G_SWIPE_STEP_PX);' \
    '                    // quantifies travel, it is not a font value.
                    dual->axis = 2;
                    dual->peak_steps = clamp_steps(dy / (UI_G_SWIPE_STEP_PX * 2));' \
    bash tools/run_ui_dualgesture_host_tests.sh

red_soft "R11 run_wifi_qr / SSID 长度上限" \
    "main/ui/ui_wifi_qr.c" \
    'strlen(ssid) > 32 || strlen(password) > 64' \
    'strlen(ssid) > 8 || strlen(password) > 64' \
    bash tools/run_wifi_qr_host_test.sh

# §13.32 的 INDIRECT：绘制侧入口体内必须仍委托 justify_target_px。
# 这正是本轮修的那条等价链——删掉委托（改成手抄算术），文本级断言必须红。
red_soft "R12 run_dict_hit / line_ink_width 必须仍委托 justify_target_px" \
    "main/book/book_layout.c" \
    '    return line_is_justified(heading, align, final_line, line_width, available)
               ? justify_target_px(line_width, available, rect_x, line_px)
               : line_width;' \
    '    const int hang = rect_x > 4 ? line_px / 3 : 0;
    (void)hang;
    return line_is_justified(heading, align, final_line, line_width, available) ? available
                                                                               : line_width;' \
    bash tools/run_dict_hit_host_tests.sh

# ---------------------------------------------------------------------------
# 自带先红的资产：judge_push_smoke 的测试本身就是「七向先红」——
# 每个判据造一份假日志，判读器必须指名道姓地红一次。接入它等于接入一组先红。
# ---------------------------------------------------------------------------
self_red() {
    local id="$1" expect="$2"
    shift 2
    local out rc
    out=$("$@" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ] || ! grep -qF "$expect" <<<"$out"; then
        echo "FAIL $id: self-red suite did not report its own coverage"
        echo "$out" | tail -6
        fail=$((fail + 1))
        return
    fi
    echo "ok   $id  ($(grep -m1 -E '七向|通过' <<<"$out"))"
    pass=$((pass + 1))
}

self_red "J1 test_judge_push_smoke / 自带七向先红" "七向 7 通过" \
    python3 tools/test_judge_push_smoke.py

echo
echo "TIER1_FIRST_RED_SUMMARY: $pass ok / $fail failed"
[ "$fail" -eq 0 ]