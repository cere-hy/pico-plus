#!/bin/sh
set -eu
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 tools/power_dialog_host_test.py --input-c "$out/power.c"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Itools/app_loop_stubs -Icomponents/pico_heapdiag/include -Imain -Icomponents/ble_page_turner/include -Icomponents/push_sink/include -Imain/app -Imain/apps -Imain/ui -Itools/ui_gesture_stubs \
  "$out/power.c" tools/app_loop_host_test.c main/app/app_loop.c main/apps/transfer_commit.c main/ui/ui_gesture.c main/ui/ui_dualgesture.c main/ui/ui_quick_menu_input.c main/ui/nav_status_watch.c main/apps/home_gesture.c -o "$out/test"
"$out/test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Itools/app_loop_stubs -Icomponents/push_sink/include -Imain/app -Imain/ui -Itools/ui_gesture_stubs \
  tools/ui_quick_input_host_test.c main/ui/ui_quick_menu_input.c -o "$out/quick"
"$out/quick"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Itools/app_loop_stubs -Icomponents/pico_heapdiag/include -Imain -Icomponents/ble_page_turner/include -Icomponents/push_sink/include -Imain/app -Imain/apps -Imain/ui -Itools/ui_gesture_stubs \
  tools/ui_menu_host_test.c main/ui/ui_menu.c -o "$out/menu"
"$out/menu"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Itools/sd_probe_stubs -Icomponents/read_pico/include \
  tools/sd_probe_host_test.c -o "$out/sd_probe"
"$out/sd_probe"

python3 tools/lock_feedback_host_test.py

python3 tools/power_dialog_host_test.py

# 判决行（契约）：只有全绿才走到这里（set -e）；门禁按它断言「跑到底了」，不是「跑了」。
# Verdict line (a contract): reached only when everything passed; the gate asserts it to tell
# "finished" from "truncated".
echo "JUDGEMENT: APP_LOOP PASS"
