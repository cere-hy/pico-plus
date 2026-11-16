/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：黑匣子桩。只留摘抄那条事件码 + 计数，宿主不需要真的环形缓冲。
 * English: black-box stub. Only the excerpt event code and a counter; the host tests do
 * not need the real ring buffer.
 */
#pragma once

enum { PICO_BOOTBB_NOTE_EXCERPT_FAIL = 6 };

extern int test_bb_notes;

static inline void pico_bootbb_note(unsigned char code, unsigned char a, unsigned short b) {
    (void)code;
    (void)a;
    (void)b;
    ++test_bb_notes;
}
