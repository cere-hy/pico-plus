#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 出包四步锁序（2026-11-03 `[122]` 事故后立）：**变异 → 树身份 → 门禁 → 构建打包**。
#
# 为什么要有这个脚本：`[122]` 那次，变异套件的 R26 改完 render.c 没被还原（FILES 漏了它），
# 错误版本留在树上，随后编出一颗**同尺寸、能过尺寸检查**的含 bug bin（acfb621e）。
# 抓它的是 B83，但它本该更早被拦——「跑过变异」与「树是干净的」是两件事，
# 中间那一步没有人负责。清单补全是保险丝（管一个遗漏点），本脚本是**总闸**：
# 每一步之间都重新核对树身份，不依赖任何清单的完备性。
#
# 顺序不能换：
#   变异在构建之前，因为它靠改源码来验判据——跑完必须证明已还原；
#   门禁在构建之前，因为门禁里有若干「只读源码」的断言（B82c 归一化哈希、B83 掩码），
#   它们查的是源码而不是 bin，提前跑能省一次全量编译；
#   构建在最后，且构建前后各核对一次树身份——IDE/工具链偶尔会顺手写回配置文件。
#
# 用法：
#   bash tools/release_gate.sh                 # 变异 + 树身份 + 门禁（不编译）
#   bash tools/release_gate.sh --build         # 再加一步：编译 + 读 bin 的两套身份
#   bash tools/release_gate.sh --build --version 0.3.9-pico26.11.12
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

DO_BUILD=0
EXPECT_VERSION=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build) DO_BUILD=1; shift ;;
        --version) EXPECT_VERSION="${2:-}"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 2 ;;
    esac
done

T0=$(python3 tools/tree_identity.py --quiet) || {
    echo "RELEASE_GATE: 树身份不可得，拒绝开跑（失败关闭）"; exit 2; }
RC=0

echo "release gate: 四步锁序，树身份 T0=${T0:0:16}"
echo

# ---- 1/4 变异套件（内部已带总闸）-------------------------------------------
echo "########## 1/4 变异套件 ##########"
if bash tools/run_refresh_mutations.sh >/tmp/rg_mutations.log 2>&1; then
    echo "step 1 ok (refresh): $(tail -1 /tmp/rg_mutations.log)"
else
    echo "STEP 1 FAILED: 刷新变异有存活/污染，详见 /tmp/rg_mutations.log"
    tail -8 /tmp/rg_mutations.log
    echo "RELEASE_GATE: FAIL (step 1)"; exit 1
fi
# 排版牙独立成套件（套件跟域走，不挤进刷新牙）：先红验证同样在构建之前跑，
# 因为它的靶点全在源码里，跑完必须证明已还原。
if bash tools/run_typography_mutations.sh >/tmp/rg_typo_mutations.log 2>&1; then
    echo "step 1 ok (typography): $(tail -1 /tmp/rg_typo_mutations.log)"
else
    echo "STEP 1 FAILED: 排版变异有存活/污染，详见 /tmp/rg_typo_mutations.log"
    tail -10 /tmp/rg_typo_mutations.log
    echo "RELEASE_GATE: FAIL (step 1 typography)"; exit 1
fi

# 电量差刷牙（26.11.16）：同样先红，四刀全在「值变才刷」的判定行与去抖常数上。
if bash tools/run_nav_watch_mutations.sh >/tmp/rg_navwatch_mutations.log 2>&1; then
    echo "step 1 ok (nav_watch): $(tail -1 /tmp/rg_navwatch_mutations.log)"
else
    echo "STEP 1 FAILED: 电量差刷变异有存活/污染，详见 /tmp/rg_navwatch_mutations.log"
    tail -8 /tmp/rg_navwatch_mutations.log
    echo "RELEASE_GATE: FAIL (step 1 nav_watch)"; exit 1
fi

# 首页手势牙（26.11.17）：五刀先在树上跑，跑完必须证明已还原。
if bash tools/run_home_gesture_mutations.sh >/tmp/rg_home_gesture_mutations.log 2>&1; then
    echo "step 1 ok (home_gesture): $(tail -1 /tmp/rg_home_gesture_mutations.log)"
else
    echo "STEP 1 FAILED: 首页手势变异有存活/污染，详见 /tmp/rg_home_gesture_mutations.log"
    tail -8 /tmp/rg_home_gesture_mutations.log
    echo "RELEASE_GATE: FAIL (step 1 home_gesture)"; exit 1
fi

# 解耦批牙（26.11.19）：宿主刀与守卫刀**分开跑**。混排会让守卫刀在宿主里找不到红、
# 被误记 SURVIVED（批方案 §4.3）——两个文件各自断言红在哪一层。
if python3 tools/run_pmu_bus_host_mutations.py >/tmp/rg_pmu_host_mut.log 2>&1; then
    echo "step 1 ok (pmu_bus host): $(tail -1 /tmp/rg_pmu_host_mut.log)"
else
    echo "STEP 1 FAILED: 取键/采样宿主刀有存活，详见 /tmp/rg_pmu_host_mut.log"
    tail -8 /tmp/rg_pmu_host_mut.log
    echo "RELEASE_GATE: FAIL (step 1 pmu_bus host)"
    exit 1
