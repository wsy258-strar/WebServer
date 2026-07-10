#!/usr/bin/env bash
# =============================================================================
# AR 场景切换模拟测试
# 用法: ./benchmark/test_ar_scene.sh
# =============================================================================
set -u

BASE="http://localhost:8080"

echo "=============================================="
echo "  AR 协同渲染 — 场景切换模拟测试"
echo "=============================================="
echo ""

# ---------- 1. 检查服务 ----------
if ! curl -s -o /dev/null "$BASE/"; then
    echo "服务器未启动: ./bin/main > /dev/null 2>&1 &"
    exit 1
fi

# ---------- 2. 模拟两个用户登录 ----------
echo "=== 1. 用户登录 ==="
echo ""
echo "用户 zhangsan 登录:"
curl -s "$BASE/api/login?username=zhangsan" | python3 -m json.tool 2>/dev/null || \
    curl -s "$BASE/api/login?username=zhangsan"
echo ""

echo "用户 lisi 登录:"
curl -s "$BASE/api/login?username=lisi" | python3 -m json.tool 2>/dev/null || \
    curl -s "$BASE/api/login?username=lisi"
echo ""

# ---------- 3. 查询会话状态 ----------
echo "=== 2. 查询当前场景 ==="
echo ""
TOKEN_ZS="token-zhangsan-001"
TOKEN_LS="token-lisi-001"

echo "zhangsan 当前场景:"
curl -s "$BASE/api/session?token=$TOKEN_ZS"
echo ""
echo ""

echo "lisi 当前场景:"
curl -s "$BASE/api/session?token=$TOKEN_LS"
echo ""

# ---------- 4. 场景切换 ----------
echo ""
echo "=== 3. AR 场景切换 ==="
echo ""

# 定义场景切换序列: user token scene
declare -A SWITCHES=(
    ["zhangsan"]="token-zhangsan-001"
    ["lisi"]="token-lisi-001"
)

for user in zhangsan lisi; do
    TOKEN="${SWITCHES[$user]}"
    for scene in factory_workshop city_sandbox medical_lab; do
        START=$(date +%s%N)
        RESULT=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/switch_scene?token=$TOKEN&scene=$scene")
        END=$(date +%s%N)
        ELAPSED=$(( (END - START) / 1000000 ))  # 纳秒 → 毫秒
        printf "  %-10s → %-20s  HTTP %s  %dms\n" "$user" "$scene" "$RESULT" "$ELAPSED"
    done
done

# ---------- 5. 验证持久化 ----------
echo ""
echo "=== 4. 验证 MySQL 持久化（场景切换后查询） ==="
echo ""

echo "zhangsan 切换后当前场景:"
curl -s "$BASE/api/session?token=$TOKEN_ZS"
echo ""
echo ""

echo "lisi 切换后当前场景:"
curl -s "$BASE/api/session?token=$TOKEN_LS"
echo ""

echo ""
echo "=== 5. 直接查 MySQL 确认写入 ==="
mysql -u root webserver -e "SELECT id, session_token, user_id, scene_id, status FROM sessions WHERE status=1;" 2>/dev/null || \
    echo "  需要手动验证: mysql -u root webserver -e 'SELECT * FROM sessions;'"

echo ""
echo "=============================================="
echo "  测试完成"
echo "=============================================="
