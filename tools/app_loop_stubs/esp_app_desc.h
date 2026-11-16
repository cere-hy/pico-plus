/* 中文：app_desc 替身——提供 26.9.7 起 app_loop 需要的 version 字段，
 * 以及 26.11.4 B62 启动横幅要的前 6 字节 ELF 指纹。
 * 版本串故意写成与 CMakeLists 的 PROJECT_VER 同形（0.3.7-pico26.9.7），让 fw_tag()
 * 的解析路径在宿主上也真的跑一遍；解析失败的 `[??]` 分支由用例单独覆盖。
 * ELF 指纹填死值：宿主横幅的职责是「打得出来、打得对」，不是复现真卡的哈希序列。
 * English: app_desc shim - the `version` field app_loop needs since 26.9.7 plus the first
 * six bytes of the ELF fingerprint that the 26.11.4 B62 banner prints. The string mirrors
 * PROJECT_VER (0.3.7-pico26.9.7) so fw_tag()'s parser really runs on the host too; the
 * `[??]` fallback is covered by its own case. The sha is a fixed value on purpose: the
 * host banner proves it prints and prints right, not that a card's hash sequence repeats. */
#pragma once

#include <stdint.h>

typedef struct {
    char version[32];
    uint8_t app_elf_sha256[32];
} esp_app_desc_t;

static inline const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t desc = {
        "0.3.7-pico26.9.7",
        { 0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
          0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
          0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c }
    };
    return &desc;
}
