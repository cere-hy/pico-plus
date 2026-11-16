#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
26.9.17「拔卡卸载、插回重挂」的结构牙。

为什么需要这份检查：热插拔失效的三层病灶里，**没有一层会让构建失败**。
不卸载，FAT 卷照样挂着，代码照常返回 ESP_OK；自锁门一关，状态机安静得像什么都没
发生；假成功那行 `if (card != NULL) return ESP_OK;` 更是每一步都"成功"。
26.9.16 的 55 个宿主场景全绿、门禁全绿，用户在真机上却看到卡永远认不回来。
能把这类缺陷关在门外的东西只有**调用存在**、**条件形状**和**语句顺序**三样。

K1  `poll_media` 的 lost 分支必须调用 `read_pico_sd_remount()`。
    这是唯一会走 `esp_vfs_fat_sdcard_unmount()` 的路径；删掉它，拔卡后卷就一直挂着
    （26.9.16 现场：`dict opendir errno=5` 而不是 ENOENT，83 条 0x107 空转）。
K2  针对 `ESP_ERR_INVALID_STATE` 的探测条件**不得**含 `invalidated`。
    `*invalidated` 只有 `ready` 能清，`ready` 要 `mounted`，`mounted` 要探测，
    探测要 `!*invalidated`——加回去就是闭环，插回多少次都不再探测。
K3  `mount_card()` 的 `card != NULL` 分支**不得**返回 `ESP_OK`。
    返回 OK 等于把「忘了卸载」伪装成挂载成功，UI 宣布有卡、每次读都是 0x107。
K4  状态行必须是五元组，含 `release=`。这是验收清单里要读的那个字段。
K5  `read_pico_sd_remount()` 必须排在 `on_media_lost(ctx)` **之后**。
    顺序反了就是先 unregister pdrv 再关句柄，悬空 FILE*。

全部按**行首语句**判定，不查子串：26.9.16 的 M49 就是被 `if (0) ESP_LOGW(...)`
这种写法骗过去的，这里不接受同样的糊弄。

用法：python3 tools/check_hotplug.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_LOOP = ROOT / "main/app/app_loop.c"
SD_SRC = ROOT / "components/read_pico/read_pico_sd.c"

fails = []


def bad(msg):
    fails.append(msg)
    print("FAIL: " + msg)


def ok(msg):
    print("ok   " + msg)


def read(p):
    try:
        return p.read_text(encoding="utf-8")
    except OSError as exc:
        bad("cannot read %s: %s" % (p, exc))
        return ""


loop_src = read(APP_LOOP)
sd_src = read(SD_SRC)


def strip_comments(src):
    """把注释替换成等长空格（保留换行），字符串/字符字面量原样保留。

    本仓库注释很密，且注释里会写出调用名（本文件的批注就提到了
    `push_sink_quiesce(`）。按原始文本判「调用在不在、谁在前」会被注释骗过去——
    那正是「在场 ≠ 生效」换到牙自己身上。规则与 tools/check_refresh_ghost.py 同源。
    """
    out = []
    i, n = 0, len(src)
    state = None  # None | 'line' | 'block' | 'str' | 'chr'
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "str"
            elif c == "'":
                state = "chr"
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = None
                out.append(c)
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = None
                out.append("  ")
                i += 2
                continue
            out.append(c if c == "\n" else " ")
            i += 1
            continue
        if c == "\\":
            out.append(c)
            if nxt:
                out.append(nxt)
            i += 2
            continue
        if (state == "str" and c == '"') or (state == "chr" and c == "'"):
            state = None
        out.append(c)
        i += 1
    return "".join(out)


def fn_body(text, name):
    """取一个函数的体（含签名），大括号配对，注释须已剥掉。"""
    m = re.search(r"^(?:static\s+)?[^\n;{}]*\b" + re.escape(name) + r"\s*\([^;{}]*?\)\s*\{",
                  text, re.M)
    if not m:
        return ""
    at, depth, quote, escape = m.end(), 1, None, False
    while at < len(text) and depth:
        c = text[at]
        if quote:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        at += 1
    return text[m.start():at]


loop_code = strip_comments(loop_src)
sd_code = strip_comments(sd_src)

# ---- 切出 poll_media() 的函数体 ---------------------------------------------
# 只在这个函数里判 K1/K2/K5：同名调用散落在 app_sd.c / usb_storage.c 里不算数。
body = ""
m = re.search(r"static\s+bool\s+poll_media\s*\([^)]*\)\s*\{", loop_src)
if not m:
    bad("poll_media() not found in main/app/app_loop.c")
else:
    tail = loop_src[m.end():]
    end = re.search(r"\n\}", tail)
    body = tail[:end.start()] if end else tail

# ---- K1：lost 分支必须卸载 ------------------------------------------------
anchor = "if (!lost) return false;"
if not body:
    pass
elif anchor not in body:
    bad("K1 lost branch anchor `%s` is gone - the removal path was rewritten; re-audit "
        "before trusting the rest" % anchor)
