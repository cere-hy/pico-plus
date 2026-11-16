/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 宿主桩：`esp_attr.h` 里与「变量躺在哪块内存」有关的那几个属性，在 PC 上没有
 * 对应物（没有 PSRAM 段，也没有 IRAM），一律展开为空。被桩掉的只有属性，不是
 * 语义——宿主上 EXT_RAM_BSS_ATTR 的缓冲仍然是普通 .bss，测试照样读写它。
 *
 * Host stub: the placement attributes from `esp_attr.h` have no PC counterpart
 * (no PSRAM segment, no IRAM), so they expand to nothing. Only the attribute is
 * stubbed, not the meaning - an EXT_RAM_BSS_ATTR buffer is plain .bss on the host
 * and the tests read and write it as usual.
 */
#pragma once

#define EXT_RAM_BSS_ATTR
#define EXT_RAM_ATTR
