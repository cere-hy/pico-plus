/* 中文：epdiy 旋转 API 替身。/ English: epdiy rotation API shim. */
#pragma once
#include "common.h"

enum EpdRotation {
    EPD_ROT_LANDSCAPE = 0,
    EPD_ROT_PORTRAIT = 1,
    EPD_ROT_INVERTED_LANDSCAPE = 2,
    EPD_ROT_INVERTED_PORTRAIT = 3,
};
enum EpdRotation epd_get_rotation(void);
/// 逻辑帧尺寸随方向变——横屏 1216x684、竖屏 684x1216——与 epdiy 语义一致。
/// Logical frame sizes follow the rotation - landscape 1216x684, portrait 684x1216.
static inline int epd_rotated_display_width(void) {
    return epd_get_rotation() == EPD_ROT_LANDSCAPE ? 1216 : 684;
}
static inline int epd_rotated_display_height(void) {
    return epd_get_rotation() == EPD_ROT_LANDSCAPE ? 684 : 1216;
}
