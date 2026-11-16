#!/usr/bin/env python3
"""统一照片亮度尺：同一批照片，同一把尺子。

用途
----
判「静置灰化」时不靠肉眼：每张照片算同一个量——
**正文白底 / 顶栏白底** 的亮度比（`ratio`），以及正文对比度与字缘锐度。

为什么用比值而不是绝对亮度
--------------------------
拍照时的曝光、灯位、距离都会变，绝对灰度不可比。顶栏不在正文刷新区
（`s_area` 不含顶栏），它的白底就是「这块屏此刻的白」。正文白底相对它的
比值只受面板状态影响，不受曝光影响——这是跨照片可比的唯一口径。

三个量
------
ratio     正文白底 p85 / 顶栏白底 p85。灰化时正文暗、顶栏不暗 → 比值下降。
contrast  (正文白底 p85 − 字芯 p03) / 正文白底 p85。字深不深看它。
edge      正文区梯度 p99.5 / 正文白底。字缘锐不锐看它（x0 黑度检查用）。

用法
----
    python3 tools/measure_photo_contrast.py 照片... [选项]

    --screen L,T,R,B   手动指定屏幕框（0–1 比例）。**推荐**：同一机位拍的一批照片
                       共用同一个框，跨照片才真正同尺；自动检测只当参考
    --debug-dir 目录   为每张图输出带框缩略图，肉眼核一次框对不对
    --csv  out.csv     写 CSV
    --label 名字=路径  指定标签（默认取文件名）
    --white-thr 0.975  ratio ≥ 此值判「白」
    --gray-thr  0.955  ratio <  此值判「灰」，中间为「灰边/待定」

输出每个文件一行；退出码恒 0（这是测量工具，不是门禁）。
"""
import argparse
import csv
import pathlib
import sys
from typing import List, Optional, Tuple

import numpy as np
from PIL import Image, ImageOps


def _gray(path: pathlib.Path) -> np.ndarray:
    im = Image.open(path)
    im = ImageOps.exif_transpose(im).convert("RGB")
    a = np.asarray(im, dtype=np.float32)
    # Rec.601，与常见的手机/OpenCV 灰度一致
    return 0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]


def _otsu(g: np.ndarray) -> float:
    hist, edges = np.histogram(g, bins=256, range=(0.0, 255.0))
    hist = hist.astype(np.float64)
    total = hist.sum()
    if total <= 0:
        return 128.0
    p = hist / total
    omega = np.cumsum(p)
    mu = np.cumsum(p * (edges[:-1] + 0.5))
    mu_t = mu[-1]
    denom = omega * (1.0 - omega)
    denom[denom <= 1e-12] = 1e-12
    sigma = (mu_t * omega - mu) ** 2 / denom
    return float(edges[int(np.argmax(sigma))] + 0.5)


def _longest_run(mask: np.ndarray, min_frac: float) -> Optional[Tuple[int, int]]:
    """mask 为 1D 布尔；返回最长连续 True 区间（长度需 ≥ min_frac * len）。"""
    best = None
    start = None
    for i, v in enumerate(mask):
        if v and start is None:
            start = i
        elif not v and start is not None:
            if best is None or i - start > best[1] - best[0]:
                best = (start, i)
            start = None
    if start is not None and (best is None or len(mask) - start > best[1] - best[0]):
        best = (start, len(mask))
    if best and (best[1] - best[0]) >= min_frac * len(mask):
        return best
    return None


def detect_screen(g: np.ndarray) -> Optional[Tuple[float, float, float, float]]:
    """返回 (l, t, r, b) 比例。用行列亮像素投影找那块「大面积高亮矩形」。"""
    thr = _otsu(g)
    mask = g > thr
    col = mask.mean(axis=0)
    row = mask.mean(axis=1)
    cx = _longest_run(col >= 0.30, 0.30)
    ry = _longest_run(row >= 0.30, 0.30)
    if not cx or not ry:
        return None
    h, w = g.shape
    l, r = cx[0] / w, cx[1] / w
    t, b = ry[0] / h, ry[1] / h
    # 太小的框视为误检（屏幕至少占画面 25% 宽、20% 高）
    if (r - l) < 0.25 or (b - t) < 0.20:
        return None
    return (l, t, r, b)


