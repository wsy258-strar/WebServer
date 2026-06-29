#!/usr/bin/env bash
# =============================================================================
# 长连接压测 — wrk (HTTP/1.1 keep-alive)
# 用法: ./benchmark/long_conn.sh [target_path]
# =============================================================================
set -uo pipefail

TARGET="${1:-/js/app.js}"
URL="http://localhost:8080${TARGET}"
THREADS=4
DURATION="15s"
LEVELS=(10 50 100 200 500)
OUTDIR="benchmark/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOGFILE="${OUTDIR}/long_conn_${TIMESTAMP}.log"

mkdir -p "$OUTDIR"

# 检查依赖
for tool in curl wrk; do
    command -v "$tool" &>/dev/null || { echo "请安装 $tool"; exit 1; }
done

# 检查服务器
curl -s -o /dev/null "$URL" || { echo "服务器未响应 $URL"; exit 1; }

# 预热
echo "预热:"
for i in $(seq 1 10); do curl -s -o /dev/null "$URL"; done
echo ""

# 写入日志头
{
    echo "# 长连接压测 (wrk) | $(date '+%Y-%m-%d %H:%M:%S')"
    echo "## 目标: $URL"
    echo "## 工具: wrk | 线程: $THREADS | 持续时间: ${DURATION} | 协议: HTTP/1.1 keep-alive"
    echo ""
    echo "| 并发 | QPS | P50 | P75 | P90 | P99 | Avg | 错误 |"
    echo "|------|-----|-----|-----|-----|-----|-----|------|"
} > "$LOGFILE"

echo "长连接压测开始 (5 梯度 × ${DURATION})"
echo "==============================================="

for c in "${LEVELS[@]}"; do
    echo -n "并发=$c ... "
    output=$(wrk -t"$THREADS" -c"$c" -d"$DURATION" --latency "$URL" 2>&1) || true

    qps=$(echo "$output"  | grep "Requests/sec:"    | awk '{printf "%.0f", $2}')
    p50=$(echo "$output"  | grep -E "^\s+50%"       | awk '{print $2}')
    p75=$(echo "$output"  | grep -E "^\s+75%"       | awk '{print $2}')
    p90=$(echo "$output"  | grep -E "^\s+90%"       | awk '{print $2}')
    p99=$(echo "$output"  | grep -E "^\s+99%"       | awk '{print $2}')
    avg=$(echo "$output"  | grep -E "^\s+Latency"   | head -1 | awk '{print $2}')  # wrk 自带单位 us/ms
    err=$(echo "$output"  | grep "Non-2xx or 3xx"   | awk '{print $NF}')

    echo "  QPS=${qps:-0}, P50=${p50:-N/A}, P99=${p99:-N/A}, Avg=${avg:-N/A}"

    printf "| %-4s | %-6s | %-5s | %-5s | %-5s | %-5s | %-5s | %-4s |\n" \
        "$c" "${qps:-0}" "${p50:-N/A}" "${p75:-N/A}" "${p90:-N/A}" "${p99:-N/A}" "${avg:-N/A}" "${err:-0}" \
        >> "$LOGFILE"

    sleep 2
done

echo ""
echo "报告: $LOGFILE"
cat "$LOGFILE"
