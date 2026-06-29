#!/usr/bin/env bash
# =============================================================================
# webserver 自动化压测脚本
#
# 用法:  ./benchmark/run.sh [target_url]
# 示例:  ./benchmark/run.sh http://localhost:8080/js/app.js
#        ./benchmark/run.sh                        # 默认测 /js/app.js
#
# 依赖:  wrk, ab (apache2-utils), curl
# 安装:  sudo apt install wrk apache2-utils curl -y
#
# 测试内容:
#   1. 长连接压测 (wrk, HTTP/1.1 keep-alive) — 测 IO 吞吐上限
#   2. 短连接压测 (ab,  HTTP/1.0)            — 测 TCP 握手/挥手 + IO
#   3. 不同并发梯度 (10/50/100/200/500)
#
# 输出:  benchmark/results/report_<timestamp>.md
# =============================================================================

set -uo pipefail
# 注意: 不使用 set -e，因为 grep 解析压测输出时可能无匹配导致非零退出

# -------------------- 配置 --------------------
readonly SERVER_PORT=8080
readonly SERVER_HOST="localhost"
readonly BASE_URL="http://${SERVER_HOST}:${SERVER_PORT}"
readonly TARGET_PATH="${1:-/js/app.js}"
readonly TARGET_URL="${BASE_URL}${TARGET_PATH}"
readonly NOTFOUND_URL="${BASE_URL}/nonexistent"
readonly BUILD_DIR="build"
readonly RESULT_DIR="benchmark/results"
readonly REPORT_FILE="${RESULT_DIR}/report_$(date +%Y%m%d_%H%M%S).md"
readonly SERVER_BIN="bin/main"
readonly WARMUP_REQUESTS=10
readonly CONCURRENCY_LEVELS=(10 50 100 200 500)
readonly WRK_THREADS=4
readonly WRK_DURATION="15s"
readonly AB_REQUESTS=30000

# -------------------- 颜色输出 --------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "\n${CYAN}━━━ $* ━━━${NC}"; }

# -------------------- 前置检查 --------------------
check_prerequisites() {
    log_step "1/7  检查依赖工具"
    local missing=()

    for tool in wrk ab curl; do
        if command -v "$tool" &>/dev/null; then
            log_info "$tool: $(command -v $tool)"
        else
            log_error "$tool 未安装"
            missing+=("$tool")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo ""
        log_warn "缺少依赖，请运行:"
        echo "  sudo apt install wrk apache2-utils curl -y"
        exit 1
    fi
}

# -------------------- 编译项目 --------------------
build_project() {
    log_step "2/7  编译项目"
    local project_root
    project_root="$(cd "$(dirname "$0")/.." && pwd)"
    cd "$project_root"

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2" > /dev/null 2>&1
    make -j"$(nproc)" > /dev/null 2>&1

    cd "$project_root"
    log_info "编译完成: ${SERVER_BIN}"
}

# -------------------- 启动服务 --------------------
SERVER_PID=""
start_server() {
    log_step "3/7  启动服务器"
    local project_root
    project_root="$(cd "$(dirname "$0")/.." && pwd)"
    cd "$project_root"

    if [[ ! -x "$SERVER_BIN" ]]; then
        log_error "找不到可执行文件: $SERVER_BIN"
        exit 1
    fi

    # 后台启动，重定向日志
    "$SERVER_BIN" > /tmp/webserver_bench.log 2>&1 &
    SERVER_PID=$!
    log_info "服务器 PID: $SERVER_PID"

    # 等待服务器就绪(最多等 10 秒)
    local retries=0
    while ! curl -s -o /dev/null "$BASE_URL/" 2>/dev/null; do
        sleep 0.5
        ((retries++))
        if [[ $retries -gt 20 ]]; then
            log_error "服务器启动超时"
            kill_server
            exit 1
        fi
    done
    log_info "服务器已就绪"
}

# -------------------- 停止服务 --------------------
kill_server() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        log_info "服务器已停止 (PID: $SERVER_PID)"
    fi
}

