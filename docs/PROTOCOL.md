# 0x1A 情景上行 · 标志位登记簿 / Scene uplink (0x1A) flag registry

**这份文件是协议登记簿，不是设计稿。** `tools/check_excerpt.py` 的 B33 直接读它：
只要出包把 `CONFIG_PICO_EXCERPT_AI_SEND` 打开（`=y`），**bit2 那一行必须同时带真实
联调日期与 APK 版本**，缺一即判 FAIL 并把开关挡回去。

> This file is the protocol registry, not a design note. Tooth B33 reads it directly: if a
> build ships with `CONFIG_PICO_EXCERPT_AI_SEND=y`, the bit2 row must carry a real interop
> date and a real APK version. Missing either one fails the build gate.

## 一、帧体（0x1A，定长 241 B，零填充）

| 偏移 | 宽度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | opcode | `0x1A` = `PUSH_ST_AI_SCENE` |
| 1 | 2 | seq | 本帧序号，LE，从 0 起 |
| 3 | 2 | total | 总帧数，LE，1..5 |
| 5 | 2 | payload | 本帧有效载荷长度，LE |
| 7 | 1 | word_len | 首帧词长；后续帧无此字段 |
| 8 | ≤63 | word | 首帧词（无 NUL 语义，按 word_len 取） |
| 8+wl | 2 | word_off | 词在完整 ctx 内的字节偏移，LE |
| 10+wl | 1 | flags | 见下表 |
| 11+wl | ≤234−wl | ctx | 本帧上下文切片，按 UTF-8 整字符切 |

后续帧（seq>0）只有 `ctx` 切片，payload = 切片长度。

## 二、flags 位

| 位 | 宏 | 值 | 语义 | 联调日期 | APK 版本 |
|---|---|---|---|---|---|
| bit0 | `PUSH_SCENE_FLAG_CTX_TRUNCATED` | 0x01 | 上下文被截断（协议 1024 B 上界或设备端 768 B 缓冲，见 §三.3） | 2026-09-18 | v1.6.0 |
| bit1 | `PUSH_SCENE_FLAG_SENTENCE_FALLBACK` | 0x02 | 句界未找到，退回页界 | 2026-09-18 | v1.6.0 |
| bit2 | `PUSH_SCENE_FLAG_EXCERPT` | 0x04 | **整段摘抄解析**；word = 选区首词（锚点），ctx = 选区全文，`word_off = 0`；与 bit0 并存时 bit0 只表示「ctx 被截断，末尾为整字符边界」（见 §三.3/§三.4）。**v1.6.2 无此位实现**（它读得到 flags、认 bit0、无视 bit2，把段落当单词答——26.11.3-B 真机档② FAIL 的根因）；v1.6.3 补齐 | 2026-11-02（三点一致，源码级） | v1.6.3 |

bit2 是 **26.9.26 M5-A S26-3** 新增，复用既有帧体，**零新 opcode**：APK 见 bit2 即把
请求分流到「整段解析」，不见则照旧走单词情景卡——老 APK 收到 bit2 帧**不会崩、不会报错**，
它只是按老语义把首词当查询词，答案读起来还挺通顺。**这正是 26.11.3-B 翻车的样子**：
静默降级比崩溃更难抓，判据不能只看「有没有回帧」，要看「回的是不是段落级内容」。

## 三、bit2 的字段语义（设备侧实现即以此为准）

1. `word` **恒为 `ctx` 的前缀**，因此 `word_off` 恒为 `0`；APK 不应依赖非零偏移。
2. `word` 由选区文本开头跳过空白与常见中英标点后的**第一段连续字符**构成，≤63 B，
   按 UTF-8 整字符收口；全篇无断点时取前 63 B 的整字符前缀（**word 永不为空**）。
3. `ctx` = 选区原文（不折行、不去空白）。**两个数都要记，别混**：
   - **协议兼容边界 1024 B**（`PUSH_SCENE_CTX_MAX`）：帧体按这个上限设计，超过它设备侧
     直接拒发（`ctx_len > PUSH_SCENE_CTX_MAX` 返回 false），不是截断、是发不出去。
   - **设备端当前实现口径 768 B**（动作栏静态缓冲 `s_boxsel_text`）：26.9.26 的 S26-3
     实现就这么大，超出的部分按整字符截断并置 bit0。
   联调对账时若 APK 只看到 768 B 就追问「为什么不是 1024」，答案在这一行：1024 是协议
   允许的上界，768 是这台设备这一版的实际口径，两者都是真话，缺一个就要扯皮。
