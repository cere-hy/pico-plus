// SPDX-FileCopyrightText: 2026 mindreset
// SPDX-License-Identifier: Apache-2.0
//
// `.part` 的账本、欠账槽与四步回退。见 push_part.h 的「为什么要抽出来」。
// The .part ledger, the owed slot and the rewind path. See push_part.h.

#include "push_part.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "push_proto.h" // PUSH_DATA_PAYLOAD_MAX

#ifdef PUSH_PART_HOST_TEST
// 宿主：只用 POSIX + stdio，路径与目录来自钩子。
// Host: POSIX + stdio only; the path comes from the hook.
#include <stdlib.h>

static char s_dir[192] = "/tmp/push_part_test";
static char s_path[256] = "/tmp/push_part_test/push.part";
#define PART_DIR s_dir
#define PART_PATH s_path
#define PART_LOGW(fmt, ...) fprintf(stderr, "W part: " fmt "\n", ##__VA_ARGS__)
#define PART_LOGE(fmt, ...) fprintf(stderr, "E part: " fmt "\n", ##__VA_ARGS__)
#define PART_LOGI(fmt, ...) fprintf(stderr, "I part: " fmt "\n", ##__VA_ARGS__)
#else
#include "esp_log.h"
#include "pico_bootbb.h"
#include "push_sink.h" // PUSH_DIR / PUSH_PART_PATH

static const char *TAG = "push_part";
#define PART_DIR PUSH_DIR
#define PART_PATH PUSH_PART_PATH
#define PART_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define PART_LOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define PART_LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#endif

static FILE *s_part; // 打开中的 .part（append 模式）/ the open partial (append mode)

// 账本：已确认写完整包的累计长度 = 下一包起点。**不用 stat 回推**：part_size()
// 用 stat，而 fflush 只到 f_write、目录项 size 不更新（见 push_part_sync 上方
// 注释）——fflush 之后 stat 读到的是旧长度，回推不出真实写入位置。起点必须在
// ARM 成功那一刻就位；若只在写成功时前进，续传会话首包一失败，回退会截到 0，
// 把 NVS 锚定的已收字节整段抹掉——"靠最后一道 VERIFY 拦下坏文件"保得住正确性，
// 保不住可用性。
// The ledger: confirmed length after whole packets = the next packet's start.
// Never derived from stat() (fflush leaves the directory entry stale). It must be
// in place the moment ARM succeeds: advancing it only on success makes a resumed
// session's first failed packet rewind to 0 and wipe what NVS anchored.
static uint32_t s_part_len;
static uint8_t s_owed[PUSH_DATA_PAYLOAD_MAX]; // 一个槽，241 B / one slot, 241 B
static uint16_t s_owed_len;
static bool s_owed_valid;
#ifndef PUSH_PART_HOST_TEST
// 本开机累计的首写失败次数（含救回的）。与 EXCERPT_FAIL 不同：那条数的是已经丢掉
// 的数据，这条数的是「第一次没写进去」——后者多数被救回来了，但它是卡在变坏的
// 早期信号，而串口只能证明「这一篇推完了」。
// First-write failures this boot (rescued ones included). Not the same as
// EXCERPT_FAIL, which counts data already lost: this counts "did not go in the
// first time", mostly rescued, yet the earliest sign of a dying card.
static uint16_t s_write_fails;
#endif

#ifdef PUSH_PART_HOST_TEST
static size_t s_test_short_by;
static int s_test_short_times;
static long s_test_last_truncate = -1;

void push_part_test_reset(const char *dir) {
    if (dir) snprintf(s_dir, sizeof(s_dir), "%s", dir);
    snprintf(s_path, sizeof(s_path), "%s/push.part", s_dir);
    if (s_part) {
        fclose(s_part);
        s_part = NULL;
    }
    (void)mkdir(s_dir, 0777);
    (void)remove(s_path);
    s_part_len = 0;
    s_owed_valid = false;
    s_owed_len = 0;
    s_test_short_by = 0;
    s_test_short_times = 0;
    s_test_last_truncate = -1;
}

void push_part_test_short_next_write(size_t short_by) {
    s_test_short_by = short_by;
    s_test_short_times = 1;
}

void push_part_test_short_writes(size_t short_by, int times) {
    s_test_short_by = short_by;
    s_test_short_times = times;
}
long push_part_test_last_truncate(void) { return s_test_last_truncate; }
#endif

void push_part_close(void) {
    if (s_part) {
        (void)fflush(s_part);
        (void)fclose(s_part);
        s_part = NULL;
    }
}