# -------------------- 预热缓存 --------------------
warmup_cache() {
    log_step "4/7  预热 LFU 缓存"
    for i in $(seq 1 "$WARMUP_REQUESTS"); do
        curl -s -o /dev/null "$TARGET_URL"
    done
    # 也预热 404
    curl -s -o /dev/null "$NOTFOUND_URL" 2>/dev/null || true
    log_info "预热完成 ($WARMUP_REQUESTS 次请求)"
}

# -------------------- 长连接压测 --------------------
run_long_conn_bench() {
    log_step "5/7  长连接压测 (wrk, HTTP/1.1 keep-alive)"

    declare -gA LONG_RESULTS_QPS
    declare -gA LONG_RESULTS_P50
    declare -gA LONG_RESULTS_P99
    declare -gA LONG_RESULTS_AVG
    declare -gA LONG_RESULTS_ERR

    for c in "${CONCURRENCY_LEVELS[@]}"; do
        echo -n "  并发=$c ... "

        local output
        output=$(wrk -t"$WRK_THREADS" -c"$c" -d"$WRK_DURATION" --latency "$TARGET_URL" 2>&1) || true

        # 解析 QPS
        local qps
        qps=$(echo "$output" | grep "Requests/sec:" | awk '{printf "%.0f", $2}')
        qps="${qps:-0}"

        # 解析延迟 (P50/P99/Avg)
        local p50;  p50=$(echo "$output"  | grep -E "^\s+50%"    | awk '{print $2}')
        local p99;  p99=$(echo "$output"  | grep -E "^\s+99%"    | awk '{print $2}')
        local avg;  avg=$(echo "$output"  | grep "Latency.*Avg"   | awk '{print $2}' | sed 's/ms//')

        # 解析错误 (Non-2xx/3xx)
        local neterr; neterr=$(echo "$output" | grep "Non-2xx or 3xx" | awk '{print $NF}' || echo "0")

        LONG_RESULTS_QPS[$c]="$qps"
        LONG_RESULTS_P50[$c]="${p50:-N/A}"
        LONG_RESULTS_P99[$c]="${p99:-N/A}"
        LONG_RESULTS_AVG[$c]="${avg:-N/A}"
        LONG_RESULTS_ERR[$c]="${neterr:-0}"

        echo "QPS=$qps, P50=${p50:-N/A}, P99=${p99:-N/A}"
        sleep 2  # 冷却间隔
    done
    log_info "长连接压测完成"
}

# -------------------- 短连接压测 --------------------
run_short_conn_bench() {
    log_step "6/7  短连接压测 (ab, HTTP/1.0, 每请求新TCP)"

    declare -gA SHORT_RESULTS_QPS
    declare -gA SHORT_RESULTS_P50
    declare -gA SHORT_RESULTS_P99
    declare -gA SHORT_RESULTS_AVG
    declare -gA SHORT_RESULTS_ERR

    for c in "${CONCURRENCY_LEVELS[@]}"; do
        echo -n "  并发=$c ... "

        local output
        output=$(ab -n "$AB_REQUESTS" -c "$c" "$TARGET_URL" 2>&1) || true

        # 解析 QPS
        local qps
        qps=$(echo "$output" | grep "Requests per second:" | awk '{printf "%.0f", $2}')
        qps="${qps:-0}"

        # 解析延迟 (Connect / Processing / Total 的 mean)
        local conn_ms;    conn_ms=$(echo "$output"    | grep "Connect:"      | head -1 | awk '{print $2}')
        local proc_ms;    proc_ms=$(echo "$output"    | grep "Processing:"   | head -1 | awk '{print $2}')
        local total_ms;   total_ms=$(echo "$output"   | grep "Total:"        | head -1 | awk '{print $2}')

        # 解析失败请求
        local failed; failed=$(echo "$output" | grep "Failed requests:" | awk '{print $3}' || echo "0")

        SHORT_RESULTS_QPS[$c]="$qps"
        SHORT_RESULTS_P50[$c]="${total_ms:-N/A}"
        SHORT_RESULTS_P99[$c]="${conn_ms:-N/A}:${proc_ms:-N/A}"  # 拆成 Connect:Process
        SHORT_RESULTS_AVG[$c]="${conn_ms:-N/A} : ${proc_ms:-N/A} : ${total_ms:-N/A}"
        SHORT_RESULTS_ERR[$c]="${failed:-0}"

        echo "QPS=$qps, Connect=${conn_ms:-N/A}ms, Process=${proc_ms:-N/A}ms, Total=${total_ms:-N/A}ms"
        sleep 2  # 冷却间隔
    done
    log_info "短连接压测完成"
}

