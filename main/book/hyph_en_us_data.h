/* 生成文件，勿手改 / GENERATED FILE - DO NOT EDIT.
 * 生成器 / generator: tools/gen_hyphen_table.py
 * 源 / source: tools/vendor/hyph-en-us.tex (Knuth hyphen.tex, lg_english)
 * 源 sha256 / source sha256: 2c18acdc04c1a066aeb1759905e7ca449f0616c314b5ed6aebe55b9d4a89b8d4
 * 模式 / patterns: 4447   例外 / exceptions: 14
 * 最小切长 / left/right min: 2/3
 * 重新生成 / regenerate: python3 tools/gen_hyphen_table.py
 *
 * 为什么把 sha256 与条数钉在文件里：表是抄来的，算法是写的。表被替换、模式被
 * 增删、源文件被改，三个数字里必有一个对不上，check_hyphen_table.py 当场红。
 */
#pragma once

#include <stdint.h>

#define HYPH_EN_US_SOURCE_SHA256 "2c18acdc04c1a066aeb1759905e7ca449f0616c314b5ed6aebe55b9d4a89b8d4"
#define HYPH_EN_US_PATTERN_COUNT 4447
#define HYPH_EN_US_EXCEPTION_COUNT 14
#define HYPH_EN_US_MAX_PATTERN 8
#define HYPH_EN_US_LEFT_MIN 2
#define HYPH_EN_US_RIGHT_MIN 3

extern const char hyph_en_us_blob[17996];
extern const uint32_t hyph_en_us_off[4448];
extern const uint32_t hyph_en_us_dig[4447];
extern const char hyph_en_us_exc_blob[134];
extern const uint32_t hyph_en_us_exc_off[15];
extern const uint32_t hyph_en_us_exc_mask[14];

