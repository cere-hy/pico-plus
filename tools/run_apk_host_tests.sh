#!/bin/bash
# APK 宿主测试：只编 AiClient + AiClientTest 并运行（不打包 APK，快）。
# APK host test: compile AiClient + AiClientTest and run (no packaging, fast).
set -e
cd "$(dirname "$0")/.."
SDK="${ANDROID_HOME:-/opt/android-sdk}"
AJ="$SDK/platforms/android-34/android.jar"
[ -f "$AJ" ] || { echo "缺少 $AJ（platforms;android-34）"; exit 1; }
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
javac --release 8 -encoding UTF-8 -nowarn -classpath "$AJ" -d "$OUT" \
    tools/android/apk/src/dev/readpico/push/*.java
javac --release 8 -encoding UTF-8 -nowarn -classpath "$AJ:$OUT" -d "$OUT" \
    tools/android/apk/test/AiClientTest.java
java -cp "$OUT" dev.readpico.push.AiClientTest
