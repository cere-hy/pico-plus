#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
26.9.26 M5-A 摘抄「结构牙」（静态，零运行时成本、零 .bss 入账）：

  B32' 模块边界牙：落盘文件操作只准出现在白名单文件里；事件路径（app_book.c）
      只准与队列 API 说话——连 tick() 都不许调（落盘属于家务片，不属于点击）。
      Module boundary: file IO lives only in whitelisted files; the event path may
      talk to the queue API and nothing else.

  B33  bit2 凭证锁：出包若放开 CONFIG_PICO_EXCERPT_AI_SEND=y，协议登记簿必须
      登记联调日期与 APK 版本，缺一即强制回 n（锁从「禁 y」变「y 要凭证」，
      退役条件内置，功能不死、绕行无门）。
      Credential lock: shipping with the bit2 switch on requires the registry to
      carry the interop date and the APK version.

  B34  开关一致性牙（S26-3）：发送调用必须**整个**待在 `#if CONFIG_PICO_EXCERPT_AI_SEND`
      块里（否则 n 的包里也照样发帧），而置灰形态必须由 `excerpt_ai_enabled()` 在
      绘制与命中的两处同时决定（否则「按不动的键」会悄悄变回「按得动」）。
      Switch consistency: the send call must sit inside the config guard, and the greyed
      form must be decided by excerpt_ai_enabled() in BOTH draw and hit-test.

  B35  挂载接线牙（S26-4）：「插卡成功 → 让行 5 s」必须是**边沿触发**。写成
      `if (ready)` 就是每拍把窗口往后推，队列永远等不到能落盘的那一拍；写成
      「没人调 notify_mounted」则是让行值永远是 0——两种都是假接线，静态可见。
      Mount wiring: the yield window arms on the mount EDGE. `if (ready)` re-arms every
      tick (the queue never flushes) and never calling notify_mounted is the same lie.

  B36  页脚契约牙（S26-4）：持久警示**每页常显**靠的是页脚这条带——它必须真的读
      队列深度；回执**按页作废**靠的是「设时盖页号、画时比页号」这一对，缺一半
      回执就会跟着翻页跑到别的页上去。
      Footer contract: the footer band must read the queue depth (that is what makes
      "pending N" visible on every page), and the receipt must be stamped and checked
      against the page number (that is what expires it on a turn).

  B38  路径口径牙（26.9.29）：**业务代码里只准有一套路径词汇**。`main/` 与
      `components/`（第三方组件除外）不得出现裸 FatFs 调用或 `ff.h`；摘抄尾巴必须
      是 VFS/POSIX（`mkdir` / `fopen "ab"` / `fwrite` / `fflush` / `fsync` / `fclose`），
      路径必须以 `/sdcard/` 开头，`mkdir` 的返回值必须被判。
      One path vocabulary: bare FatFs is banned outside the SD driver component; the
      excerpt tail must be VFS/POSIX on a `/sdcard/` path, with mkdir's result judged.
      注：**挂载点改名时本牙同步**（把 SD_MOUNT_POINT 改掉，这里的前缀也要改）。

  B37  回执等级牙（S26-4）：每个回执调用都带等级；坏消息（未发送 / 已满 / 内存不足 /
      [截]）是 ALERT，好消息（已存 ✓ / AI 已发）是 INFO；设置函数必须真的实现
      「ALERT 不被 INFO 顶掉」。一个没等级的调用点就是一处「坏消息会被好消息盖掉」。
      Receipt class: every call carries a class, bad news is ALERT, good news is INFO,
      and the setter really keeps ALERT over INFO.

用法 / usage: python3 tools/check_excerpt.py   （退出码 0 = 通过 / 0 = pass）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# B32' 白名单（显式清单；新增许可须改清单 + 提交说明双留痕，不在逻辑里硬编码单文件名）。
# B32' whitelist (explicit list; adding a file requires editing this list AND saying so in
# the commit message - two trails, never a hardcoded single name buried in the logic).
# 26.9.29 起**清空**：本仓已无裸 FatFs 落盘点。旧的 `push_sink.c` 条目一直是错的——
# 它从头到尾用的是 `fopen/fwrite/fflush/fsync`（VFS），只是注释里在讲 f_write/f_sync 的
# 语义；把注释当成"先例"入册，正是 v4 文档那句"FATFS 追写先例"的来源。范围更大的
# 禁令由 B38 接管（含 `ff.h`、`f_read`、`f_opendir` 等 B32' 没列到的入口）。
# Emptied in 26.9.29: nothing in this repo calls bare FatFs any more. The old push_sink
# entry was wrong from the start - it has always used the VFS, and only its COMMENTS talk
# about f_write/f_sync. B38 now covers the wider surface.
IO_WHITELIST = ()

IO_CALLS = ("f_open", "f_write", "f_sync", "f_mkdir", "f_close")
# 事件路径不许碰的东西：文件操作 + 落盘驱动（tick 属家务片，不属点击）。
# The event path must touch neither file IO nor the flush driver (tick belongs to the slot).
EVENT_PATH_FORBIDDEN = IO_CALLS + ("excerpt_store_tick",)
# 粒度是**点击路径的函数体**，不是整个文件：家务片（on_tick）本来就该驱动 tick，
# S26-2 接线后这一点要明确写在代码里，否则这颗牙会把正常工作一起咬掉。
# Granularity is the CLICK-PATH FUNCTION BODY, not the whole file: the housekeeping slot
# (on_tick) is supposed to drive tick(). Without this precision the tooth would bite the
# very wiring it protects.
EVENT_PATH_FUNCS = (
    r"static app_redraw_t boxsel_action\(",
    r"static app_redraw_t boxsel_touch\(",
    r"static app_redraw_t on_key\(",
    r"static app_redraw_t on_key_long\(",
)

PROTO_REGISTRY = ROOT / "docs/PROTOCOL.md"
SDKCONFIG = ROOT / "sdkconfig"
APP_BOOK = ROOT / "main/apps/app_book.c"
APP_LOOP = ROOT / "main/app/app_loop.c"

# 凭证的判据是**真值**，不是「这行写得像登记过」：联调日期要 4-2-2 的数字，
# APK 版本要 vX.Y.Z。占位词（待联调 / TBD / ——）一律不认。
# Credentials mean real values, not rows that merely look filled in: a 4-2-2 date and a
# vX.Y.Z APK version. Placeholders (TBD, 待联调, --) never satisfy the tooth.
DATE_RE = re.compile(r"\b20\d{2}-\d{2}-\d{2}\b")
APK_RE = re.compile(r"\bv\d+\.\d+\.\d+\b")

failures = []


def bad(msg):
    failures.append(msg)


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return "\n".join(line.split("//")[0] for line in src.splitlines())


def check_b32_module_boundary():
    for path in sorted(ROOT.glob("main/**/*.c")) + sorted(ROOT.glob("components/**/*.c")):
        rel = path.relative_to(ROOT).as_posix()
        if rel.startswith("build"):
            continue
        body = strip_comments(path.read_text(errors="replace"))
        hits = [c for c in IO_CALLS if re.search(r"\b%s\s*\(" % c, body)]
        if hits and rel not in IO_WHITELIST:
            bad("B32' %s calls %s outside the whitelist %s: the flush belongs to the "
                "housekeeping module, not to whichever file finds it convenient"
                % (rel, "/".join(hits), list(IO_WHITELIST)))
    book = strip_comments((ROOT / "main/apps/app_book.c").read_text(errors="replace"))

    def func_body(pattern):
        m = re.search(pattern + r"[^;{]*\{", book)
        if not m:
            return ""
        depth, i = 0, m.end() - 1
        while i < len(book):
            if book[i] == '{':
                depth += 1
            elif book[i] == '}':
                depth -= 1
                if depth == 0:
                    return book[m.end():i]
            i += 1
        return ""

    for pattern in EVENT_PATH_FUNCS:
        body = func_body(pattern)
        if not body:
            bad("B32' the click path %s vanished: the event path disappeared instead of being "
                "guarded" % pattern)
            continue
        for sym in EVENT_PATH_FORBIDDEN:
            if re.search(r"\b%s\s*\(" % sym, body):
                bad("B32' %s calls %s(): the click path may only enqueue - IO and flushing "
                    "belong to the housekeeping slot" % (pattern, sym))
    # 反证：家务片必须真的挂着 tick，否则「先写后亮」永远等不到第二拍。
    # Counter-proof: the housekeeping slot must really drive tick(), or "write first, tick
    # second" waits forever for a second beat that never comes.
    tick_body = func_body(r"static app_redraw_t on_tick\(")
    if "excerpt_footer_tick(ctx)" not in tick_body:
        bad("B32' on_tick() no longer drives excerpt_footer_tick(): the queue would fill and "
            "never flush (half-wired modules are exactly what --gc-sections hides)")

    # 白名单里的文件必须真的有落盘实现（防「先列白名单再空实现」）。
    # A whitelisted file must really carry the implementation (no listing a file first and
    # implementing it later).
    for rel in IO_WHITELIST:
        if not (ROOT / rel).exists():
            bad("B32' whitelisted %s does not exist" % rel)


def check_b33_credential_lock():
    if not SDKCONFIG.exists():
        return  # 无 sdkconfig（宿主/门禁环境）时跳过 / nothing to check without a sdkconfig
    cfg = SDKCONFIG.read_text(errors="replace")
    if re.search(r"^CONFIG_PICO_EXCERPT_AI_SEND=y\s*$", cfg, flags=re.M):
        if not PROTO_REGISTRY.exists():
            bad("B33 the protocol registry %s is missing while bit2 is enabled"
                % PROTO_REGISTRY.relative_to(ROOT).as_posix())
            return
        # 登记簿 = docs/PROTOCOL.md 里**含 bit2 的那一行**：日期与版本必须落在同一行上，
        # 否则「bit0 那行有日期」也能把 bit2 蒙过去。
        # The registry is the bit2 ROW of docs/PROTOCOL.md: date and version must sit on
        # the same line, or bit0's credentials would silently vouch for bit2.
        rows = [ln for ln in PROTO_REGISTRY.read_text(errors="replace").splitlines()
                if "bit2" in ln.lower() or "flag_excerpt" in ln.lower()]
        ok = any(DATE_RE.search(ln) and APK_RE.search(ln) for ln in rows)
        if not ok:
            bad("B33 bit2 is enabled but %s has no bit2 row carrying BOTH a real interop date "
                "(YYYY-MM-DD) and a real APK version (vX.Y.Z): force "
                "CONFIG_PICO_EXCERPT_AI_SEND back to n"
                % PROTO_REGISTRY.relative_to(ROOT).as_posix())


def _guarded_ranges(src, macro):
    """返回被 `#if <macro>` 包住的字符区间（#else 分支不算）。/ Ranges guarded by `#if macro`."""
    out = []
    stack = []
    pos = 0
    for line in src.splitlines(keepends=True):
        st = line.strip()
        if st.startswith("#if"):
            stack.append({"g": macro in st, "start": pos, "else": False})
        elif st.startswith("#else") and stack:
            top = stack[-1]
            if top["g"] and not top["else"]:
                out.append((top["start"], pos))
                top["g"] = False
            top["else"] = True
        elif st.startswith("#endif") and stack:
            top = stack.pop()
            if top["g"]:
                out.append((top["start"], pos))
        pos += len(line)
    return out


