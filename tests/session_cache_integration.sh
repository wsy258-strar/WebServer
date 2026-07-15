#!/usr/bin/env bash
set -euo pipefail

base="${BASE_URL:-http://localhost:8080}"
name="cache-test-$(date +%s)"
auth=$(curl -fsS -X POST "$base/api/auth?username=$name&password=test-password")
token=$(printf '%s' "$auth" | sed -n 's/.*"session_token":"\([^"]*\)".*/\1/p')

test -n "$token"
redis-cli EXISTS "session:$token" | grep -qx 1
redis-cli TTL "session:$token" | awk '$1 >= 1790 && $1 <= 1800'
curl -fsS -X POST "$base/api/session/enter?token=$token&scene=cache-test-scene" >/dev/null
redis-cli GET "session:$token" | grep -q cache-test-scene
curl -fsS "$base/api/session?token=$token" | grep -q '"status":"ok"'
redis-cli TTL "session:$token" | awk '$1 >= 1790 && $1 <= 1800'
redis-cli DEL "session:$token" | grep -qx 1
curl -fsS "$base/api/session?token=$token" | grep -q '"status":"ok"'
redis-cli EXISTS "session:$token" | grep -qx 1
redis-cli TTL "session:$token" | awk '$1 >= 1790 && $1 <= 1800'
curl -fsS -X POST "$base/api/session/exit?token=$token" >/dev/null
redis-cli EXISTS "session:$token" | grep -qx 0