else:
    lost_part = body[body.index(anchor):]
    # 只认行首的调用语句，注释里提到这个名字不算数
    if not re.search(r"^[ \t]*s_last_release\s*=\s*read_pico_sd_remount\s*\(\s*\)\s*;",
                     lost_part, re.M):
        bad("K1 the lost branch never calls read_pico_sd_remount(): the FAT volume stays "
            "mounted on a card that has left (errno=5, 0x107 spin)")
    else:
        ok("K1 lost branch releases the volume via read_pico_sd_remount()")

# ---- K2：兜底探测条件不得含 invalidated -----------------------------------
if body:
    # 抓任何以 if 开头、带 ESP_ERR_INVALID_STATE 的比较行
    probe_lines = [
        ln for ln in body.splitlines()
        if re.match(r"^[ \t]*if\s*\(", ln) and "ESP_ERR_INVALID_STATE" in ln
    ]
    if not probe_lines:
        bad("K2 no `if (... ESP_ERR_INVALID_STATE ...)` probe guard found in poll_media()")
    else:
        dirty = [ln.strip() for ln in probe_lines if "invalidat" in ln]
        if dirty:
            bad("K2 probe guard reads the page-level invalidation flag again -> the "
                "removal/invalidation deadlock returns (0 probe, 0 remount): %s" % dirty[0])
        elif not any("!*mounted" in ln for ln in probe_lines):
            bad("K2 probe guard lost the `!*mounted` term: a removal would be mistaken "
                "for a fresh insertion")
        else:
            ok("K2 probe guard is INVALID_STATE && !*mounted, no invalidation flag")

# ---- K3：mount_card 不得假成功 --------------------------------------------
m = re.search(r"static\s+esp_err_t\s+mount_card\s*\([^)]*\)\s*\{", sd_src)
if not m:
    bad("K3 mount_card() not found in components/read_pico/read_pico_sd.c")
else:
    tail = sd_src[m.end():]
    end = re.search(r"\n\}", tail)
    fn = tail[:end.start()] if end else tail
    guard = re.search(r"if\s*\(\s*card\s*!=\s*NULL\s*\)\s*\{", fn)
    if guard:
        block_end = fn.find("}", guard.end())
        block = fn[guard.end():block_end if block_end > 0 else len(fn)]
        if re.search(r"return\s+ESP_OK\s*;", block):
            bad("K3 mount_card() still returns ESP_OK while `card` is stale - a skipped "
                "unmount will masquerade as a successful mount")
        elif not re.search(r"return\s+ESP_ERR_INVALID_STATE\s*;", block):
            bad("K3 mount_card() stale-card branch no longer returns ESP_ERR_INVALID_STATE")
        else:
            ok("K3 mount_card() rejects a stale card instead of faking success")
    else:
        # 允许写成单行 `if (card != NULL) return X;`
        single = re.search(r"if\s*\(\s*card\s*!=\s*NULL\s*\)\s*return\s+([A-Z_0-9]+)\s*;", fn)
        if single and single.group(1) == "ESP_OK":
            bad("K3 mount_card() returns ESP_OK on a stale card (single-line form)")
        elif single:
            ok("K3 mount_card() rejects a stale card instead of faking success")
        else:
            bad("K3 the `card != NULL` guard disappeared from mount_card(); nothing "
                "stops a stale handle from being mounted over")

# ---- K4：状态行必须是五元组 ----------------------------------------------
m = re.search(r'"sd: err=%s present=%d mounted=%d invalidated=%d([^"]*)"', loop_src)
if not m:
    bad("K4 the `sd: err=` status line is gone - the field log loses its evidence")
elif "release=" not in m.group(1):
    bad("K4 the status line lost `release=`; the acceptance checklist reads that field")
else:
    ok("K4 status line carries the five-tuple including release=")

# ---- K5：先关句柄，再卸载 ------------------------------------------------
if body:
    lost_part = body[body.index(anchor):] if anchor in body else ""
    i_close = lost_part.find("on_media_lost(ctx)")
    i_remount = lost_part.find("read_pico_sd_remount()")
    if i_close < 0:
        bad("K5 on_media_lost(ctx) is no longer called from the lost branch - pages "
            "would keep their FILE* across the unmount")
    elif i_remount < 0:
        bad("K5 no read_pico_sd_remount() in the lost branch (see K1)")
    elif i_remount < i_close:
        bad("K5 read_pico_sd_remount() sits BEFORE on_media_lost(ctx): the volume is "
            "released while pages still hold handles")
    else:
        ok("K5 lost branch closes page handles before releasing the volume")

# ---- B77：quiesce 的「停」按「有没有在动介质」判，不按「有没有连接」判 ---------
# 26.11.5 现场：手机 APK 常驻、BLE 长连，用户拔卡。原判据 `state == PUSH_STATE_IDLE`
# 把「连着」当成「没停」——quiesce 永不返回 true、永不卸载、卷永远非空、插回被
# `stale card` 拒，闩锁等一个永远不会来的边沿。日志签名：`state=3` 连打 4 次，
# 0 probe start / 0 remount / 0 latch 释放三个零。
PUSH_SINK = ROOT / "components/push_sink/src/push_sink.c"
PUSH_SESSION = ROOT / "components/push_sink/src/push_session.c"
sink_code = strip_comments(read(PUSH_SINK))
sess_code = strip_comments(read(PUSH_SESSION))
q_body = fn_body(sink_code, "push_sink_quiesce")
if not q_body:
    bad("B77 push_sink_quiesce() not found - the tooth has no target")
