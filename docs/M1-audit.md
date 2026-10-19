# 二期 M1「安卓 BLE 蓝牙推流」· gen11 审计报告 + gen12 修复附录

> 范围：六步执行序终步审计 + 用户审计意见核销 + gen12 真机缺陷回归修复。基线 `main` @ `ae6cf52`。
> gen11 可烧录包（含四 bin + 安卓四件套）：**`/workspace/read-pico-gen11.zip`**。
> gen12 修复包（GATT 服务空白，见第七节）：**`/workspace/read-pico-gen12.zip`**。
> 编译与打包由本沙箱完成（用户侧为纯 Windows，无 ESP-IDF 工具链）。

---

## 一、审四件核销

### ① 地基验证全套 — ✅ 通过（三值改显式）

| 项 | 结果 |
|----|------|
| 13/13 宿主 runner | **全绿**，逐个 `exit=0` |
| **黄金三值（显式，不再「隐含」）** | 单栏 **`d3a187989f1f617c`** / page_map **`39cc94d66cbeb2b1`** / settings_panel **`81dffeaf94d63657`** —— 三值内嵌于 `book_layout_single_column` / `reader_page_map` / `reader_settings_panel` 三套 golden 对比，由 `run_book_host` / `run_display*` 等 runner 驱动；本轮 13/13 绿即三值未漂移 |
| 整树干净重建 | `idf.py clean && build` **exit 0** |
| 镜像体积 | `Read_Pico.bin` = **0x3dc0a0 = 4,047,008 B**，分区 0x580000，余 **30%** |
| 全量警告 | **8 条，与 gen10 基线同集**（M1 零新增） |

警告明细（清一色 `-Wunused-function`，均在 app_book.c / ui_pinpad.c）：

```
ui_pinpad.c:106   rounded_inside        app_book.c:718   save_shelf_cover_fast
app_book.c:683    load_shelf_cover_gray app_book.c:1392  editor_start
app_book.c:700    load_shelf_cover_fast app_book.c:5234  resize_text
app_book.c:6022   adjust_spacing        app_book.c:6049  select_reading_font
```

### ② push_sink 宿主红绿 + mutation 有牙 — ✅ 通过（补重建哈希铁证）

- 基线 **168 checks / 0 red**（含本轮新增的 py↔C 跨语言向量 19 条，见 ④）。
- 有牙演示：`push_session.c:68` `if (b->token != s->token)` 注入恒假 `&& 0`
  → **1 red @ line 411**（token 比对失效被 tooth-5 抓到）。
- **精确还原铁证**：还原该行后重建，`Read_Pico.bin` sha256 仍为
  `07e4e4722938274964bbfbfafaf578a5940f9fe16386427e21cad49397dd92c9`，
  与注入前逐字节相同 —— 证明还原无残留（此为版本串 bump 之前的锚，bump 后见第三节）。

### ③ gen11 锚定 — ✅ 补全（段表已补）

| 文件 | 大小 (B) | sha256 |
|------|---------|--------|
| `Read_Pico.bin` | **4,047,008** (0x3dc0a0) | `6490999f89bfa66411f971e29fbbe77c7a5fae321adeb90127f187c5d84e817e` |
| `bootloader.bin` | 21,232 | `229b7d357d3d3cb64dc2ada602baf2193a874cfb7a9edcf02d267c92aa042b27` |
| `partition-table.bin` | 3,072 | `9d5d890b49862fdb8a2cdd0df434e35b8cca5f4768d4b1a0980513b1af2d9e58` |
| `ota_data_initial.bin` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |

**seg1 双锚段表**（esptool image-info，7 段全表）：

```
Seg  Length   Load addr (vaddr)  File offs (paddr)   Memory types
  0  0x232e28 0x3c190020         0x00000018          DROM
  1  0x08a8c  0x3fc9c700         0x00232e48          BYTE_ACCESSIBLE, MEM_INTERNAL, DRAM
  2  0x04734  0x40374000         0x0023b8dc          MEM_INTERNAL, IRAM
  3  0x188054 0x42000020         0x00240018          IROM
  4  0x13f94  0x40378734         0x003c8074          MEM_INTERNAL, IRAM
  5  0x00024  0x500003dc         0x003dc010          RTC_DATA
  6  0x00030  0x600fe000         0x003dc03c          RTC_DRAM, RTC_IRAM
```