// 追加模式打开：不存在则创建，存在则接着写（续传靠这个）。
// Open in append mode: create when missing, continue when present (resume relies on it).
static bool part_open_append(void) {
    if (s_part) return true;
    if (mkdir(PART_DIR, 0777) && errno != EEXIST) return false;
    s_part = fopen(PART_PATH, "ab");
    return s_part != NULL;
}

bool push_part_open_fresh(void) {
    push_part_close();
    if (mkdir(PART_DIR, 0777) && errno != EEXIST) return false;
    s_part = fopen(PART_PATH, "wb");
    if (s_part == NULL) return false;
    // 全新会话的起点：账本、欠账槽全部归零。"wb" 已经把文件截到 0，账本若还
    // 留着上一个会话的长度，第一次回退就会把文件延长成 0 填充。
    // A fresh session starts from zero. "wb" already truncated the file, and a
    // ledger left over from the previous session would make the first rewind
    // extend it with zeros.
    s_part_len = 0;
    s_owed_valid = false;
    s_owed_len = 0;
    return true;
}

// FATFS 上真正的「落地」是 f_sync（回写目录项的 size）；fflush 只到 f_write，
// 目录项不动 —— stat()/回读看到的仍是旧长度（gen15 真机 err=4 的根因）。
// On FATFS real durability is f_sync (it rewrites the size in the directory
// entry); fflush() stops at f_write and leaves the entry stale, so stat() and
// readback still see the old length (the gen15 err=4 root cause).
bool push_part_sync(void) {
    if (!s_part) return false;
    if (fflush(s_part) != 0) return false;
    return fsync(fileno(s_part)) == 0;
}

long push_part_size(void) {
    struct stat st;
    if (stat(PART_PATH, &st) != 0) return -1;
    return (long)st.st_size;
}

// 回退到本包起点。三步缺一不可：
// ① 先放句柄——FS_LOCK=0 下同一文件不许两次打开（ffconf.h 明文禁止），而
//    truncate() 走 VFS 会自己再 f_open 一遍；
// ② truncate 到 s_part_len（沿用 adopt 的写法），返回值必判；
// ③ 重开 "ab"——append 的位置由目录项 size 决定，truncate 改的就是它。
// Rewind to this packet's start: drop the handle (FS_LOCK=0 forbids two opens and
// truncate() reopens it), truncate to s_part_len (check the return), reopen "ab".
static bool part_rewind(void) {
    if (s_part) {
        if (fclose(s_part) != 0) {
            s_part = NULL;
            return false;
        }
        s_part = NULL;
    }
#ifdef PUSH_PART_HOST_TEST
    s_test_last_truncate = (long)s_part_len;
#endif
    if (truncate(PART_PATH, (off_t)s_part_len) != 0) return false;
    return part_open_append();
}

// 写整包。只有整包成功才推进账本——它是"下一包起点"的唯一真源。
// fflush 的返回值必须判：f_write 失败时位置已脏，装作成功就是把后续包静默写到
// 错误偏移上（P0-2a ① 的静默）。
// Write one whole packet; only a complete one advances the ledger, the single
// source of truth for "where the next packet starts". fflush's return must be
// checked: a failed f_write leaves the position dirty.
static bool part_put(const uint8_t *data, size_t len) {
    if (s_part == NULL && !part_open_append()) return false;
    size_t want = len;
#ifdef PUSH_PART_HOST_TEST
    // 真短写：少落 n 字节，fwrite 返回值因此小于 len，与卡满/瞬时忙同形。
    // A real short write: fewer bytes land, so fwrite returns < len, exactly the
    // shape of a full or transiently busy card.
    if (s_test_short_times > 0) {
        const size_t by = s_test_short_by;
        s_test_short_times--;
        want = len > by ? len - by : 0;
    }
#endif
    if (fwrite(data, 1, want, s_part) != len) return false;
    if (fflush(s_part) != 0) return false;
    s_part_len += (uint32_t)len;
    return true;
}

