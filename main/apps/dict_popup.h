/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：点词查译弹窗的外部接口；被 app_book 阅读页调用，不导出 app_desc_t。
 * English: External API of the lookup popup; called by the reader page, exports no app_desc_t.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epdiy.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 卡片区域（绘制与命中共用）。/ Card rectangle, shared by draw and hit-test.
EpdRect dict_popup_rect(void);
/// 避让锚点：点词所在侧与分半几何开关（v2 §5.2）。须在 dict_popup_open() 之前调用——
/// 开放时按最终卡片宽度折行释义。col=0 点词在左半（卡片弹出到右半），col=1 反之；
/// split=false 时横屏维持铺满（避让规则是双栏特性，单栏不做无谓的行为分裂）。
/// Avoidance anchor: the tapped side and the split-geometry switch (v2 section 5.2).
/// Must be called before dict_popup_open() — the definition wraps to the final card
/// width at open time. col=0 taps on the left half (card pops to the right half) and
/// vice versa; split=false keeps landscape full width (the avoidance is a two-column
/// feature; single column does not split behavior for nothing).
void dict_popup_set_anchor(int col, bool split);
/// 查词并准备弹窗内容；成功返回 true（无词典或未收录返回 false）。
/// Look up the word and stage the content; false when no dictionary or no entry.
bool dict_popup_open(const char* word);
/// gen26 AI 释义：用外部提供的释义直接开卡——不碰本地词典，走同一暂存、折行、
/// 绘制、命中与保存链路（手机 0x07 回流或预取缓存命中由调用方喂进来）。
/// word/def 为空、或折行后无行可显示，返回 false。
/// gen26 AI definition: stage a card straight from an externally provided
/// definition - the local dictionary is never touched; the same staging,
/// wrapping, draw, hit-test and save chain applies (the phone's 0x07 return or
/// a prefetch-cache hit is fed in by the caller). False on empty input or when
/// wrapping produced no displayable line.
bool dict_popup_show_def(const char* word, const char* def);
/// gen26.3 「AI 查询中…」卡：词典 miss 且 AI 开着时立即进入查询态——卡面显示的是
/// 真实状态，不是假卡；0x07 到达后由 dict_popup_show_def 原地更新为释义。词为空、
/// 或暂存失败（含 PSRAM 分配失败）返回 false，且不留下半张卡。
/// gen26.3 "AI querying..." card: on a dictionary miss with AI on, enter the
/// query state at once - the card shows a real state, not a fake one; when 0x07
/// lands, dict_popup_show_def updates it in place into the definition. False on
/// an empty word or a staging failure (PSRAM allocation included), leaving no
/// half card behind.
bool dict_popup_show_pending(const char* word);
/// 谓词：当前是否为查询态卡，且查的就是 word 这个词——app_book 双门判定的核心
/// （门 B：本词的查询卡原地更新为释义；门 A：用户手开的词典卡照旧丢弃进缓存）。
/// word 传 NULL 只问「是不是查询态」。词长超过词头缓冲时按截断后的词头匹配：
/// 卡面显示的本来就是这个词的截断形，0x07 带回全词也要能更新它。
/// Predicate: is the card in the query state for THIS word - the core of
/// app_book's double gate (gate B: this word's querying card updates in place;
/// gate A: a user-opened dict card is still dropped into the cache). A NULL word
/// asks only about the state. An overlong word matches on its truncated head:
/// the card shows that word's truncated form, so the 0x07 full word must still
/// update it.
bool dict_popup_is_pending(const char* word);
/// 查询态超时判定（esp_timer 微秒）：手机迟迟不回帧（查询失败/链路断）时，调用方
/// 据此把卡片降级为失败提示——查询卡显示真实状态，绝不无限期假活。非查询态恒假。
/// Query-state timeout in esp_timer microseconds: when the phone never answers
/// (query failed / link down) the caller degrades the card to a failure notice -
/// the querying card shows a real state and never fakes forever. Always false
/// outside the query state.
bool dict_popup_pending_expired(int64_t timeout_us);
/// gen26.3 ⑤ AI 释义显示容量（全角字符数）：按当前方向的卡片布局实算——竖屏
/// 10 行 x 20 字 = 200（40 字预算占 2 行，匹配；横屏同公式得 400，预算校准取
/// 竖屏这个保守值）。推导钉死在 dict_popup.c 的函数注释里。
/// gen26.3 item-5 AI definition capacity in full-width chars, computed from the
/// card layout in the current rotation: portrait 10 x 20 = 200 (a 40-char budget
/// takes 2 lines - it fits; the same formula gives 400 in landscape, and the
/// portrait value is the conservative one for prompt calibration). The
/// derivation is pinned in the function comment in dict_popup.c.
int dict_popup_ai_capacity_chars(void);
/// 释放内容与词头。/ Release staged content.
void dict_popup_reset(void);
/// 在阅读页内容之上绘制卡片。/ Paint the card over the reader content.
void dict_popup_draw(uint8_t* fb);
/// 「保存」chip 的区域（绘制与命中共用）。/ Rectangle of the save chip, shared by draw and hit-test.
EpdRect dict_popup_save_rect(void);

/// chip 点击的结果。调用方据此决定「关卡片」「留着」还是「删完要重开视图」。
/// What a chip tap did; the caller decides whether to close the card, keep it, or
/// re-open the vocabulary view after a removal.
typedef enum {
    DICT_POPUP_TAP_MISSED,  ///< 没点到 chip，调用方照旧关卡片 / not the chip; close as usual
    DICT_POPUP_TAP_KEPT,    ///< 命中并切换了状态，卡片保持打开 / toggled; keep the card open
    DICT_POPUP_TAP_REMOVED, ///< 命中并从生词本移除了一条 / removed one entry
    DICT_POPUP_TAP_NOT_READY, ///< gen26.3 查询态卡：释义没到，chip 点击忽略、卡片保持打开
                              ///< gen26.3 querying card: the def has not landed, the chip tap is
                              ///< ignored and the card stays open
} dict_popup_tap_t;

/// 点击落在 chip 上时切换生词本保存状态。未命中由调用方关卡片。
/// Toggle the vocabulary-book entry when the tap lands on the chip; a miss is the
/// caller's job to close.
dict_popup_tap_t dict_popup_tap(uint16_t x, uint16_t y);

#ifdef __cplusplus
}
#endif
