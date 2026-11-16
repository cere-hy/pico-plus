#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 中文：验证主页面黑白/灰阶分步、横条局部移动与底栏边界。/ English: Verify staged black/white and gray main updates, locally moving markers and navigation bounds.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/book-tests
link_gc=(-Wl,--gc-sections)
if [[ "$(uname -s)" == "Darwin" ]]; then link_gc=(-Wl,-dead_strip); fi
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections "${link_gc[@]}" \
    -Itools/display_host_stubs -Icomponents/continuous_du/include -Icomponents/e0470_page_turn/include -Imain -Imain/app \
    tools/display_navigation_host_test.c main/display.c main/ui/ui_image_dither.c main/ui/ui_gray_lut.c -lm -o build/book-tests/display-navigation
build/book-tests/display-navigation
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Imain -Imain/app \
    -Itools/waveform_host_stubs -Icomponents/epdiy/include -Icomponents/epdiy/src \
    -Icomponents/e0470_epaper_waveform/include -Icomponents/e0470_epaper_waveform/waveforms \
    tools/navigation_waveform_host_test.c \
    components/e0470_epaper_waveform/e0470_epaper_waveform.c \
    components/e0470_epaper_waveform/e0470_waveform_trim.c \
    main/display_main_waveform.c main/ui/ui_image_dither.c main/ui/ui_gray_lut.c -lm -o build/book-tests/navigation-waveform
build/book-tests/navigation-waveform

# 26.11.7：reader-waveform 这一段随 display_reader_waveform 模块一起退休。那张表只为
# GL16 排相序（白底补不回来），S1 换成 E0470 的 GC16 之后没有调用方，测它等于给一个
# 不存在的模块站岗。/ 26.11.7: the reader-waveform block retired with the module it
# tested - the table was ordered for GL16 only, and S1 left it without a caller.
