#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 在仓库根运行管理功能回归；网页脚本另用 Node 验证。
# Run management regressions from the repository root; validate web interactions separately with Node.
set -euo pipefail

# 放行「已登记形态」的失败（tools/KNOWN_FAILURES.md §7）：只认登记过的关键字，
# 其它任何失败形态照旧让 runner 红——隔离不等于放弃守门。
# Tolerate ONLY the registered failure shapes (KNOWN_FAILURES.md §7); every other shape
# still fails the runner - isolation is not abandonment.
KNOWN_HITS=0
tolerant() {
    local tag="$1" shape="$2"; shift 2
    local log; log=$(mktemp)
    if "$@" >"$log" 2>&1; then
        tail -1 "$log"; rm -f "$log"; return 0
    fi
    if grep -qE "$shape" "$log"; then
        KNOWN_HITS=$((KNOWN_HITS + 1))
        # 2026-11-01 起：跳过允许存在，但不许在输出里隐形。每次放行必须留一行，
        # 带测试名与**实际命中片段**——`test_sd_media_guard` 案就是靠「输出全绿、
        # 却从未跑过」藏了一整年：tolerant 按 `fatal error: esp_timer.h` 把头文件
        # 缺失当成已登记形态吞了，而门禁只看到 exit 0。
        # Since 2026-11-01 a skip is allowed but never invisible: one line per tolerance,
        # with the test name and the exact text that matched. That is how the
        # `test_sd_media_guard` case hid a never-compiled test behind a green gate.
        local hit
        hit=$(grep -m1 -oE "$shape" "$log" | cut -c1-100)
        echo "$tag: TOLERATED-SKIP — did NOT pass; matched: ${hit:-<unprintable>}"
        rm -f "$log"; return 0
    fi
    cat "$log"; rm -f "$log"; return 1
}

# 登记簿先过账本牙：字段齐全、条数与下面 tolerant 的实际放行数相等、无过期条目。
# 到期未还账即红——「只增不减的登记簿是坟场」，这条让坟场长不出草。
# The ledger tooth runs first: fields complete, row count equals the tolerant calls below,
# nothing overdue. An overdue row fails the gate instead of quietly ageing.
python3 tools/check_known_failures.py

# 送验台账：验收对象是**跑六步当时的那个 bin**。这条咬「台账最新一行 == 当前产物」，
# 也咬「bin 变了还写 CARRIED（沿续）」。没有产物时打印 SKIP 而不是静默跳过。
# Acceptance ledger: the field run is bound to the exact image. This bites "newest row equals
# the current artifact" and "CARRIED is illegal once a byte moves"; no artifact prints SKIP.
python3 tools/check_pkg_validity.py
# 掉盘往返的结构牙：掉盘关书源、插回先重开且必须排在 load_chapter 之前。
# 26.11.1 真机「插回卡被踢回书架」= book_close() 后不重开就 load，签名
# ESP_ERR_INVALID_ARG。行为牙在 book_source_host_test（重开前后各一条），
# 这条补的是 app 层「有没有那一句」——删掉它所有宿主测试仍全绿。
# Structural tooth for the offline round trip: closing the source on removal and
# reopening before load on return. The field bounce-to-shelf had exactly this shape.
python3 tools/check_offline_reopen.py
# 摘抄反馈的结构牙：入队必须当场在屏幕上说话（顶部气泡，全屏也画），队列深度的
# 页脚基线必须跨拍比对。26.11.1 真机：无卡连摘 3 条、冲账成功，屏幕一个字没有——
# 旧 tick 判据 `before == after` 在入队场景恒真（基线采在入队之后），页脚永不重画；
# 而页脚在全屏阅读不画、离开阅读页就失效。删掉那两句，所有宿主测试仍全绿。
# Structural tooth for excerpt feedback: the enqueue branch must speak on screen and the
# footer depth baseline must live across ticks (an enqueue between ticks is invisible to a
# baseline sampled inside the tick).
python3 tools/check_excerpt_feedback.py
# 26.11.2 P0-4/P0-5/P1-2 的结构牙：enqueue 必须过净化、渲染必须 tmp→rename+软顶、
# peek 必须零出队、书架三处命中矩形互斥且有编译期断言。带五向负向自检。
# Structural teeth for 26.11.2: the sanitize fold on enqueue, tmp-then-rename with the
# render cap, the zero-dequeue peek, and the mutually-exclusive shelf hit rectangles.
python3 tools/check_excerpt_view.py
# 26.11.3 真机两缺陷的结构牙：三按钮字号恒等、文案放得下（字节级实测字宽）、
# 分类感应区零重叠 + 按钮优先守卫、边界 332/333 归属互换。带十向负向自检。
# Structural teeth for the two field defects: one font size across the buttons, a label
# that actually fits (width measured off the ttf), a zero-overlap sensing rect with a
# button-first guard, and the 332/333 boundary ownership. Ten self-kill mutations.
python3 tools/check_shelf_header_hit.py
bash tools/run_book_host_tests.sh
python3 tools/test_book_epub.py
mkdir -p build/book-tests/settings-card
cc -std=gnu11 -Wall -Wextra -Werror -Itools/settings_backup_stubs \
    -Icomponents/read_pico_transfer/include \
    tools/settings_backup_host_test.c -o build/book-tests/settings-backup
