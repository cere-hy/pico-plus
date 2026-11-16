#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# S26-1 摘抄落盘宿主测试（在仓库根目录运行）。/ S26-1 excerpt host tests (run from the repo root).
set -euo pipefail
mkdir -p build/excerpt-tests
flags=(-std=c11 -D_DEFAULT_SOURCE -DEXCERPT_HOST_TEST -Wall -Wextra -Werror -g
       -fsanitize=address,undefined -fno-omit-frame-pointer -Imain/book -Itools/excerpt_stubs)
gcc "${flags[@]}" tools/excerpt_store_host_test.c main/book/excerpt_store.c -o build/excerpt-tests/excerpt
build/excerpt-tests/excerpt

# 26.11.2 P0-5：渲染路径（真 stdio + 临时目录注入，路径宏 #ifndef 可注入）。
# 与上面桩 FS 的写路径测试互不混装：桩测写入逻辑，这一层测卡上文件读回来的样子。
# 26.11.2 P0-5: the render path (real stdio + injected temp dir via the #ifndef macros).
# Deliberately separate from the stub-FS tests above: stubs test the writer, this tests
# what the file on the card looks like when read back.
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Imain/book -Itools/excerpt_stubs \
    -DEXCERPT_DIR='"build/excerpt-tests/fsview"' \
    -DEXCERPT_PATH='"build/excerpt-tests/fsview/excerpts.txt"' \
    -DEXCERPT_VIEW='"build/excerpt-tests/fsview/excerpt_view.txt"' \
    -DEXCERPT_VIEW_TMP='"build/excerpt-tests/fsview/.excerpt_view.tmp"' \
    tools/excerpt_view_host_test.c main/book/excerpt_store.c -o build/excerpt-tests/excerpt-view
build/excerpt-tests/excerpt-view

# S26-3：0x1A bit2 的黄金向量（手推字节 vs 固件编码器）。/ S26-3 golden vectors.
bash "$(dirname "$0")/run_excerpt_ai_golden.sh"

# 26.9.29：IO 契约用例（设备同源 ff.c + RAM 盘）。桩层测逻辑，这一层测桩与真系统之间的契约
# ——路径词汇、失败码、f_open 失败清理语义，桩一辈子测不到。
# 26.9.29: the IO contract tests (device-identical ff.c on a RAM disk). The stub layer tests
# logic; this layer tests the contract the stubs can never see.
bash "$(dirname "$0")/run_excerpt_io_contract.sh"

# 26.11.4：段落解析的切节函数（三态 + 四条边界 + 旧节序）。
# 纯函数、零依赖——输入的字节就是全部上下文，因此这一层不需要桩。
# 26.11.4: the section cutter (three states, four edges, old section order). Pure
# functions with no dependencies: the input bytes are the whole context.
bash "$(dirname "$0")/run_excerpt_section.sh"
