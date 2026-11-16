/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * M3-B：情景请求待命态——**纯状态机**，无 IO、无绘制、无时钟读取（now 由调用方
 * 传入）。抽出来的唯一理由：这段逻辑决定「一条 0x08 归不归情景视图」，错了就是
 * 把 A 页的分析贴到 B 页上，必须能在宿主上逐条断言，而不是埋在 app_book 里靠
 * 真机碰。
 * M3-B: the scene-request pending state - a pure state machine with no IO, no
 * painting and no clock reads (the caller passes `now`). It is extracted for one
 * reason: this logic decides whether a 0x08 belongs to the scene view, and getting
 * it wrong pins page A's analysis onto page B. That must be asserted line by line
 * on the host instead of being discovered on hardware.
 *
 * 关联键三项（协议零改动的代价）：**词 + 发起页 + 期限**。0x08 帧里没有「这是
 * 情景答案」的标记，设备只能靠这三者互认；任何一项对不上就不认。
 * Correlation key (the price of zero protocol change): word + originating page +
 * deadline. The 0x08 frame carries no "this is a scene answer" mark, so the device
 * can only match on those three; any mismatch means "not mine".
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 词上限与 DICT_WORD_MAX 同源（点词缓冲就是这么大）。/ Word cap, same as DICT_WORD_MAX.
#define SCENE_REQ_WORD_MAX 64

/// 待命态。armed 为 true 表示「0x1A 已发出，等一条 0x08」。
/// The pending state. armed means "0x1A went out, waiting for one 0x08".
typedef struct {
    bool armed;
    /// 整段选文（bit2）：答案open「选段」视图而不是词卡。/ whole passage (bit2).
    bool excerpt;
    char word[SCENE_REQ_WORD_MAX]; ///< 关联键：被点词 / correlation: the tapped word
    uint16_t word_off;             ///< 词在上下文里的偏移（引用条高亮用）/ offset inside the context
    uint8_t word_len;              ///< 词字节长 / word length in bytes
    size_t page;                   ///< 关联键：发起页 / correlation: originating page
    int64_t deadline_ms;           ///< 关联键：期限（含）/ correlation: deadline (inclusive)
} scene_req_t;

/// 置位：发出一份 0x1A 时调用。空词视为不置位并清态（没词无法关联）。
/// Arm: called when a 0x1A goes out. An empty word arms nothing and clears the
/// state - without a word there is nothing to correlate against.
void scene_req_arm(scene_req_t* r, const char* word, uint16_t word_off, uint8_t word_len,
                   size_t page, int64_t deadline_ms);

/// 置位（整段选文，bit2）：与 scene_req_arm 同一套关联键，多一个 excerpt 标记。
/// 多这一个标记是因为**回流的 0x08 没有「这是段落答案」的位**（协议零改动），
/// 设备只能靠发起时的记忆决定开「选段」标题 + 整段引用条，还是开词卡式视图。
/// 26.11.3-C：摘抄路径此前既不 arm 也不填上下文，答案认领不上，落成词卡——
/// 这条路径存在的理由就是让「发了必须有人认」覆盖到整段请求。
/// Arm for a whole-passage request (bit2). Same correlation key as scene_req_arm
/// plus an excerpt mark, because the returning 0x08 carries no "this is a passage"
/// bit: only the originating state can decide between the "选段" view and the word
/// card. 26.11.3-C: the excerpt path armed nothing and its answer landed as a card.
void scene_req_arm_excerpt(scene_req_t* r, const char* word, uint16_t word_off,
                           uint8_t word_len, size_t page, int64_t deadline_ms);

/// 认领一条回流答案。返回 true 表示「这是我的，且已消费」——**成功即销号**，
/// 同一条答案不可能既开视图又落卡。
/// 返回 false 的三种情形（都不改状态，除超出期限外）：
///   · 未置位 / 无词；
///   · 词不匹配（用户的另一单）；
///   · 页不匹配（用户已翻页）；
/// 超出期限时**顺手销号**并返回 false——僵尸态不允许存在。
/// Claim a returning answer. True means "mine, and consumed" - a successful claim
/// disarms, so one answer can never both open a view and land a card. False in
/// three cases (state untouched except the last): not armed, word mismatch
/// (another request of the user's), page mismatch (the user turned the page). Past
/// the deadline it also disarms and returns false - no zombie state allowed.
bool scene_req_claim(scene_req_t* r, const char* word, size_t page, int64_t now_ms);

/// 是否该超时销号（调用方在 tick 里问）。纯查询，不改状态；真正的销号由
/// scene_req_expire 做，避免「一次查询顺手改了状态」的隐式副作用。
/// Whether the request should expire (asked from the tick). Pure query; the actual
/// disarm is scene_req_expire, so a query never mutates state behind your back.
bool scene_req_expired(const scene_req_t* r, int64_t now_ms);

/// 超时销号（若确实超时则清态并返回 true，用于打一条日志）。
/// Expire it: clears the state and returns true when it really expired (for one log line).
bool scene_req_expire(scene_req_t* r, int64_t now_ms);

/// 无条件清态（新点词、换书、关页时调用）。/ Clear unconditionally (new tap, book switch, page exit).
void scene_req_disarm(scene_req_t* r);

/// 引用条取句（纯函数）：在 ctx 里以 [word_off, word_off+word_len) 为锚，向两侧
/// 扩到句界（。！？；!?; 与换行）或 ctx 边界；句长超过 max_chars 时**保词居中**
/// 截断。返回句长，out_off/out_len 为 ctx 内的字节区间。句界与 ctx 边界重合时
/// 置 *fallback（用于区分「本来只有一句」与「句界识别失败」，NULL 允许）。
/// 按码位扫描而非按字节——UTF-8 标点按字节扫会落在续字节上，把句子切成半个字。
/// Quote-sentence extraction (pure): anchor [word_off, word_off+word_len) inside
/// ctx and expand both ways to a sentence boundary (。！？；!?; plus newline) or the
/// ctx edge; past max_chars the window is truncated AROUND THE WORD. Returns the
/// sentence length; out_off/out_len are the byte span inside ctx. *fallback is set
/// when no boundary was found (telling "one sentence" apart from "detection
/// failed"; NULL allowed). Scans code points, not bytes - byte scanning lands
/// inside multi-byte punctuation and cuts the sentence mid-glyph.
size_t scene_req_quote_sentence(const char* ctx, size_t ctx_len,
                                size_t word_off, size_t word_len,
                                size_t max_chars, size_t* out_off, size_t* out_len,
                                bool* fallback);

#ifdef __cplusplus
}
#endif
