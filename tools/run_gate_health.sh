#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 门禁体检批（2026-11-01 立项，独立前置批）。
#
# 这批检查的**不是产品**，是门禁自己。四次捕获（M124 弱断言 / B41 死牙 / B46-B47 迁移 /
# 31b 第 1 条未注册）全部来自「先红验证」这一个人工动作，而另外四次（宿主桩没跟上新接口）
# 全靠记忆兜底。记忆项连挂四次，就该由脚本接管，而不是第五次记住。
#
#   ① 盘点：tools/ 下每个测试型文件，要么在总入口闭包里，要么在登记簿里写明为什么在外。
#   ② 注册：定义了 check_* 就必须被 main() 可达 —— 31b 那次 `\n` 转义的正面教训。
#   ③ 接口：桩头副本与真实头的同名常量必须相等 —— 宿主跑在与固件不同的数字上，绿也没意义。
#   ④ 事件码：枚举 / 名字表 / 关键级三元必须同时到位 —— 「写了但读不出来」= 没写。
#
# 零产品码：这四个脚本只读源码，不改、不生成、不编译。
cd "$(dirname "$0")/.." || exit 1
set -o pipefail
rc=0

echo "GATE_HEALTH: 只证门禁自身可信，不证产品可用。"

echo
echo "########## H1/4 门禁资产盘点（总入口内 / 外 + 孤儿）##########"
python3 tools/check_gate_inventory.py || rc=1

echo
echo "########## H2/4 注册区自检（定义数 == 调用数）##########"
python3 tools/check_selftest_registry.py || rc=1

echo
echo "########## H3/4 接口副本一致性（桩头 vs 真实头同名常量）##########"
python3 tools/check_api_surface.py || rc=1

echo
echo "########## H4/4 黑匣子事件码三元（枚举 / 名字表 / 关键级）##########"
python3 tools/check_bootbb_codes.py || rc=1

echo
echo "GATE_HEALTH_RC=$rc"
exit $rc
