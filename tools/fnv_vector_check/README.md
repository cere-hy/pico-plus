# FNV-1a32 book_id 三端向量对表（守门证据链）

> **用途**：`book_id`（FNV-1a32）是七场景①「实时语境查询」的跨端契约键——设备侧写入
> 0x17/0x06 上下文帧的 `book_id` 必须与手机侧重算的 `book_id` 完全相等，否则语境查询
> 静默退化（且只有真机现形）。本目录把 C / Java / Python 三端的对表运行器**收进仓库**，
> 作为永久证据链：任何改动 `main/book/book_id.c`、`tools/android/push_pico.py` 或
> `tools/android/apk/.../PushProto.java` 后，跑一次即可确认三端仍逐字节全等。
>
> 口径（三端同源，已冻结）：FNV-1a32（basis `0x811C9DC5` / prime `0x01000193`）作用于
> 文件名 **basename**（最后一个 `/` 之后）的 **UTF-8 原始字节**；去最后一个扩展名（`.`
> 为首字符的隐藏文件不切）；大小写不折叠、不做 Unicode 归一化；空路径返回 `0`、哈希 `0`
> 保底 `1`。

## 运行

```bash
# 在仓库根目录执行（脚本内部按相对路径定位三端源文件）
bash tools/fnv_vector_check/run_checks.sh
```

脚本会：
1. 编译并运行 C 端（`tri_c.c` + `main/book/book_id.c`）；
2. 运行 Python 端（`tri_py.py` import `tools/android/push_pico.py`）；
3. 编译 `PushProto.java` 并运行 Java 端（`TriVec.java`）；
4. 对三端输出的 9 行哈希做**逐字节 diff**，全等则 `PASS` 并退出 0，否则 `FAIL` 并打印差异、退出 1。

## 9 个对表样本（三端同一套、同一顺序）

| # | 文件名 | 类别 |
|---|--------|------|
| 1 | `/books/Hold On to Your Kids.epub` | 带空格 |
| 2 | `/books/战争与和平.txt` | 中文书名 |
| 3 | `/books/The.Great.Gatsby.epub` | 多点文件名 |
| 4 | `/books/note.tar.gz` | 多扩展名 |
| 5 | `/books/README` | 无扩展名 |
| 6 | `/books/UPPER.EPUB` | 大写（与 7 仅大小写不同，证明不折叠） |
| 7 | `/books/upper.epub` | 小写 |
| 8 | `/books/.hidden` | 隐藏文件（不切扩展名） |
| 9 | `/sd/books/中文 书名.epub` | 中文+空格+深层目录 |

> 当前（gen26.5）三端全等向量表见交付文档 `book_id_FNV_三端向量对表.md`（/workspace 下）。

## 文件

- `tri_c.c` — C 端 runner，编译时链 `main/book/book_id.c`
- `tri_py.py` — Python 端 runner，import `tools/android/push_pico.py`
- `TriVec.java` — Java 端 runner，复用 `dev.readpico.push.PushProto.bookIdFromPath`
- `run_checks.sh` — 三端编排 + 全等断言
