#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 中文：IO 契约用例（26.9.29 常驻门禁）。用**设备同源**的 FatFs 源在宿主 RAM 盘上跑，
# 钉住桩层永远测不到的契约：路径词汇、失败码、f_open 失败清理语义。
# 找不到 IDF 时打印 SKIP（SKIP 必须出现在门禁输出里——静默跳过等于假绿）。
# English: IO contract tests against the device-identical FatFs. Prints SKIP when no IDF
# is available: a silent skip is a fake green.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

IDF="${IDF_PATH:-/opt/esp-idf}"
SRC="$IDF/components/fatfs/src"
OUT=build/excerpt-io-contract

if [ ! -f "$SRC/ff.c" ]; then
    echo "SKIP: no FatFs sources at $SRC (set IDF_PATH) - the IO contract layer did NOT run"
    exit 0
fi

mkdir -p "$OUT"
cp "$SRC/ff.c" "$SRC/ff.h" "$SRC/ffunicode.c" "$SRC/ffconf.h" "$SRC/diskio.h" "$OUT/"

# 唯一一处让步：宿主没有 FreeRTOS 互斥量。其余开关与设备一致（见 fatfs_host_config.h）。
sed -i 's/#define FF_FS_REENTRANT\t1/#define FF_FS_REENTRANT\t0/' "$OUT/ffconf.h"
if grep -q "#define FF_FS_REENTRANT	1" "$OUT/ffconf.h"; then
    echo "FAIL: could not relax FF_FS_REENTRANT for the host (FatFs version drift?)"
    exit 1
fi

flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g
       -fsanitize=address,undefined -fno-omit-frame-pointer
       -I"$OUT" -Itools/excerpt_io_contract)

gcc "${flags[@]}" \
    "$OUT/ff.c" "$OUT/ffunicode.c" \
    tools/excerpt_io_contract/ramdisk.c \
    tools/excerpt_io_contract/contract_test.c \
    -o "$OUT/contract" || exit 1

"$OUT/contract"