else:
    # 旧判据的形状：写在同一行里的 `state == PUSH_STATE_IDLE &&`（换成元组/括号也算）
    if "push_quiesce_idle(" not in q_body:
        bad("B77 push_sink_quiesce() no longer calls push_quiesce_idle(): the predicate is "
            "inlined again, where the host test cannot reach it")
    elif re.search(r"s_sess\.state\s*==\s*PUSH_STATE_IDLE", q_body):
        bad("B77 quiesce is back on `s_sess.state == PUSH_STATE_IDLE`: a connected-but-idle "
            "phone (the resident-APK norm) counts as 'still running', so the volume is never "
            "unmounted and a reinserted card is refused as stale")
    else:
        ok("B77 quiesce asks the medium, not the link (push_quiesce_idle)")

p_body = fn_body(sess_code, "push_quiesce_idle")
if not p_body:
    bad("B77 push_quiesce_idle() not found in components/push_sink/src/push_session.c")
elif not re.search(r">=\s*PUSH_STATE_RECEIVING", p_body):
    bad("B77 push_quiesce_idle() lost the enum-ordered `>= PUSH_STATE_RECEIVING` test: the "
        "predicate must be 'not transferring', not 'equal to IDLE'")
elif not re.search(r"<=\s*PUSH_STATE_PROCESSING", p_body):
    bad("B77 push_quiesce_idle() lost the upper bound: COOLDOWN (the post-success grace) "
        "would count as 'in transfer' again, so pulling the card right after a push waits "
        "out a countdown that means nothing once the medium is gone")
elif not re.search(r"live_files\s*==\s*0", p_body):
    bad("B77 push_quiesce_idle() dropped the live-file conjunct: a third-party FILE* (the "
        "B51 shape) would be released out from under its reader")
elif not re.search(r"_Static_assert\s*\(\s*PUSH_STATE_CONNECTED\s*<\s*PUSH_STATE_RECEIVING",
                   sess_code):
    bad("B77 the enum-order _Static_assert is gone: reordering push_state_t would silently "
        "flip the predicate's meaning (no build error, no red test, only a dead card)")
elif not re.search(r"_Static_assert\s*\(\s*PUSH_STATE_COOLDOWN\s*>\s*PUSH_STATE_PROCESSING",
                   sess_code):
    bad("B77 the COOLDOWN-outside-the-window _Static_assert is gone: moving COOLDOWN inside "
        "the window turns the common 'push then pull' sequence back into a 5 s wait")
else:
    ok("B77 push_quiesce_idle() is enum-ordered, window-bounded, file-count conjunctive")

# ---- B78：detach 必须出现在 quiesce 之前；插回边沿必须还回可连性 --------------
# 只放行卸载而不收链路：手机下一次 BEGIN 会写向一个不存在的卷。只断链不恢复：
# 「插回不认卡」被修成「插回认卡、手机再也连不上」——31a 家族的第三个入口。
# 所以这一条咬**序关系**（照 B75 的写法）与**对称义务**两半。
pm_stripped = fn_body(loop_code, "poll_media")
if not pm_stripped:
    bad("B78 poll_media() not found in the stripped source - the tooth has no target")
else:
    lost_at = pm_stripped.find("if (!lost) return false;")
    if lost_at < 0:
        bad("B78 the `if (!lost) return false;` anchor is gone; re-audit the lost branch")
    else:
        lost = pm_stripped[lost_at:]
        i_detach = lost.find("push_sink_detach_media(")
        i_quiesce = lost.find("push_sink_quiesce(")
        if i_detach < 0:
            bad("B78 the lost branch never calls push_sink_detach_media(): with a resident "
                "APK the link stays up while the volume goes, and the next BEGIN writes into "
                "a card that is not there")
        elif i_quiesce < 0:
            bad("B78 no push_sink_quiesce() in the lost branch (see K1)")
        elif i_detach > i_quiesce:
            bad("B78 push_sink_detach_media() sits AFTER push_sink_quiesce(): the three "
                "beats run against a link that is still live")
        else:
            ok("B78 lost branch drops the link before the three beats")
    gained_at = pm_stripped.find("if (gained)")
    if gained_at < 0:
        bad("B78 the `if (gained)` remount edge is gone")
    elif "push_sink_resume_after_media(" not in pm_stripped[gained_at:]:
        bad("B78 the remount edge does not call push_sink_resume_after_media(): we cut the "
            "link, so we owe the reconnect - otherwise the phone can never connect again")
    else:
        ok("B78 remount edge repays the connectivity the detach took")

print()
if fails:
    print("check_hotplug: %d FAILED" % len(fails))
    sys.exit(1)
print("check_hotplug: all checks passed (K1-K5 hotplug, B77 quiesce predicate, B78 detach/reattach)")
