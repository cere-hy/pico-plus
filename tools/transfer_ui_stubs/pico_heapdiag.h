/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：取证组件的宿主桩。传书页测试直接 #include app_transfer.c，而它现在会调
 * pico_heap_dump()。本测试关心的是界面状态机，不是堆清单——真实现要拖进
 * esp_heap_caps / freertos 一整套桩，收益为零。留成空实现，调用点仍然要过编译，
 * 「调用点存在」这件事照样被这条编译路径守着。
 * English: host stub for the evidence component. The transfer test #includes
 * app_transfer.c directly and that file now calls pico_heap_dump(). This test cares
 * about the UI state machine, not the heap listing - the real implementation would drag
 * in esp_heap_caps / freertos stubs for zero benefit. Empty body it is; the call sites
 * still have to compile, so "the call site exists" stays guarded by this path.
 */
#pragma once

static inline void pico_heap_dump(const char *site, const char *who) {
    (void)site;
    (void)who;
}