def check_b34_switch_consistency():
    """S26-3：开关必须真的管住发送，置灰必须真的画出来。"""
    src = strip_comments(APP_BOOK.read_text(errors="replace"))
    ranges = _guarded_ranges(src, "CONFIG_PICO_EXCERPT_AI_SEND")

    def inside(pos):
        return any(a <= pos < b for a, b in ranges)

    # 摘抄上行是**带 FLAG_EXCERPT 的那条**：M3 的单词情景查询也在同一个文件里调同一个
    # 发送函数，那是既有功能，不归本开关管。
    # Only the FLAG_EXCERPT call belongs to this switch; the M3 word lookup shares the
    # same sender and is none of the switch's business.
    marks = [m.start() for m in re.finditer(r"\bPUSH_SCENE_FLAG_EXCERPT\b", src)]
    if not marks:
        bad("B34 app_book.c never sets PUSH_SCENE_FLAG_EXCERPT: the switch would turn on "
            "nothing, which is exactly the half-wired module --gc-sections hides")
    for pos in marks:
        if not inside(pos):
            bad("B34 PUSH_SCENE_FLAG_EXCERPT at offset %d sits OUTSIDE "
                "`#if CONFIG_PICO_EXCERPT_AI_SEND`: a build with the switch off would still "
                "air an excerpt frame" % pos)
    send_guarded = any(
        a <= m.start() < b
        for a, b in ranges
        for m in re.finditer(r"\bpush_sink_ai_scene_send\s*\(", src)
    )
    if not send_guarded:
        bad("B34 the send call is not inside the switch guard: with the switch off the "
            "excerpt path would still air a frame")

    def body_of(pattern):
        m = re.search(pattern + r"[^;{]*\{", src)
        if not m:
            return ""
        depth, i = 0, m.end() - 1
        while i < len(src):
            if src[i] == '{':
                depth += 1
            elif src[i] == '}':
                depth -= 1
                if depth == 0:
                    return src[m.end():i]
            i += 1
        return ""

    # 按键线（S26-4 收口账 4）：实体键不触达动作栏——它只走「关框选」那一条，不许
    # 调 boxsel_action()。否则「触摸按不动的 AI 键」会被三个实体键按动，置灰就白置了。
    # Key path (S26-4 item four): physical keys never reach the action bar - they only
    # close the selection. Otherwise a greyed key that touch cannot press could be pressed
    # by a key, and the grey would be decoration.
    for fn, name in ((r"static app_redraw_t on_key\(", "on_key"),
                     (r"static app_redraw_t on_key_long\(", "on_key_long")):
        kbody = body_of(fn)
        if not kbody:
            bad("B34 %s() is gone: the key-path clause lost its anchor" % name)
        elif "boxsel_action(" in kbody:
            bad("B34 %s() calls boxsel_action(): a physical key must only close the "
                "selection, never press an action-bar key" % name)

    for fn, name, why in ((r"static void boxsel_draw\(", "boxsel_draw", "draw the key greyed"),
                          (r"static app_redraw_t boxsel_touch\(", "boxsel_touch",
                           "not consume the tap")):
        body = body_of(fn)
        if not body:
            bad("B34 %s() is gone: the greyed-button contract lost its anchor" % name)
        elif "excerpt_ai_enabled()" not in body:
            bad("B34 %s() no longer consults excerpt_ai_enabled(): the greyed key must be "
                "decided by the same switch in draw and hit-test (%s)" % (name, why))


def _body_of(src, pattern):
    """返回匹配到的函数体（不含花括号）。/ Body of the first function matching `pattern`."""
    m = re.search(pattern + r"[^;{]*\{", src)
    if not m:
        return ""
    depth, i = 0, m.end() - 1
    while i < len(src):
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                return src[m.end():i]
        i += 1
    return ""


def check_b35_mount_wiring():
    """S26-4 / E-2：让行必须由**挂载代号**驱动，不是状态边沿，也不是每一拍。

    S26-4 原本咬的是「挂在哪个条件上」。E-2 把锚点从条件换成了事件，这条牙跟着换：
    现在要咬的是「传入的是代号」以及「不许退回状态边沿」。E-2 现场正是后者失败——
    `gained = !*mounted && ready` 在拔插快于一次轮询时全程为 false，让行一次都没落下，
    100.977 挂载、100.994 就写卡（17 ms），而 `mounted=1` 到 101.863 才出现。
    S26-4 asked "which condition". E-2 moved the anchor from a condition to an event, so the
    tooth now asks "is the argument the epoch" and "did it fall back to a state edge".
    """
    src = strip_comments((ROOT / "main/app/app_loop.c").read_text(errors="replace"))
    poll = _body_of(src, r"static bool poll_media\(")
    if not poll:
        bad("B35 poll_media() is gone: mount observation lost its anchor, so nothing can "
            "ever arm the excerpt yield window")
        return
    if not re.search(r"excerpt_store_mount_yield\s*\(\s*read_pico_sd_mount_epoch\s*\(\s*\)", poll):
        bad("B35 poll_media() does not arm the yield window from the mount EPOCH: either the "
            "call is gone (the queue flushes straight into the shelf scan) or it is fed "
            "something other than the epoch (the anchor is back to a snapshot)")
    # 反证：退回状态边沿。这是 E-2 的原始病灶，必须点名拦下而不是笼统说"接线不对"。
    # Counter-proof: falling back to a state edge - the exact E-2 defect.
    if re.search(r"if\s*\(\s*gained\s*\)\s*\{?\s*excerpt_store_", poll):
        bad("B35 the yield is armed inside `if (gained)` again: that edge is a state "
            "difference, and a replug faster than one poll interval keeps it false for the "
            "whole window (886 ms in the 26.9.31 trace)")


def check_b36_footer_contract():
    """S26-4：常显靠页脚读队列深度，作废靠「设时盖页号、画时比页号」。"""
    src = strip_comments(APP_BOOK.read_text(errors="replace"))
    band = _body_of(src, r"static void reader_footer_band_paint\(")
    if not band:
        bad("B36 reader_footer_band_paint() is gone: the only strip that is on every page "
            "lost its anchor, so 'pending N' has nowhere to live")
        return
    if "excerpt_footer_prefix(" not in band:
        bad("B36 reader_footer_band_paint() no longer calls excerpt_footer_prefix(): the "
            "persistent warning would disappear from ordinary pages")
    prefix = _body_of(src, r"static void excerpt_footer_prefix\(")
    if "excerpt_store_pending(" not in prefix:
        bad("B36 excerpt_footer_prefix() no longer reads excerpt_store_pending(): the only "
            "truth behind 'pending N' would be replaced by a guess")
    if not re.search(r"s_excerpt_hint_page\s*==\s*s_page", prefix):
        bad("B36 the receipt is not page-bound: excerpt_footer_prefix() must compare "
            "s_excerpt_hint_page against s_page, or a receipt would follow the reader "
            "onto a page it never belonged to")
    setter = _body_of(src, r"static void excerpt_hint_set\(")
    if not re.search(r"s_excerpt_hint_page\s*=\s*s_page", setter):
        bad("B36 excerpt_hint_set() no longer stamps the page number: the comparison in "
            "excerpt_footer_prefix() would then be against a stale page")


def check_b37_receipt_class():
    """S26-4：坏消息不被好消息顶掉——前提是每条回执都带等级。"""
    src = strip_comments(APP_BOOK.read_text(errors="replace"))
    calls = re.findall(r"excerpt_hint_set\s*\(([^;]*?)\)\s*;", src)
    if not calls:
        bad("B37 app_book.c never sets a receipt: the footer channel is dead")
    for arg in calls:
        if "," not in arg:
            bad("B37 an excerpt_hint_set() call carries no class (%r): an unclassified "
                "receipt is exactly how bad news gets overwritten by good news" % arg.strip()[:48])
    for text in ("已存 ✓", "AI 已发"):
        m = re.search(r'excerpt_hint_set\s*\(\s*"%s"\s*,\s*([A-Z_]+)' % re.escape(text), src)
        if m and m.group(1) != "EXCERPT_HINT_INFO":
            bad("B37 %r is good news but is filed as %s" % (text, m.group(1)))
    for text in ("未发送", "摘抄已满", "内存不足", "[截] 已按上限摘录", "无选中文本", "存储失败"):
        for m in re.finditer(r'excerpt_hint_set\s*\(\s*"%s"\s*,\s*([A-Z_]+)' % re.escape(text), src):
            if m.group(1) != "EXCERPT_HINT_ALERT":
                bad("B37 %r demands a decision but is filed as %s" % (text, m.group(1)))
    setter = _body_of(src, r"static void excerpt_hint_set\(")
    if "EXCERPT_HINT_ALERT" not in setter or "EXCERPT_HINT_INFO" not in setter:
        bad("B37 excerpt_hint_set() no longer distinguishes the two classes: the rule "
            "'alert survives info' has to live in the setter, not in the callers' discipline")
    elif "return;" not in setter:
        bad("B37 excerpt_hint_set() never returns early: the alert-survives-info rule is "
            "written but not enforced")


# ---- B38：路径口径 ------------------------------------------------------------
# 为什么要有这颗牙：26.9.28 的事故里，代码是"对的写法 + 错的词汇"——裸 FatFs 收到 VFS
# 挂载点路径，恒 FR_NO_PATH；而栈上 FIL 未清零让这条失败变成了重启。任何一条都不在
# 原有牙的射程内：B32' 只管"文件操作该在哪个文件里"，不管路径长什么样。
# Why this tooth exists: in 26.9.28 the code was the right shape with the wrong vocabulary.
# B32' only cared about WHICH file does IO, never about what the path looks like.
RAW_FATFS_CALLS = (
    "f_open", "f_write", "f_read", "f_sync", "f_close", "f_mkdir", "f_opendir", "f_closedir",
    "f_stat", "f_unlink", "f_rename", "f_getfree", "f_mount", "f_lseek", "f_truncate",
)
# 白名单只剩一处，且理由必须写在这里：SD 驱动层持有卡句柄，盘号在运行期由
# `ff_diskio_get_pdrv_card()` 取、路径用 `"%u:"` 拼——不写死盘号，也不混 VFS 路径。
# One entry only: the SD driver layer owns the card handle and derives the drive number at
# runtime, so it never hardcodes a drive and never mixes vocabularies.
RAW_FATFS_WHITELIST = {
    "components/read_pico/read_pico_sd.c":
        "SD 驱动层：盘号运行期取，路径 \"%u:\" 拼 / SD driver layer, drive at runtime",
}
VENDOR_PREFIXES = ("components/espressif__",)  # 第三方组件，不属本仓代码 / vendor code


def check_b38_path_vocabulary():
    for path in sorted(ROOT.glob("main/**/*.c")) + sorted(ROOT.glob("components/**/*.c")):
        rel = path.relative_to(ROOT).as_posix()
        if rel.startswith("build") or rel.startswith(VENDOR_PREFIXES):
            continue
        body = strip_comments(path.read_text(errors="replace"))
        hits = [c for c in RAW_FATFS_CALLS if re.search(r"\b%s\s*\(" % c, body)]
        if re.search(r'#\s*include\s*[<"]ff\.h[>"]', body):
            hits.append('#include "ff.h"')
        if hits and rel not in RAW_FATFS_WHITELIST:
            bad("B38 %s uses bare FatFs (%s): pick ONE vocabulary - business code goes "
                "through the VFS (`/sdcard/...`), only the SD driver layer may talk to FatFs"
                % (rel, "/".join(hits)))
    for rel, why in RAW_FATFS_WHITELIST.items():
        if not (ROOT / rel).exists():
            bad("B38 whitelisted %s does not exist (%s)" % (rel, why))

    ex = strip_comments((ROOT / "main/book/excerpt_store.c").read_text(errors="replace"))
    if "#include \"ff.h\"" in ex:
        bad("B38 excerpt_store.c includes ff.h: the module must stay on the VFS side")
    if '"/sdcard/' not in (ROOT / "main/book/excerpt_store.h").read_text(errors="replace"):
        bad("B38 EXCERPT_PATH no longer starts with /sdcard/: the VFS mount point is the only "
            "path vocabulary this module may speak (rename the mount point, rename this tooth)")
    # 先写后亮的物理心跳：fsync 必须真的在尾巴上，且 fopen 必须是追加。
    # 咬的是**生产那条宏**，不是散文里出现的 "fsync" 三个字母：注释和日志里那几处
    # "fsync (" 曾经把这条判据骗过去一次（M115 存活），所以这里要求整行原样。
    # This bites the production macro itself, not the word "fsync" in prose: log strings
    # like "fsync (data is durable)" fooled the loose version once, and M115 survived.
    if not re.search(r"#define\s+ex_sync\(f\)\s+fsync\(fileno\(f\)\)", ex):
        bad("B38 the flush tail no longer maps ex_sync() to fsync(fileno()): 'saved' would be "
            "a guess instead of a fact")
    if not re.search(r'fopen\(\(p\),\s*"ab"\)', ex):
        bad("B38 the flush tail no longer opens in append mode: every save would overwrite "
            "the history")
    if not re.search(r"if\s*\(\s*ex_mkdir\([^)]*\)\s*!=\s*0", ex):
        bad("B38 mkdir's return value is not judged: a missing directory would be discovered "
            "one step later, at open() - and that is exactly where 26.9.28 died")


# B41（26.9.30 commit 2 上线）：提交段封送。COMMIT 在 BLE 回调里只许投递，四拍
# 只在主循环的 push_sink_run_commit() 里跑。
# B41 (live with 26.9.30 commit 2): the commit hand-off. COMMIT on the BLE callback
# may only post; the four beats belong to push_sink_run_commit() on the main loop.
PUSH_SINK_C = ROOT / "components/push_sink/src/push_sink.c"
PUSH_PART_C = ROOT / "components/push_sink/src/push_part.c"

