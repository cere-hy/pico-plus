/* SPDX-License-Identifier: Apache-2.0
 * 排版增强总开关（[124] 开关批）。
 *
 * 一个开关管三条路径：正文两端对齐（`ttf_draw_text_px_fitted`）、音节断词
 * （`book_layout` 的 `take_line`）、字距（`ttf_kern_px`）。关掉 = 回到 `[122]`
 * 的经典左对齐态（无 justify / 无断词 / 无 kern）。
 *
 * 为什么必须是**一个**开关而不是三个：验收判据是「关掉后分页结果与 `[122]` 一致」。
 * 只关一半（例如保留 justify、关掉断词）会得到一个**没有历史对应物**的混搭态，
 * 那种态既没有基线可比、也没有用户语义，判据无从下手。
 * One switch for all three paths. A half-off state has no historical counterpart,
 * hence no baseline and no judge — that is exactly why it is not offered.
 */
#pragma once

#include <stdbool.h>

/// 默认 true（增强开启）。false = 经典排版：左对齐、不断词、无字距。
/// Defaults to true. False = classic typography: ragged right, no hyphenation, no kerning.
bool app_typography_enhanced(void);
