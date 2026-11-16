/* SPDX-License-Identifier: Apache-2.0
 * 排版宿主门（门 1）用的设置桩：只提供 ttf_font.c 真正引用的两个字体路径接口。
 * Settings shim for the typography gate: only the two font-path calls ttf_font.c uses.
 */
#pragma once
static inline const char *app_settings_font_path(void) { return "builtin"; }
static inline const char *app_settings_fonts_dir(void) { return "."; }