# 禁列 = 「把 .part 变成成品」这一步（rename / remove / truncate）与关句柄。
# `part_*` 不在此列：接收期写 .part 是允许的，被封送的是提交那一刻。
# Forbidden = turning .part into the product (rename / remove / truncate) plus
# closing the handle. `part_*` is deliberately absent: writing the partial while
# receiving is fine; what gets handed off is the commit instant.
B41_FORBIDDEN = ("rename", "remove", "truncate", "fclose", "do_commit")


def push_func_body(src, pattern):
    """按大括号配平取函数体（已 strip_comments）。/ Brace-balanced body, comments off."""
    m = re.search(pattern + r"[^;{]*\{", src)
    if not m:
        return ""
    depth, i = 0, m.end() - 1
    while i < len(src):
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                return src[m.end():i]
        i += 1
    return ""


def check_b41_commit_offload():
    if not PUSH_SINK_C.exists():
        bad("B41 %s is missing: the commit path cannot be judged" % PUSH_SINK_C)
        return
    src = strip_comments(PUSH_SINK_C.read_text(errors="replace"))
    bodies = {
        "exec_actions": push_func_body(src, r"static void exec_actions\("),
        "exec_actions_body": push_func_body(src, r"static void exec_actions_body\("),
    }
    # 两个函数体都判：commit 1 已把 wrapper 拆成「body(act) + 按状态 part_close()」，
    # 只判 wrapper 时 part_close 不在禁列，判据当场假绿。
    # Both bodies are judged: commit 1 split the wrapper into body(act) plus a
    # state-gated part_close(); judging the wrapper alone is green by construction.
    for name, body in bodies.items():
        if not body:
            bad("B41 %s() is gone: the hand-off lost its anchor - a deleted function passes "
                "a purely negative check silently" % name)
            continue
        hits = [c for c in B41_FORBIDDEN if re.search(r"\b%s\s*\(" % c, body)]
        if hits:
            bad("B41 %s() still performs storage work (%s): COMMIT may only post - the four "
                "beats belong to push_sink_run_commit() on the main loop, never to the BLE "
                "callback stack" % (name, "/".join(hits)))
    # 正向断言：把整段 COMMIT 删掉也能让上面的负判据通过，所以「投递」本身要钉住。
    # Positive: deleting the whole COMMIT branch satisfies the negative check, so the
    # hand-off call itself has to be proven present.
    body = bodies["exec_actions_body"]
    if body and "ui_post_commit_request(" not in body:
        bad("B41 exec_actions_body() no longer posts a commit request: the negative check "
            "would pass on a deleted branch")
    run = push_func_body(src, r"bool push_sink_run_commit\(")
    if not run:
        bad("B41 push_sink_run_commit() is gone: every request would be taken and dropped")
    elif not re.search(r"\bdo_commit\s*\(", run):
        bad("B41 push_sink_run_commit() does not call do_commit(): the four beats vanished "
            "from the only place allowed to run them")
    # 抽层补强（26.9.30 commit 5 审计核对项 3，测试侧，不动产品代码）：.part 的
    # FILE* 全部搬进 push_part.c 之后，上面四个禁名在 push_sink.c 里已不可能出现
    # ——负判据对「经 push_part API 在回调栈上做提交段工作」失明。改为许可清单：
    # BLE 回调栈（exec_actions / body）只许调**接收路径**的 push_part_*；提交瞬间
    # 的操作（push_part_rewind = truncate+重开，push_part_adopt = 账本对齐）与未来
    # 新增的任何 push_part 入口，一旦出现在回调体里即判红。
    # Layering hardening (commit 5 audit item 3, test-side): once every FILE* moved
    # into push_part.c, the four forbidden names can no longer appear in
    # push_sink.c - the negative check is blind to commit-stage work done through
    # the push_part API on the callback stack. Switch to an allowlist: the BLE
    # callback stack may only call the receiving-path push_part_* entries;
    # commit-instant calls (rewind = truncate + reopen, adopt = ledger alignment)
    # and any future push_part entry in a callback body is a red.
    B41_CB_ALLOWED = ("push_part_discard", "push_part_open_fresh",
                      "push_part_drain", "push_part_append", "push_part_sync",
                      "push_part_close", "push_part_len")
    for name, body in bodies.items():
        if not body:
            continue
        calls = sorted(set(re.findall(r"\b(push_part_\w+)\s*\(", body)))
        strangers = [c for c in calls if c not in B41_CB_ALLOWED]
        if strangers:
            bad("B41 %s() reaches push_part entries outside the receiving set (%s): the "
                "BLE callback stack may touch .part only while receiving - commit-instant "
                "work belongs to run_commit on the main loop" % (name, "/".join(strangers)))


# B47（26.9.30 commit 4 上线，审计 2026-10-31 附注 1）：一次 take 只许一条终态出口。
# 缺了它：死线外的 abort 只清 carry、不清「谁还欠这次提交」，而泵每帧都进——要么
# 每帧重复 abort（第二次 reply 破坏会话），要么读者恰在此间释放、run_commit 照跑，
# abort 被静默绕过，那正是「幽灵提交」。
# B47 (live with commit 4, audit note 1): one take, exactly one terminal exit.
# Without it the abort outside the deadline clears only the carry, not "who still
# owes this commit" - and the pump runs every frame: either a repeated abort (a
# second reply wrecks the session) or the reader releases in between and
# run_commit runs anyway, silently bypassing the abort (a ghost commit).
def check_b47_commit_ticket():
    if not PUSH_SINK_C.exists():
        bad("B47 %s is missing: the commit ticket cannot be judged" % PUSH_SINK_C)
        return
    src = strip_comments(PUSH_SINK_C.read_text(errors="replace"))
    if not re.search(r"\bbool\s+commit_live\s*;", src):
        bad("B47 s_ui has no commit_live field: nothing marks 'this take still owes a "
            "terminal exit', so abort cannot be made idempotent")
        return
    take = push_func_body(src, r"bool push_sink_take_commit_request\(")
    if not take or not re.search(r"if\s*\(\s*pending\s*\)\s*s_ui\.commit_live\s*=\s*true\s*;", take):
        bad("B47 take_commit_request does not open the ticket on a live hand-off: an "
            "expired post must not arm the gate, and a live one must")
    run = push_func_body(src, r"bool push_sink_run_commit\(")
    if not run:
        bad("B47 push_sink_run_commit() is gone")
    else:
        i_ticket = run.find("commit_live")
        i_do = re.search(r"\bdo_commit\s*\(", run)
        if i_ticket < 0:
            bad("B47 run_commit has no ticket check: a stray call would rename/remove with "
                "no push in flight (ghost commit)")
        elif i_do and i_ticket > i_do.start():
            bad("B47 run_commit checks the ticket AFTER do_commit(): the four beats already "
                "ran - checking late cannot stop a re-entry")
        elif not re.search(r"s_ui\.commit_live\s*=\s*false\s*;", run):
            bad("B47 run_commit never consumes the ticket: one take could run twice")
    abort = push_func_body(src, r"void push_sink_commit_abort\(")
    if not abort:
        bad("B47 push_sink_commit_abort() is gone")
    else:
        i_gate = abort.find("commit_live")
        i_reply = re.search(r"\breply_err\s*\(", abort)
        if i_gate < 0:
            bad("B47 commit_abort is not idempotent: a second arrival would reply a second "
                "ERR for a commit that already ended")
        elif i_reply and i_gate > i_reply.start():
            bad("B47 commit_abort replies BEFORE checking the ticket: the duplicate reply "
                "wrecks the session before the gate is ever consulted")
        elif not re.search(r"s_ui\.commit_live\s*=\s*false\s*;", abort):
            bad("B47 commit_abort never consumes the ticket: the next frame could abort again")
    # 等待态先清、再 abort：顺序错则下一帧 now > deadline 仍成立，泵会重入 abort。
    # The wait flag clears BEFORE the abort call; reversed, the next frame still has
    # now > deadline and re-enters the abort.
    if APP_TRANSFER_C.exists() or TRANSFER_COMMIT_C.exists():
        body = commit_pump_body()
        if not body:
            bad("B47 transfer_pump_commit() is gone")
        else:
            i_clear = body.find("s_commit_hold_wait = false;")
            i_abort = body.find("push_sink_commit_abort(")
            if i_clear < 0:
                bad("B47 the pump never clears s_commit_hold_wait: the wait is an one-shot "
                    "advancer, not a session property")
            elif i_abort >= 0 and i_clear > i_abort:
                bad("B47 the pump calls commit_abort BEFORE clearing s_commit_hold_wait: the "
                    "next frame re-enters the abort")


# B40（26.9.30 commit 4 上线）：四处静默。写失败必须留痕，尤其是「位置已脏」那一处——
# fwrite 判了、fflush 没判，等于后续包静默写到错位。
# B40 (live with commit 4): the four silent spots. A write failure must leave a trace,
# especially "the position is dirty": checking fwrite but not fflush silently shifts
# every later packet.
def check_b40_silent_writes():
    if not PUSH_PART_C.exists():
        bad("B40 %s is missing: the write path cannot be judged" % PUSH_PART_C)
        return
    src = strip_comments(PUSH_PART_C.read_text(errors="replace"))
    put = push_func_body(src, r"static bool part_put\(")
    if not put:
        bad("B40 part_put() is gone: the only place that advances the ledger")
    elif not re.search(r"if\s*\(\s*fflush\s*\(\s*s_part\s*\)\s*!=\s*0\s*\)\s*(return|\{)", put):
        bad("B40 part_put() does not check fflush(): f_write can fail while fwrite already "
            "reported len, and every later packet lands at a shifted offset")
    if not re.search(r"if\s*\(\s*fwrite\([^;]*\)\s*!=\s*len\s*\)", src):
        bad("B40 part_put() no longer checks the short write: a partial packet would be "
            "counted as a whole one")
    sink = strip_comments(PUSH_SINK_C.read_text(errors="replace")) if PUSH_SINK_C.exists() else ""
    for needle, what in (
        ("append seq=%u errno=%d", "the append failure line (seq/errno/lost/offset)"),
        ("checkpoint sync failed", "the checkpoint sync failure line"),
        ("part open(%s) errno=%d", "the part-open failure line"),
    ):
        if needle not in sink:
            bad("B40 %s is gone: a failure without a trace is a defect of its own" % what)
    # `(void)fflush` 白名单：只允许关闭前那一处（fclose 会 sync，判它没有意义）。
    # Whitelist: only the pre-close one - fclose syncs, so judging it there says nothing.
    for path, text in ((PUSH_PART_C, src), (PUSH_SINK_C, sink)):
        if not text:
            continue
        n = len(re.findall(r"\(void\)\s*fflush\s*\(", text))
        if n > (1 if path == PUSH_PART_C else 0):
            bad("B40 %s has %d swallowed fflush calls (whitelist: 1 pre-close in push_part.c, "
                "0 elsewhere): a silent write failure is back" % (path.name, n))


# B49（26.9.30 commit 1 上线，commit 5 补牙）：四拍顺序与「禁删成品」。
# 顺序是修复本体：先删再改名（gen16 为躲 EEXIST 的写法）一旦改名失败，旧书就没了——
# 26.9.30 调查里"文件消失"的唯一机制。拍 2 失败必须回滚，且回滚要在日志里指名 .bak。
# B49 (live with commit 1, tooth added in 5): the four-beat order and the product
# removal ban. Remove-then-rename loses the old book the moment the rename fails.
def check_b49_commit_dance():
    if not PUSH_SINK_C.exists():
        bad("B49 %s is missing" % PUSH_SINK_C)
        return
    src = strip_comments(PUSH_SINK_C.read_text(errors="replace"))
    dance = push_func_body(src, r"static bool commit_dance\(")
    if not dance:
        bad("B49 commit_dance() is gone: the four beats have no home")
        return
    park = dance.find("rename(PUSH_TEXT_PATH, PUSH_BAK_PATH)")
    install = dance.find("rename(PUSH_PART_PATH, PUSH_TEXT_PATH)")
    if park < 0:
        bad("B49 beat 1 (park text into .bak) is gone: the old book would be removed "
            "instead of parked")
    if install < 0:
        bad("B49 beat 2 (install part as text) is gone: nothing ever becomes the product")
    if park >= 0 and install >= 0 and park > install:
        bad("B49 beat 2 runs BEFORE beat 1: the new book lands over the old one and the "
            "park has nothing left to save")
    if install >= 0:
        tail = dance[install:]
        if "rename(PUSH_BAK_PATH, PUSH_TEXT_PATH)" not in tail:
            bad("B49 beat 2 has no rollback: a failed install would leave the reader with "
                "neither the new book nor the old one")
    if re.search(r"\bremove\s*\(\s*PUSH_TEXT_PATH\s*\)", src):
        bad("B49 the product is removed somewhere: that is the gen16 EEXIST fix, and it is "
            "exactly what lost the old book")


