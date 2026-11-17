#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""首页手势批（26.11.17）的结构牙 / structural teeth for the home-gesture batch.

只查「接线形态」，不查行为（行为在 app_loop 宿主用例与 5 刀变异里）：
  1. 主机三文件零改动——本批的全部改动必须落在 home_gesture.c 与 app_dashboard.c；
  2. desc 里真的注册了 on_dual_gesture（不注册，双指块整段不跑，H1-a 形态的老坑）；
  3. 长按/空白判定真的被页面引用（不是「模块写好了、页面没用」）；
  4. 热区常量单源（页面引用 HOME_HIT_*，不再自带一份字面量）；
  5. 视图编号由 _Static_assert 钉住；
  6. 判定模块不引入 ESP 依赖（保持宿主可编）。

主机基线是**本批开工时的字节哈希**：以后谁改了 app_loop/ui_gesture/ui_dualgesture，
这颗牙会红并提示「结构不变式到期」——红是通知，不是故障；到期时把基线更新到新值即可。
The three host files are pinned by the bytes they had when this batch opened. If a later
batch edits them, this tooth goes red and says the invariant expired - that red is a
notice, not a fault; re-baseline it then.

**红了怎么判（常驻注记，2026-11-06 立——以后不用再现场推导）**

1. 先分清是哪一条红：只有 `host untouched: <file>` 这一类是**基线过期**；
   其余（desc 注册 / 页面引用 / 热区单源 / _Static_assert / 无 ESP 依赖）是真断言，红了就是真回归。
2. 对红的那个文件跑 `git diff`，确认改动是**本批语义需要的**（例：采样支成块、门谓词搬家），
   不是顺手改的格式或注释。
3. 看**同文件下方的语义断言**是否仍 PASS——尤其 `dual dispatch block still in the loop`：
   - 语义在、只剩哈希过期 ⇒ **到期通知**：更新 `HOST_BASELINE`，并在字典上方写清「哪一批、改了什么」；
   - 语义断言也红 ⇒ **真回归**：不许重定基线，去查谁把派发块搬走了。
4. 重定基线时**只改那一个文件的哈希**，别顺手把另外两个也刷一遍——那会让真正的漂移失去哨兵。

先例两例，同型：26.11.18（改 `nav_battery_watch`）、26.11.19（采样支 + 两戳分离）。
共同点：**红的是哈希基线，绿的是语义断言**。这个组合一出现，基本可以直接判到期。