4. 一旦 `ctx` 被截断（768 B 实现口径或 1024 B 协议上界任一条触发），**必须同时置 bit0**，
   且截断点落在**整字符边界**（不会切出半个 UTF-8 序列）；APK 可据此提示「仅解析了
   前半段」。bit0 在 bit2 语境下只有这一个含义——**不会**出现「bit2 + bit0 表示别的」。
   APK 写分流时按这一条判断，不必猜。
5. 帧数与切分由 `push_scene_frame_count/push_scene_frame_at` 决定，任一帧未送达即
   **整条失败**（对端在 `seq==0` 重置），设备侧不投缺帧请求。

## 四、黄金向量

手推字节的对照向量在 `tools/excerpt_ai_golden_test.c`（`bash tools/run_excerpt_ai_golden.sh`）：
单帧短文本、两帧塞满首帧、bit0+bit2 组合各一组。**期望值产自本文件与
`push_proto.h` 的布局，不是从编码器打印回来的**——两者不一致时，先怀疑实现。

## 五、开关与凭证

- `CONFIG_PICO_EXCERPT_AI_SEND` **默认 n**。n 时框选动作栏的「AI解析」键**置灰不可点**，
  点击不消费（不退框选、不发帧）。
- 改 y 的前置条件：本文件 bit2 行的「联调日期」「APK 版本」两格填入真值，并跑过
  一次真机联调（APK 收到 `bit2=1` 且按整段解析返回结果）。
- **凭证分期（2026-11-02）——上一版的写法已作废，原因入册**：B 线出包时本行填的是
  「出包日 + `ReadPicoPush-v1.6.2.apk` 的 dex 含 `sceneSystemPrompt`/`sceneReassembler`」。
  那是**证据为真、推论越界**：它证明「情景释义存在」，**不证明 `bit2` 存在**。真机档② 当场
  抓出 v1.6.2 源码无 `SCENE_FLAG_EXCERPT`、`MainActivity` 无条件按单词分派。教训：
  **跨端标志位只能在对端源码或二进制里 grep 到同名常量才算凭证**，自家文档写「已登记」不算。
- **本行的凭证形态（26.11.3-C 起）**：三点一致——① 设备 `push_proto.h` 的
  `PUSH_SCENE_FLAG_EXCERPT (1u << 2)`；② APK `PushProto.SCENE_FLAG_EXCERPT = 1 << 2` +
  `MainActivity` 的 `scene.excerpt()` 分派 + `AiClient.excerptSystemPrompt()`；③ 本文件这一行。
  三者由 `tools/check_excerpt.py` 的 **B68 跨端牙**逐条比对（含数值相等）。源码级一致
  **不等于**真机通过：真机联调（APK 收到 `bit2=1` 且回段落四节）押在 26.11.3-C 复测卡，
  FAIL 即把开关回滚 n 并重出包、本行改「待联调」。
- B33 的锁**不删、只退役**：功能不死（开关在），绕行无门（凭证是唯一钥匙）。

## 六、0x08 承载上限：门梯（26.11.3-C2 起）

一条答案从模型到屏幕要过六道门。**最窄的那道门就是这条链的真实上限**，任何一道窄于
8,192 B，答案就在那一跳被砍——而且除了 APK 的字节帽会补「…」，其余几道都是**无声**的。
所以容量不是「够用就行」的缓冲，是一条必须同数的链。