# B50（同上）：先捡后清，且 BAK 的清理只能在 install 之后。
# 拍 0 的 recover 必须在第一处 remove(PUSH_BAK) 之前——否则上一次被打断的提交留下的
# 唯一那份书会被当成垃圾删掉。拍 3 的清理若跑到拍 2 之前，就是"新书还没就位就扔旧的"。
# B50: recover before clear, and BAK is only cleared after the install. Reversed,
# the only surviving copy from an interrupted commit is deleted as garbage.
def check_b50_bak_order():
    if not PUSH_SINK_C.exists():
        bad("B50 %s is missing" % PUSH_SINK_C)
        return
    src = strip_comments(PUSH_SINK_C.read_text(errors="replace"))
    dance = push_func_body(src, r"static bool commit_dance\(")
    if not dance:
        bad("B50 commit_dance() is gone")
        return
    recover = dance.find("recover_bak_if_needed(")
    first_remove = dance.find("remove(PUSH_BAK_PATH)")
    install = dance.find("rename(PUSH_PART_PATH, PUSH_TEXT_PATH)")
    if recover < 0:
        bad("B50 beat 0 no longer recovers .bak: an interrupted commit's only copy would be "
            "treated as garbage")
    elif first_remove >= 0 and recover > first_remove:
        bad("B50 the stale-bak clear runs BEFORE the recovery: the surviving copy is deleted "
            "before anyone looks at it")
    if install >= 0:
        last_remove = dance.rfind("remove(PUSH_BAK_PATH)")
        if last_remove >= 0 and last_remove < install:
            bad("B50 .bak is cleared before the new book lands: the old one is thrown away "
                "while nothing has replaced it")
    allowed = {"PUSH_BAK_PATH", "PUSH_PART_PATH"}
    for m in re.finditer(r"\bremove\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", src):
        if m.group(1) not in allowed:
            bad("B50 remove(%s) is outside the whitelist (bak/part only): the product or a "
                "user file would be deleted" % m.group(1))


# B46（26.9.30 commit 3 上线）：读者句柄释放（机制二）。提交侧的 rename/remove 会把
# 读者脚下那条簇链还给 FAT，所以提交前必须把读者对 push.txt 的持有放掉——而且是
# **完整关书**，不是只关句柄（book_close() 会 memset s_book，下一页 fread(NULL)）。
# B46 (live with 26.9.30 commit 3): releasing the reader's handle (mechanism #2). The
# commit side returns the reader's clusters to FAT, so the hold on push.txt has to go
# before it - as a full close, never as handle-only (book_close() memsets s_book).
APP_BOOK_C = ROOT / "main/apps/app_book.c"
APP_TRANSFER_C = ROOT / "main/apps/app_transfer.c"
# 26.11.1：两拍泵抽到独立文件（宿主跑真身）。牙跟着检查对象走——§13.4 牙体检
# 通例：检查对象搬走就必须同批移栽，不许记「照常 PASS」。
# 2026-11-01: the two-beat pump moved to its own file so the host can run the real
# body. The teeth follow their subject (house rule §13.4: when the subject moves,
# transplant in the same batch - never record "still passing").
TRANSFER_COMMIT_C = ROOT / "main/apps/transfer_commit.c"
HOLD_BITS = ("BOOK_HOLD_PREP", "BOOK_HOLD_DRAW", "BOOK_HOLD_TURN", "BOOK_HOLD_WATER",
             "BOOK_HOLD_FOOTER", "BOOK_HOLD_BOXSEL", "BOOK_HOLD_PANEL")


def commit_pump_body():
    """两拍泵的函数体。26.11.1 起在 `transfer_commit.c`（`transfer_commit_pump`），
    `app_transfer.c` 只剩同名封装 `transfer_pump_commit` -> 转发。两种形态都认，
    但**封装必须真的转发**，否则等于泵被摘掉而牙还绿——那就是死牙的形状。
    The pump body: since 2026-11-01 it lives in transfer_commit.c as
    transfer_commit_pump(); app_transfer.c keeps a same-named wrapper that forwards.
    Both shapes are accepted, but the wrapper must really forward - a wrapper that
    no longer calls the pump is a dead tooth with a green light."""
    if TRANSFER_COMMIT_C.exists():
        b = push_func_body(strip_comments(TRANSFER_COMMIT_C.read_text(errors="replace")),
                           r"void transfer_commit_pump\(")
        if b:
            return b
    if APP_TRANSFER_C.exists():
        return push_func_body(strip_comments(APP_TRANSFER_C.read_text(errors="replace")),
                              r"void transfer_pump_commit\(")
    return ""


def commit_pump_wired():
    """若泵已抽层，`app_transfer.c` 的封装必须调它。/ If extracted, the wrapper must call it."""
    if not TRANSFER_COMMIT_C.exists():
        return True
    if not APP_TRANSFER_C.exists():
        return False
    wrapper = push_func_body(strip_comments(APP_TRANSFER_C.read_text(errors="replace")),
                             r"void transfer_pump_commit\(")
    return bool(wrapper) and "transfer_commit_pump(" in wrapper


# B54（31b 第 2 条 B′，2026-11-01）：拔卡**不许**跳书架。
# 三件事缺一件就是回退，每件都能静态看见：
#   ① `book_enter_offline()` 还在、且不把 `s_view` 拨回 SHELF（拨了就是又跳书架）；
#   ② 掉盘页脚用固定文案常量（章节名在卡上，拿不到就会退化成「未命名章节」）；
#   ③ `load_chapter_at()` 的掉盘闸还在——那是唯一的读卡入口闸，删了第二条路就漏过去。
# 另外 `free_book()` 不许出现在掉盘路径里：正文留在 RAM 才是就地阅读的前提。
# B54: card removal must not bounce the reader to the shelf. Three static facts: the
# offline entry point exists and does not touch s_view; the footer prints the fixed
# offline line; the single chapter-load gate is still there. And the offline path must
# not free the book - keeping the body text in RAM is what in-place reading rests on.
B54_NOTE = "READER_OFFLINE_NOTE"
B54_TAIL = "READER_OFFLINE_TAIL"


PUSH_SINK_C = ROOT / "components/push_sink/src/push_sink.c"
APP_LOOP_C = ROOT / "main/app/app_loop.c"


def check_b55_verify_single_exit():
    """31b 第 1 条：verify 的 FILE* 计数必须成对，且 do_verify 单出口。

    三合取②把 verify 的读句柄从「顺序推断」变成「可查询事实」，代价是多了一笔
    必须减的账。这类计数最经典的 bug 是**异常路径漏减**：中途 return 不减 →
    计数虚高 → quiesce 永远等不到 0 → 症状是「push 永久死，只有重启能出」，
    与 31a 五环死锁同族。所以这里咬两件事：两端都在、整函数没有 return。
    """
    src = strip_comments(PUSH_SINK_C.read_text(errors="replace"))
    body = _body_of(src, r"static\s+void\s+do_verify\s*\(\s*void\s*\)")
    if not body:
        bad("B55 do_verify() is gone: conjunct 2 (live FILE* count) lost its only "
            "measurement point")
        return
    inc = body.find("++s_verify_depth")
    dec = body.find("--s_verify_depth")
    if inc < 0 or dec < 0:
        bad("B55 do_verify() no longer counts s_verify_depth on both ends: quiesce cannot "
            "see the verify reader, so the conjunction degrades to an inference again")
        return
    if dec < inc:
        bad("B55 do_verify() decrements s_verify_depth before incrementing it: the count "
            "goes negative and quiesce passes while the reader is still open")
    # 单出口：void 函数里出现任何 return，就会有一条路径跳过 --。
    m = re.search(r"\breturn\b", body)
    if m:
        line = body[:m.start()].count("\n") + 1
        bad("B55 do_verify() has a return at body line %d: that path skips the --, the "
            "count stays high, quiesce waits forever and push dies until reboot" % line)


def check_b56_three_beat_wiring():
    """31b 第 1 条：三拍的接线必须在，且解冻只能由 app 观测边沿驱动。

    组件不反向读 SD（§13.8 任务拓扑）——SD 发布事实、app 观测、app 调解冻。
    这里咬四件事：三处入口判点、两处解冻边沿、两处 quiesce 调用、看门狗落地。
    """
    src = strip_comments(PUSH_SINK_C.read_text(errors="replace"))
    for fn, pat in (
        ("on_ctrl_write", r"static\s+void\s+on_ctrl_write\s*\("),
        ("on_data_write", r"static\s+void\s+on_data_write\s*\("),
    ):
        body = _body_of(src, pat)
        if "entry_frozen_reply" not in body:
            bad("B56 %s() lost the frozen-entry guard: a session can enter while the "
                "volume is being unmounted" % fn)
    adopt = _body_of(src, r"static\s+void\s+try_adopt\s*\(\s*void\s*\)")
    if "push_sink_frozen" not in adopt:
        bad("B56 try_adopt() no longer checks the freeze: ARM's truncate carries no "
            "handle, so conjunct 2 can never see it - only this guard can")
    # 看门狗要**真的在判**，不能只留符号：`false && ...` 或 `if (0)` 都能让
    # 符号检查全绿而看门狗永不触发——"存在"不等于"生效"。
    # The watchdog must actually decide: a `false &&` short-circuit keeps every
    # symbol in place and the watchdog dead.
    m = re.search(r"const bool freeze_expired\s*=\s*([^;]+);", src)
    if not m:
        bad("B56 the freeze watchdog lost its predicate: a missed terminal branch leaves "
            "push frozen until reboot (the 31a shape)")
    else:
        expr = m.group(1)
        if ("FRZ_FROZEN" not in expr or "FRZ_HOLD" not in expr
                or "PUSH_FREEZE_WATCHDOG_MS" not in expr):
            bad("B56 the watchdog predicate no longer tests frozen/hold/deadline together: "
                "a poisoned-volume hold would be force-thawed onto a dead volume")
        elif re.sub(r"\s+", "", expr).startswith(("false&&", "0&&")):
            bad("B56 the watchdog predicate is short-circuited: the symbols are all there "
                "and it never fires (presence is not effect)")

    loop = strip_comments(APP_LOOP_C.read_text(errors="replace"))
    if loop.count("push_sink_quiesce(") < 2:
        bad("B56 app_loop no longer quiesces before both release paths (main + latch)")
    if loop.count("push_sink_thaw(") < 3:
        bad("B56 app_loop lost a thaw edge: unmount-done (main + latch) and remount must "
            "all thaw, or the freeze outlives the three-beat")
    if 'push_sink_thaw("remount edge")' not in loop:
        bad("B56 the remount edge no longer thaws: that is the primary thaw path for "
            "format/USB/sleep, where the card never leaves")
    # 两条岔路都要标记：quiesce 全败（会话没停）与卸载失败，症状不同、处置相同。
    # 只留一处，另一条岔路会静默地把毒卷解冻。
    if loop.count("push_sink_quiesce_hold(") < 2:
        bad("B56 the latch no longer marks both poisoned-volume branches (quiesce never "
            "satisfied / unmount failed): the watchdog would force a thaw onto a dead "
            "volume")
    if not re.search(r"pico_bootbb_note\s*\(\s*PICO_BOOTBB_NOTE_PUSH_FREEZE", loop):
        bad("B56 the watchdog event no longer reaches the blackbox")
    if re.search(r"if\s*\(\s*0\s*\)\s*pico_bootbb_note", loop):
        bad("B56 the blackbox call is dead code (`if (0)`): the note is written never and "
            "the log line says otherwise")

    # 审计 2026-11-01 补账：**hold 进入必须留痕**。三拍全败进 hold 时只有一行 ESP_LOGW，
    # 那次会话的串口日志一丢，下次开机查「push 为什么死了」就没有任何依据。
    # 咬两处：组件里 hold 自己写黑匣子（不是靠调用方记得写），以及原因码被真传下去。
    # Audit 2026-11-01: entering a hold must leave a trace. Before this, an all-fail hold
    # printed one ESP_LOGW line; lose the capture and the next boot cannot answer "why is
    # push dead". The note must be written by the hold itself, not by a caller's memory.
    hold = _body_of(src, r"void\s+push_sink_quiesce_hold\s*\(")
    if not hold:
        bad("B56 push_sink_quiesce_hold() is gone: the poisoned-volume hold lost its home")
    elif not re.search(r"pico_bootbb_note\s*\(\s*PICO_BOOTBB_NOTE_PUSH_HOLD", hold):
        bad("B56 push_sink_quiesce_hold() writes no blackbox note: a hold that only ever "
            "reached the UART did not happen as far as the next boot is concerned")
    elif "why_code" not in hold:
        bad("B56 push_sink_quiesce_hold() ignores its reason code: every hold would land in "
            "the blackbox as the same anonymous event")
    # 两个原因码**各自**都要被用过。只数总数不够：两处都传 QUIESCE 时总数也是 2，
    # 而黑匣子里两条岔路从此长得一模一样——弱牙三型里「count >= 2 防单岔」的边界正在
    # 这里：它防的是只接一条，防不住两条接到同一个码上。
    # Each reason code must appear on its own. Counting the total is not enough: two call
    # sites passing the SAME code still count 2 while the blackbox can no longer tell the
    # branches apart. That is exactly where "count >= 2" stops being a tooth.
    for code, label in (("PUSH_HOLD_WHY_QUIESCE", "the conjunction never closed"),
                        ("PUSH_HOLD_WHY_RELEASE", "the unmount kept failing")):
        if not re.search(r"%s\b" % code, loop):
            bad("B56 app_loop never passes %s (%s): that branch lands in the blackbox "
                "unnamed, and 'why is push dead' stays unanswered next boot" % (code, label))


