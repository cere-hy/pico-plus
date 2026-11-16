/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：段落解析的**切节**——从整篇五节答案里只取【原文对照】一节的正文，
 * 折成一行、按句边界截断，交给摘抄栈落盘。纯函数，无依赖，宿主全量可测。
 * English: section cutting for a passage answer - take only the 原文对照 section
 * out of the five, fold it to one line, cut at a sentence boundary, hand it to the
 * excerpt stack. Pure functions, no dependencies, fully host-testable.
 *
 * 裁定依据（26.11.4 切割裁决）：
 *   · **谁写盘，谁拥有存储契约** —— 切割逻辑在设备（excerpts.txt 的单行契约、
 *     fold_ws 净化、1024 行帽、`[截]` 标记全在这套栈上），切割只是给它多喂一种来源；
 *   · **节标记升格为协议契约** —— 设备识别 `【原文对照】` 这件事写进 PROTOCOL.md，
 *     跨端牙断言 APK 模板含该标记。设备硬编码而不上账，就是第二处 bit2 事故：
 *     APK 改个节名，设备静默断链；
 *   · **找不到标记时诚实失败**，永不静默 —— 调用方据此亮气泡并留日志。
 * The ruling (26.11.4): whoever writes the disk owns the storage contract, so the cut
 * lives on the device; the section marker is promoted to a PROTOCOL constant so an
 * APK rename cannot silently break the link; and a missing marker fails loudly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 唯一的节标记。**协议常量**（PROTOCOL.md §七）：APK 模板必须逐字输出它，
/// 设备按它定位。改这个字符串 = 改协议，两面同时改，跨端牙会先红。
/// The one section marker, a protocol constant: the APK template must emit it
/// verbatim and the device locates by it. Changing it is a protocol change.
#define EXCERPT_SECTION_MARKER "【原文对照】"

/// 节尾判定用的行首标记起首字节（`【`，UTF-8 E3 80 90）。
/// 下一节的标题一定以它起行；用它找本节的下界，不必认识其余四节的名字。
/// The lead byte of a section header (UTF-8 `【`); every section starts a line with it,
/// so the next such line bounds this section without knowing the other four names.
#define EXCERPT_SECTION_HEAD_BYTE "\xE3\x80\x90"

/// 截断标记（与摘抄栈的 fold_ws 同形：紧贴正文，不加空格）。
/// 两处标记长得不一样会让读者以为它们不是一件事。
/// The truncation marker, byte-identical to the one fold_ws appends.
#define EXCERPT_SECTION_CUT "[截]"

typedef enum {
    /// 取到对照节（可能已按句边界截断，cut 出参说明）。
    /// Found the section (possibly sentence-cut; see the `cut` out-param).
    EXCERPT_SECTION_OK = 0,
    /// 全文里没有该节标记——中文选段的正常形态，也可能是模板坏了。调用方按
    /// 选段语言分流：中文 → 兜底存选段原文；英文 → 气泡「无对照可存」+ 日志。
    /// No marker in the answer: normal for a Chinese selection, a broken template
    /// otherwise. The caller splits by the selection's language.
    EXCERPT_SECTION_NO_MARKER,
    /// 有标记、节内为空（模型输出了标题却没内容）。
    /// The marker is there but the section is empty.
    EXCERPT_SECTION_EMPTY,
} excerpt_section_status_t;

/// 取【原文对照】一节，折成单行写入 out（NUL 结尾），*out_len = 字节数。
///
/// 三个动作按序：**定位**（标记 → 下一个行首 `【`）→ **折行**（`\r\n\t` 与连续
/// 空白折成单空格，行首空白不落，与 fold_ws 同规则——存储层的一行契约在入口
/// 就满足，落盘不再二次变形）→ **按句边界截**（超 caps 时退到最近的句末，
/// 再补 `[截]`；找不到句末才退到 UTF-8 边界硬切，同样补标记）。
///
/// 为什么截在句边界而不是字节处：句中被砍的译文比短一截更糟——读者看不出
/// 那是断的。截断必须是**声明的降级**，标记就是声明。
/// Locate, fold, then cut at a sentence boundary. Cutting mid-sentence is worse than
/// a shorter answer because the reader cannot tell it was cut; the marker declares it.
///
/// cap 是正文预算（调用方传 EXCERPT_TEXT_MAX），out 至少 cap 字节。返回值非 OK 时
/// out 内容未定义、*out_len = 0。
/// cap is the body budget (pass EXCERPT_TEXT_MAX); out holds at least cap bytes.
excerpt_section_status_t excerpt_section_pick(const char* def, size_t def_len,
                                             char* out, size_t cap,
                                             size_t* out_len, bool* cut);

#ifdef __cplusplus
}
#endif