// 返回值语义（调用方只认这一个）：true = 数据已保住（在盘上或在欠账槽）；
// false = 没接住（位置回不去，或欠账槽装不下）→ 调用方 CANCEL + ERR。
// true = the data is safe (on disk or in the owed slot); false = not caught.
static bool part_append_locked(const uint8_t *data, size_t len) {
    if (len == 0) return true;
    if (len > sizeof(s_owed)) return false; // 协议外，不该发生 / outside the protocol
    // 欠账先补：顺序追加一旦乱序就是永久损坏。这条不变式放在这里，不放在调用方
    // ——调用点会新增（END 那次是显式的），而"顺序"是数据层的性质，不是某个
    // 分支的礼貌。M120 咬的就是这一行。
    // The owed packet goes first: append-only storage is permanently corrupt once
    // out of order. The invariant lives here, not at the call sites - "order" is a
    // property of the data layer, not a courtesy of one branch.
    if (!push_part_drain()) return false;
    if (part_put(data, len)) return true;
    // 一次写失败不再等于整篇重传：先回到本包起点重写一次（救"瞬时忙"）。
    // One failed write no longer means resending the whole article: rewind to this
    // packet's start and retry once (rescues a transiently busy card).
#ifndef PUSH_PART_HOST_TEST
    s_write_fails++;
    pico_bootbb_note(PICO_BOOTBB_NOTE_PUSH_WRITE_FAIL, (uint8_t)(errno & 0xFF), s_write_fails);
#endif
    PART_LOGW("append failed, errno=%d - rewind to %u and retry", errno, (unsigned)s_part_len);
    if (!part_rewind()) return false; // 回不去 = 保证不了不重复 / cannot prove no duplication
    if (part_put(data, len)) return true;
    if (!part_rewind()) return false;
    memcpy(s_owed, data, len); // 位置已干净，整包进欠账槽 / position clean, park the packet
    s_owed_len = (uint16_t)len;
    s_owed_valid = true;
    return true;
}

bool push_part_append(const uint8_t *data, size_t len) { return part_append_locked(data, len); }

bool push_part_probe(void) { return part_open_append(); }

bool push_part_is_open(void) { return s_part != NULL; }

uint32_t push_part_len(void) { return s_part_len; }

// 欠账补写。false = 补不上，会话必须终止（只有一个槽；下一包还在 BLE 回调栈
// 上，再等就是丢包）。
// false = still failing, the session must end (one slot only; the next packet
// lives on the BLE callback stack, and waiting any longer loses it).
bool push_part_drain(void) {
    if (!s_owed_valid) return true;
    if (part_put(s_owed, s_owed_len)) {
        s_owed_valid = false;
        PART_LOGI("owed %u B written after the retry", (unsigned)s_owed_len);
        return true;
    }
    PART_LOGE("owed %u B still fails, errno=%d - ending session", (unsigned)s_owed_len, errno);
    s_owed_valid = false;
    // 这里**不**回退，与 part_append_locked 刻意不同：会话要结束了，截到"本包
    // 起点"没有意义——续传的锚是 NVS 记账（每 64 包一次，比本包起点更旧），下次
    // ARM 的 adopt 会按它截断。但句柄必须关：CANCEL 走 goto_idle 只回 ADV_OFF、
    // 不 discard，句柄会跨会话残留，而 adopt 的 truncate 要求文件没被打开。
    // Deliberately no rewind here: the session is ending, so cutting to "this
    // packet's start" is pointless - the resume anchor is the NVS bookkeeping, and
    // the next ARM's adopt cuts back to it. The handle must close though: CANCEL
    // goes to goto_idle, which never discards, so the handle would survive into
    // the next session - and adopt's truncate needs the file closed.
    push_part_close();
    return false;
}

push_part_adopt_t push_part_adopt(uint32_t received) {
    const long sz = push_part_size();
    if (sz < 0) return PUSH_PART_ADOPT_NO_CARD; // 卡不在：簿记原样留着
    if ((uint32_t)sz < received) return PUSH_PART_ADOPT_CORRUPT; // 文件比账本短 = 损坏
    if ((uint32_t)sz > received && truncate(PART_PATH, (off_t)received) != 0) {
        return PUSH_PART_ADOPT_CORRUPT;
    }
    // 账本与文件长度同时就位。缺了这一行，续传会话的首包一旦写失败，回退会按
    // s_part_len=0 截断，把 NVS 锚定的已收字节整段抹掉。M121b 咬的就是这一行。
    // The ledger and the file length come up together. Without this line a resumed
    // session's first failed packet rewinds to 0 and wipes the NVS anchor.
    s_part_len = received;
    s_owed_valid = false;
    s_owed_len = 0;
    return PUSH_PART_ADOPT_OK;
}

void push_part_discard(void) {
    push_part_close();
    (void)remove(PART_PATH);
    // 文件没了，账本必须一起归零：否则下一个会话的回退会按旧长度 truncate，
    // 把文件延长成一段 0 填充的假数据。
    // The file is gone, so the ledger goes with it; otherwise the next session's
    // rewind truncates to a stale length and extends the file with zeros.
    s_part_len = 0;
    s_owed_valid = false;
    s_owed_len = 0;
}
