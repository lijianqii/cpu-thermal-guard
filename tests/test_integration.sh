#!/bin/bash
# test_integration.sh - 集成测试: mock 温度 + mock 限幅器 + ctlguard
#
# 在无 thermal zone / 无 cpufreq 的虚拟机上完整测试调度器行为。
# 通过 --mock-temp 从文件读温度，--mock-limit 内存模拟限幅器，
# --dry-run 不写 sysfs，ctlguard get 验证状态。
#
# 用法: ./tests/test_integration.sh

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/cpu_thermal_guard"
CTL="$ROOT/ctlguard"
SOCK="/tmp/ctg-test-$$.sock"
MOCK="/tmp/ctg-mock-$$.temp"
LOG="/tmp/ctg-test-$$.log"
PASS=0
FAIL=0

# 使用 1s 间隔，每次 sleep 1.1s 确保恰好一轮轮询
INTERVAL=1000
STEP=200

cleanup() {
    rm -f "$MOCK" "$LOG" "$SOCK"
}
trap cleanup EXIT

check() {
    local desc="$1" cond="$2"
    if eval "$cond"; then
        PASS=$((PASS+1))
        echo "  PASS: $desc"
    else
        FAIL=$((FAIL+1))
        echo "  FAIL: $desc"
    fi
}

get_state() {
    "$CTL" -s "$SOCK" get 2>/dev/null
}

wait_daemon() {
    local n=0
    while [ $n -lt 50 ]; do
        if "$CTL" -s "$SOCK" get >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        n=$((n+1))
    done
    return 1
}

echo "=== Integration Test: Adaptive Scheduler ==="
echo ""

# 启动守护进程 (mock 温度 + mock 限幅器 + dry-run + verbose)
echo 40000 > "$MOCK"
"$BIN" -v --mock-temp "$MOCK" --mock-limit --dry-run \
    -S "$SOCK" -H 80 -L 72 -s $STEP -i $INTERVAL \
    --warm-margin 3 --critical-margin 8 --cool-confirm 3 \
    > "$LOG" 2>&1 &
DAEMON_PID=$!

if ! wait_daemon; then
    echo "ERROR: daemon failed to start"
    cat "$LOG"
    exit 1
fi

echo "Daemon started (PID=$DAEMON_PID)"
echo ""

# 等待初始稳定
echo 60000 > "$MOCK"
sleep 1.2

# ---- 测试 1: COOL 区间保持 ----
echo "--- Test 1: COOL zone (75C) should hold ---"
echo 75000 > "$MOCK"
sleep 1.2
STATE=$(get_state)
check "action is 保持" "echo '$STATE' | grep -q 'action=保持'"
check "limit at ceiling" "echo '$STATE' | grep -q '2800 MHz'"

# ---- 测试 2: HOT 区间收紧 ----
echo "--- Test 2: HOT zone (82C) should tighten ---"
echo 82000 > "$MOCK"
sleep 1.2
STATE=$(get_state)
check "action contains 收紧" "echo '$STATE' | grep -q 'action=收紧'"
LIMIT_MHZ=$(echo "$STATE" | grep '^limit=' | grep -o 'max_freq=[0-9]*' | grep -o '[0-9]*')
echo "  Current limit: ${LIMIT_MHZ} MHz"
check "limit decreased" "[ '${LIMIT_MHZ:-0}' -lt 2800 ]"

# ---- 测试 3: HOT 更高温更大步长 ----
echo "--- Test 3: HOT zone (86C) larger step ---"
PREV=$LIMIT_MHZ
echo 86000 > "$MOCK"
sleep 1.2
STATE=$(get_state)
LIMIT_MHZ=$(echo "$STATE" | grep '^limit=' | grep -o 'max_freq=[0-9]*' | grep -o '[0-9]*')
echo "  Current limit: ${LIMIT_MHZ} MHz"
check "limit decreased further" "[ '${LIMIT_MHZ:-0}' -lt '${PREV:-2800}' ]"

# ---- 测试 4: CRITICAL 跳到地板 ----
echo "--- Test 4: CRITICAL zone (90C) should force floor ---"
echo 90000 > "$MOCK"
sleep 1.2
STATE=$(get_state)
check "action contains 紧急" "echo '$STATE' | grep -q 'action=紧急'"
check "limit at floor (400)" "echo '$STATE' | grep -q 'max_freq=400'"

# ---- 测试 5: COLD 需要确认 (cool_confirm=3) ----
echo "--- Test 5: COLD zone (50C) needs 3 confirm rounds ---"
echo 50000 > "$MOCK"
# 轮 1
sleep 1.2
STATE=$(get_state)
check "round 1: still at floor" "echo '$STATE' | grep -q 'max_freq=400'"
# 轮 2
sleep 1.2
STATE=$(get_state)
check "round 2: still at floor" "echo '$STATE' | grep -q 'max_freq=400'"
# 轮 3: 应该放宽了
sleep 1.2
STATE=$(get_state)
check "round 3: relaxed above floor" "echo '$STATE' | grep -vq 'max_freq=400'"
echo "  $(echo "$STATE" | grep '^limit=')"

# ---- 测试 6: 完全恢复到 ceiling ----
echo "--- Test 6: Full recovery to ceiling ---"
echo 50000 > "$MOCK"
# step=200, floor=400, ceiling=2800 -> 需要 12 步恢复
sleep 15
STATE=$(get_state)
check "fully recovered to 2800" "echo '$STATE' | grep -q 'max_freq=2800'"

# ---- 测试 7: ctlguard SET 修改参数 ----
echo "--- Test 7: ctlguard SET warm-margin ---"
"$CTL" -s "$SOCK" set warm-margin 5 >/dev/null 2>&1
STATE=$(get_state)
check "warm-margin updated to 5" "echo '$STATE' | grep -q 'warm-margin=5'"

# ---- 测试 8: WARM 预警 (快速上升) ----
echo "--- Test 8: WARM zone with fast rise (preemptive tighten) ---"
# 先确保在 ceiling
echo 50000 > "$MOCK"
sleep 15
STATE=$(get_state)
check "at ceiling before rise" "echo '$STATE' | grep -q 'max_freq=2800'"
# 快速上升到 WARM 区 (78C)
echo 78000 > "$MOCK"
sleep 1.2
STATE=$(get_state)
check "preemptive tighten triggered" "echo '$STATE' | grep -vq 'max_freq=2800'"
echo "  $(echo "$STATE" | grep '^limit=')"

# ---- 清理 ----
kill "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
