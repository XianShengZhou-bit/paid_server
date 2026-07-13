#!/usr/bin/env bash
set -euo pipefail

FRONTEND_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${FRONTEND_DIR}/.." && pwd)"
ENV_FILE="${ROOT_DIR}/.env"
RUN_DIR="${ROOT_DIR}/.run"
LOG_DIR="${RUN_DIR}/logs"
PID_DIR="${RUN_DIR}/pids"
NGINX_PREFIX="${RUN_DIR}/nginx"
NGINX_CONF="${RUN_DIR}/nginx/nginx.conf"
NGINX_TEMPLATE="${FRONTEND_DIR}/nginx.conf.template"

read_env_var() {
  local key="$1"
  local default="${2:-}"
  if [[ ! -f "${ENV_FILE}" ]]; then
    echo "${default}"
    return
  fi
  local line
  line="$(grep -E "^${key}=" "${ENV_FILE}" | head -1 || true)"
  if [[ -z "${line}" ]]; then
    echo "${default}"
    return
  fi
  local value="${line#*=}"
  value="$(echo "${value}" | sed 's/[[:space:]]*#.*$//' | tr -d '\r' | xargs)"
  echo "${value}"
}

if ! command -v nginx >/dev/null 2>&1; then
  echo "未找到 nginx，请先安装: sudo apt install nginx" >&2
  exit 1
fi

if [[ ! -f "${NGINX_TEMPLATE}" ]]; then
  echo "缺少 Nginx 模板: ${NGINX_TEMPLATE}" >&2
  exit 1
fi

mkdir -p "${LOG_DIR}" "${PID_DIR}" \
  "${NGINX_PREFIX}/temp/client_body" \
  "${NGINX_PREFIX}/temp/proxy" \
  "${NGINX_PREFIX}/temp/fastcgi" \
  "${NGINX_PREFIX}/temp/uwsgi" \
  "${NGINX_PREFIX}/temp/scgi"

PORT="$(read_env_var PAYMENT_NGINX_PORT 9994)"
PUBLIC_HOST="$(read_env_var PAYMENT_NGINX_IP 127.0.0.1)"
GATEWAY_IP="$(read_env_var PAYMENT_GATEWAY_IP 127.0.0.1)"
GATEWAY_HTTP_PORT="$(read_env_var PAYMENT_GATEWAY_HTTP_PORT 9993)"
GATEWAY_WS_PORT="$(read_env_var PAYMENT_GATEWAY_WEBSOCKET_PORT 9992)"

sed "s|@ROOT_DIR@|${ROOT_DIR}|g; s|@PORT@|${PORT}|g; \
     s|@GATEWAY_HTTP_UPSTREAM@|${GATEWAY_IP}:${GATEWAY_HTTP_PORT}|g; \
     s|@GATEWAY_WS_UPSTREAM@|${GATEWAY_IP}:${GATEWAY_WS_PORT}|g; \
     s|@ERROR_LOG@|/dev/stderr|g; s|@ACCESS_LOG@|/dev/stdout|g" \
  "${NGINX_TEMPLATE}" >"${NGINX_CONF}"

nginx -t -p "${NGINX_PREFIX}" -c "${NGINX_CONF}"

echo "Nginx 前台启动: http://${PUBLIC_HOST}:${PORT}"
echo "  登录页: http://${PUBLIC_HOST}:${PORT}/login"
echo "  API 反代: /api/* -> ${GATEWAY_IP}:${GATEWAY_HTTP_PORT}"
echo "  WS  反代: /ws/*  -> ${GATEWAY_IP}:${GATEWAY_WS_PORT}"
echo "  配置文件: ${NGINX_CONF}"
echo "按 Ctrl+C 停止"
echo

exec nginx -g 'daemon off;' -p "${NGINX_PREFIX}" -c "${NGINX_CONF}"
