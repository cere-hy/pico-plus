#!/usr/bin/env bash
# FNV-1a32 book_id 三端（C / Java / Python）向量对表编排 + 全等断言。
# 约定：在仓库根目录或任意位置执行均可，脚本内部按相对路径定位三端源文件。
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"          # .../read-pico-github
ANDROID="$REPO_ROOT/tools/android"
PUSH_PROTO_SRC="$ANDROID/apk/src/dev/readpico/push/PushProto.java"
BOOK_DIR="$REPO_ROOT/main/book"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

C_OUT="$TMP/c.out"; PY_OUT="$TMP/py.out"; J_OUT="$TMP/java.out"
JCLS="$TMP/classes"

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- 1. C 端 ----
echo "== C 端 =="
gcc -I "$BOOK_DIR" -o "$TMP/tri_c" "$HERE/tri_c.c" "$BOOK_DIR/book_id.c" \
  || fail "C 编译失败（gcc + main/book/book_id.c）"
"$TMP/tri_c" > "$C_OUT" || fail "C 运行失败"
cat "$C_OUT"

# ---- 2. Python 端 ----
echo "== Python 端 =="
python3 "$HERE/tri_py.py" > "$PY_OUT" || fail "Python 运行失败（import tools/android/push_pico.py）"
cat "$PY_OUT"

# ---- 3. Java 端 ----
echo "== Java 端 =="
[ -f "$PUSH_PROTO_SRC" ] || fail "PushProto.java 未找到: $PUSH_PROTO_SRC"
mkdir -p "$JCLS"
javac -d "$JCLS" "$PUSH_PROTO_SRC" "$HERE/TriVec.java" \
  || fail "Java 编译失败（PushProto + TriVec）"
java -cp "$JCLS" TriVec > "$J_OUT" || fail "Java 运行失败"
cat "$J_OUT"

# ---- 4. 三端全等断言 ----
echo "== 三端全等断言 =="
if diff -q "$C_OUT" "$PY_OUT" >/dev/null && diff -q "$C_OUT" "$J_OUT" >/dev/null; then
  echo "PASS: C / Java / Python 对同一组 9 文件名 book_id 哈希逐字节全等"
  exit 0
else
  echo "FAIL: 三端输出不一致" >&2
  echo "--- C vs Python ---"; diff "$C_OUT" "$PY_OUT"
  echo "--- C vs Java ---";   diff "$C_OUT" "$J_OUT"
  exit 1
fi