fi
if python3 tools/run_pmu_bus_guard_mutations.py >/tmp/rg_pmu_guard_mut.log 2>&1; then
    echo "step 1 ok (pmu_bus guard): $(tail -1 /tmp/rg_pmu_guard_mut.log)"
else
    echo "STEP 1 FAILED: 结构守卫自身不红（守卫的守卫），详见 /tmp/rg_pmu_guard_mut.log"
    tail -8 /tmp/rg_pmu_guard_mut.log
    echo "RELEASE_GATE: FAIL (step 1 pmu_bus guard)"
    exit 1
fi

# ---- 2/4 树身份（边界复验，不信上一步的自证）--------------------------------
echo
echo "########## 2/4 树身份 ##########"
if python3 tools/tree_identity.py --expect "$T0" --quiet; then
    echo "step 2 ok: worktree byte-identical to T0 (${T0:0:16})"
else
    echo "STEP 2 FAILED: 变异套件之后工作树与 T0 不同——任何产物都不可送验"
    echo "RELEASE_GATE: FAIL (step 2)"; exit 1
fi

# ---- 3/4 门禁（只读源码，不编译）-------------------------------------------
echo
echo "########## 3/4 门禁 ##########"
GATES=(
    "tools/check_refresh_ghost.py|刷新牙 B79/B80/B82/B83"
    "tools/check_pkg_validity.py|台账与产物一致性"
    "tools/check_ota_manifest.py|OTA清单过设备解析器判据（字段cap/版本/URL白名单/size）"
    "tools/check_hotplug.py|插拔边沿"
    "tools/run_push_sink_host_tests.sh|push sink 宿主用例"
    "tools/ui_refresh_feedback_host_test.py|刷新回执宿主用例"
    "tools/check_typography.py|排版结构牙（折行单源 / 对齐唯一计划 / 断词接线）"
    "tools/check_hyphen_table.py|连字符表可重生成（源 sha256 + 条数 + 逐字节）"
    "tools/check_kern_table.py|kerning 表可重生成（字体 sha256 + 键升序 + 逐字节）"
    "tools/hyphen_host_test.py|连字符行为（金字招牌 + 独立实现差分）"
    "tools/typography_gate1.py|门 1 运行时零差值（容差 0，六档字号）"
    "tools/check_typography_switch.py|排版开关双向等价（off==[122] / on==当前树 / 往返）"
    "tools/run_nav_watch_host_tests.sh|电量差刷真值表（值变才刷 / 2s 去抖 / 不可见不刷）"
    "tools/check_home_gesture.py|首页手势结构牙（接线 / 单源 / 主机零改动）"
    "tools/check_pmu_bus_guard.py|PMU 总线门（9 动点 5/3/1 / 取键自足 / 两戳 / 谓词单源 / PEEK 不消费）"
    "tools/pmu_bus_host_test.py|取键路径寄存器级用例（K2a 零值链读 / K4 / K9.2）"
)
for g in "${GATES[@]}"; do
    cmd="${g%%|*}"; name="${g##*|}"
    if [ "${cmd##*.}" = "sh" ]; then runner="bash"; else runner="python3"; fi
    if $runner "$cmd" >/tmp/rg_gate.log 2>&1; then
        echo "  PASS  $name  ($cmd)"
    else
        echo "  FAIL  $name  ($cmd)"
        tail -8 /tmp/rg_gate.log | sed 's/^/        /'
        RC=1
    fi
done
[ "$RC" -ne 0 ] && { echo "RELEASE_GATE: FAIL (step 3)"; exit 1; }
echo "step 3 ok: ${#GATES[@]}/${#GATES[@]} gates green"

# ---- 4/4 构建（可选） ------------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
    echo
    echo "########## 4/4 构建 ##########"
    # shellcheck disable=SC1091
    source tools/idf_env.sh || { echo "STEP 4 FAILED: tools/idf_env.sh 加载失败"; exit 1; }
    if ! ninja -C build >/tmp/rg_build.log 2>&1; then
        echo "STEP 4 FAILED: 编译失败，详见 /tmp/rg_build.log"
        tail -12 /tmp/rg_build.log
        echo "RELEASE_GATE: FAIL (step 4)"; exit 1
    fi
    echo "build ok: $(tail -1 /tmp/rg_build.log)"
    if [ -n "$EXPECT_VERSION" ]; then
        python3 tools/bin_identity.py build/Read_Pico.bin --expect-version "$EXPECT_VERSION" \
            || { echo "RELEASE_GATE: FAIL (version)"; exit 1; }
    else
        python3 tools/bin_identity.py build/Read_Pico.bin
    fi
    # 构建不许改源码：IDE/工具链偶发写回配置文件，这一步兜住。
    if ! python3 tools/tree_identity.py --expect "$T0" --quiet; then
        echo "STEP 4 FAILED: 构建改动了已跟踪文件"
        echo "RELEASE_GATE: FAIL (step 4 tree)"; exit 1
    fi
    echo "step 4 ok: 产物身份如上；树身份仍为 T0"
fi

echo
echo "RELEASE_GATE: PASS"