| # | 门 | 常量 | 值 | 内存 | 这道门是瓶颈时，回执说什么 |
|---|---|---|---|---|---|
| ① | 提示词 | `AiClient.excerptSystemPrompt()` | 无上限 | — | （不设上限；若要改回限额，B73 牙先红） |
| ② | APK 字节帽 | `AiClient.EXCERPT_DEF_MAX_BYTES` | 8192 | APK 堆 | 日志 `truncated=1` + 「选段被截断：输出超过 8192 B」 |
| ③ | 0x08 分帧 | `PushProto.AI_SEG_MAX` = 166 B/帧 | ≤ 50 帧 | — | 帧数上限 = ⌈8192/165⌉ = 50；`seg_total` 为 u16，未到协议顶 |
| ④ | 设备重组器 | `AI_SEGASM_MAX` | 8192 | **PSRAM** | `ai seg overflow: > 8192 bytes` + `PUSH_ERR_TOO_LARGE`（整条丢弃，不截断） |
| ⑤ | 设备接收槽 | `PUSH_AI_DEF_MAX` | 8192 | **PSRAM** ×2（sink 槽 + 主循环槽） | 同上，槽位在溢出前不会成为瓶颈 |
| ⑥ | 呈现 | `AI_READER_DEF_MAX` / `AI_READER_MAX_PAGES` | 8192 / 64 | **PSRAM** | 页数够（8 KB ≈ 8–12 页）；`AI_READER_MAX_PAGES` 打满时现有分页逻辑截页 |

**词义路径是另一条链，不许跟着涨**：`PUSH_AI_CARD_MAX` = 1200（卡片缓存 / 弹卡段缓冲 /
生词批槽 / `push_ai_result_t`）。一条词义 1 KB 够用，按 8 KB 尺寸化就是八个 7 KB 的空槽。

**跨端同数**：②⑤ 必须相等（`EXCERPT_DEF_MAX_BYTES == PUSH_AI_DEF_MAX`），由
`tools/check_excerpt.py` 的 **B72 门梯牙**静态咬；`tools/run_ladder_mutations.sh` 用 8 个
反例证明它咬得住（槽缩回 4096 / 重组器留在 4096 / 跨端不同数 / 忘搬 PSRAM / 装回 340 帽 /
删掉「不设总字数上限」/ 设备等待窗长过 APK 超时 / 日志不报 truncated）。

## 七、段落解析的节标记（26.11.4 起）

整段解析（bit2）的答案是一篇五节文本。设备要从里面取出**一节**落进摘抄本，靠的不是
位置，是标记：

```
【原文对照】   ← 唯一的设备契约（EXCERPT_SECTION_MARKER）
【段意】【脉络】【关键表达】【作用】
```

**约定分两层，改动的代价完全不同**：

| 层 | 内容 | 谁定义 | 改动代价 |
|---|---|---|---|
| **契约** | `【原文对照】` 这五个字逐字节一致 | 两端同源，`PROTOCOL.md` 登记 | 改字 = 改协议，两端同一提交，B75 牙先红 |
| **呈现约定** | 五节的先后顺序 | 仅 APK（26.11.5 起对照在首节） | 改序 = 只改 APK 模板，**设备零改动** |

设备按标记定位、取到下一个行首 `【` 为止，**与节的先后无关**。这条不是巧合而是裁定：
26.11.5 把【原文对照】提到首节时，正因为节序没有升格成契约，这一刀才只落在 APK 一端，
设备 bin `[116]` 不重铸。

**失败形态（必须说话）**：

- 英文选段本应有此节却没有 → 气泡「无对照可存」+ WARN 日志，**不存**。宁可不存，
  也不把一篇没有对照的答案存成摘抄。
- 中文选段本就没有此节（模板规定只有英文才出）→ 兜底存**选段原文**（`ai_reader`
  另存的全文副本，不是 384 B 的引用条）。
- 有标记但节内为空 → 同上「无对照可存」。

**落盘形态**：节内文本折成一行（`\r\n\t` 与连续空白折单空格，行首空白不落——与
`fold_ws` 同规则），超 700 B 正文预算时**在句末标点处截**并补 `[截]`；找不到句末
才退到 UTF-8 边界硬切，同样补标记。截断永远是**声明的降级**。

牙：`tools/check_excerpt.py` 的 **B75**（标记逐字节一致 + 序关系 + 模板只提一次 +
设备不得按位置取节）与 **B76**（保存分派 + 两条路径各自的落点 + 逐分支的诚实失败）。
先红证明：`tools/run_section_mutations.sh`，8 刀全 killed。