build/book-tests/settings-backup
cc -std=c11 -Wall -Wextra -Werror -Imain tools/auto_lock_host_test.c -o build/book-tests/auto-lock
build/book-tests/auto-lock
cc -std=gnu11 -Wall -Wextra -Werror -Itools/settings_backup_stubs \
    tools/book_history_backup_host_test.c -o build/book-tests/book-history-backup
build/book-tests/book-history-backup
# book_ui_host_test.py / book_layout_draw_host_test.py：切片式 harness，切片清单落后于
# app_book.c 的增长（S26-2 起 on_key 变包装、动作栏引入 boxsel_*、正文记录区引入
# inv_* 辅助），属**已登记的已知红灯**（KNOWN_FAILURES §7）；test_sd_media_guard.py 需要
# IDF 头文件路径，属宿主环境依赖。只放行登记形态。
# Sliced harnesses whose slice lists fell behind app_book.c (registered §7), plus one
# harness that needs IDF headers. Only the registered shapes are tolerated.
# known-fail: KF-7.1
tolerant book_ui 'BOXSEL_EXIT_KEY_LONG|implicit declaration of function .on_key_impl.|book_store_free_snapshot' python3 tools/book_ui_host_test.py
python3 tools/reader_fullscreen_host_test.py
python3 tools/reader_controls_host_test.py
python3 tools/reader_ui_scale_host_test.py
python3 tools/reader_settings_panel_host_test.py
python3 tools/reader_page_map_host_test.py
python3 tools/lock_font_host_test.py
python3 tools/file_sd_format_host_test.py
python3 tools/lock_collage_host_test.py
python3 tools/lock_pin_host_test.py
python3 tools/ui_pinpad_host_test.py
python3 tools/lock_screen_host_test.py
python3 tools/lock_style_host_test.py
python3 tools/shelf_fast_cache_host_test.py
python3 tools/bookshelf_list_host_test.py
python3 tools/list_cover_cache_host_test.py
python3 tools/home_cover_cache_host_test.py
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -Itools/settings_backup_stubs -Imain \
    tools/boot_state_host_test.c -o build/book-tests/boot-state
