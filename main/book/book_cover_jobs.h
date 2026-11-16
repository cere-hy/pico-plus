/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "book_cover.h"
#define BOOK_COVER_JOB_SLOTS 9
/// 请求只存快照，不借用页面字符串。/ Requests own snapshots instead of borrowing page strings.
typedef struct {
    char path[288],title[256],author[128];
    bool list,fast,natural;
    unsigned width,height,stride,contrast,phase;
    int x,y;
} book_cover_job_t;
typedef struct {
    uint8_t *pixels;
    unsigned width,height;
    bool fast,white_edge;
    off_t size;
    int64_t modified;
} book_cover_result_t;
/// 新页面代次取消旧结果；切页无需等待解码。/ A new page generation discards stale results without joining a decoder.
unsigned book_cover_jobs_begin(void);
bool book_cover_jobs_submit(unsigned generation,unsigned slot,const book_cover_job_t *request);
/// 0待处理，1完成并转交像素所有权，-1失败；无封面时仅在UI生成字形。
/// 0 pending,1 transfers pixels,-1 failed; missing artwork renders its glyphs only on the UI thread.
int book_cover_jobs_take(unsigned generation,unsigned slot,book_cover_result_t *result);
void book_cover_jobs_cancel(unsigned generation);
/// 作废当前代次的结果，**不等待解码器**。插拔/换页这类「只要旧结果消失」的场合用它；
/// `quiesce(0)` 不是它的替代——那是「宣布收口却不等」，B42 牙咬死这种写法。
/// / Drop the current generation's results without joining the decoder. Use this when
/// stale results must simply disappear; `quiesce(0)` is not a substitute.
void book_cover_jobs_discard(void);
/// 仅卸载/睡眠前收齐文件句柄，导航不等待。/ Join handles only before unmount/sleep; navigation never waits.
bool book_cover_jobs_quiesce(unsigned timeout_ms);