- **seg1 vaddr `0x3fc9c700` —— 四代不变锚保持**（gen8/gen9/gen10/gen11 同为 `0x3fc9c700`）。
- seg1 **File offs `0x00232e48`**（esptool 报的是**段头**偏移）；
  真机 boot 日志的 **seg1 paddr 预测值 = `0x00242e50`** —— 公式
  `paddr = File offs + 8(段头) + 0x10000(ota_0 基址)`，
  **由 gen5/gen6/gen10 三轮真机日志实证**（gen10：File offs `0x00232038` →
  实测日志 `paddr=00242040` ✓，见 `realhw-session-20261016/prod-log-gen10-round1.txt`）。
  相对 gen10 的 paddr 差 **+0x810 = 2,064 B**，与 seg0(DROM) 增长顺移一致，量级合理。
- 术语纪律：本文档此前把 esptool 的 File offs 直接称作 paddr，**两者相差段头 8B**，
  已就地更正；真机日志对表须用 paddr 口径（`0x242e50`），不要用 File offs。
- 镜像内部标识：`Project name: Read_Pico` / `App version: 0.3.3-pico11` /
  `Compile time: Oct 18 2026 15:34:25` / `Validation hash: 98008f3b…dd5ffd (valid)`。
- 相对 gen10（4,013,568 B）：**+33,440 B**，全部归因 M1 的 push_sink 组件 + 武装页
  （`tools/` 不进 bin）；分区余量 30%，安全。

### ④ 帧对表 — ✅ 升级为机器级（py↔C 不再靠人眼）

- py 自检：`push_pico.py --selftest` **20/20 PASS**。
- py↔js：`push_frames.js` 与 `push_pico.py` 同一 BEGIN 的十六进制 **diff 完全一致**。
- **py↔C（本轮新增，补齐单腿）**：把 `push_pico.py --hex` 生成的字节钉进
  `tools/push_sink_host_test.c` 的 `test_py_cross_vector()`，用 C 侧解码器断言：
  - BEGIN(83B) → token=0x1A2B、type=TEXT、total_len=300、crc32=0xf4ab327b、session_id=1、
    title=`跨语言向量✓`（多字节 UTF-8，含 ✓）
  - DATA 全帧（seq=0/241B + seq=1/59B）→ seq 与长度正确、载荷指针 == 帧+3
  - CTRL END / RESUME_QUERY 公共头 opcode 正确
  - **端到端 CRC**：两包载荷拼回正文，用 C 的 `push_crc32` / `push_crc32_continue` 重算，
    结果必须等于 py 写进 BEGIN 的 crc32 字段 —— CRC、长度、分包边界、seq 语义一次对死
  - 测试规模 **149 → 168 checks（+19）**，仍 **0 red**
- 两份自动化 XML `xml.dom.minidom` 良构校验通过。

**踩坑记录（诚实）**：首版只把 DATA 的 3 字节头喂给 `push_data_decode`，结果 4 条红线——
该解码器会校验 `len >= 3 + n`（载荷确实在缓冲区里），只喂头解不出来。这是实现**正确**的
行为，测试写法错了；改为嵌入 py 生成的完整 DATA 帧后转绿。

---

## 二、协议变更记录（PUSH_DATA：WWR → write-no-response）

> 审计裁定：草案 §3.1 原定 Write-With-Response，实现改为 write-no-response 且未声明，
> 属「设计变更被静默冻结」。裁决：**接受**，附三条件——协议冻结的前提是冻结的是**记录过的**协议。

**变更内容**：`PUSH_DATA`（`…5f11`）由 Write-With-Response 改为 write-no-response。
`PUSH_CTRL`（`…5f12`）仍为 write（有响应），`PUSH_STATUS`（`…5f13`）仍为 read + notify。

**变更原因**：
1. 端到端完整性不依赖链路层确认 —— 已由**应用层四层防线**承担：seq 连续性检查（保序）、
   长度对账、CRC32 校验、RESUME 续传（补包）。
