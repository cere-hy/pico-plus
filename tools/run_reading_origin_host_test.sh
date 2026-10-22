#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 首页续读命中/推流卡 deep-link 回归（gen24.3 纳管：此前不在任何 run_*.sh
# 门禁内，gen18 起静默腐坏）。/ Home continue-card hit / push-card
# deep-link regression (brought under the gate in gen24.3: it sat outside
# every run_*.sh and had silently rotted since gen18).
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/reading_origin_host_test.py