def check_b54_offline_reader():
    src = strip_comments(APP_BOOK_C.read_text(errors="replace"))
    body = _body_of(src, r"static\s+bool\s+book_enter_offline\s*\(")
    if not body:
        bad("B54 book_enter_offline() is gone: card removal falls back to the old teardown "
            "that bounces the reader to the shelf")
        return
    if re.search(r"\bs_view\s*=\s*SHELF\b", body):
        bad("B54 book_enter_offline() sets s_view = SHELF: that is exactly the jump this "
            "item removes")
    if "s_media_offline = true" not in body:
        bad("B54 book_enter_offline() never raises s_media_offline: both the gate and the "
            "footer line would stay asleep")
    if "reader_relayout(" not in body:
        bad("B54 book_enter_offline() does not re-typeset: the page table keeps the face "
            "that is about to disappear")
    if re.search(r"\bfree_book\s*\(\s*\)", body):
        bad("B54 book_enter_offline() calls free_book(): the body text in RAM is what makes "
            "in-place reading possible")
    gate = _body_of(src, r"static\s+bool\s+load_chapter_at\s*\(")
    head = gate.split("book_chapter_load_blocks_target")[0]
    if "s_media_offline" not in head:
        bad("B54 load_chapter_at() lost its offline gate: every chapter path reads a card "
            "that is no longer there")
    footer = _body_of(src, r"static\s+void\s+reader_footer_band_paint\s*\(")
    if "s_media_offline" not in footer or B54_NOTE not in footer:
        bad("B54 the footer band no longer prints the fixed offline line")
    if B54_NOTE not in src or B54_TAIL not in src:
        bad("B54 offline copy constants are gone (fixed slot and the tail notice)")


def check_b46_reader_hold():
    for p in (APP_BOOK_C, APP_TRANSFER_C):
        if not p.exists():
            bad("B46 %s is missing: the reader release cannot be judged" % p)
            return
    if not commit_pump_wired():
        bad("B46 transfer_pump_commit() no longer forwards to transfer_commit_pump(): the "
            "pump is unplugged at the call site while the body still looks right")
    pump = commit_pump_body()
    if not pump:
        bad("B46 transfer_pump_commit() is gone: the commit lost its safe point")
    else:
        rel = pump.find("app_book_release_hold(")
        run = pump.find("push_sink_run_commit(")
        if rel < 0:
            bad("B46 the pump never calls app_book_release_hold(): the reader's FILE* survives "
                "the rename/remove and every later fread reads freed clusters (mechanism #2)")
        elif run < 0:
            bad("B46 the pump no longer calls push_sink_run_commit(): every request dies")
        elif rel > run:
            bad("B46 the pump releases the reader AFTER run_commit(): releasing the handle once "
                "the product is already gone is a no-op - the order is the whole fix")
        elif "PUSH_TEXT_PATH" not in pump[rel:rel + 64]:
            bad("B46 the pump releases some path other than PUSH_TEXT_PATH: only the book being "
                "replaced needs releasing")
        if "push_sink_commit_abort(" not in pump:
            bad("B46 a held reader that never clears would drop the request silently: the "
                "deadline branch must reply ERR(BUSY) through push_sink_commit_abort()")
    src = strip_comments(APP_BOOK_C.read_text(errors="replace"))
    rel = push_func_body(src, r"bool app_book_release_hold\(")
    if not rel:
        bad("B46 app_book_release_hold() is gone: nothing frees the reader's handle")
    else:
        if "free_book(" not in rel:
            bad("B46 app_book_release_hold() never calls free_book(): the handle survives the "
                "commit")
        if "book_close(" in rel:
            bad("B46 app_book_release_hold() calls book_close() itself: that is the fake fix - "
                "book_close() memsets s_book, so the next turn freads NULL while the page cache "
                "still points at the old book")
    busy = push_func_body(src, r"uint32_t app_book_hold_busy\(")
    if not busy:
        bad("B46 app_book_hold_busy() is gone: the seven occupancies lost their only home")
    else:
        missing = [b for b in HOLD_BITS if b not in busy]
        if missing:
            bad("B46 app_book_hold_busy() dropped %s: an occupancy that is not reported cannot "
                "gate the release" % "/".join(missing))


EXCERPT_STORE_C = ROOT / "main/book/excerpt_store.c"
EXCERPT_STORE_H = ROOT / "main/book/excerpt_store.h"


def check_b57_excerpt_backoff():
    """31b 第 3 条（E-1 / §19.3）：冲账失败的固定三档退避。

    现场：拔卡 4.7 s 内 389 次重试、49.4 KB 日志 = 115200 的 89%。根因不是「重试」
    本身，而是**失败不记下次时刻**——每个家务片都无条件再试一次。所以这条牙咬的
    不是「有没有重试」，是「重试有没有被时间挡住」。

    五件事，缺一条这条修复就退回原状：
      ① 表就得是 {50,200,1000}（值可静态断言，不许 4× 递进那种两读）；
      ② 索引封顶末档——没有递增律；
      ③ 单一时间判据：旧字段 s_ready_at_ms 必须消失，让行与退避共用 s_next_try_ms；
         两个字段各判各的，就是「让行在挡、退避不在挡」的复发形态；
      ④ 三条失败路径（mkdir / open / fsync+close）必须都进 backoff——只接一条的话，
         剩下两条照样每片重试，风暴只少三分之一；
      ⑤ 成功必须补 `flushed after N retries (T ms)`，否则验收的 N 与 T 无从读起。

    弱牙三型自检：
      · 谓词正则防短路：不只看符号存在，还看调用点是否在判定/返回路径上；
      · count >= 2 防单岔：失败路径按 ≥3 计数，单岔接线当场红；
      · 拒死代码形态：`if (0)` / `if (false)` 包住的接线不算接线。
    """
    if not EXCERPT_STORE_C.exists():
        bad("B57 %s is missing: the backoff cannot be judged" % EXCERPT_STORE_C)
        return
    src = strip_comments(EXCERPT_STORE_C.read_text(errors="replace"))
    hdr = strip_comments(EXCERPT_STORE_H.read_text(errors="replace"))

    # ③ 单一时间判据：旧字段回归即红（这是本条最容易被「顺手改回去」的地方）
    for f in (src, hdr):
        if "s_ready_at_ms" in f:
            bad("B57 s_ready_at_ms is back: the yield window and the retry backoff would be "
                "judged by two separate fields again - that split IS the 389-retry storm")
            break

    # ① 表逐值：{50,200,1000}，后缀 u 可有可无，顺序不许变
    m = re.search(r"k_backoff_ms\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", src)
    if not m:
        bad("B57 k_backoff_ms[] is gone: the fixed three-step table lost its only home")
    else:
        vals = [v.strip().rstrip("uU").strip() for v in m.group(1).split(",") if v.strip()]
        if vals != ["50", "200", "1000"]:
            bad("B57 k_backoff_ms[] is %s, not {50,200,1000}: section 19.3 closed two "
                "competing readings of '4x step' and only the fixed table may enter code"
                % (vals,))
        if len(vals) != 3:
            bad("B57 k_backoff_ms[] has %d steps: EXCERPT_BACKOFF_STEPS is 3, and a longer "
                "table silently changes the ladder" % len(vals))
    if not re.search(r"#define\s+EXCERPT_BACKOFF_STEPS\s+3u?\b", hdr):
        bad("B57 EXCERPT_BACKOFF_STEPS is not 3 in the header: the step count is the one "
            "number the host tests freeze, and it just drifted")

    # ④ 三条失败路径都要进 backoff（单岔接线是弱牙三型的第二型）
    n_cost = len(re.findall(r"backoff_on_fail\s*\(", src))
    if n_cost < 4:  # 1 定义 + 3 调用
        bad("B57 backoff_on_fail() appears %d time(s): expected one definition plus three "
            "call sites (mkdir / open / fsync+close). A single wired branch leaves the other "
            "two retrying every slot - the storm only drops by a third" % n_cost)
    body = _body_of(src, r"static\s+void\s+backoff_on_fail\s*\(")
    if not body:
        bad("B57 backoff_on_fail() is gone: the ladder has no place to live")
        return
    # 拒死代码形态：接线被 `if (0)` / `if (false` 包住等于没接
    if re.search(r"if\s*\(\s*(0|false)\s*\)", body):
        bad("B57 backoff_on_fail() is wrapped in a never-true branch: a wired-but-dead "
            "retry looks identical to a working one in every log line")

    # ② 封顶：只在未到末档时递增；无条件 ++ 会越过表尾
    if not re.search(r"s_backoff_idx\s*\+\s*1u?\s*<\s*\(uint8_t\)\s*EXCERPT_BACKOFF_STEPS", body):
        if re.search(r"\+\+\s*s_backoff_idx", body):
            bad("B57 s_backoff_idx is incremented without the clamp: past the third failure "
                "the index runs off the table and the gap becomes whatever follows it in .rodata")
        else:
            bad("B57 s_backoff_idx is never advanced: every failure retries at 50 ms and the "
                "ladder is decorative")

    # ② ' 设置点必须就是「now + 表值」，不许乘除（递增律的两种写法都在这里被拦下）
    if not re.search(r"s_next_try_ms\s*=\s*now_ms\s*\+\s*k_backoff_ms\s*\[\s*step\s*\]\s*;", body):
        bad("B57 the next-try deadline is not exactly now_ms + k_backoff_ms[step]: any "
            "multiplication here is the growth law 19.3 explicitly rejected")

    # ③ tick 的判点：一个是让行/退避共用的门，必须在任何 IO 之前
    tick = _body_of(src, r"excerpt_flush_t\s+excerpt_store_tick\s*\(")
    if not tick:
        bad("B57 excerpt_store_tick() is gone: the flush loop lost its entry")
        return
    gate = re.search(r"if\s*\(\s*s_next_try_ms\s*&&\s*now_ms\s*<\s*s_next_try_ms\s*\)", tick)
    if not gate:
        bad("B57 the single gate is gone from excerpt_store_tick(): without it every "
            "housekeeping slot retries, which is precisely the 389-retry storm")
    else:
        first_io = min([p for p in (tick.find("ex_mkdir"), tick.find("ex_open_append")) if p >= 0] or [10 ** 9])
        if gate.start() > first_io:
            bad("B57 the gate sits AFTER the first IO call: the backoff would be consulted "
                "only after the work has already been done")

    # ⑤ 验收行
    if not re.search(r'"flushed after %u retries \(%u ms\)"', src):
        bad("B57 the acceptance line 'flushed after N retries (T ms)' is gone: section 19.3 "
            "accepts this fix on exactly N and T, and neither can be read without it")


READ_PICO_SD_C = ROOT / "components/read_pico/read_pico_sd.c"
READ_PICO_SD_H = ROOT / "components/read_pico/include/read_pico_sd.h"


