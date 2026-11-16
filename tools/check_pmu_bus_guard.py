#!/usr/bin/env python3
"""26.11.19 解耦批：结构守卫（K2b / K2a / 两戳 / 门谓词单源 / PEEK 不消费）。

这类守卫咬的都是「形态」，不是「行为」：行为已经由宿主用例咬住了，
这里防的是**下一次改动把形态悄悄改回去**——尤其是「我只是加个缓存」「我只是
顺手多刷一次」这两种滑坡，它们在行为测试里往往照样绿。

三件守卫（方案 §5）：
  1. 9 行动点清单（K2b）——ACK 7 行 + CLEAR_ALL 2 行，目录 5/3/1，不增不减。
  2. 取键路径自足（K2a）——take / drain 函数体内不出现 refresh_core / poll。
  3. 两戳单向 + 门谓词单源 + PEEK 不消费（§5.2 / §5.3）。

失败即非零退出；每行给「期望 vs 实际」，不给人留二次解释的空间。
"""

from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
failures = []


def read(rel):
    return (root / rel).read_text()


def strip_comments(text):
    """去掉 // 行尾注释与 /* */ 块注释，保留行数与换行。

    Why: 注释里写「另一枚戳不许出现」是给人读的，不该被守卫当成违规；
    但注释里写「这里调用 Ack」也不能被当成消费者。两侧都要清净。
    """
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return "\n".join(line.split("//")[0] for line in text.split("\n"))


def function_body(code, name):
    m = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^;{}]*?\)\s*\{", code, re.M)
    if not m:
        return None
    n = m.end()
    level = 1
    while level:
        if code[n] == "{":
            level += 1
        elif code[n] == "}":
            level -= 1
        n += 1
    return code[m.start():n]


def check(label, got, want):
    if got != want:
        failures.append(f"{label}: 期望 {want}，实际 {got}")


# ---------------------------------------------------------------- 1. K2b 动点
sources = list(root.glob("main/**/*.c")) + list(root.glob("components/**/*.c"))
ack_lines, clear_lines = [], []
for f in sources:
    rel = f.relative_to(root).as_posix()
    for i, line in enumerate(strip_comments(f.read_text()).split("\n"), 1):
        if "read_pico_pmu_event_ack(" in line and "esp_err_t read_pico_pmu_event_ack" not in line:
            ack_lines.append((rel, i))
        if "PMU_CMD_EVENTS_CLEAR_ALL" in line:
            clear_lines.append((rel, i))

check("K2b ACK 行数", len(ack_lines), 7)
check("K2b CLEAR_ALL 行数", len(clear_lines), 2)
check("K2b 动点总数", len(ack_lines) + len(clear_lines), 9)


def bucket(path):
    if path.startswith("components/read_pico_pmu/"):
        return "component"
    if path.startswith("main/apps/"):
        return "apps"
    if path.startswith("main/factory/"):
        return "factory"
    return "other"


dist = {"component": 0, "apps": 0, "factory": 0, "other": 0}
for p, _ in ack_lines + clear_lines:
    dist[bucket(p)] += 1
check("K2b 目录分布（组件/页面/工厂/其它）",
      (dist["component"], dist["apps"], dist["factory"], dist["other"]), (5, 3, 1, 0))

# ---------------------------------------------------------------- 2. K2a 取键自足
comp = strip_comments(read("components/read_pico_pmu/read_pico_pmu.c"))
for fn in ("read_pico_pmu_take_key_action", "read_pico_pmu_drain_events"):
    body = function_body(comp, fn)
    if body is None:
        failures.append(f"K2a: 找不到 {fn}")
        continue
    check(f"K2a {fn} 内的 refresh_core 调用", body.count("refresh_core("), 0)
    check(f"K2a {fn} 内的 read_pico_pmu_poll 调用", body.count("read_pico_pmu_poll("), 0)

# ACK 内部也不许把整份快照拉回来（便车的最后一截）。
ack_body = function_body(comp, "read_pico_pmu_event_ack")
if ack_body is None:
    failures.append("K2a: 找不到 read_pico_pmu_event_ack")
else:
    check("K2a event_ack 内的 read_pico_pmu_poll 调用", ack_body.count("read_pico_pmu_poll("), 0)

# ---------------------------------------------------------------- 3. §5.3 PEEK 不消费
refresh_body = function_body(comp, "refresh_core")
if refresh_body is None:
    failures.append("§5.3: 找不到 refresh_core")
else:
    check("§5.3 refresh_core 内的 ACK 调用（PEEK 升级为消费者的标志）",
          refresh_body.count("read_pico_pmu_event_ack("), 0)

# ---------------------------------------------------------------- 4. 两戳单向
loop = strip_comments(read("main/app/app_loop.c"))
check("§5.2 last_sample_ms 出现次数（声明 + due + 赋值）", loop.count("last_sample_ms"), 3)
check("§5.2 last_key_ms 出现次数（声明 + due + 赋值）", loop.count("last_key_ms"), 3)
check("§5.2 采样侧 due 配对", loop.count("pmu_bus_due(ctx.now_ms, last_sample_ms)"), 1)
check("§5.2 按键侧 due 配对", loop.count("pmu_bus_due(ctx.now_ms, last_key_ms)"), 1)
cross = [i for i, line in enumerate(loop.split("\n"), 1)
         if "last_sample_ms" in line and "last_key_ms" in line]
check("§5.2 同一行出现两枚戳（串写的前兆）", cross, [])

# ---------------------------------------------------------------- 5. 门谓词单源
# 手写三条件的形态特征：ready() 紧接 !holds_pmu（中间允许换行）。别处的
# holds_pmu 判断（在线服务、BLE、对话框）不属于这条门，不能一并算违规。
manual_gate = re.findall(r"read_pico_pmu_ready\(\)\s*&&\s*!current->holds_pmu", loop)
check("§5.2 app_loop 里手写展开的三条件门", manual_gate, [])
check("§5.2 app_loop 调用单源谓词", loop.count("pmu_bus_static_ok("), 1)
header = strip_comments(read("main/app/pmu_bus.h"))
check("§5.2 pmu_bus_static_ok 定义处", header.count("static inline bool pmu_bus_static_ok("), 1)
check("§5.2 pmu_bus_due 定义处", header.count("static inline bool pmu_bus_due("), 1)
other_defs = [f.relative_to(root).as_posix() for f in sources
              if f.relative_to(root).as_posix() != "main/app/pmu_bus.h"
              and "pmu_bus_static_ok(" in strip_comments(f.read_text())
              and "static inline bool pmu_bus_static_ok(" in strip_comments(f.read_text())]
check("§5.2 谓词的第二个定义点", other_defs, [])

# ---------------------------------------------------------------- 判决
if failures:
    print("FAIL: PMU bus guards")
    for line in failures:
        print("  -", line)
    print("  动点实况：")
    for p, i in ack_lines + clear_lines:
        print(f"    {p}:{i}")
    sys.exit(1)

print("PASS: PMU bus guards (K2b 9 action points 5/3/1, K2a self-sufficient take, "
      "two non-crossing stamps, single gate predicate, PEEK not a consumer)")
