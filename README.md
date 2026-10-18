# ReadPico 横屏双栏 · 墨水屏阅读器固件（修改版）

> 给 MindReset RDP-G01-W 墨水屏阅读器用的**第三方修改版固件**：在官方开源固件基础上，新增
> **横屏界面、双栏排版、离线英汉词典、生词本**等功能。
>
> 本项目与 MindReset 官方无关，也不是官方发布。固件按"原样"提供，刷机有风险，
> 请阅读文末[免责声明](#七免责声明)。

**当前版本：v1.0（内部代号 gen10，固件版本号 0.3.3-rc95）**

---

## 一、这个固件能干什么

在原版功能（书架、EPUB/TXT 阅读、阅读进度、字体导入、锁屏、USB 存储、WiFi 传书）之上，
本修改版主要新增：

| 功能 | 说明 |
|---|---|
| 横屏显示 | 界面支持横屏摆放，握持阅读更接近普通书籍 |
| 双栏排版 | 一页左右两栏，翻页次数减半；可在阅读设置中开关 |
| 离线词典 | 阅读中取词查义，无需联网（**词典文件需自行下载并放到 TF 卡，见下方重要提示**） |
| 生词本 | 查过的词自动记录，随时复习 |
| 更快的翻页 | 纯文字翻页不闪黑、不擦白，局部刷新 |
| 水波纹翻页 | 翻页带水波纹动画效果（可选） |

> ## ⚠️ 重要：离线词典必须自备词典文件
>
> **本固件不包含词典数据。** 查词功能依赖放在 TF 卡里的词典文件：
> 需自行下载 `dict.bin`，并放入 TF 卡根目录的 `dict` 文件夹下
> （即 `/sdcard/dict/dict.bin`），**否则无法使用查词功能**。
> 下载地址与详细步骤见[第四节「刷完之后」](#四刷完之后把书放进设备)。

原版的传书、WiFi、USB 存储、TF 卡读写等能力全部保留（见[第四节](#四刷完之后把书放进设备)）。

## 二、刷机方法（三选一）

> **刷机前**：确保设备电量充足（建议 50% 以上）；请使用 **USB 数据线**
> （部分充电线只能充电、不能传数据）。

### 方法 1：网页一键刷机（推荐，最简单）

不需要安装任何软件，用电脑浏览器就能刷。

1. 用 USB 数据线把设备连接到电脑；
2. 用电脑打开网页刷机页面（需 **Chrome 或 Edge** 浏览器，暂不支持手机浏览器和 Safari）：

   ```
   https://<你的用户名>.github.io/<仓库名>/
   ```

   （本仓库开启 GitHub Pages 后，上面的地址就是刷机页——`web-flasher/` 目录会被发布为站点根目录。
   如果你还没把仓库放到 GitHub，也可以在电脑上进入 `web-flasher` 目录后运行
   `python -m http.server 8000`，然后用浏览器打开 `http://localhost:8000` 刷机。）
3. 点击页面上的 **「安装固件」** 按钮；
4. 在弹窗中选择设备对应的串口（如 `COM3`、`USB JTAG/serial debug unit`），点击连接；
5. 等待进度条走完（约 1～2 分钟），设备自动重启，刷机完成。

> 之前刷过官方固件的设备，工具会提示**擦除全部数据**，属正常现象
> （官方固件与本固件的分区布局不同，必须整片擦除后才能写入），确认即可。

**遇到问题？**

- 按钮点了没反应 / 找不到串口 → 换一根数据线、换一个 USB 口、关掉其他可能占用串口的软件；
- 页面一直转圈或按钮不出现 → 是刷机脚本没加载出来，换个网络环境重试；
- 提示浏览器不支持 → 必须用电脑版 Chrome / Edge；
- 烧到一半失败 → 重新插拔 USB 后重试。

### 方法 2：Windows 一键刷机脚本

1. 下载本仓库 Release 中的 `release-v1.0-gen10.zip` 并解压
   （内含 4 个 `.bin` 文件、一个「一键刷机.bat」批处理和一份说明文本；
   **词典文件不在这个包里**，需另外下载，见[第四节](#四刷完之后把书放进设备)）；
2. 安装 Python（到 [python.org](https://www.python.org/downloads/) 下载，
   安装时**务必勾选 "Add Python to PATH"**）；
3. 按 `Win + R`，输入 `cmd` 回车，执行：

   ```
   pip install esptool
   ```

4. 用 USB 线连接设备，**双击解压目录里的「一键刷机.bat」**；
5. 按提示等待，看到 `Flash done` 即完成。

脚本会自动校验四个固件文件的完整性，校验不通过会拒绝烧录并提示。

### 方法 3：电脑命令行线刷（Linux / macOS / Windows 通用）

适合熟悉命令行的用户：

```sh
pip install esptool   # 先安装 esptool

python -m esptool --chip esp32s3 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0      bootloader.bin \
  0x8000   partition-table.bin \
  0x10000  Read_Pico.bin \
  0xb10000 ota_data_initial.bin
```

四个偏移对应 16MB 分区布局（app 分区 `ota_0` 在 `0x10000`，故 `otadata` 落在 `0xb10000`）。
烧录中途报错，可把 `-b 460800` 改为 `-b 230400` 重试。

## 三、TF 卡升级（不用电脑，在设备上直接升级）

1. 把固件包里的 `Read_Pico.bin` **重命名为 `Pico-update.bin`**；
2. 用读卡器把它拷到 TF 卡（FAT32 格式）的**根目录**；
3. TF 卡插回设备，进入 **设置 → 升级和恢复**；
4. 点击「安装 TF 卡升级包」，等待自动完成并重启。

> 如果设备当前已刷入**相同版本**的固件，会提示「升级包与当前版本相同」——
> 此时想重刷，请用上面的网页刷机或线刷。

> **重要：不要使用设备里的「在线升级」。** 在线升级检查的是官方固件地址，
> 会刷回官方版本，丢失本修改版的横屏、双栏等功能。本修改版请只用本页三种方式升级。

## 四、刷完之后：把书放进设备

三种方式任选：

- **WiFi 传书（最方便）**：设备上打开「传书」应用 → 按屏幕提示让手机/电脑连接设备热点 →
  在浏览器里打开屏幕提示的地址，即可上传 EPUB / TXT；
- **USB**：设备开启 USB 存储模式后，直接当 U 盘拖入书籍；
- **TF 卡**：把书拷到 TF 卡，在设备文件管理中打开。

### 离线词典设置（不设置就无法查词）

> **本固件不包含词典数据。** 查词功能依赖放在 TF 卡里的词典文件，
> 必须按下面的步骤放置，否则无法使用查词功能。

1. 下载词典文件（二选一，推荐第一个）：
   - [`dict.bin`](https://github.com/cere-hy/pico-plus/releases/download/v1.0-gen10/dict.bin)
     ——推荐，约 8.9 MB，12.4 万词，SHA-256 `2bb228e4…9ee7`；
   - [`dict-full.bin`](https://github.com/cere-hy/pico-plus/releases/download/v1.0-gen10/dict-full.bin)
     ——词库更全，约 23 MB，40 万词，SHA-256 `7c17cca7…f61b`；
2. 如果下载的是 `dict-full.bin`，先把它**重命名为 `dict.bin`**；
3. 把 `dict.bin` 复制到 TF 卡的 `dict` 文件夹下，即 **`/sdcard/dict/dict.bin`**
   （没有 `dict` 文件夹就新建一个；设备插卡启动后也会自动创建该文件夹）；
4. TF 卡插回设备并重启，之后阅读时选中单词即可查词，查过的词自动进生词本。

> **命名与位置必须严格一致**：文件名叫 `dict.bin`，放在 `/sdcard/dict/` 目录下，
> 其他名字、其他位置都不会生效；`dict` 文件夹里只放一个词典文件，放多个时打开哪一个不确定。
>
> 词典数据由 [ECDICT](https://github.com/skywind3000/ECDICT)（MIT）转换生成，
> 你也可以用仓库里的 `tools/build_dict.py` 自行构建词典。

## 五、开发者说明

- 开发环境：**ESP-IDF v6.1**；
- 编译：

  ```sh
  idf.py set-target esp32s3
  idf.py build
  ```

- 分区表：`partitions_16M.csv`（app 分区 5.5MB × 2，`otadata` 在 `0xb10000`）；
- 宿主测试：`tools/` 下有 **125 个无需硬件即可运行的测试**（C / Python / JS）
  和 12 个一键运行脚本，CI 每次提交自动执行主要回归；
- 目录速览：
  - `main/` 固件主体——`book/` 文本解析与排版引擎、`dict/` 词典与生词本、`apps/` 各应用；
  - `components/` 硬件与功能组件——`read_pico_transfer/` 为 WiFi 传书组件；
  - `web-flasher/` 网页刷机页（manifest + 固件 bin）；
  - `tools/build_dict.py` 词典构建工具（ECDICT CSV → 设备用 RPD1 `dict.bin`）；
- 内部机制约定（虚拟书、传书策略、波形等）见 `docs/ONBOARDING.md`、`docs/HANDOFF.md` 与 `AGENTS.md`。

## 六、未来规划

本项目在代码层面**保留了完整的局域网文本传输能力**
（`components/read_pico_transfer`：WiFi + HTTP Server），并保留了「虚拟书」打开机制
（`open_book_impl`，生词本即通过它呈现）。

后续计划在此基础上扩展：

- **手机副屏 / 局域网推流**：手机端把文本、网页片段通过局域网推送到设备，
  设备接收后直接以「虚拟书」形式打开阅读，无需经过 TF 卡或 USB 拷贝；
- 推送协议与接收端界面的持续完善。

这些能力的代码位置已预留，将在后续版本中逐步实现。

## 七、免责声明

1. 本固件是第三方修改版本，**按"原样"提供，不作任何明示或默示担保**；
   对于因刷机或使用本固件造成的设备损坏、数据丢失或其他损失，本项目不承担任何责任；
2. 刷机存在固有风险（包括极低概率下变砖、需拆机救砖），请确认了解并愿意承担该风险后再操作；
3. 本项目与 MindReset 官方无关；「RDP-G01-W」等硬件名称的权利归其权利人所有；
4. 固件中的新增原创内容适用[个人学习许可](licenses/kiikoread-Personal-Study-1.0.txt)，
   允许个人学习使用，未经单独书面授权不得商用；上游代码许可（Apache-2.0 等）不受影响，
   完整范围见 [LICENSE](LICENSE) 与 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 八、致谢

- 基于 [MindReset 官方开源示例固件](https://github.com/MindReset/read_pico_firmware) 开发；
- 词典数据来自 [ECDICT](https://github.com/skywind3000/ECDICT)（MIT）；
- 网页刷机基于 [esp-web-tools](https://github.com/espressif/esp-web-tools)（Apache-2.0）；
- 感谢上游项目与所有第三方组件作者，完整许可清单见 `licenses/` 与 `THIRD_PARTY_NOTICES.md`。