def check_b58_mount_epoch():
    """31b E-2（§19.4 / §19.4a）：让行的锚点必须是**挂载代号**，而不是状态边沿。

    现场：`Mounted` 100.977 → 队列 100.994 就写卡（17 ms）→ `mounted=1` 101.863 才计 5 s。
    根因不是「接在哪个 if 上」，而是**锚错了对象**：`gained = !*mounted && ready` 是状态
    **差分**，拔插快于一次轮询时 `*mounted` 全程为 true，这条边沿一次都不成立。那 886 ms
    里的每一次家务片都在无让行状态下冲账——判据被绕过，而且日志上看不出任何异常。

    五件事，少一条这条修复就退回去：
      ① 组件侧有代号，且 **只在真挂载成功处 +1**。放进 `publish_info()` 就错：空间回填
         会再发布一次，同一张卡被算成第二次挂载，窗口推到 20 s 之后（读起来像"永不落盘"）；
      ② 代号有 getter，消费方读得到（不许让 app 去猜状态位的变化）；
      ③ 消费侧幂等：同一代号重复到达不重开窗口。两处调用点（poll 与家务片）都每拍/频繁
         调用，没有幂等就是 `if (ready)` 那个坑的等价形态，窗口被每拍往后推；
      ④ 家务片把观测压在 `excerpt_store_tick` **之前且相邻**——挂载发生在 sd_probe 任务里，
          poll 最早下一拍才知道，只有贴着写卡才能力把窗口压到 0；
      ⑤ 旧入口 `excerpt_store_notify_mounted` 零残留。留着它，下一次"顺手"就会绕过代号。
    """
    for f in (READ_PICO_SD_C, READ_PICO_SD_H, EXCERPT_STORE_C, APP_BOOK):
        if not f.exists():
            bad("B58 %s is missing: the mount epoch cannot be judged" % f)
            return
    sd = strip_comments(READ_PICO_SD_C.read_text(errors="replace"))
    sdh = strip_comments(READ_PICO_SD_H.read_text(errors="replace"))
    ex = strip_comments(EXCERPT_STORE_C.read_text(errors="replace"))
    book = strip_comments(APP_BOOK.read_text(errors="replace"))

    # ① 代号存在
    if not re.search(r"static\s+uint32_t\s+s_mount_epoch\s*;", sd):
        bad("B58 read_pico_sd holds no s_mount_epoch: without an epoch the consumer can only "
            "compare state bits, and a fast replug makes that comparison disappear")
        return
    if not re.search(r"uint32_t\s+read_pico_sd_mount_epoch\s*\(\s*void\s*\)", sdh):
        bad("B58 read_pico_sd.h does not declare read_pico_sd_mount_epoch(): the fact exists "
            "inside the component and cannot reach the side that needs it")
    if not re.search(r"uint32_t\s+read_pico_sd_mount_epoch\s*\(\s*void\s*\)\s*\{", sd):
        bad("B58 read_pico_sd_mount_epoch() has no body: a declaration without a reader is "
            "the same as no epoch at all")

    # ①' 递增点：在 mount_card 里，**不在** publish_info 里
    mount = _body_of(sd, r"static esp_err_t\s+mount_card\s*\(")
    if not mount:
        bad("B58 mount_card() is gone: the epoch has no legitimate increment point")
    elif not re.search(r"\+\+\s*s_mount_epoch", mount):
        bad("B58 mount_card() never bumps the epoch: every mount would look like the first, "
            "and the second insertion would never arm the window")
    publish = _body_of(sd, r"static void\s+publish_info\s*\(")
    if "s_mount_epoch" in publish:
        bad("B58 publish_info() touches the epoch: it is called a SECOND time by the deferred "
            "space scan, so one card would count as two mounts and the yield window would be "
            "pushed past the moment the queue was supposed to flush")

    # ③ 幂等
    my = _body_of(ex, r"void\s+excerpt_store_mount_yield\s*\(")
    if not my:
        bad("B58 excerpt_store_mount_yield() is gone: the yield window lost its only entry")
        return
    if not re.search(r"if\s*\(\s*mount_epoch\s*==\s*s_mount_epoch_seen\s*\)\s*return", my):
        bad("B58 excerpt_store_mount_yield() is not idempotent: both call sites fire often, "
            "so every repeat would push the window five seconds forward - the `if (ready)` "
            "trap, relocated into the callee")
    if not re.search(r"s_mount_epoch_seen\s*=\s*mount_epoch", my):
        bad("B58 excerpt_store_mount_yield() never records the epoch it consumed: idempotence "
            "would then depend on a value that is read but never written")
    rst = _body_of(ex, r"void\s+excerpt_store_reset\s*\(")
    if rst and "s_mount_epoch_seen" not in rst:
        bad("B58 excerpt_store_reset() forgets s_mount_epoch_seen: after a reset the first "
            "real mount of the boot would match the stale value and never arm the window")

    # ④ 家务片：观测在写卡之前且相邻
    tick = _body_of(book, r"static bool\s+excerpt_footer_tick\s*\(")
    if not tick:
        bad("B58 excerpt_footer_tick() is gone: the write-side observation point vanished")
    else:
        obs = re.search(r"excerpt_store_mount_yield\s*\(\s*read_pico_sd_mount_epoch\s*\(\s*\)", tick)
        wr = tick.find("excerpt_store_tick(")
        if not obs:
            bad("B58 the housekeeping slot does not observe the epoch: the mount happens on "
                "another task, so poll_media alone can be a whole tick late")
        elif wr < 0:
            bad("B58 excerpt_footer_tick() no longer calls excerpt_store_tick(): nothing to guard")
        elif obs.start() > wr:
            bad("B58 the epoch is observed AFTER the write attempt: the window would be armed "
                "one write too late - exactly the 17 ms in the 26.9.31 trace")
        else:
            between = tick[obs.end():wr]
            if between.count(';') > 2:
                bad("B58 %d statements sit between observing the epoch and writing: keep them "
                    "adjacent or the zero-width window is a comment, not a property"
                    % between.count(';'))

    # ⑤ 旧入口零残留
    for path in (ROOT / "main").rglob("*.c"):
        if "excerpt_store_notify_mounted" in strip_comments(path.read_text(errors="replace")):
            bad("B58 %s still calls excerpt_store_notify_mounted(): the old entry has no "
                "epoch, so anything reaching it arms the window without an event"
                % path.relative_to(ROOT))
            break


def check_b64_ai_receipts():
    """26.11.4 B 线（AI 键点亮）的四条回执契约。

    ① 拒发门先于发送：>1024 B 的选区**不发帧**。落在发送之后等于「先砍到 1024 再问」，
       答案会指着被砍掉的那半段说话，而用户看不出来。
    ② 拒发必须带文案：拒发 + 一个不说话的气泡 = 静默丢弃。
    ③ 回执必须上**顶部气泡**：全屏阅读不画页脚带，只写 hint 槽等于没写（26.11.1 的坑）。
    ④ B63 show/hide 两头留痕 + B62 启动横幅带 ELF 指纹：视觉子句与版本归因都得有日志。
    """
    if not APP_BOOK.exists():
        return
    src = strip_comments(APP_BOOK.read_text(errors="replace"))
    body = _body_of(src, r"static\s+app_redraw_t\s+boxsel_ai_send\s*\(")
    if not body:
        bad("B64 boxsel_ai_send() not found - the AI send path is exactly what this tooth guards")
        return

    gate = re.search(r"len\s*>\s*PUSH_SCENE_CTX_MAX", body)
    send = body.find("push_sink_ai_scene_send(")
    if not gate:
        bad("B64 the >PUSH_SCENE_CTX_MAX refusal gate is gone: an oversized selection would be "
            "cut to 1024 and answered as if it were complete")
    elif send < 0 or gate.start() > send:
        bad("B64 the refusal gate sits after push_sink_ai_scene_send(): it must refuse BEFORE "
            "anything goes on the air")

    if "选段过长 · 请缩小范围" not in body:
        bad("B64 the refusal toast text is missing: refusing without saying why is a silent drop")

    if "reader_notice_set(receipt" not in body:
        bad("B64 the AI receipt never reaches the toast: a hint-only receipt is invisible in "
            "full-screen reading (the 26.11.1 trap)")

    setter = _body_of(src, r"static\s+void\s+reader_notice_set\s*\(")
    if not setter or "notice: show" not in setter:
        bad("B64 reader_notice_set() carries no `notice: show` log - the visual clause loses "
            "its evidence again")
    if "notice: hide" not in src:
        bad("B64 the toast expiry branch carries no `notice: hide` log")

    if APP_LOOP.exists():
        run_body = _body_of(strip_comments(APP_LOOP.read_text(errors="replace")),
                            r"void\s+app_loop_run\s*\(")
        if "app_elf_sha256" not in run_body or "build: ver=" not in run_body:
            bad("B62 the boot banner lost the ELF fingerprint: a version string alone cannot "
                "tell two builds apart (26.11.3 moved the bin hash with the version unchanged)")


def check_b66_excerpt_route():
    """26.11.3-C：整段上行这条路的四条形状判据（每一条都对应真机上摔过的一跤）。

    ① **拒发门在复制之前**：先复制再判，>1024 的选段永远先被截断，那道审计裁决就成了
       装饰品（26.11.3-B 的 768 B 缓冲正是这么把门变成不可达代码的）。
    ② **上下文缓冲就是协议上限 + 1**：768 B 的框选缓冲把 879/925 B 的段落砍到 767，
       而回执还写着「已按上限发送」——上限明明是本地缓冲。缓冲与协议必须同源。
    ③ **发出去必须 arm**：只发不 arm，0x08 回来无人认领，答案落成词卡（档② 的第二个
       原因，日志 `dict: ai def` 就是证据）。
    ④ **形态判在发送之前**：一个词（≤32 B 无空白）走点词路径，不发 0x1A。

    Four shape rules for the passage uplink, each one a fall the hardware already took:
    refuse before copying, the buffer is the protocol cap + 1, arm on send, and a
    single-word selection takes the tap-word path instead of a 0x1A.
    """
    if not APP_BOOK.exists():
        return
    src = strip_comments(APP_BOOK.read_text(errors="replace"))
    body = _body_of(src, r"static\s+app_redraw_t\s+boxsel_ai_send\s*\(")
    if not body:
        bad("B66 boxsel_ai_send() not found - the passage uplink is what this tooth guards")
        return

    gate = body.find("PUSH_SCENE_CTX_MAX")
    copy = body.find("box_select_copy_text(")
    send = body.find("push_sink_ai_scene_send(")
    if gate < 0:
        bad("B66 the >PUSH_SCENE_CTX_MAX refusal gate is gone")
    elif copy < 0 or gate > copy:
        bad("B66 the refusal gate runs AFTER box_select_copy_text(): once the selection has "
            "been copied into a smaller buffer the gate can never fire (the 768 B dead code)")
    if "sizeof(s_scene_ctx)" not in body:
        bad("B66 the passage context is not copied into s_scene_ctx: a box-select-sized "
            "buffer silently cuts 879/925 B paragraphs to 767 B (26.11.3-B log `copy=767 cut=1`)")
    if not re.search(r"s_scene_ctx\s*\[\s*PUSH_SCENE_CTX_MAX\s*\+\s*1\s*\]", src):
        bad("B66 s_scene_ctx is not declared as PUSH_SCENE_CTX_MAX + 1: buffer and protocol "
            "must share one source or the cap drifts again")

    arm = body.find("scene_req_arm_excerpt(&s_scene_req")
    if send < 0:
        bad("B66 no push_sink_ai_scene_send() in the passage path")
    elif arm < 0 or arm < send:
        bad("B66 the 0x1A send does not arm the scene state: the answer is claimed by nobody "
            "and lands as a word card instead of the 选段 view (26.11.3-B档②)")

    word_test = body.find("BOXSEL_WORD_MAX")
    tap = body.find("push_sink_ai_tap_word_ctx(")
    if word_test < 0 or tap < 0:
        bad("B66 the single-word shape test is gone: a one-word selection would be sent as a "
            "passage and the phone would invent three sections about one token")
    elif word_test > send:
        bad("B66 the single-word shape test runs after the 0x1A send: by then the frame is "
            "already on the air")