def measure(g: np.ndarray, box: Tuple[float, float, float, float]) -> dict:
    h, w = g.shape
    l, t, r, b = box
    x0, x1 = int(l * w), int(r * w)
    y0, y1 = int(t * h), int(b * h)
    sw, sh = x1 - x0, y1 - y0

    def crop(fx0, fy0, fx1, fy1):
        return g[y0 + int(fy0 * sh): y0 + int(fy1 * sh),
                 x0 + int(fx0 * sw): x0 + int(fx1 * sw)]

    header = crop(0.04, 0.005, 0.96, 0.075)
    body = crop(0.08, 0.16, 0.92, 0.86)
    if header.size == 0 or body.size == 0:
        raise ValueError("screen box too small")

    hw = float(np.percentile(header, 85))
    bw = float(np.percentile(body, 85))
    ink = float(np.percentile(body, 3))
    ratio = bw / hw if hw > 1.0 else float("nan")
    contrast = (bw - ink) / bw if bw > 1.0 else float("nan")

    gy, gx = np.gradient(body)
    grad = np.hypot(gx, gy)
    edge = float(np.percentile(grad, 99.5)) / bw if bw > 1.0 else float("nan")

    return {
        "screen_l": round(l, 4), "screen_t": round(t, 4),
        "screen_r": round(r, 4), "screen_b": round(b, 4),
        "header_white": round(hw, 2), "body_white": round(bw, 2),
        "ink": round(ink, 2), "ratio": round(ratio, 4),
        "contrast": round(contrast, 4), "edge": round(edge, 4),
    }


def verdict(ratio: float, white_thr: float, gray_thr: float) -> str:
    if not np.isfinite(ratio):
        return "?"
    if ratio >= white_thr:
        return "白"
    if ratio < gray_thr:
        return "灰"
    return "灰边"


