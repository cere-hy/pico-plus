#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""树身份：工作树里**已跟踪文件**的字节级指纹，用于「变异套件自己有没有污染工作树」。

为什么要有这个工具（2026-11-03 `[122]` 事故）：
    变异脚本 R26 改了 `components/epdiy/src/render.c`，但 `FILES` 清单漏了它，
    `restore` 覆盖不到 → 错误版本留在树上，随后编出一颗**同尺寸、能过尺寸检查**的
    含 bug 候选 bin（`acfb621e`）。抓住它的是 B83，不是清单，也不是人眼。
    `FILES` 补全是**保险丝**（管住那一个遗漏点），本工具是**总闸**：不依赖任何清单
    的完备性——清单遗漏、restore 失败、套件半途崩溃，全收进「跑前 == 跑后」一个检查。

口径（写死，免得再漂）：
    对 `git ls-files` 列出的每个已跟踪文件，按路径排序后逐个喂
    `路径 + \\0 + 长度 + 内容` 进 SHA-256。只认**工作树的内容**，不认 git 状态文本，
    也不认未跟踪文件（构建产物目录天天在变，纳进来只会制造假红）。
    与 `git diff` 的分工：内容哈希是判据（字节级），`git status -uno` 只是差异的说明书。

用法：
    python3 tools/tree_identity.py                    # 打印树身份
    python3 tools/tree_identity.py --expect <hash>    # 比对，不一致即 exit 1
    python3 tools/tree_identity.py --json             # 机读
非 git 仓库一律 exit 2（**失败关闭**）：宁可不跑，也不给一个「看起来通过」的假绿。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys


def _git(*args: str) -> bytes:
    r = subprocess.run(["git", *args], capture_output=True)
    if r.returncode != 0:
        raise SystemExit("tree_identity: git %s failed: %s"
                         % (" ".join(args), r.stderr.decode("utf-8", "replace").strip()))
    return r.stdout


def identity() -> tuple[str, int]:
    raw = _git("ls-files", "-z")
    files = sorted(p for p in raw.split(b"\0") if p)
    h = hashlib.sha256()
    h.update(b"tree-identity-v1\0")
    n = 0
    for rel in files:
        p = pathlib.Path(rel.decode("utf-8", "surrogateescape"))
        if not p.is_file():
            # 已跟踪但工作树里没有（被删）：用哨兵值参与哈希，删除本身也是一种变化。
            h.update(rel + b"\0missing\0")
            n += 1
            continue
        data = p.read_bytes()
        h.update(rel + b"\0")
        h.update(str(len(data)).encode() + b"\0")
        h.update(data)
        n += 1
    return h.hexdigest(), n


def dirty_summary(limit: int = 40) -> str:
    """差异说明书：只报已跟踪文件（-uno），最多 limit 行。"""
    out = subprocess.run(["git", "status", "--porcelain", "-uno"],
                         capture_output=True, text=True).stdout.strip()
    if not out:
        return "(已跟踪文件无改动)"
    lines = out.splitlines()
    body = "\n".join("  " + l for l in lines[:limit])
    if len(lines) > limit:
        body += "\n  … 另有 %d 行" % (len(lines) - limit)
    return body


def main() -> int:
    ap = argparse.ArgumentParser(description="已跟踪文件的字节级树身份")
    ap.add_argument("--expect", help="期望的树身份；不一致则 exit 1")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true", help="只打印哈希")
    args = ap.parse_args()

    if subprocess.run(["git", "rev-parse", "--is-inside-work-tree"],
                      capture_output=True).returncode != 0:
        print("tree_identity: 不是 git 仓库，树身份不可得（失败关闭）", file=sys.stderr)
        return 2

    got, n = identity()
    if args.json:
        print(json.dumps({"tree": got, "files": n, "expect": args.expect,
                          "match": (args.expect is None or got == args.expect)},
                         ensure_ascii=False))
    elif args.quiet:
        # --expect --quiet = 纯判据模式，成功时一声不响（调用方自己打印一行）。
        if args.expect is None:
            print(got)
    else:
        print("tree identity: %s  (tracked=%d)" % (got[:16], n))

    if args.expect is not None and got != args.expect:
        print("TREE CHANGED: 跑前 %s → 跑后 %s" % (args.expect[:16], got[:16]))
        print("已跟踪文件改动如下（`git status --porcelain -uno`）：")
        print(dirty_summary())
        print("含义：变异套件/门禁没有把工作树还原，或有人在中途改了源码。"
              "此时**任何构建产物都不可作为送验对象**。")
        return 1

    if args.expect is not None and not args.quiet and not args.json:
        print("tree identity unchanged: %s" % got[:16])
    return 0


if __name__ == "__main__":
    sys.exit(main())