def check_b68_cross_end_bit2():
    """26.11.3-C：**跨端启用门**（事前那道门，不是事后牙）。

    协议位的病根是「声明了、单端实现了、就当它存在了」。这条牙把三点一致变成可执行：
    ① 设备宏 `PUSH_SCENE_FLAG_EXCERPT (1u << N)`；② APK 常量 `SCENE_FLAG_EXCERPT = 1 << N`
    （**N 必须相等**）+ 分派分支 + 段落提示词；③ 登记簿的行（由 B33 咬）。缺一条，位就是
    单端的，真机就会静默降级成另一种功能——26.11.3-B 花了整整一轮才看清这件事。

    The cross-end enable gate: device macro, APK constant (same shift count), APK dispatch
    and prompt must all exist before the bit counts as enabled. One-sided bits degrade
    silently into a different feature, which is exactly what cost 26.11.3-B a round.
    """
    dev = ROOT / "components/push_sink/include/push_proto.h"
    apk_dir = ROOT / "tools/android/apk/src/dev/readpico/push"
    jproto, jmain, jai = apk_dir / "PushProto.java", apk_dir / "MainActivity.java", apk_dir / "AiClient.java"
    for f in (dev, jproto, jmain, jai):
        if not f.exists():
            bad("B68 %s is missing: the cross-end gate cannot be checked" % f.relative_to(ROOT))
            return

    m_dev = re.search(r"#define\s+PUSH_SCENE_FLAG_EXCERPT\s+\(1u\s*<<\s*(\d+)\)", dev.read_text())
    if not m_dev:
        bad("B68 the device macro PUSH_SCENE_FLAG_EXCERPT (1u << N) is gone")
        return
    m_apk = re.search(r"SCENE_FLAG_EXCERPT\s*=\s*1\s*<<\s*(\d+)", jproto.read_text())
    if not m_apk:
        bad("B68 APK PushProto has no SCENE_FLAG_EXCERPT: the device has been sending bit2 "
            "since 26.9.26 and v1.6.2 dropped it on the floor (26.11.3-B档② FAIL)")
        return
    if m_dev.group(1) != m_apk.group(1):
        bad("B68 bit2 shift disagrees across ends: device 1<<%s vs APK 1<<%s"
            % (m_dev.group(1), m_apk.group(1)))
    if not re.search(r"SCENE_FLAG_KNOWN\s*=[^;]*SCENE_FLAG_EXCERPT", jproto.read_text(), re.S):
        bad("B68 SCENE_FLAG_KNOWN does not include SCENE_FLAG_EXCERPT: an unknown-bit WARN "
            "would fire on the one bit the APK now handles")

    main_src = jmain.read_text()
    # 咬**分派形状**而不是两个字符串同时在场：`if (false) { onExcerptRequest(…) }` 里两个
    # 名字都还在，只有形状能证明「bit2 帧真的走段落路」。
    # Bite the dispatch shape, not mere presence: a dead `if (false)` branch still contains
    # both names and would prove nothing.
    if not re.search(r"if\s*\(\s*scene\.excerpt\(\)\s*\)\s*\{\s*onExcerptRequest\(", main_src):
        bad("B68 APK MainActivity has no `if (scene.excerpt()) { onExcerptRequest(...) }` "
            "dispatch: a bit2 frame would still go down the word-card path")
    if "AiClient.excerpt(" not in main_src:
        bad("B68 APK MainActivity never calls AiClient.excerpt(): the passage prompt is dead code")

    ai_src = jai.read_text()
    exc_body = _body_of(ai_src, r"static\s+String\s+excerptSystemPrompt\s*\(\s*\)")
    word_body = _body_of(ai_src, r"static\s+String\s+sceneSystemPrompt\s*\(\s*\)")
    if not exc_body:
        bad("B68 AiClient.excerptSystemPrompt() is missing: bit2 would reuse the word prompt")
    elif "该词" in exc_body:
        bad("B68 the excerpt prompt says 该词: sharing the word template's wording is exactly "
            "how a passage gets answered as one token (bite the excerpt template, not the word one)")
    if not word_body or "该词" not in word_body:
        bad("B68 the word-scene prompt no longer says 该词 - the tooth above may be biting the "
            "wrong template, so it proves nothing")



def _one_int(src, pattern):
    """从源码里取一个整型常量；取不到返回 None。/ One integer constant, or None."""
    m = re.search(pattern, src)
    return int(m.group(1)) if m else None


def check_b72_gate_ladder():
    """26.11.3-C2（审计裁定：**门梯表**）：六道门必须同一条链，且长缓冲必须落 PSRAM。

    为什么这条是牙而不是文档：8192 只在**每一跳都不低于它**时才是真门。任何一跳缩回去，
    答案就在那一跳被无声砍断，而日志上看不出来（这正是 1152 时代的形态，只是更隐蔽）。
    故这条咬三件事：
      ① 序关系：CARD_MAX ≤ DEF_MAX ≤ SEGASM_MAX，且 READER_DEF_MAX ≥ DEF_MAX；
      ② 跨端同数：APK EXCERPT_DEF_MAX_BYTES == 设备 PUSH_AI_DEF_MAX；
      ③ 搬家属性的**在场**：四个长缓冲必须带 EXT_RAM_BSS_ATTR——有人把槽改大却忘了
         搬家，内部 RAM（只剩 112 B）会当场爆，而那是在真机上才炸的错误类型。

    The gate ladder: 8192 is a real ceiling only if NO rung sits below it. This tooth bites
    the ordering, the cross-end equality, and the presence of the PSRAM attribute on every
    long buffer - a widened slot that forgot to move blows internal RAM on hardware.
    """
    proto_p = ROOT / "components/push_sink/include/push_proto.h"
    seg_p = ROOT / "components/push_sink/include/ai_segasm.h"
    reader_h = ROOT / "main/apps/ai_reader.h"
    apk_ai = ROOT / "tools/android/apk/src/dev/readpico/push/AiClient.java"
    for f in (proto_p, seg_p, reader_h, apk_ai):
        if not f.exists():
            bad("B72 %s is missing: the gate ladder cannot be checked" % f.relative_to(ROOT))
            return

    proto = strip_comments(proto_p.read_text())
    seg = strip_comments(seg_p.read_text())
    reader = strip_comments(reader_h.read_text())
    jai = apk_ai.read_text()

    card = _one_int(proto, r"#define\s+PUSH_AI_CARD_MAX\s+(\d+)u")
    slot = _one_int(proto, r"#define\s+PUSH_AI_DEF_MAX\s+(\d+)u")
    segasm = _one_int(seg, r"#define\s+AI_SEGASM_MAX\s+(\d+)u")
    rdef = _one_int(reader, r"#define\s+AI_READER_DEF_MAX\s+(\d+)")
    apk_cap = _one_int(jai, r"EXCERPT_DEF_MAX_BYTES\s*=\s*(\d+)")
    if None in (card, slot, segasm, rdef, apk_cap):
        bad("B72 one of the five ladder constants is gone (card=%s slot=%s segasm=%s reader=%s apk=%s)"
            % (card, slot, segasm, rdef, apk_cap))
        return
    if card > slot:
        bad("B72 ladder inverted: PUSH_AI_CARD_MAX(%d) > PUSH_AI_DEF_MAX(%d)" % (card, slot))
    if segasm < slot:
        bad("B72 the reassembler is narrower than the slot (%d < %d): an answer would be cut "
            "mid-sentence with no notice at all - worse than the 1152-era ellipsis"
            % (segasm, slot))
    if rdef < slot:
        bad("B72 AI_READER_DEF_MAX(%d) < PUSH_AI_DEF_MAX(%d): the last hop shrinks the answer"
            % (rdef, slot))
    if slot < 8192:
        bad("B72 the transport slot fell back to %d (< 8192): the ladder's top rung is the one "
            "the acceptance card measures, so lowering it silently lowers every claim below it"
            % slot)
    if apk_cap != slot:
        bad("B72 cross-end mismatch: APK EXCERPT_DEF_MAX_BYTES=%d vs device PUSH_AI_DEF_MAX=%d "
            "- one end would truncate what the other happily produced" % (apk_cap, slot))
    if not re.search(r"\"max_tokens\"\s*:\s*\"\s*\+\s*maxTokens", jai) and \
            "max_tokens" not in jai:
        bad("B72 the passage request carries no explicit max_tokens: the server default is "
            "somebody else's constant, and the link is built for 8192 bytes")

    # ③ 搬家属地在场。/ The PSRAM attribute must be on every long buffer.
    movers = [
        ("components/push_sink/src/push_sink.c", r"EXT_RAM_BSS_ATTR\s+char\s+s_ui_ai_def_text"),
        ("components/push_sink/src/push_sink.c", r"EXT_RAM_BSS_ATTR\s+ai_segasm_t\s+s_ai_segasm"),
        ("main/app/app_loop.c", r"EXT_RAM_BSS_ATTR\s+char\s+ai_def\s*\["),
        ("main/apps/ai_reader.c", r"EXT_RAM_BSS_ATTR\s+ai_reader_session_t\s+s_session"),
    ]
    for rel, pat in movers:
        f = ROOT / rel
        if not f.exists():
            bad("B72 %s is missing" % rel)
            continue
        if not re.search(pat, f.read_text()):
            bad("B72 %s: a long buffer lost EXT_RAM_BSS_ATTR - internal bss has 112 B left, "
                "so it would blow up on hardware, not on the host" % rel)

    # 等待窗：设备 90 s、APK 120 s，且设备先放弃。/ Wait windows: 90 s device, 120 s APK.
    book = (ROOT / "main/apps/app_book.c").read_text()
    dev_wait = _one_int(book, r"#define\s+EXCERPT_REQ_TIMEOUT_MS\s+(\d+)")
    apk_wait = _one_int(jai, r"EXCERPT_TIMEOUT_MS\s*=\s*(\d+)")
    if dev_wait is None or dev_wait < 90000:
        bad("B72 device passage wait window is %s (< 90000): the device would expire first and "
            "the returning 0x08 would be claimed by nobody" % dev_wait)
    if apk_wait is None or apk_wait < 120000:
        bad("B72 APK passage read timeout is %s (< 120000): a 40-70 s generation would be cut "
            "at random" % apk_wait)
    if dev_wait is not None and apk_wait is not None and dev_wait >= apk_wait:
        bad("B72 the device wait (%d) must stay under the APK timeout (%d), or a dead request "
            "parks forever on the device" % (dev_wait, apk_wait))

    # 词义路径不许跟着段落一起长。/ The word path must NOT grow with the passage path.
    popup = (ROOT / "main/apps/dict_popup.c").read_text()
    if not re.search(r"#define\s+DICT_POPUP_AI_SEG\s+\(\s*PUSH_AI_CARD_MAX\s*\+\s*1u?\s*\)", popup):
        bad("B72 DICT_POPUP_AI_SEG must derive from PUSH_AI_CARD_MAX: following the transport "
            "slot borrows 8 KB of PSRAM for every word card")
    sink = (ROOT / "components/push_sink/src/push_sink.c").read_text()
    if not re.search(r"char\s+def\[\s*PUSH_AI_CARD_MAX\s*\]\s*;\s*\n\s*bool\s+used", sink):
        bad("B72 the prefetch cache entry must be sized by PUSH_AI_CARD_MAX (eight 7 KB lies "
            "is exactly the cost this split exists to avoid)")


def check_b73_prompt_budget():
    """26.11.3-C2：**「敷衍」的判据化**——提示词里不许再有字数帽，词卡的不许被顺手拆掉。

    「太敷衍」过去没法咬：字数在系统提示里是一个自然语言约束，读代码的人看得见、工具
    看不见。这条把它变成断言：
      ① 段落模板**不得出现**「不超过 N 字 / 不超过 N 行」这类硬帽；
      ② 段落模板**必须**明写「不设总字数上限」——否则「删掉帽」和「忘写要求」长得一样；
      ③ 段落模板**不得引用 SCENE_CHAR_LIMIT**（词卡的常量）——这正是 26.11.3-C 的形态；
      ④ 词卡模板**必须仍然**带 SCENE_CHAR_LIMIT 的硬帽（防拆常量时顺手改坏词卡）。

    Turning "it feels lazy" into a static fact: no character cap in the passage template, the
    phrase "no total limit" present, no reference to the word card's constant, and the word
    template still capped - so removing the cap cannot quietly uncap the word card too.
    """
    jai = (ROOT / "tools/android/apk/src/dev/readpico/push/AiClient.java").read_text()
    exc = _body_of(jai, r"static\s+String\s+excerptSystemPrompt\s*\(\s*\)")
    word = _body_of(jai, r"static\s+String\s+sceneSystemPrompt\s*\(\s*\)")
    if not exc:
        bad("B73 AiClient.excerptSystemPrompt() is missing")
        return
    if not word:
        bad("B73 AiClient.sceneSystemPrompt() is missing")
        return
    if re.search(r"不超过\s*\d+\s*[字行]", exc):
        bad("B73 the passage prompt still carries a numeric length cap (不超过 N 字/行): that "
            "single clause is what compressed a 1000-byte passage into 305 characters")
    if "不设总字数上限" not in exc:
        bad("B73 the passage prompt must say 不设总字数上限 in so many words: a missing cap and "
            "a forgotten requirement look identical to a reader")
    if "SCENE_CHAR_LIMIT" in exc:
        bad("B73 the passage prompt still references SCENE_CHAR_LIMIT: the word card's budget "
            "leaking into passage mode is the 26.11.3-C failure shape")
    for need in ("原文对照", "中译"):
        if need not in exc:
            bad("B73 the passage prompt no longer asks for %s: the sentence-by-sentence "
                "comparison section is the whole point of the long answer" % need)
    if "长文模式" not in exc:
        bad("B73 the passage prompt has no 长文模式 escape hatch: an over-long selection must "
            "degrade by declaration, never by silently dropping sentences")
    if "SCENE_CHAR_LIMIT" not in word:
        bad("B73 the word-scene prompt lost its SCENE_CHAR_LIMIT cap - the split must not "
            "uncap the word card by accident (that is the regression this tooth exists for)")


