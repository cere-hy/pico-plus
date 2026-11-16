#!/usr/bin/env python3
"""B66 / B68 负向自检：逐个靶点改坏源码，牙必须红；全红才算这份牙活着。

B66 咬设备侧「整段上行」的四条形状（拒发门序 / 缓冲同源 / 发即 arm / 形态判在前），
B68 咬**跨端三点一致**（设备宏、APK 常量与分派、段落提示词）。八个靶点全是真机上
已经摔过或一改就会重演的形状，不是假想。

用法：python3 tools/selftest_b66.py
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BOOK = ROOT / "main/apps/app_book.c"
JPROTO = ROOT / "tools/android/apk/src/dev/readpico/push/PushProto.java"
JMAIN = ROOT / "tools/android/apk/src/dev/readpico/push/MainActivity.java"
JAI = ROOT / "tools/android/apk/src/dev/readpico/push/AiClient.java"

# (名字, 文件, 原文, 替换成) —— 原文必须**恰好出现一次**，否则自检本身失败（防靶点漂移）
TARGETS = [
    ("拒发门被搬到复制之后（B66）", BOOK,
     "    if (len > PUSH_SCENE_CTX_MAX) {",
     "    (void)box_select_copy_text(&s_boxsel, s_text, s_text_len, text, sizeof(s_scene_ctx));\n"
     "    if (len > PUSH_SCENE_CTX_MAX) {"),
    ("上下文退回 768 B 框选缓冲（B66）", BOOK,
     "text,\n                                          sizeof(s_scene_ctx));",
     "text,\n                                          sizeof(s_boxsel_text));"),
    ("发出去不 arm（B66）", BOOK,
     "scene_req_arm_excerpt(&s_scene_req", "scene_req_noop(&s_scene_req"),
    ("形态判被删（B66）", BOOK,
     "if (n <= BOXSEL_WORD_MAX && !boxsel_has_space(text, n)) {", "if (0) {"),
    ("APK bit2 常量被删（B68）", JPROTO,
     "public static final int SCENE_FLAG_EXCERPT = 1 << 2;",
     "public static final int SCENE_FLAG_EXCERPT_GONE = 1 << 2;"),
    ("APK 位移改成 1<<3（B68 两端不一致）", JPROTO,
     "public static final int SCENE_FLAG_EXCERPT = 1 << 2;",
     "public static final int SCENE_FLAG_EXCERPT = 1 << 3;"),
    ("APK 分派分支被删（B68）", JMAIN,
     "if (scene.excerpt()) {", "if (false) {"),
    ("段落提示词混入「该词」（B68）", JAI,
     "【段意】这段在说什么", "【段意】该词这段在说什么"),
]


def run_gate():
    return subprocess.run([sys.executable, "tools/check_excerpt.py"], cwd=ROOT,
                          capture_output=True, text=True)


def main():
    paths = {BOOK, JPROTO, JMAIN, JAI}
    saved = {p: p.read_text() for p in paths}
    survived = 0
    try:
        for name, path, old, new in TARGETS:
            src = saved[path]
            count = src.count(old)
            if count != 1:
                print("SELFTEST-BROKEN: %s (anchor matched %d times)" % (name, count))
                return 2
            path.write_text(src.replace(old, new))
            r = run_gate()
            path.write_text(src)
            if r.returncode == 0:
                print("SURVIVED: %s  <- 牙没咬住" % name)
                survived += 1
            else:
                first = next((ln for ln in r.stdout.splitlines()
                              if ln.startswith("FAIL") and ("B66" in ln or "B68" in ln)), "")
                if not first:
                    first = next((ln for ln in r.stdout.splitlines() if ln.startswith("FAIL")), "")
                print("killed  : %s  | %s" % (name, first[:110]))
    finally:
        for p, text in saved.items():
            p.write_text(text)
    total = len(TARGETS)
    print("\nSELF-TEST %s: %d/%d targets killed" %
          ("PASS" if survived == 0 else "FAIL", total - survived, total))
    return 0 if survived == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
