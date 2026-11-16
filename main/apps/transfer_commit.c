// SPDX-FileCopyrightText: 2026 mindreset
// SPDX-License-Identifier: Apache-2.0
//
// 提交泵的两拍状态机。原在 `app_transfer.c`，26.11.1 抽出以便宿主跑真身
// （理由见 transfer_commit.h 的文件头注释）。函数体与日志文本与抽取前逐字相同：
// 真机判据（`commit beat 1/2`、`commit beat 2/2`、`dropped after N frames`）是按
// 这些字符串建的，动一个字就是把已验收的判据作废。
//
// The two-beat commit state machine, extracted from app_transfer.c on 2026-11-01 so
// the host can run the real thing. Body and log strings are byte-identical to the
// pre-extraction version: the hardware criteria are built on these exact strings and
// changing one character would void a criterion that already passed review.

#include "transfer_commit.h"

#include <stdio.h>

#include "app_content_open.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "push_sink.h"

static const char *TAG = "transfer_page";

// 26.9.30 commit 3：提交前释放读者句柄的两拍簿记。请求被 take 取走后若这一拍关
// 不干净（prep 任务还在跑），泵自己记着「待续」和一个死线——这就是 commit 2
// 注释里说的「重试的所有权在 app 层」。死线外必须明确回 ERR(7) 并放弃：无界
// 重投正是 2 s 上界要禁的东西。上限沿用让路链的 PUSH_YIELD_WAIT_MS：同一个等
// prep 停机的语义，不许出现第二个数。
// Commit 3's two-beat bookkeeping for the reader release. If the frame that took
// the request cannot close cleanly (prep still running) the pump remembers
// "pending" plus a deadline - the retry ownership commit 2's comment assigns to
// the app layer. Past the deadline it gives up with an explicit ERR(7): unbounded
// re-posting is what the 2 s cap forbids. The ceiling reuses PUSH_YIELD_WAIT_MS
// (same "wait for prep to stop" semantics - never a second number for it).
static bool s_commit_hold_wait;
static int64_t s_commit_hold_deadline;
// 26.9.30 commit 4（审计附注 3）：拍号。两拍各打一行 INFO，真机出问题时先看
// 「有没有第二拍」就能把「prep 停不下来」与「四拍自己失败」分开。只增不减，
// 每次新请求归 1。
// Beat counter (audit note 3): one INFO line per beat. On hardware the first
// question is "did beat 2 happen at all?" - that separates "prep never stops"
// from "the four beats themselves failed". Reset to 1 on every new request.
static uint8_t s_commit_hold_beat;

// 26.9.30 commit 2：提交段的执行体。app_loop 取走请求后调到这里——四拍
// （rename/remove）只在主循环帧间跑，绝不上 BLE 回调栈：那里和读者的 fread
// 抢同一个卷，而且没有「读者已释放」这个前提。
// commit 3 会在这行之前加 book_release_hold()：释放读者句柄是四拍成立的前提。
// The commit executor: app_loop calls it after taking the request, so the four
// beats run between frames and never on the BLE callback stack - that stack
// contends with the reader's fread for the same volume and carries no "reader
// released" precondition. Commit 3 adds book_release_hold() right here.
void transfer_commit_pump(void) {
    // 请求在这里取（而不是在 app_loop 里），因为「取走→几拍后才提交」需要跨帧
    // 记住状态；take 的读即清语义不变，超时仍由 push_sink 自己回 ERR(7)。
    // The request is taken here (not in app_loop) because "taken, then committed a
    // few frames later" needs cross-frame state; take's read-and-clear semantics
    // are unchanged and expiry still replies ERR(7) inside push_sink.
    if (push_sink_take_commit_request()) {
        s_commit_hold_wait = true;
        s_commit_hold_beat = 1;
        s_commit_hold_deadline = esp_timer_get_time() / 1000 + PUSH_YIELD_WAIT_MS;
        // 拍 1/2：请求已交到 app 层，开始等读者让路。这一行存在但迟迟没有拍 2，
        // 现场结论就是「prep 没停下来」，不必翻代码。
        // Beat 1/2: the request reached the app layer and the wait for the reader
        // begins. This line with no beat 2 means "prep never stopped" - no code
        // archaeology needed.
        ESP_LOGI(TAG, "commit beat 1/2: hold starts, %d ms deadline", PUSH_YIELD_WAIT_MS);
    }
    if (!s_commit_hold_wait) return;
    // 首行：先把读者对这条路径的持有放掉（完整关书，不是只关句柄）。
    // 放不掉就下一帧再来——但只在死线内；越过死线就是「这一拍真提交不了」。
    // First line: release the reader's hold on this path (a full close, not just
    // the handle). If it cannot be released, come back next frame - but only
    // inside the deadline; past it this commit genuinely cannot happen.
    if (!app_book_release_hold(PUSH_TEXT_PATH)) {
        if (esp_timer_get_time() / 1000 < s_commit_hold_deadline) {
            if (s_commit_hold_beat < 0xFFu) s_commit_hold_beat++;
            return;
        }
        s_commit_hold_wait = false;
        push_sink_commit_abort(PUSH_ERR_BUSY);
        ESP_LOGW(TAG, "push commit dropped after %u frames: reader hold did not "
                      "clear in %d ms", (unsigned)s_commit_hold_beat, PUSH_YIELD_WAIT_MS);
        return;
    }
    s_commit_hold_wait = false;
    ESP_LOGI(TAG, "commit beat 2/2: reader hold cleared after %u frame(s), running commit",
             (unsigned)s_commit_hold_beat);
    if (!push_sink_run_commit()) {
        // 错误已由 push_sink 内部 reply（ERR(5) 掉卡 / ERR(7) 超时）。这里只留
        // 一行现场证据：旧书仍在，读者不需要做任何事。
        // The error was already replied inside push_sink (ERR(5) card / ERR(7)
        // timeout). One field line only: the old book is still there and the
        // reader has to do nothing.
        ESP_LOGW(TAG, "push commit failed; the previous text stays readable");
    }
}

bool transfer_commit_waiting(void) { return s_commit_hold_wait; }

uint8_t transfer_commit_beat(void) { return s_commit_hold_beat; }
