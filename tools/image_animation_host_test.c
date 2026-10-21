#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ui_gray_lut.h"
#include "ui_image_dither.h"

/// 图片先翻成墨量过共享伽马再翻回亮度，抖动的量化基准也随之改变。
/// Images flip into ink space through the shared gamma and back, which moves the dither base.
static unsigned lifted_gray(unsigned gray) {
    return 255u - ui_gray_lut_map((uint8_t)(255u - gray));
}

static void test_dither(void) {
    for (int gray = 0; gray <= 255; ++gray) {
        uint8_t first[16];
        const unsigned lifted = lifted_gray((unsigned)gray);
        const unsigned lo = lifted / 17u * 17u;
        const unsigned hi = lo < 255u ? lo + 17u : 255u;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            uint8_t out = ui_image_dither_gray((uint8_t)gray, x, y);
            first[y * 4 + x] = out;
            assert(out % 17 == 0);
            assert(out == lo || out == hi);
            assert(out == ui_image_dither_gray((uint8_t)gray, x, y));
        }
        for (int i = 0; i < 16; ++i)
            assert(first[i] == ui_image_dither_gray((uint8_t)gray, i & 3, i >> 2));
    }
    // 端点守恒：纯黑仍是 0，纯白仍是 255。/ Endpoints stay pinned to black and white.
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        assert(ui_image_dither_gray(0, x, y) == 0);
        assert(ui_image_dither_gray(255, x, y) == 255);
    }
    // 伽马抬升：原本落在深色两级的中间调，现在量化得更深。
    // Gamma lift: midtones that used to sit at bright levels now quantize darker.
    assert(ui_image_dither_gray(119, 0, 0) < 119);
    assert(lifted_gray(128u) < 128u);
    // 单调性：输入越亮，输出的物理级不减。/ Monotonic: brighter input never lowers the level.
    unsigned previous = 0;
    for (int gray = 0; gray <= 255; ++gray) {
        const unsigned now = ui_image_dither_gray((uint8_t)gray, 0, 0);
        assert(now >= previous);
        previous = now;
    }
}

int main(void) {
    test_dither();
    puts("image dither host tests passed");
    return 0;
}
