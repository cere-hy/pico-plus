@echo off
rem 生产版烧写脚本（Windows）/ Flash script for the PRODUCTION build (Windows)
rem
rem 用法 / Usage:
rem   flash.bat COM3
rem   flash.bat COM3 230400        （烧录报错/校验不过时降波特率重试）
rem
rem 警告 / WARNING: Read_Pico.bin 必须落在 0x10000。写到 0x0 会覆盖 bootloader /
rem                 分区表 / NVS / phy_init。
rem                 Read_Pico.bin MUST go to 0x10000; 0x0 would overwrite the
rem                 bootloader, partition table, NVS and phy_init.
rem
rem 四个偏移对应 partitions_16M.csv 的 5.5MB app 布局：ota_0 在 0x10000、
rem ota_1 在 0x590000，故 otadata 落在 0xb10000。不是上游默认的 4MB 布局。
rem These offsets match our partitions_16M.csv 5.5MB app layout; otadata is at
rem 0xb10000 - not upstream's 4MB default.
rem
rem 烧前请把完整输出存盘（esptool 的哈希/摘要行是烧录证据）。
rem Save the FULL output (esptool hash lines are the flashing evidence).

setlocal enabledelayedexpansion
rem 锚：gen26.3 五项终版 Read_Pico.bin 4,070,704 B / d441107b…（2026-10-24 重锚，
rem   与 firmware/flash.sh 同值；三代未更新的 gen22 锚 fdd48866… 会把正确 bin 也
rem   拒烧——certutil 拒烧语义同 flash.sh：exit 1）。
rem Anchor: the gen26.3 five-item final Read_Pico.bin 4,070,704 B / d441107b...
rem (re-anchored 2026-10-24, same value as firmware/flash.sh; the three-generation
rem stale gen22 anchor fdd48866... would refuse even the correct bin - certutil
rem refusal semantics match flash.sh: exit 1).
set EXPECTED=d441107b6d6c0fddf1ae56cf5be5474922ae81d4e3db5b5649540a21907f04cc
set DIR=%~dp0
set PORT=%1
if "%PORT%"=="" set PORT=COM3
set BAUD=%2
if "%BAUD%"=="" set BAUD=460800

rem certutil 输出第二行才是哈希；取到即跳出 / certutil prints the hash on line 2.
set ACTUAL=
for /f "skip=1 tokens=1" %%a in ('certutil -hashfile "%DIR%Read_Pico.bin" SHA256') do (
  if not defined ACTUAL (
    set ACTUAL=%%a
  )
)

if not defined ACTUAL (
  echo WARN: certutil unavailable; skipping pre-flash check
) else (
  if /i not "%ACTUAL%"=="%EXPECTED%" (
    echo FATAL: Read_Pico.bin sha256 mismatch - refusing to flash.
    echo   expected: %EXPECTED%
    echo   actual:   %ACTUAL%
    echo   Check package integrity; do NOT flash a bin off its anchor.
    exit /b 1
  )
  echo sha256 OK: %ACTUAL% ^(production build, anchor verified^)
)

python -m esptool --chip esp32s3 -b %BAUD% --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 "%DIR%bootloader.bin" 0x8000 "%DIR%partition-table.bin" 0x10000 "%DIR%Read_Pico.bin" 0xb10000 "%DIR%ota_data_initial.bin"

echo Flash done (production build).
endlocal