2. 四件套的宏路径（Tasker / MacroDroid 原生 BLE 写能力弱，已声明事实）对 WNR 更友好。
3. 吞吐预期更优（免去逐包 ATT 往返）。

**条件落实**：
- **① 可靠性模型修订已文档化（本节即记录）**：§3.4 三层防线之第一层（ATT 逐包确认）
  **退场**，重传职责**移交 RESUME 机制**。变更日期 2026-10-18，随 gen11 首次生效。
- **② 速率承诺保守化**：文档维持 **8–25 KB/s** 保守值并标注「**WNR 实测后更新**」；
  §3.6 吞吐数学随真机数据重算，不在真机轮前据 WNR 理论值上修。
- **③ M2 路线图同步**：原「Write-Without-Response 提速」条目已被本变更**部分消费**，
  改为「**视真机吞吐决定是否引入窗口 ACK**」。

**附带同步**：BEGIN 83B 中的 `token(u16)` 字段系开工指令 a（token = 广播名后缀本身）的落地，
草案 §3.2 帧表原无此字段 —— 已同步进协议文档与四件套，按**记录过的**协议处理，不另计漂移。

---

## 三、真机前必解两项

### 1. 版本串 bump — ✅ 已完成

`0.3.3-rc95` → **`0.3.3-pico11`**（fork 专属、与 gen10 可区分，TF 卡升级不会被判「版本相同」拒绝）。

| 位置 | 改动 |
|------|------|
| `CMakeLists.txt:14` | `set(PROJECT_VER "0.3.3-pico11")` |
| `README.md:11` | 当前版本行 → gen11 / 0.3.3-pico11 |
| `web-flasher/manifest.json:3` | `"version": "0.3.3-pico11"` |
| Release tag | **用户侧待办**（真机轮八项通过前不推送，见第五节） |

bin 内实测 `App version: 0.3.3-pico11` ✓。

### 2. EVT_READ（0x14）— ✅ 已实现，本轮补齐触发入口

审计指出「汇报的 STATUS 清单里没有 0x14，Step 4 描述也没有标记已读入口」。核查结论分两层：

- **通知通道本就存在**：`push_proto.h:48` `PUSH_ST_EVT_READ = 0x14`；
  `push_sink.h:148` 声明 `void push_sink_notify_read(void)`；
  `push_sink.c:1198` 实现（编码 0x14、空 body、notify）。
- **但零调用方** —— 通道有、入口无，属模糊态，审计点破正确。

**裁定：补齐（非降级）**，惊喜2 ⭐ 属 M1 批准范围。落点：

> `main/apps/app_book.c` `book_on_exit()`：若离开的是手机推来的虚拟书
> （`strcmp(s_path, PUSH_TEXT_PATH) == 0`），则 `push_sink_notify_read()`。

选退出处而非翻开处的理由：**读没读完只有退出这一刻才说得上**，且不动阅读页任何既有路径。
宿主侧 `book_ui_host_test.py` 补 `PUSH_TEXT_PATH` / `push_sink_notify_read` 桩
（抽取式测试会抽走 `book_on_exit`），13/13 仍全绿。

**证据**：`grep -rn push_sink_notify_read main/` → `app_book.c`（book_on_exit 内 1 处调用）；
编码与 notify 由 push_sink 宿主测试覆盖（168 checks 0 red）。

---

## 四、gen11 交付包

`/workspace/read-pico-gen11.zip`（2.2 MB，**12 条目**，`unzip -t` 完整性通过；
条目数含 `android-tools/` 目录项；`flash.sh` 为第四节之二同步后补入，提交 `13b2c4d`）

```
Read_Pico.bin            4,047,008 B   ← 0x10000
bootloader.bin              21,232 B   ← 0x0
partition-table.bin          3,072 B   ← 0x8000
ota_data_initial.bin         8,192 B   ← 0xb10000
flash.sh                               烧录脚本（EXPECTED 锚定自检 + 拒烧防呆）
烧录说明-NOTES.md                      偏移/锚定/新功能/真机轮八项
android-tools/push_pico.py            参考实现（--selftest/--hex/--scan）
android-tools/push_frames.js          帧构建 JS（Tasker/MacroDroid 内嵌）
android-tools/ReadPicoPush.tasker.xml Tasker 导入件
android-tools/ReadPicoPush.macro      MacroDroid 导入件
android-tools/nrf-connect-manual.md   手动验证手册
```