python3 tools/boot_recovery_host_test.py
build/book-tests/boot-state
bash tools/run_text_input_host_tests.sh
python3 tools/reader_images_host_test.py
python3 tools/book_layout_single_column_host_test.py
# known-fail: KF-7.2
tolerant book_layout_draw 'line_ink_width|hit_line_source_offset|inv_dirty_add|justify_target_px' python3 tools/book_layout_draw_host_test.py
python3 tools/ui_font_coverage_host_test.py
python3 tools/ui_hanzi_host_test.py
# AA 契约重建完成（2026-10-16，KNOWN_FAILURES.md 第 2 条关闭）：判据从「BW 恒等」重造为
# 「灰度域 + AA 参考对照 + 结构」三层 oracle（三变异验证有牙：单侧退化 memcmp 擒、双侧
# 同时退化 gray_px 擒）。隔离分支随之退役——测试必须直绿，任何失败都是新缺陷。
# AA contract rebuilt (2026-10-16, KNOWN_FAILURES.md entry 2 closed): the criterion was
# rebuilt from "BW identity" into a three-layer oracle (grayscale domain + AA reference +
# structure), mutation-verified to have teeth (one-sided degradation caught by memcmp,
# both-sided degradation caught by the gray-domain assert). The quarantine branch retires
# with it — the test must pass outright; any failure is a new defect.
python3 tools/title_font_host_test.py
# 上游自带过期测试：仍然运行以保持可见，但只放行登记形态（编译层缺 r93 on_enter
# 符号，首个报错 `s_cover_generation`）。换成别的失败形态（如运行期刷新断言）就是
# 新缺陷，照旧拦。账期与补偿方见 §7 KF-7.4。
# Upstream ships the stale test itself: still run so it stays visible, but only the
# registered shape is tolerated. Any other shape is a new defect and still fails.
# known-fail: KF-7.4
tolerant home_recent 's_cover_generation' python3 tools/test_home_recent.py
python3 tools/test_book_toc.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itools/book_cover_auto_stubs -Imain/book -Imain/font \
    tools/book_cover_auto_host_test.c main/book/book_cover_auto.c -o build/book-tests/auto-cover
build/book-tests/auto-cover
cc -std=c11 -Wall -Wextra -Werror -Imain/ui \
    tools/wallpaper_crop_host_test.c -o build/book-tests/wallpaper-crop
build/book-tests/wallpaper-crop
python3 tools/test_search.py
flags=(-std=gnu11 -Wall -Wextra -Werror -Wno-deprecated-declarations -g -fsanitize=address,undefined)
gcc "${flags[@]}" -Itools/file_tree_stubs -Imain/apps tools/file_tree_host_test.c main/apps/file_tree.c -o build/book-tests/file-tree
build/book-tests/file-tree
gcc "${flags[@]}" -Icomponents/read_pico_search/include tools/transfer_host_test.c components/read_pico_search/read_pico_search.c -o build/book-tests/transfer
build/book-tests/transfer
gcc "${flags[@]}" -Imanaged_components/espressif__cjson/cJSON tools/transfer_wifi_host_test.c managed_components/espressif__cjson/cJSON/cJSON.c -lm -o build/book-tests/transfer-wifi
build/book-tests/transfer-wifi
python3 tools/test_transfer_wifi_forget.py
gcc "${flags[@]}" -Itools/transfer_ui_stubs -Icomponents/read_pico_transfer/include -Icomponents/read_pico_search/include -Imain/ui -Icomponents/ble_page_turner/include \
    tools/transfer_ui_host_test.c main/ui/ui_text_edit.c main/ui/ui_text_input.c main/ui/ui_keyboard.c main/ui/ui_ime.c components/read_pico_search/read_pico_search.c main/apps/transfer_commit.c -o build/book-tests/transfer-ui
python3 tools/test_usb_storage.py
build/book-tests/transfer-ui
bash tools/run_app_loop_host_tests.sh
# gen26.3 词卡查询态 + AI 释义容量（第 15 runner）。
# gen26.3 querying card + AI definition capacity (runner 15).
bash tools/run_dict_popup_host_tests.sh
bash tools/run_display_host_test.sh
bash tools/run_display_navigation_tests.sh
bash tools/run_water_turn_host_test.sh
python3 tools/test_lcd_frame_lifecycle.py
# known-fail: KF-7.3
# 合流（2026-11-01）把这一条从「任意编译失败」收窄到「恰好一条过期断言」：
# 桩层已补齐（esp_rom_sys / esp_timer / pico_bootbb / ESP_LOGx / heap_caps / host_init /
# taskYIELD / uxTaskGetStackHighWaterMark），编译不再被容忍——**桩再缺一个就红**。
# 剩下这一条咬的是 31a 改过的语义（`detect_fail` 时快照该说什么），按新状态机核对
# 属独立小批（B59），不在这里顺手改判据。
# Narrowed from "any compile failure" to exactly one stale assertion: the stub layer is
# complete now, so a missing symbol is red again. The remaining one bites 31a's semantics.
tolerant sd_media_guard 'Assertion .*capacity_bytes.*failed' python3 tools/test_sd_media_guard.py

