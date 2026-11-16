# 31b 第 1 条 · 三拍封送（quiesce 三合取）落码回执

**日期**：2026-11-01
**批**：31b 落码批（第 2 条已落；本条为第 1 条）
**bin**：`11858c7e0123…` / **4,135,584 B**（未出包）
**门禁**：待复跑（本回执随提交入库，日志另附）

---

## 一、裁定六项：逐项落实

| 裁定 | 落法 | 位置 |
|---|---|---|
| **① 通道修正**：禁 SD 直接调 push | SD 侧只发布事实（`s_last_release` / `gained` / `lost` 都是既有量），`app_loop` 观测边沿后调 `push_sink_thaw()`；`read_pico_sd.c` **一行未改** | `app_loop.c:790 / 876 / 623` |
| **② mount 边沿升格为主通道** | 措辞与代码都改了：`push_sink_thaw("remount edge")` 在 `on_media_ready` **之前**调用，注释写明「这是主通道，不是兜底」 | `app_loop.c:790` |
| **③ 冻结生存期绑定三拍区间** | 终态枚举两条 + 一条显式保持，**没有第三种出口**（见 §三） | — |
| **④ C 否决** | 没有「第 3 次强制解冻」。闩锁全败 → `push_sink_quiesce_hold()` 显式保持冻结 | `app_loop.c:611 / 626` |
| **⑤ 看门狗 20 s** | `PUSH_FREEZE_WATCHDOG_MS 20000`，只在 `FRZ_FROZEN ∧ ¬FRZ_HOLD ∧ 超时` 三合取时触发；触发即解冻 + 一次性事件 → `PICO_BOOTBB_NOTE_PUSH_FREEZE` | `push_sink.c:2661`、`app_loop.c:562` |
| **⑥ B 拆解，边界写死** | 只做防御性最小集：冻结标志 + 设置点（lost 一处）+ 两解冻边沿 + 三处判点。**未碰** ARM/init 判据扩展与挂起/拒收选型（B52） | 见 §二 |

---

## 二、改了什么

### 组件侧（`push_sink`，只发布事实，不认识 SD）

| 新增 | 作用 |
|---|---|
| `push_sink_quiesce(timeout_ms)` | 先冻再等；三合取成立返回 true（**冻结保持**），超时返回 false（**冻结也保持**）并留痕「谁没停、卡在哪」 |
| `push_sink_thaw(why)` | 解冻；幂等；清毒卷标记 |
| `push_sink_quiesce_hold(why)` | 毒卷显式保持：重新起算看门狗（这是「有意」不是「卡死」） |
| `push_sink_frozen()` / `push_sink_live_files()` | 观测值，供诊断与宿主断言 |
| `push_sink_take_freeze_timeout(&ms)` | 一次性事件，app 取走写黑匣子（组件不反向依赖 bootbb） |

**三处判点**（合取③）：

```
on_ctrl_write()   → if (entry_frozen_reply()) return;   // 回 BUSY，不回 NOT_ARMED
on_data_write()   → 同上
try_adopt()       → if (push_sink_frozen()) return;     // truncate 无句柄，只有这道闸能挡
```

> 为什么回 `BUSY` 而不是 `NOT_ARMED`：会话可能还在（只是暂时不许进）。`BUSY` 让手机退避重试，
> `NOT_ARMED` 会被读成「用户取消了」，手机直接放弃这篇。

### 判据②：从推断变测量

`do_verify()` 的 `FILE *r` 是局部变量，函数返回即消失——外部**根本无从观测**。B51 当年被判
「不可见」正是这个原因，降格结论是「顺序推断，当前拓扑不重叠」。本条把它变成事实：

```
s_verify_depth：进入 do_verify +1，出函数 -1
push_sink_live_files() = s_verify_depth + push_part_is_open()
```

**单出口是这笔账的生命线**：`do_verify()` 全函数没有一处 `return`，中途加一个就是「计数虚高 →
quiesce 永远等不到 0 → push 永久死」。B55 咬这一条，含一枪「中途 return 不减」的变异。

