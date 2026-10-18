# 二期 M1「安卓 BLE 蓝牙推流」· gen11 审计报告

> 范围：六步执行序终步审计 + 用户审计意见核销。基线 `master` @ `ae6cf52`。
> gen11 可烧录包（含四 bin + 安卓四件套）：**`/workspace/read-pico-gen11.zip`**。
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

- **seg1 vaddr `0x3fc9c700` —— 三代不变锚保持**（gen8/gen9/gen10 同为 `0x3fc9c700`）；
  seg1 paddr（file offs）`0x00232e48`。
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

`/workspace/read-pico-gen11.zip`（2.2 MB，11 条目，`unzip -t` 完整性通过）

```
Read_Pico.bin            4,047,008 B   ← 0x10000
bootloader.bin              21,232 B   ← 0x0
partition-table.bin          3,072 B   ← 0x8000
ota_data_initial.bin         8,192 B   ← 0xb10000
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