def check_b74_ratio_receipt():
    """26.11.3-C2：产出比判据的**机器-readable 载体**必须在场。

    判据是「def 字节 / 输入字节 ≥ 3×（英文）/ ≥ 2×（中文）」。判据要有地方读——APK 日志
    必须同时打**字节**（字符数会掩盖中英混排的萎缩，883→305 就是这么被读成「还行」的），
    并且必须把 truncated 报出来。缺任何一项，判据就退回成「看截图」。

    The output-ratio criterion needs a machine-readable carrier: the APK log must print BYTES
    (character counts hide mixed-script shrinkage) and the truncation flag. Without them the
    criterion degrades back into reading screenshots.
    """
    jmain = (ROOT / "tools/android/apk/src/dev/readpico/push/MainActivity.java").read_text()
    jai = (ROOT / "tools/android/apk/src/dev/readpico/push/AiClient.java").read_text()
    if "outBytes" not in jmain or "inBytes" not in jmain:
        bad("B74 the passage log line no longer prints input/output BYTES: the ratio criterion "
            "would have nothing to read")
    if "truncated=" not in jmain:
        bad("B74 the passage log line no longer reports truncated=: a cut answer would be "
            "indistinguishable from a short one")
    if not re.search(r"public\s+final\s+boolean\s+truncated", jai):
        bad("B74 AiClient.Result has no truncated field: the APK cannot say whether the cap bit")
    if not re.search(r"Result\.ok\(\s*text\s*,\s*cut\s*\)", jai):
        bad("B74 AiClient.excerpt() no longer reports the truncation flag it computed")
    if "EXCERPT_DEF_MAX_BYTES" not in jai:
        bad("B74 the passage byte cap has no name of its own - one constant cannot govern both "
            "a 1 KB word card and an 8 KB passage analysis")



def check_b75_section_contract():
    """26.11.4：**节标记是跨端契约，节序只是呈现约定**。

    这条牙咬三件事，对应裁定的三层：
      ① 设备侧的标记字符串与 APK 模板**逐字节相同**——设备硬编码一个 APK 侧自由
         改名的字符串，就是第二处 bit2 事故（对方改名，我方静默断链）；
      ② APK 模板里【原文对照】排在【段意】之前（26.11.5 的节序调整），但**只断言
         存在性 + 序关系**，不断言整段模板文本——模板措辞是 APK 的自由，不是契约；
      ③ 设备切割函数**不得**依赖节序：源码里不许出现「按行号取第 N 节」这类写法，
         判据是 `excerpt_section.c` 里只有标记定位、没有位置常量。

    为什么 ② 要有牙：节序是 26.11.5 唯一的需求，也是最容易被下一次改模板顺手
    改回去的东西；而改回去没有功能损坏（只是对照又跑到第三屏），没人会发现。
    The marker is a contract; the order is a presentation choice - and the order needs
    a tooth precisely because reverting it breaks nothing visible.
    """
    sect_h = ROOT / "main/book/excerpt_section.h"
    sect_c = ROOT / "main/book/excerpt_section.c"
    jai = ROOT / "tools/android/apk/src/dev/readpico/push/AiClient.java"
    for f in (sect_h, sect_c, jai):
        if not f.exists():
            bad("B75 %s is missing: the section contract cannot be checked" % f.relative_to(ROOT))
            return

    hsrc = sect_h.read_text()
    m = re.search(r'#define\s+EXCERPT_SECTION_MARKER\s+"([^"]+)"', hsrc)
    if not m:
        bad("B75 the device has no EXCERPT_SECTION_MARKER constant - the marker would be a "
            "bare literal in .c, invisible to this tooth")
        return
    marker = m.group(1)
    if marker != "【原文对照】":
        bad("B75 the device marker changed to %r: it is a protocol constant, so a change here "
            "must be a protocol change (and the APK must move in the same commit)" % marker)

    jsrc = jai.read_text()
    exc = _body_of(jsrc, r"static\s+String\s+excerptSystemPrompt\s*\(\s*\)")
    if not exc:
        bad("B75 AiClient.excerptSystemPrompt() is missing")
        return
    if marker not in exc:
        bad("B75 the APK template no longer emits the device's marker %s - the cut would fail "
            "on every English passage, silently" % marker)
        return
    # ② 序关系：对照在段意之前。/ The comparison section precedes 段意.
    pos_cmp = exc.find(marker)
    pos_sense = exc.find("【段意】")
    if pos_sense < 0:
        bad("B75 the APK template lost 【段意】: the order assertion has nothing to compare")
        return
    if pos_cmp > pos_sense:
        bad("B75 【原文对照】 must come BEFORE 【段意】 (26.11.5 moved it to the head: the "
            "reader should see the original and the translation on the first screen)")
    # 节标记在模板里只出现一次：重复会让设备取到错的那一节。
    if exc.count(marker) != 1:
        bad("B75 the APK template mentions %s %d times: the device takes the first match, so a "
            "second mention (e.g. \"不要输出【原文对照】\") would be taken as the section head"
            % (marker, exc.count(marker)))

    # ③ 切割不依赖节序：源码里不许有「第 N 节」这类位置判据。
    csrc = strip_comments(sect_c.read_text())
    if re.search(r"\b(section_index|nth_section|SECTION_ORDER|第[一二三四五]节)", csrc):
        bad("B75 excerpt_section.c appears to locate by section POSITION: the cut must find the "
            "marker, never count sections - position coupling is what would force a device "
            "rebuild every time the APK reorders its answer")
    if "EXCERPT_SECTION_MARKER" not in csrc:
        bad("B75 excerpt_section.c does not use the marker constant")


def check_b76_save_dispatch():
    """26.11.4：段落视图的保存**必须分流**，词卡路径**必须不动**。

    这条咬的是「死键」的根：同一个 chip 位置，词卡存生词、段落存摘抄。第一版把两者
    接到同一个函数上，真机日志留下 6 次 `scene save "选段": not in dictionary` ——
    能按、永远不成功，而且没有任何报错。

    三条：
      ① 分派函数同时引用两条路径（`scene_save_passage` 与 `scene_save_word`）；
      ② 词卡路径**仍**走 `vocab_log_save`（词义进生词本是正常路径，牙不许咬错面）；
      ③ 段落路径**必须**调 `excerpt_store_enqueue` 与 `excerpt_section_pick`，
         且失败分支给出气泡（诚实失败）。
    """
    book = (ROOT / "main/apps/app_book.c").read_text()
    csrc = strip_comments(book)
    if "ai_reader_is_excerpt" not in csrc:
        bad("B76 app_book never asks whether the session is a passage: one chip would keep "
            "serving two different meanings (the dead-key shape)")
    m = re.search(r"static\s+app_redraw_t\s+scene_save\s*\(\s*void\s*\)\s*\{(.*?)\n\}", csrc, re.S)
    if not m:
        bad("B76 scene_save() dispatcher is missing")
        return
    body = m.group(1)
    for need in ("scene_save_passage", "scene_save_word"):
        if need not in body:
            bad("B76 the dispatcher no longer reaches %s(): one of the two meanings is orphaned"
                % need)
    if "vocab_log_save" not in csrc:
        bad("B76 the word card's save no longer uses vocab_log_save: the split must not change "
            "what a word card writes")
    pm = re.search(r"static\s+app_redraw_t\s+scene_save_passage\s*\(\s*void\s*\)\s*\{(.*?)\n\}",
                   csrc, re.S)
    if not pm:
        bad("B76 scene_save_passage() is missing")
        return
    pbody = pm.group(1)
    # 带左括号匹配：`excerpt_section_pick_unused(...)` 这类改名不算「还在调」——
    # 26.11.4 的 S6 变异就靠这个前缀差活过一次，是牙松，不是变异弱。
    # Match the call, not the prefix: a rename to *_unused is not "still calling it".
    for need in ("excerpt_section_pick(", "excerpt_store_enqueue("):
        if need not in pbody:
            bad("B76 scene_save_passage() does not call %s...) - the passage path would let the "
                "whole answer through, or write nothing at all" % need)
    # 诚实失败要逐分支咬：整个文件里有这个字符串不够——删掉 NO_MARKER 那一处、
    # 只留 EMPTY 那一处，用户遇到「英文答案没有对照节」时仍然一声不响。
    # Bite per branch, not per file: leaving only the EMPTY branch would still be silent
    # for the exact case this batch exists for.
    for branch, label in (("NO_MARKER", "an English answer without the section"),
                          ("EMPTY", "an empty section")):
        # 分支体止于 4 空格缩进的右括号（else-if 与外层 if 同级）；内层 if 的
        # 8 空格右括号不会提前截断。
        # The branch ends at a 4-space brace; inner blocks use 8 and do not cut it short.
        m2 = re.search(r"st == EXCERPT_SECTION_%s\)\s*\{(.*?)\n    \}" % branch, pbody, re.S)
        if not m2:
            bad("B76 the EXCERPT_SECTION_%s branch is gone or reshaped: %s would take an "
                "unhandled path" % (branch, label))
        elif '"无对照可存"' not in m2.group(1):
            # 咬具体文案而不是「有没有气泡」：分支里另有「无可存内容」那一句，
            # 只查 reader_notice_set( 会让删掉本条文案的变异存活（S7 就是这么活的）。
            # Bite the wording, not "some toast": the branch has a second message, so
            # checking reader_notice_set( alone lets a deleted message survive.
            bad("B76 the EXCERPT_SECTION_%s branch no longer says 无对照可存 (%s): a silent "
                "failure is how the first dead key survived six taps" % (branch, label))


def main():
    check_b32_module_boundary()
    check_b33_credential_lock()
    check_b34_switch_consistency()
    check_b35_mount_wiring()
    check_b36_footer_contract()
    check_b37_receipt_class()
    check_b38_path_vocabulary()
    check_b41_commit_offload()
    check_b47_commit_ticket()
    check_b40_silent_writes()
    check_b49_commit_dance()
    check_b50_bak_order()
    check_b46_reader_hold()
    check_b54_offline_reader()
    check_b55_verify_single_exit()
    check_b56_three_beat_wiring()
    check_b57_excerpt_backoff()
    check_b58_mount_epoch()
    check_b64_ai_receipts()
    check_b66_excerpt_route()
    check_b68_cross_end_bit2()
    check_b72_gate_ladder()
    check_b73_prompt_budget()
    check_b74_ratio_receipt()
    check_b75_section_contract()
    check_b76_save_dispatch()
    if failures:
        for f in failures:
            print("FAIL: " + f)
        print("\nexcerpt structure teeth: %d failure(s)" % len(failures))
        return 1
    print("excerpt structure teeth: B32' module boundary + B33 bit2 credential lock + "
          "B34 switch consistency + B35 mount wiring + B36 footer contract + "
          "B37 receipt class + B38 path vocabulary + B41 commit hand-off + "
          "B46 reader hold + B47 commit ticket + B40 silent writes + "
          "B49 commit dance + B50 bak order + B54 offline reader + B55/B56 three-beat + "
          "B57 excerpt backoff + B58 mount epoch + B64 AI receipts/B62 banner + B66 excerpt route + "
          "B68 cross-end bit2 + B72 gate ladder + B73 prompt budget + B74 ratio receipt + "
          "B75 section contract + B76 save dispatch - all PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