### app 侧（`app_loop`，观测边沿）

```
lost 分支：on_media_lost → 字体切内置 → push_sink_quiesce(2000) ─┬ 成功 → remount → thaw("unmount done")
                                                                └ 失败 → 不 remount，置 pending，返回
闩锁重试：quiesce(2000) ─┬ 成功 → remount → thaw("unmount done (latch)")
                         └ 失败 → tries++ ≥3 → quiesce_hold("…never stopped")
gained 边沿：thaw("remount edge") → on_media_ready
每帧：take_freeze_timeout → PICO_BOOTBB_NOTE_PUSH_FREEZE
```

---

## 三、终态枚举：没有第三种出口

| 终点 | 处置 | 谁打 |
|---|---|---|
| 卸卷成功（主路径） | `thaw("unmount done")` | app_loop |
| 卸卷成功（闩锁） | `thaw("unmount done (latch)")` | app_loop |
| 闩锁 3 次全败（会话没停） | `quiesce_hold("…never stopped")` | app_loop |
| 闩锁 3 次全败（卸载失败） | `quiesce_hold("…unmount failed")` | app_loop |
| 插回重新挂载 | `thaw("remount edge")` | app_loop |
| **终态漏了 / 流程卡死** | 看门狗 20 s 强制解冻 + 黑匣子 | 组件 |

前两行与后两行是「同一个终态的两条岔路」，都单独标记——只留一处，另一条会静默地把毒卷解冻。

---

## 四、牙：B55 / B56，十向先红

| 牙 | 咬什么 |
|---|---|
| **B55** | `do_verify` 计数两端都在、顺序正确（先加后减）、**全函数无 return** |
| **B56** | 三处入口判点存在；看门狗谓词**真的在判**（不是 `false &&` 短路）；app_loop 两处 quiesce、三处 thaw、两处 hold、黑匣子落点 |

**十向先红结果：10/10 红**（初测 7/10，三个漏掉后加严）。

| # | 变异 | 初测 | 加严后 |
|---|---|---|---|
| ① | 删 `++` | ✓ | ✓ |
| ② | 删 `--` | ✓ | ✓ |
| ③ | 中途插 `return`（漏减） | ✓ | ✓ |
| ④ | ctrl 判点消失 | ✓ | ✓ |
| ⑤ | data 判点消失 | ✓ | ✓ |
| ⑥ | try_adopt 判点消失 | ✓ | ✓ |
| ⑦ | 看门狗短路 `false &&` | **✗ MISSED** | ✓ |
| ⑧ | remount 解冻边沿消失 | ✓ | ✓ |
| ⑨ | 只留一条岔路标记 | **✗ MISSED** | ✓ |
| ⑩ | 黑匣子调用设成 `if (0)` | **✗ MISSED** | ✓ |

三个漏的共同病根：**检查只到「符号存在」，到不了「真的在用」**。加严三处：

- ⑦ 改成正则抓谓词表达式，判 `FRZ_FROZEN / FRZ_HOLD / PUSH_FREEZE_WATCHDOG_MS` 三者齐，且不以 `false &&` / `0 &&` 开头；
- ⑨ 从「至少一处」改成「两条岔路各一处」（`count >= 2`）；
- ⑩ 要求调用不是 `if (0) pico_bootbb_note(...)` 的死代码。

> 加严 ⑦ 时第一次误报：`!= 0 &&` 里的 `0 &&` 被当成短路。改成「去空格后以 `false&&`/`0&&` 开头」才准。
> 检查本身也会过严——过严会逼人改代码迁就牙，比漏更坏。

### 宿主用例（`app_loop`，58 场景 + 3 条断言）

| 用例 | 断言 | 先红 |
|---|---|---|
| 顺序 | `quiesce_calls==1 && remounts_before_quiesce==0 && remounts==1 && thaw_calls==1` | ✓（把 quiesce 挪到 remount 之后、**保持 remount 次数不变**，红） |
| 失败不卸卷 | `quiesce_ok=false → remounts==0 && thaw_calls==0` | ✓ |
| 插回解冻 | 既有热插拔用例加 `thaw_calls>=2` | ✓ |