**包内 bin 实测**：解压后 `Read_Pico.bin` 与 `build/Read_Pico.bin` `cmp` 逐字节一致 ✓，
包内 `App version` 读数 `0.3.3-pico11` ✓。

---

## 四之二、烧录防呆：flash.sh EXPECTED 同步 + 双路径干跑（gen11 烧前最后一件）

> 边界更正：发布管线（`/workspace/read-pico-realhw/production/`）本就在工作区内，
> 此前「gen11 打包由用户发布环境收口」的表述是**边界判错**，standing SOP 不该跳。

- **gen10 归档**：`Read_Pico.bin` → `Read_Pico.bin.gen10-9e573c84`（4,013,568 B，勿烧）。
- **四 bin 换入 gen11**：Read_Pico.bin（4,047,008）/ bootloader.bin / partition-table.bin /
  ota_data_initial.bin；实测 sha256 与第三节锚定表一致。
- **EXPECTED 同步**：`9e573c84…` → **`6490999f…4e817e`**。
- **flash.sh 头部锚定注释同步**：gen11 段表（seg1 vaddr / len / File offs / 预测 paddr）、
  体积 +33,440 B、版本串 `0.3.3-pico11`、bootloader 非确定性归因、partition/ota 逐字节不变。
- **双路径干跑**（留痕 `read-pico-realhw/realhw-session-20261019/flash-dryrun-gen11-20261019.txt`）：

| 路径 | 操作 | 结果 | exit |
|------|------|------|------|
| 1 | EXPECTED = gen11 正确值 | `sha256 OK: 6490999f… (production build, anchor verified)` → esptool 止于无串口设备 | **2**（沙箱无设备，与 gen10 轮同形态） |
| 2 | EXPECTED 篡改为 gen10 值 | `FATAL: Read_Pico.bin sha256 mismatch - refusing to flash.` | **1** ✅ 拒烧 |
| 复验 | 还原回 gen11 值 | `sha256 OK` | 2 |

烧录防呆有效：锚定值不符必定拒烧。gen8/gen9/gen10 三轮同形态，本轮补齐 gen11。

## 五、工程卫生：M1 已本地提交（不推送）

审计指出 1,300+ 行未提交改动躺在 pico-plus 公开树（ae6cf52）存在误推送与分叉风险。
**已按组件本地提交**（写实提交信息），**真机轮八项通过前不推送 pico-plus**
（公开仓只见稳定发布面）。canonical 开发位置记入 `docs/HANDOFF.local.md`。

提交分组：① `components/push_sink/*`（新组件 + 第 13 runner）② `components/ble_page_turner/*`
（三薄钩子）③ `main/app` + `main/sleep.c` + `main/app_main.c`（调度/睡眠豁免）
④ `main/apps/app_transfer.c` + `app_files.c`（武装页/入口）⑤ `main/apps/app_book.c`
（EVT_READ 入口）⑥ `tools/*`（桩/runner/四件套）⑦ 版本串三处 ⑧ `docs/*`（账本/审计）。

---

## 六、真机轮八项判读（用户侧，随协议漂移微调）

1. 推送后**自动唤醒全屏显示**，不需点屏（死线③）。
2. **连招**（以 EVT_READ 为前置）：复制 → 推 → 读 → 退出推送书 → 手机收到「已读」→ 剪贴板清空。
3. 中文长文（多包）**不掉字不乱序**（死线②）。
4. **断点续传**（重要度上升）：中途断连 → 重连自动续推（本包测的是 WNR 下的 RESUME 路径）。
5. 单词卡：推送单词 → 弹词典卡片，不开整书。
6. 充电常开：插电不计时；**拔电即退出**。
7. 翻页器让位：开启期间暂停，退出自动接回。
8. 防闪烁：收完统一写盘、一次全刷（死线④）。
9. **（gen12 起前置项）Services 页验收**：连上后 Services 页须显示推流服务
   `e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10`，展开含三特征 `…5f11`（写）/
   `…5f12`（写）/ `…5f13`（读+通知）；扫描列表广播即带该服务 UUID。
   gen11 此处空白（根因见第七节），boot 日志 seg1 paddr 对表 `0x00242f90`。

