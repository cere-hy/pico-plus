#!/bin/sh
# 26.11.20 出厂 bin 线刷恢复兜底脚本
# 包内 Read_Pico.bin 是唯一可刷的 app；自检锚定它的 sha256，不符即拒。
#
# 用法 / Usage:
#   sh flash.sh /dev/ttyUSB0          # Linux / macOS（默认 460800）
#   sh flash.sh /dev/ttyUSB0 230400   # 中途报错或校验不过时降速
#   sh flash.sh COM3                  # Windows (Git Bash / MSYS2)
#
# 烧前把完整输出存盘（esptool 的哈希行是烧录证据）：
#   sh flash.sh /dev/ttyUSB0 2>&1 | tee flash-20261106-decouple.log
#
# 本包与上一包（短码 5e6f8a89e039 / 4,216,784 B）**只差 app 内容**：
# bootloader / 分区表 / ota_data 三个辅助件逐字节相同，分区布局未动。

set -e
PORT="${1:-/dev/ttyUSB0}"
BAUD="${2:-460800}"
DIR="$(cd "$(dirname "$0")" && pwd)"

EXPECTED="e9ce5b41a3dd9a767468ee841a5c1b41e94496e10bd9339b9878a8f337b2810c"
if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL=$(sha256sum "$DIR/Read_Pico.bin" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
    ACTUAL=$(shasum -a 256 "$DIR/Read_Pico.bin" | cut -d' ' -f1)
else
    ACTUAL=""
    echo "WARN: 找不到 sha256 工具，跳过自检（建议先手工核对 SHA256SUMS）" >&2
fi

if [ -n "$ACTUAL" ] && [ "$ACTUAL" != "$EXPECTED" ]; then
    echo "FATAL: Read_Pico.bin sha256 不符，拒绝烧录。" >&2
    echo "  expected: $EXPECTED" >&2
    echo "  actual:   $ACTUAL" >&2
    exit 1
fi
[ -n "$ACTUAL" ] && echo "sha256 OK: $ACTUAL  (e9ce5b41a3dd / 4,217,088 B)"

echo "烧完先核版本串：0.3.9-pico26.11.20（串口横幅与设置页两处一致）"
echo "主 bin 必须落在 0x10000；写到 0x0 会覆盖 bootloader / 分区表 / NVS / phy_init。"

if command -v python3 >/dev/null 2>&1; then
    ESPTOOL="python3 -m esptool"
else
    ESPTOOL="esptool"
fi

$ESPTOOL --chip esp32s3 -p "$PORT" -b "$BAUD" \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0      "$DIR/bootloader.bin" \
  0x8000   "$DIR/partition-table.bin" \
  0x10000  "$DIR/Read_Pico.bin" \
  0xb10000 "$DIR/ota_data_initial.bin"

echo "Flash done. 版本串 0.3.9-pico26.11.20 核对后再开真机卡 B 组。"
