/* 生成文件，勿手改 / GENERATED FILE - DO NOT EDIT.
 * 生成器 / generator: tools/gen_kern_table.py
 * 源字体 / source: main/assets/builtin.ttf
 * 字体 sha256 / font sha256: 1c5e1437ebd03691dea94c29ca4347c7162a8a0f1c9dad43a9a7ca8c1d9f3fb5
 * 对数 / pairs: 1206    upem: 1000    key: (left << 11) | right
 *
 * 键升序、运行期二分；值是**字体设计单位**，换算成像素必须与 advance 用同一把尺
 * （门 1 咬的就是这条）。字体一换，本文件的键全部失效——sha256 钉在这里就是为了让
 * `check_kern_table.py` 当场红，而不是悄悄按错的字形对去调字距。
 */
#pragma once

#include <stdint.h>

#define KERN_PAIR_COUNT 1206
#define KERN_GID_BITS 11
#define KERN_UNITS_PER_EM 1000
#define KERN_FONT_SHA256 "1c5e1437ebd03691dea94c29ca4347c7162a8a0f1c9dad43a9a7ca8c1d9f3fb5"

extern const uint32_t kern_pair_keys[KERN_PAIR_COUNT];
extern const int16_t kern_pair_values[KERN_PAIR_COUNT];