---

## 七、gen12 修复附录：GATT 服务空白（gen11 真机缺陷回归）

> 触发：gen11 真机轮反馈「BLE 能连上 `PicoPush-XXXX`，但 nRF Connect 的 Services 页空白，
> 看不到推流服务」。本节记录根因的源码实证链、修复形态与 gen12 门禁全表。

### 7.1 根因：NimBLE GATT 注册时序（源码实证，非猜测）

- `ble_hs_start()`（`host/src/ble_hs.c:815`）在 host 启动路径里**一次性**调用
  `ble_gatts_start()`，**然后**才 `ble_hs_sync()` 触发 sync 回调。
- `ble_gatts_start()` 消费 `ble_gatts_add_svcs()` 排进的定义队列，末尾
  `ble_gatts_free_svc_defs()` 把队列整个释放；属性表与句柄由它分配填充。
- 两阶段启动链：`ble_hs_init()`（nimble_port_init 内）enqueue stage1 → host 任务
  stage2 → `ble_hs_start()` → `ble_gatts_start()` → `ble_hs_sync()` → sync_cb。
  `BLE_HS_AUTO_START` 启用，stage2 必定消费队列。
- gen11 的 `register_gatt()` 排在 **sync 回调里**——排在一场**已经散会**的队列后面：
  `add_svcs` 返回 0 只代表**排队成功**，而消费队列的 `ble_gatts_start()` 早已跑完。
  注册成功的唯一实证是三个句柄非 0；排队成功 ≠ 注册成功。

### 7.2 修复形态（三处，均按用户指定：常驻注册 + 显式 start + 三次调用错误日志 + adv 带 UUID）

| # | 落点 | 内容 |
|---|------|------|
| 1 | `push_sink.c` 新增 `push_sink_gatt_register()` + `queue_svcs()` | `ble_gatts_count_cfg()`（累加上限，必须在 start 前调）→ `ble_gatts_add_svcs()`，**两步返回码都查**，失败 `ESP_LOGE` 留证；`ble_page_turner.c` `start_locked()` 在 `bonds_load()` 之后、`xTaskCreatePinnedToCore(host_task)` 之前调用，让 host 自己的 `ble_gatts_start()` 自然消费 |
| 2 | `push_sink_on_stack_sync()` 改 `gatt_ensure_registered()` | 以 `s_data/ctrl/status_handle` 全非 0 为注册实证；未注册则**显式** `ble_gatts_start()` 补一次（第三次调用），成功/失败分别 `ESP_LOGI`/`ESP_LOGE` 打印三句柄。源码路径上不该触发（AUTO_START 下 stage2 必消费）；真触发即排队或注册失败，日志会喊 |
| 3 | `adv_start()` | 广播数据带 128-bit 服务 UUID（`fields.uuids128`），nRF Connect 扫描列表连上前即可见 `e2f5a1c0…` |

**簿记语义修正**：`s_gatt_registered` → `s_gatt_queued`（定义已排队 ≠ 已注册）；
`push_sink_on_stack_stop()` 同步改。每启动周期恰好排队一次：`start_locked` 失败回退走
`nimble_port_deinit()`（`ble_gatts_stop` → `ble_gatts_free_svc_defs` 清队列），
stop 事件 FIFO 晚于 stage2，`s_running` 早退守卫兜底。

**设计权衡（为何不是另两种形态）**：显式 start 前置于 host 启动会被 stage2 的二次
start 清表（`ble_gatts_free_mem` + 空队列重注册）；仅靠 sync 回调兜底则依赖 host 已
启动且 sync 完成的时序。故取「常驻排队让 host 自己的 start 消费（主路径）+ sync 里
以句柄为证的防御性显式 start（兜底）」。另实证：本构建无人调用
`ble_svc_gap_init()`，队列中只有我们的服务，兜底 start 无误伤面。