顺序用**调用时刻采样**（`remounts_before_quiesce`），不是事后问——事后问区分不出「先 quiesce 后 remount」
与「先 remount 后 quiesce」，与 commit 4 的 abort 采样同一条纪律。

---

## 五、必须申报的两件事

### ① 我的 B55/B56 第一次「全绿」是假的：定义了但没进清单

写完牙跑 `check_excerpt.py`，输出全 PASS。十向先红时 **10 个全 MISSED**——原因是我注册那两行时
用了 `\n` 转义，字符串没匹配上，`replace` 静默失效：**函数定义了，`main()` 里没调用**。

这跟 §13.12「没跑 ≠ 绿了」是同一个形状的第二次发作，只是这次发生在**牙自己身上**：
牙齿装上了、也通电了，但没接到嘴里。区别在于——这次是**先红验证在 5 分钟内抓住了它**，
而不是等它放行一个真缺陷。修好注册后十向才逐条变红。

**入册动作**：`check_excerpt.py` 的注册区应该有一条自检测（「定义的 check_b* 函数数 == 被调用的数」），
我下一纸面批补，不夹带进本条。

### ② bss 超预算 16 B，**送裁不自决**

| 点 | 值 |
|---|---|
| 26.9.30 出包 | `0x21550` |
| 31b 第 2 条后 | `0x21560`（+16，批内） |
| 本条后 | `0x21580`（**再 +32**） |
| **31b 累计** | **+48 B，超预算 +32 共 16 B** |

本条新增静态量实际只有 **6 B**：

```
s_freeze_flags 1 + s_frozen_ms 4 + s_verify_depth 1
```

其余 26 B 是链接对齐效应（三个符号挤在 `0x…b4–0x…bc` 的 8 B 块里，段仍涨 32）。

已做的压缩（为回到预算）：三个布尔并成一个标志字节、`s_verify_depth` 改 `uint8_t`、看门狗时长复用
`s_frozen_ms`（触发时改写为「已冻结多久」）。**分开放时段增 32 B，合并后仍是 32 B**——说明瓶颈不在数据量。

按裁定「超出 +32 回来重议预算，不许先斩后奏」，**停在这里送裁**，两个选项：

- **A（推荐）**：31b 预算追加到 **+48 B**，本条按现状交付。理由：6 B 是真实数据，26 B 是链接器的事，
  再压只能靠把状态塞进别人的 padding（可读性和语义都要付代价）。
- **B**：我继续压——把三个量做成 packed struct、或塞进已有 bool 群，但**不保证**段增量能降（对齐由链接器定）。

---

## 六、边界申报（没做的，写明白）

| 项 | 状态 |
|---|---|
| 格式化（`app_files.c:999`）/ USB（`usb_storage.c:82`）/ 深睡（`sleep.c:216,413`）三处 | **未加** push quiesce。裁定第 ⑥ 条只点了「lost 触发三拍处」，我照边界做，不从旁边扩。**风险**：这三处动卷时 push 若在收包，句柄仍有可能被抽走。归 B52 的完整入口闸 |
| ARM/init 判据扩展（`commit_live ∨ hold 等待`）、挂起 vs 拒收选型 | 未做（B52） |
| 真机验证 | **零**。本批只有宿主牙 + 静态牙 + 编译 |
| 第 2 条材料 | 本批一并投（见 `26.11.1-31b第2条落码-20261101.md`） |

---

## 七、下一步

1. 本条材料 + 第 2 条材料一并送核销（bin `11858c7e0123`，bss 与超支一并报）。
2. 核销后按七条继续：第 3 条 E-1 常量表 → 第 4 条 E-2（`read_pico_sd` 的 `s_mount_epoch`）→ 第 6 条 B42。
3. 合流批次仍在 31b 落码完成后。
