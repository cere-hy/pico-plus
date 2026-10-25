/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：见 tts_page.h。U+FFFC 的 UTF-8 编码是 EF BF BC；逐字节比对剔除，不做
 * Unicode 归一化（原文其余字节原样搬，朗读引擎自行断句）。NVS 开关照
 * vocab_log 的 rp_vocab/autosave 模式（宿主替身见 tools/tts_page_stubs/）。
 * English: see tts_page.h. U+FFFC encodes as EF BF BC in UTF-8; it is removed by
 * byte comparison with no Unicode normalisation (every other byte moves verbatim
 * and the engine does its own sentence breaking). The NVS switch follows the
 * rp_vocab/autosave pattern (host stand-ins in tools/tts_page_stubs/).
 */

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "tts_page.h"

#ifndef TTS_NVS_NS
#define TTS_NVS_NS "rp_tts"
#endif
#ifndef TTS_NVS_KEY_ENABLED
#define TTS_NVS_KEY_ENABLED "enabled"
#endif

static const char *TAG = "tts";

// U+FFFC（OBJECT REPLACEMENT CHARACTER）的 UTF-8 字节序。/ U+FFFC's UTF-8 bytes.
#define TTS_FFFC_B0 0xEFu
#define TTS_FFFC_B1 0xBFu
#define TTS_FFFC_B2 0xBCu

static bool s_switch_on; // 缺省关，init 时读 NVS / off until NVS says on

// 是 UTF-8 空白吗（ASCII 空白 + 常见全角空格 U+3000 = E3 80 80）。TTS 对全角
// 空格与 ASCII 空格同样视为静默，修掉不改变语义。/ Is UTF-8 blank (ASCII blanks
// plus the ideographic space U+3000 = E3 80 80)? TTS treats both as silence, so
// trimming them changes nothing semantically.
static size_t blank_len_at(const char *s, size_t len, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') return 1;
    if (i + 2 < len && c == 0xE3u && (unsigned char)s[i + 1] == 0x80u &&
        (unsigned char)s[i + 2] == 0x80u)
        return 3; // U+3000
    return 0;
}

size_t tts_page_extract(const char *text, size_t text_len, size_t start, size_t end,
                        char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!text || start >= end || end > text_len) return 0;
    size_t n = 0;
    for (size_t i = start; i < end && n + 1 < cap;) {
        // 剔除 U+FFFC 图片占位。/ Drop the U+FFFC image placeholder.
        if (i + 2 < end && (unsigned char)text[i] == TTS_FFFC_B0 &&
            (unsigned char)text[i + 1] == TTS_FFFC_B1 && (unsigned char)text[i + 2] == TTS_FFFC_B2) {
            i += 3;
            continue;
        }
        const size_t bl = blank_len_at(text, end, i);
        if (bl) {
            // 空白一律推迟：仅在「已产出内容且后面还有非空白内容」时补一个 ASCII
            // 空格，于是段间多空行折叠为单空格、首尾空白被修剪、FFFC 两侧的空白
            // 不叠成双空格。/ Blanks are deferred: emit one ASCII space only when
            // content was already produced and non-blank content follows, so runs
            // of blanks collapse to a single space, leading/trailing ones vanish,
            // and blanks flanking an FFFC never stack into a double space.
            size_t j = i;
            while (j < end) {
                const size_t b2 = blank_len_at(text, end, j);
                if (!b2) break;
                j += b2;
            }
            if (j < end && n > 0 && out[n - 1] != ' ' && n + 1 < cap) out[n++] = ' ';
            i = j;
            continue;
        }
        out[n++] = text[i++];
    }
    // 上述推迟策略保证尾字节非空白；双保险削掉可能的尾空格（cap 截断时）。
    // The deferral guarantees a non-blank tail; trim a possible trailing space
    // anyway (only reachable when cap truncated the range).
    while (n && out[n - 1] == ' ') out[--n] = 0;
    out[n] = 0;
    return n;
}

void tts_switch_init(void) {
    s_switch_on = false; // 缺省关：读不到、读失败、怪值都按关 / default off
    nvs_handle_t h;
    if (nvs_open(TTS_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    // 只有明确读到 1 才是开。/ Only an explicit 1 turns it on.
    if (nvs_get_u8(h, TTS_NVS_KEY_ENABLED, &v) == ESP_OK && v == 1) s_switch_on = true;
    nvs_close(h);
    ESP_LOGI(TAG, "read-aloud switch: %s", s_switch_on ? "on" : "off");
}

bool tts_switch_on(void) { return s_switch_on; }

void tts_switch_set(bool on) {
    s_switch_on = on;
    nvs_handle_t h;
    if (nvs_open(TTS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "switch set: nvs open failed (runtime updated, not persisted)");
        return;
    }
    // 写失败不回滚运行期：本次生效、重启复原，比假装持久化诚实。
    // A failed write does not roll back the runtime value: this session honors
    // it and a reboot restores the old one - honest versus faking persistence.
    if (nvs_set_u8(h, TTS_NVS_KEY_ENABLED, on ? 1 : 0) != ESP_OK ||
        nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "switch set: nvs write failed (runtime updated, not persisted)");
    nvs_close(h);
}