**协议注释澄清（零行为变更）**：PUSH_DATA 特性位保持 `F_WRITE`，但 gen11 协议变更
记录（第二节）已裁定走 write-no-response——NimBLE ATT 服务端对两种写一视同仁
（`ble_att_svr_rx_write_no_rsp` 与 `rx_write` 均汇入 `write_handle → ha_cb`，
`MYNEWT_VAL_BLE_ATT_SVR_WRITE_NO_RSP=1` 已实证），故 WNR 与手动 WWR 都能到达
`push_access_cb`。注释已按此重写，消除「位上是 WWR」与「协议是 WNR」的表观矛盾。

### 7.3 gen12 门禁全表

| 项 | 结果 |
|----|------|
| 13/13 宿主 runner | **全绿**（`for s in tools/run_*.sh` 逐个 exit 0）；push_sink **168 checks / 0 red** |
| 整树干净重建 | `idf.py clean && build` **exit 0** |
| 全量警告 | **8 条 = 基线同集**（`ui_pinpad.c`×1 + `app_book.c`×7，unused-function；M1 与 gen12 零新增） |
| 版本串 | `0.3.3-pico11` → **`0.3.3-pico12`**（CMakeLists.txt / README.md / web-flasher/manifest.json 三处），bin 内实测 `App version: 0.3.3-pico12` |
| 镜像体积 | **4,048,208 B**（0x3dc550）；**+1,200 B = 4,048,208 − 4,047,008**（双操作数）；分区 0x580000，余 30% |
| seg1 双锚 | vaddr **`0x3fc9c700`（五代不变）** / len **`0x08a8c`（与 gen11 持平）** / File offs **`0x00232f88`**（gen11 `0x00232e48`，+320B，与 seg0 DROM 增长顺移一致）；boot 日志 paddr 预测 **`0x00242f90`** = `0x00232f88 + 8 + 0x10000`（公式 gen5/gen6/gen10 三轮实证） |
| 镜像 sha256 | **`219e0529d20193c24f0cb6a93d09eeb53d7d6fff85aa6cdc4d5a8cd849729d1c`**；bootloader / partition-table / ota_data_initial 与 gen11 逐字节不变 |
| 宿主测试适配 | `tools/ble_turner_host_test.py` 补 `push_sink_gatt_register` 桩（start_locked 被抽取编译，隐式声明触发 -Werror） |
| flash.sh 防呆 | EXPECTED 同步 gen12 值 + 锚点注释块重写 + gen11 bin 归档 `Read_Pico.bin.gen11-6490999f`；双路径 dry run：锚对 → `sha256 OK` 放行至 esptool（exit 2，沙箱无串口）；篡改 → `FATAL … refusing to flash`（exit 1）✅ 拒烧 |
| 交付包 | `/workspace/read-pico-gen12.zip` **12 条目**（四 bin + flash.sh + NOTES + android-tools/ 五件），`unzip -t` 通过；包内 bin 与 production 逐字节一致；安卓四件套与 gen11 逐字节相同（协议未变） |

**注释改动的哈希插曲（诚实记录）**：先编 gen12（`a4c975a1`）→ 后改注释 → 增量重建得
`219e0529`。逐字节比对仅 **65 B** 差异（`0xB0–0xCF` app_desc 内嵌 ELF 哈希 +
`0x3DC52F–0x3DC54F` 镜像尾校验和），系 DWARF 行表平移所致，非代码行为变化。
处置：**以 `219e0529` 为最终锚**，重部署 bin / EXPECTED / dry run / zip——
保证发出的 bin 与提交源码严格同源。

### 7.4 真机验收点（八项判读之前先看）

1. Services 页显示服务 `e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10` + 三特征（5f11/5f12/5f13）——本条即 gen11 缺陷的修复判据。
2. 扫描列表广播可见服务 UUID（连上前确认对面是推流服务）。
3. boot 日志 seg1 paddr 对表 `00242f90`；`App version: 0.3.3-pico12`。
4. 通过后再走第六节八项判读（判读项 2 以 EVT_READ 为前置、项 4 测 WNR 下 RESUME）。