def _write_debug(path: pathlib.Path, box, out: pathlib.Path) -> None:
    from PIL import ImageDraw
    out.parent.mkdir(parents=True, exist_ok=True)
    im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    w, h = im.size
    l, t, r, b = box
    d = ImageDraw.Draw(im)
    lw = max(3, w // 250)
    d.rectangle([l * w, t * h, r * w, b * h], outline=(255, 0, 0), width=lw)
    sw, sh = (r - l) * w, (b - t) * h
    x0, y0 = l * w, t * h
    d.rectangle([x0 + 0.04 * sw, y0 + 0.005 * sh, x0 + 0.96 * sw, y0 + 0.075 * sh],
                outline=(0, 160, 255), width=lw)
    d.rectangle([x0 + 0.08 * sw, y0 + 0.16 * sh, x0 + 0.92 * sw, y0 + 0.86 * sh],
                outline=(0, 255, 0), width=lw)
    im.thumbnail((800, 800))
    im.save(out)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="统一照片亮度尺（正文/顶栏）")
    ap.add_argument("photos", nargs="+")
    ap.add_argument("--screen", help="L,T,R,B（0–1 比例），跳过自动检测")
    ap.add_argument("--debug-dir", help="把带框缩略图写到该目录，用于人工核对")
    ap.add_argument("--csv")
    ap.add_argument("--label", action="append", default=[],
                    help="名字=路径，可重复；未给标签的用文件名")
    ap.add_argument("--white-thr", type=float, default=0.975)
    ap.add_argument("--gray-thr", type=float, default=0.955)
    ap.add_argument("--baseline", help="以该 label 为白基准，额外输出 rel = ratio / 基准 ratio")
    args = ap.parse_args(argv)

    labels = {}
    for item in args.label:
        if "=" in item:
            k, v = item.split("=", 1)
            labels[str(pathlib.Path(v).resolve())] = k

    manual = None
    if args.screen:
        parts = [float(x) for x in args.screen.split(",")]
        if len(parts) != 4:
            print("--screen 需要四个数 L,T,R,B", file=sys.stderr)
            return 2
        manual = tuple(parts)  # type: ignore[assignment]

    rows = []
    for p in args.photos:
        path = pathlib.Path(p).resolve()
        if not path.exists():
            print(f"跳过（不存在）：{p}", file=sys.stderr)
            continue
        g = _gray(path)
        box = manual or detect_screen(g)
        auto = box is not None
        if box is None:
            print(f"警告：{path.name} 自动找屏失败，退化为全图——请用 --screen 指定", file=sys.stderr)
            box = (0.0, 0.0, 1.0, 1.0)
        elif not manual and (box[2] - box[0]) < 0.45:
            print(f"警告：{path.name} 自动框偏窄（宽 {(box[2]-box[0]):.2f}，可能是拼接图或多物），"
                  f"建议 --screen 手填并用 --debug-dir 核对", file=sys.stderr)
        try:
            m = measure(g, box)
        except ValueError as e:
            print(f"跳过（{e}）：{p}", file=sys.stderr)
            continue
        m["file"] = path.name
        m["label"] = labels.get(str(path), path.stem)
        m["box"] = "manual" if manual else ("auto" if auto else "full-fallback")
        m["verdict"] = verdict(m["ratio"], args.white_thr, args.gray_thr)
        if args.debug_dir:
            _write_debug(path, box, pathlib.Path(args.debug_dir) / (path.stem + "-box.png"))
        rows.append(m)

    if not rows:
        print("没有可测的照片。", file=sys.stderr)
        return 1

    cols = ["label", "ratio", "rel", "verdict", "contrast", "edge", "body_white",
            "header_white", "ink", "box", "file"]
    base = None
    if args.baseline:
        for r in rows:
            if r["label"] == args.baseline:
                base = r["ratio"]
                break
        if base is None or not np.isfinite(base) or base <= 0:
            print(f"警告：找不到基准 label「{args.baseline}」，rel 留空", file=sys.stderr)
            base = None
    for r in rows:
        if base:
            r["rel"] = r["ratio"] / base
            r["verdict"] = ("白" if r["rel"] >= 0.985 else
                            ("灰" if r["rel"] < 0.965 else "灰边"))
        else:
            r["rel"] = ""

    print(f"{'label':<18} {'ratio':>7} {'rel':>7} {'判定':<4} {'对比度':>7} {'锐度':>7} "
          f"{'正文':>6} {'顶栏':>6} {'字芯':>6}  box")
    print("-" * 96)
    for r in sorted(rows, key=lambda x: -x["ratio"]):
        rel = f"{r['rel']:.4f}" if isinstance(r["rel"], float) else "-"
        print(f"{r['label']:<18} {r['ratio']:>7.4f} {rel:>7} {r['verdict']:<6} "
              f"{r['contrast']:>7.4f} {r['edge']:>7.4f} "
              f"{r['body_white']:>6.1f} {r['header_white']:>6.1f} {r['ink']:>6.1f}  {r['box']}")
    print("-" * 96)
    rr = [r["ratio"] for r in rows if np.isfinite(r["ratio"])]
    if rr:
        print(f"n={len(rr)}  最大 {max(rr):.4f}  最小 {min(rr):.4f}  极差 {max(rr)-min(rr):.4f}")
    if base:
        print(f"基准「{args.baseline}」ratio={base:.4f}；rel≥0.985 白，<0.965 灰。")
    else:
        print(f"判读：ratio≥{args.white_thr:.3f} 白；<{args.gray_thr:.3f} 灰；中间灰边。"
              f"跨照片只比同一批——曝光变了绝对值就不可比。")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=cols + ["screen_l", "screen_t", "screen_r", "screen_b"])
            w.writeheader()
            for r in rows:
                w.writerow({k: r.get(k, "") for k in w.fieldnames})
        print(f"CSV: {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