# -------------------- 生成报告 --------------------
generate_report() {
    log_step "7/7  生成压测报告"

    local project_root
    project_root="$(cd "$(dirname "$0")/.." && pwd)"
    cd "$project_root"

    mkdir -p "$RESULT_DIR"

    local cpu_cores cpu_model total_ram
    cpu_cores=$(nproc)
    cpu_model=$(lscpu 2>/dev/null | grep "Model name" | cut -d':' -f2 | xargs || echo "Unknown")
    total_ram=$(free -h 2>/dev/null | awk '/Mem:/ {print $2}' || echo "Unknown")

    local tmp_file="${RESULT_DIR}/.tmp_report.md"
    > "$tmp_file"

    # ---------- 报告头部 ----------
    cat >> "$tmp_file" <<EOF
# 🔬 webserver 压测报告

**生成时间:** $(date '+%Y-%m-%d %H:%M:%S')
**测试目标:** \`${TARGET_URL}\`
**服务器线程数:** $(grep "setThreadNum" src/main.cpp 2>/dev/null | grep -oP '\d+' || echo "3")

---

## 测试环境

| 项目 | 值 |
|------|----|
| CPU | ${cpu_model} |
| CPU 核心数 | ${cpu_cores} |
| 内存 | ${total_ram} |
| OS | $(uname -s) $(uname -r) |
| 编译器 | $(g++ --version 2>/dev/null \| head -1 || echo "N/A") |

## 测试方法

| 参数 | 长连接 (wrk) | 短连接 (ab) |
|------|-------------|-------------|
| 工具 | wrk | ab (ApacheBench) |
| 协议 | HTTP/1.1 keep-alive | HTTP/1.0 (每请求新TCP) |
| 线程数 | ${WRK_THREADS} | 1 (ab 单线程) |
| 持续时间 | ${WRK_DURATION} | 至 ${AB_REQUESTS} 请求完成 |
| 预热 | ✅ ${WARMUP_REQUESTS}次 | — |
| 每个梯度冷却 | 2s | 2s |

---

## 一、长连接 (HTTP/1.1 Keep-Alive) — IO 吞吐上限

| 并发连接数 | QPS (req/s) | 平均延迟 | P50 延迟 | P99 延迟 | 错误数 |
|-----------|-------------|----------|---------|---------|-------|
EOF

    for c in "${CONCURRENCY_LEVELS[@]}"; do
        printf "| %-9s | %-11s | %-8s | %-7s | %-7s | %-5s |\n" \
            "$c" \
            "${LONG_RESULTS_QPS[$c]}" \
            "${LONG_RESULTS_AVG[$c]}ms" \
            "${LONG_RESULTS_P50[$c]}" \
            "${LONG_RESULTS_P99[$c]}" \
            "${LONG_RESULTS_ERR[$c]}" \
            >> "$tmp_file"
    done

    # ---------- 短连接表 ----------
    cat >> "$tmp_file" <<EOF

---

## 二、短连接 (HTTP/1.0) — TCP 建连 + IO 综合

| 并发连接数 | QPS (req/s) | 平均延迟 (Total) | Connect | Processing | 失败请求 |
|-----------|-------------|-----------------|---------|------------|---------|
EOF

    for c in "${CONCURRENCY_LEVELS[@]}"; do
        printf "| %-9s | %-11s | %-16s | %-7s | %-10s | %-8s |\n" \
            "$c" \
            "${SHORT_RESULTS_QPS[$c]}" \
            "${SHORT_RESULTS_P50[$c]}ms" \
            "$(echo ${SHORT_RESULTS_P99[$c]} | cut -d: -f1)ms" \
            "$(echo ${SHORT_RESULTS_P99[$c]} | cut -d: -f2)ms" \
            "${SHORT_RESULTS_ERR[$c]}" \
            >> "$tmp_file"
    done

    # ---------- 对比分析表 ----------
    cat >> "$tmp_file" <<EOF

---

## 三、长/短连接 QPS 对比

| 并发连接数 | 长连接 QPS | 短连接 QPS | ratio (短/长) | 说明 |
|-----------|-----------|-----------|---------------|------|
EOF

    for c in "${CONCURRENCY_LEVELS[@]}"; do
        local lqps="${LONG_RESULTS_QPS[$c]}"
        local sqps="${SHORT_RESULTS_QPS[$c]}"
        local ratio="N/A"

        if [[ "$lqps" != "0" && "$lqps" != "" && "$sqps" != "0" && "$sqps" != "" ]]; then
            ratio=$(awk "BEGIN {printf \"%.1f%%\", ($sqps/$lqps)*100}")
        fi

        local note="-"
        if [[ "$ratio" != "N/A" ]]; then
            local pct; pct=$(echo "$ratio" | sed 's/%//')
            if (( $(echo "$pct > 70" | bc -l 2>/dev/null || echo 0) )); then
                note="🟢 TCP 握手开销低，建连效率高"
            elif (( $(echo "$pct > 40" | bc -l 2>/dev/null || echo 0) )); then
                note="🟡 正常范围"
            else
                note="🔴 TCP 握手开销显著，检查 accept() 路径"
            fi
        fi

        printf "| %-9s | %-11s | %-11s | %-13s | %-s |\n" \
            "$c" "$lqps" "$sqps" "$ratio" "$note" \
            >> "$tmp_file"
    done

    # ---------- 结论 ----------
    local peak_long_qps=0
    for c in "${CONCURRENCY_LEVELS[@]}"; do
        local v="${LONG_RESULTS_QPS[$c]}"
        if [[ "$v" -gt "$peak_long_qps" ]]; then
            peak_long_qps="$v"
        fi
    done

    local peak_short_qps=0
    for c in "${CONCURRENCY_LEVELS[@]}"; do
        local v="${SHORT_RESULTS_QPS[$c]}"
        if [[ "$v" -gt "$peak_short_qps" ]]; then
            peak_short_qps="$v"
        fi
    done

    cat >> "$tmp_file" <<EOF

---

## 四、结论

- **长连接峰值 QPS:**   \`${peak_long_qps}\`  (IO 吞吐上限)
- **短连接峰值 QPS:**   \`${peak_short_qps}\`  (TCP 建连 + IO 综合)
- **长/短连接比:**      \`$(awk "BEGIN {printf \"%.1f%%\", ($peak_short_qps/$peak_long_qps)*100}")\`

### 性能画像

\`\`\`
IO 吞吐:     ████████████████████████  ${peak_long_qps} req/s (长连接上限)
TCP建连+IO:  ████████████              ${peak_short_qps} req/s (短连接上限)
\`\`\`

> **解读:**
> - 长连接 QPS 反映了框架的**纯粹 IO 处理能力**（无 TCP 握手开销）。
> - 短连接 QPS 是**真实单请求场景**的预期性能（包含 accept/close 开销）。
> - 两者比值越高，说明 TCP 握手/挥手逻辑越高效。
> - P50 和 P99 差距过大（>100×）时，请检查是否有**日志刷盘阻塞**、**线程数不匹配**等问题。

---

*报告由 benchmark/run.sh 自动生成*
EOF

    # 生成最终文件
    mv "$tmp_file" "$REPORT_FILE"

    echo ""
    log_info "报告已生成: ${REPORT_FILE}"
}

# -------------------- 清理 --------------------
cleanup() {
    kill_server
    log_info "清理完成"
}
trap cleanup EXIT

# ======================== 主流程 ========================

echo ""
echo -e "${BLUE}╔══════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║       🔬  webserver 自动化压测脚本            ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════╝${NC}"
echo ""
echo "  目标URL:       $TARGET_URL"
echo "  并发梯度:      ${CONCURRENCY_LEVELS[*]}"
echo "  长连接工具:    wrk (${WRK_THREADS}线程, ${WRK_DURATION})"
echo "  短连接工具:    ab (${AB_REQUESTS}次请求/梯度)"
echo "  冷却间隔:      2s"
echo ""

check_prerequisites
build_project
start_server
warmup_cache
run_long_conn_bench
run_short_conn_bench
generate_report

# 输出摘要
echo -e "\n${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  压测完成!${NC}"
echo -e "${GREEN}  完整报告: ${REPORT_FILE}${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
