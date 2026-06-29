#!/usr/bin/env bash
# =============================================================================
# 短连接压测 — ab (HTTP/1.0, 每请求新 TCP 连接)
# 用法: ./benchmark/short_conn.sh [target_path]
# =============================================================================
set -uo pipefail

TARGET="${1:-/js/app.js}"
URL="http://localhost:8080${TARGET}"
BASE="http://localhost:8080/"
REQUESTS=5000
LEVELS=(10 50 100 200 500)
OUTDIR="benchmark/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOGFILE="${OUTDIR}/short_conn_${TIMESTAMP}.log"
SERVER_BIN="bin/main"

mkdir -p "$OUTDIR"

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

# 检查依赖
for tool in curl ab; do
    command -v "$tool" &>/dev/null || { echo "请安装 $tool (sudo apt install apache2-utils)"; exit 1; }
done

# -------------------- 工具函数 --------------------
check_server() {
    curl -s -o /dev/null --max-time 2 "$BASE" 2>/dev/null && return 0 || return 1
}

start_server() {
    echo "  重启服务器..."
    pkill -f "bin/main" 2>/dev/null || true
    sleep 2
    "$SERVER_BIN" > /tmp/webserver_bench.log 2>&1 &
    # 等待就绪
    for i in $(seq 1 20); do
        sleep 0.5
        check_server && { echo "  服务器已就绪 (PID $!)"; return 0; }
    done
    echo "  服务器启动失败!"; return 1
}

# -------------------- 主流程 --------------------
check_server || { echo "服务器未启动, 尝试启动..."; start_server || exit 1; }

# 预热
echo "预热:"
for i in $(seq 1 5); do curl -s -o /dev/null "$URL"; done
echo ""

# 写入日志头
{
    echo "# 短连接压测 (ab) | $(date '+%Y-%m-%d %H:%M:%S')"
    echo "## 目标: $URL"
    echo "## 工具: ab | 请求数/梯度: ${REQUESTS} | 协议: HTTP/1.0 (每请求新 TCP)"
    echo "## ab 输出: min mean[+/-sd] median max, 延迟取 mean 列"
    echo ""
    echo "| 并发 | QPS | Total(mean) | Connect(mean) | Process(mean) | 失败请求 |"
    echo "|------|-----|-------------|---------------|---------------|----------|"
} > "$LOGFILE"

echo "短连接压测开始 (5 梯度 × ${REQUESTS} 请求)"
echo "==============================================="

for c in "${LEVELS[@]}"; do
    # 梯度间检查服务器，崩了就重启
    if ! check_server; then
        echo "  服务器已崩溃，自动重启…"
        start_server || { echo "  重启失败，跳过剩余测试"; break; }
        sleep 2
    fi

    echo -n "并发=$c ... "

    output=$(ab -n "$REQUESTS" -c "$c" "$URL" 2>&1) || true

    # ab 输出 "Requests per second:    3818.64 [#/sec] (mean)" → 用 grep -oP 提取数字
    qps=$(echo "$output"     | grep -oP "Requests per second:\s+\K[\d.]+")
    qps="${qps:-0}"

    # ab Connection Times 表格 — 数据行以字段名开头(无前导空格):
    #   Connect:    min mean[+/-sd] median max
    #   Processing: min mean[+/-sd] median max
    #   Total:      min mean[+/-sd] median max
    # 注意: "Total:" 会同时命中 "Total transferred:"，用 \d 过滤只取延迟行
    conn=$(echo "$output"    | grep -E "^Connect:"      | awk '{print $3}')
    proc=$(echo "$output"    | grep -E "^Processing:"   | awk '{print $3}')
    # "Total:" 出现在两处: "Total transferred:"(字节数) 和延迟表。延迟表在后面, 用 tail -1
    total=$(echo "$output"   | grep -E "^Total:"        | tail -1 | awk '{print $3}')
    # Complete requests / Failed requests
    complete=$(echo "$output" | grep "Complete requests:"   | grep -oP "\d+")
    failed=$(echo "$output"   | grep "Failed requests:"     | grep -oP "\d+")

    if [[ "$qps" != "0" ]]; then
        echo "  QPS=$qps, Total=${total:-N/A}ms, Connect=${conn:-N/A}ms, Process=${proc:-N/A}ms (完成: ${complete:-?}/${REQUESTS})"
    else
        # 打印诊断信息
        echo "  失败! (完成: ${complete:-0}/${REQUESTS})"
        echo "$output" | grep -E "apr_socket|Complete requests|Failed requests" | sed 's/^/    /'
    fi

    printf "| %-4s | %-6s | %-11sms | %-13sms | %-13sms | %-8s |\n" \
        "$c" "$qps" "${total:-N/A}" "${conn:-N/A}" "${proc:-N/A}" "${failed:-0}" \
        >> "$LOGFILE"

    sleep 3  # 释放 TIME_WAIT 端口
done

echo ""
echo "报告: $LOGFILE"
cat "$LOGFILE"