python3 tools/test_transfer_netif.py
python3 tools/test_pmu_battery.py
python3 tools/ble_turner_host_test.py
for test in ota_release ota_online; do
    # R5：宏默认已是 0（启用自家源）；这里仍显式 -D=0，保证管线用例不随默认值漂移。
    # The default is now 0 (self-hosted feed enabled); keep -D=0 explicit so the pipeline
    # tests do not silently depend on whatever the header happens to default to.
    # -D_GNU_SOURCE：Linux/glibc 需要它才看得到 strnlen 与 PTHREAD_MUTEX_RECURSIVE。
    # -D_GNU_SOURCE: Linux/glibc needs it to expose strnlen and PTHREAD_MUTEX_RECURSIVE.
    cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-deprecated-declarations \
        -DPICO_ONLINE_UPDATES_DISABLED=0 -fsanitize=address,undefined \
        -Itools/ota_stubs -Imain -Imanaged_components/espressif__cjson/cJSON \
        "tools/${test}_host_test.c" main/ota_release.c managed_components/espressif__cjson/cJSON/cJSON.c \
        -lm -pthread -o "build/book-tests/${test}"
    "build/book-tests/${test}"
done
# R5：默认开关已改为 0（启用），故这里显式 -D=1 单编一次，验证 kill switch 断开路径
# （不建任务 / 不联网 / 不动启动槽）。不再依赖「默认即停用」的旧语义。
# The default is now 0; compile once with -D=1 explicitly to prove the kill switch still cuts
# the line (no worker, no feed, no boot slot change).
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=address,undefined \
    -DPICO_ONLINE_UPDATES_DISABLED=1 \
    -Itools/ota_stubs -Imain -Imanaged_components/espressif__cjson/cJSON \
    tools/ota_online_host_test.c main/ota_release.c managed_components/espressif__cjson/cJSON/cJSON.c \
    -lm -pthread -o build/book-tests/ota_online_disabled
build/book-tests/ota_online_disabled
python3 tools/ota_disabled_host_test.py
python3 tools/ota_ui_host_test.py

python3 tools/image_page_host_test.py
python3 tools/pmic_gate_host_test.py
python3 tools/settings_scroll_host_test.py
python3 tools/ui_refresh_feedback_host_test.py

# 登记条数入眼。注意本 runner **嵌套调用 run_book_host_tests.sh**（它的 1 条会先打印），
# 所以这里只报「本 runner 自身」的 4 条，行尾带 runner 名以免把 1 和 4 读混。
# 总数看开头那行：5 registered = 1 (book_host) + 4 (management)。
# Print this runner's own count. The nested book_host runner prints its 1 first, so the
# lines carry the runner name to keep 1 and 4 from being misread as a mismatch.
echo "known-failures: ${KNOWN_HITS} tolerated (run_book_management_tests.sh; registry: tools/KNOWN_FAILURES.md §7)"

# 判决行（契约，不是注释）：**只有全绿才会走到这一行**（set -e），而门禁按这一行断言
# 「runner 跑到底了」。少了它，一个被截断的 runner 与一个真跑完的 runner 在日志里长得一样
# ——26.11.5 那次 92 s 的半截账就是这么骗过三遍关键字扫描的。
# Verdict line (a contract, not a comment): only reached when everything above passed, so
# the gate can tell "finished" from "truncated" - those two look identical in a log.
echo "JUDGEMENT: BOOK_MGMT PASS tolerated=${KNOWN_HITS}"