How to triage a red (standing note, 2026-11-06): only the `host untouched:` assertions are
baseline-expiry notices; every other assertion is a real check. Diff the file, confirm the
change is what this batch semantically needs, then look at the semantic assertions below -
if they still pass, re-baseline that one hash and record which batch changed what; if they
also fail, it is a real regression and the baseline must not be touched.
"""
import hashlib
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DASH = ROOT / "main/apps/app_dashboard.c"
MOD = ROOT / "main/apps/home_gesture.c"
MOD_H = ROOT / "main/apps/home_gesture.h"

# 26.11.18 节拍批重定基线（app_loop.c）：只改了 nav_battery_watch——分钟源 + 矩形选择
# + commit 多一个参数，手势派发块逐字未动（下面有一条守卫专门咬它）。这正是本文件开头
# 说的「红了是到期通知」：手势批的不变式是「手势接线不进主机」，不是「主机永远不许改」。
# Re-baselined for the 26.11.18 tempo batch: only nav_battery_watch changed (minute source,
# rect choice, commit arity). The dual-gesture dispatch block is untouched and has its own
# guard below - the invite is "gesture wiring does not enter the host", not "the host is frozen".
#
# 26.11.19 解耦批再重定（app_loop.c）：采样支独立成块（`pmu_bus_static_ok` /
# `pmu_bus_due` / `last_sample_ms`）、按键门改为两戳之一（`last_key_ms`）、
# 谓词改从 `pmu_bus.h` 取。**手势派发块仍逐字未动**——下面「dual dispatch block still
# in the loop」那条守卫照咬，它才是手势批的不变式本体。基线只是「到期通知」的落账处。
# Re-baselined again for the 26.11.19 decoupling batch: the sampling branch became its own
# block, the key gate now uses `last_key_ms`, and the gate predicate comes from pmu_bus.h.
# The dual-gesture dispatch block is still untouched and still guarded below.
#
# 26.11.31 再重定（app_loop.c）：4ba71cc「导航仲裁 D/C 落码、文件页提速归位」改了
# app_loop.c 218+/13- 行，但 `git show | grep -i gesture|dual|dispatch|retract` 为空——
# 手势派发块逐字未动，`dual dispatch block still in the loop` 守卫仍 PASS。属正当落码后
# 的基线到期（脚注定义），按纪律重签；不变式本体未变。
# Re-baselined for 4ba71cc (nav arbitration D/C): 218+/13- lines changed in app_loop.c
# but none touch the gesture/dual/dispatch block (grep clean, dispatch guard still green).
HOST_BASELINE = {
    "main/app/app_loop.c": "e4499bc09825001bdef7cf74e87441362fcfcf0b0a3d4c729043d57263fafea4",
    "main/ui/ui_dualgesture.c": "5f1bd101cf69e913fa5fc440f669debf588145501f58ae2045014bc4e355c04c",
    "main/ui/ui_gesture.c": "86dbf6d7442405f8f45f7f82d84381fdf8a8c8289e7f04586d24b3c021baab26",
}

failures = []


def check(name, ok, detail=""):
    print(("  PASS  " if ok else "  FAIL  ") + name + (("  " + detail) if detail else ""))
    if not ok:
        failures.append(name)


def sha256_of(rel):
    return hashlib.sha256((ROOT / rel).read_bytes()).hexdigest()


def main():
    dash = DASH.read_text(encoding="utf-8")
    mod = MOD.read_text(encoding="utf-8")
    mod_h = MOD_H.read_text(encoding="utf-8")

    for rel, want in HOST_BASELINE.items():
        got = sha256_of(rel)
        check(
            "host untouched: " + rel,
            got == want,
            "" if got == want else "got %s… (invariant expired? re-baseline deliberately)" % got[:12],
        )

    check(
        "desc registers on_dual_gesture",
        ".on_dual_gesture=on_dual_gesture," in dash,
        "without it the dual block never runs for this page",
    )
    check(
        "page calls the decider (blank / long press)",
        dash.count("home_long_decide(") == 1 and dash.count("home_dual_wants_push(") == 1,
        "exactly one call each - a second one would be a second truth",
    )
    check(
        "page calls the honest-failure router",
        dash.count("home_push_route(") == 1,
        "empty inbox must route, not stay silent",
    )
    check(
        "hot-zone constants are single-sourced",
        "return HOME_HIT_CONTINUE;" in dash and "#define PUSH_CARD_HIT  HOME_HIT_PUSH" in dash,
        "the page must not keep its own copy of the rects",
    )
    check(
        "view ids pinned by _Static_assert",
        "_Static_assert(" in dash and "home_gesture view ids must match" in dash,
        "reordering either enum must fail the build",
    )
    loop = (ROOT / "main/app/app_loop.c").read_text(encoding="utf-8")
    check(
        "dual dispatch block still in the loop",
        "current->on_dual_gesture && err == ESP_OK" in loop
        and "ui_dual_feed(&dual, &ctx, &dev)" in loop,
        "re-baselining the host must not quietly drop the dual dispatch",
    )
    check(
        "status info band excluded from blank",
        "if (rect_hit(HOME_RESERVED_INFO, x, y)) return false;" in mod,
        "26.11.17 narrowing: the info strip is not gesture territory",
    )
    check(
        "decider stays ESP-free",
        not any(line.lstrip().startswith("#include") and "esp_" in line for line in mod.splitlines()),
        "it must compile on the host with stubs only",
    )
    check(
        "sequence latch lives in the decider",
        "seq->fired" in mod and "UI_DUAL_EVT_RETRACT" in mod,
        "RETRACT is the only sequence boundary available to a page",
    )
    check(
        "steps<0 anchored in the header comment",
        "steps<0" in mod_h or "steps < 0" in mod_h,
        "direction must name the site it was read from (discipline 10)",
    )

    if failures:
        print("JUDGEMENT: HOME_GESTURE STRUCTURE FAIL (%d)" % len(failures))
        return 1
    print("JUDGEMENT: HOME_GESTURE STRUCTURE PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
