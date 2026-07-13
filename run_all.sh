#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="${ROOT_DIR}/.env"
RUN_DIR="${ROOT_DIR}/.run"
LOG_DIR="${RUN_DIR}/logs"
PID_DIR="${RUN_DIR}/pids"
NGINX_PREFIX="${RUN_DIR}/nginx"
NGINX_CONF="${RUN_DIR}/nginx/nginx.conf"
NGINX_TEMPLATE="${ROOT_DIR}/frontend/nginx.conf.template"

BACKEND_DIR="${ROOT_DIR}/backend"
GATEWAY_DIR="${ROOT_DIR}/gateway"
FRONTEND_DIR="${ROOT_DIR}/frontend"

BACKEND_BIN="${BACKEND_DIR}/build/payment_backend"
GATEWAY_BIN="${GATEWAY_DIR}/build/payment_gateway"

mkdir -p "${LOG_DIR}" "${PID_DIR}" \
  "${NGINX_PREFIX}/temp/client_body" \
  "${NGINX_PREFIX}/temp/proxy" \
  "${NGINX_PREFIX}/temp/fastcgi" \
  "${NGINX_PREFIX}/temp/uwsgi" \
  "${NGINX_PREFIX}/temp/scgi"

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

print_paths() {
  echo "Backend 可执行文件: ${BACKEND_BIN}"
  echo "Gateway 可执行文件: ${GATEWAY_BIN}"
  echo "Nginx 配置文件: ${NGINX_CONF}"
}

build_backend() {
  cmake -S "${BACKEND_DIR}" -B "${BACKEND_DIR}/build"
  cmake --build "${BACKEND_DIR}/build" -j"$(nproc)"
}

build_gateway() {
  cmake -S "${GATEWAY_DIR}" -B "${GATEWAY_DIR}/build"
  cmake --build "${GATEWAY_DIR}/build" -j"$(nproc)"
}

prepare_nginx_conf() {
  if [[ ! -f "${NGINX_TEMPLATE}" ]]; then
    echo "缺少 Nginx 模板: ${NGINX_TEMPLATE}" >&2
    exit 1
  fi
  if ! command -v nginx >/dev/null 2>&1; then
    echo "未找到 nginx，请先安装: sudo apt install nginx" >&2
    exit 1
  fi

  local port
  local gateway_ip
  local gateway_http_port
  local gateway_ws_port
  port="$(read_env_var PAYMENT_NGINX_PORT 9994)"
  gateway_ip="$(read_env_var PAYMENT_GATEWAY_IP 127.0.0.1)"
  gateway_http_port="$(read_env_var PAYMENT_GATEWAY_HTTP_PORT 9993)"
  gateway_ws_port="$(read_env_var PAYMENT_GATEWAY_WEBSOCKET_PORT 9992)"
  sed "s|@ROOT_DIR@|${ROOT_DIR}|g; s|@PORT@|${port}|g; \
       s|@GATEWAY_HTTP_UPSTREAM@|${gateway_ip}:${gateway_http_port}|g; \
       s|@GATEWAY_WS_UPSTREAM@|${gateway_ip}:${gateway_ws_port}|g; \
       s|@ERROR_LOG@|${ROOT_DIR}/.run/logs/nginx_error.log|g; \
       s|@ACCESS_LOG@|${ROOT_DIR}/.run/logs/nginx_access.log|g" \
    "${NGINX_TEMPLATE}" >"${NGINX_CONF}"

  nginx -t -p "${NGINX_PREFIX}" -c "${NGINX_CONF}"
}

start_proc() {
  local name="$1"
  local command="$2"
  local pid_file="${PID_DIR}/${name}.pid"
  local log_file="${LOG_DIR}/${name}.log"

  if [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
    echo "${name} 已在运行 (pid=$(cat "${pid_file}"))"
    return
  fi

  nohup bash -lc "${command}" >"${log_file}" 2>&1 &
  echo $! >"${pid_file}"
  echo "${name} 已启动 (pid=$!, log=${log_file})"
}

start_nginx() {
  local pid_file="${PID_DIR}/nginx.pid"
  if [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
    echo "nginx 已在运行 (pid=$(cat "${pid_file}"))"
    return
  fi

  prepare_nginx_conf
  nginx -p "${NGINX_PREFIX}" -c "${NGINX_CONF}"
  echo "nginx 已启动 (pid_file=${pid_file}, access_log=${LOG_DIR}/nginx_access.log)"
}

stop_proc() {
  local name="$1"
  local pid_file="${PID_DIR}/${name}.pid"
  if [[ ! -f "${pid_file}" ]]; then
    echo "${name} 未运行"
    return
  fi

  local pid
  pid="$(cat "${pid_file}")"
  if kill -0 "${pid}" 2>/dev/null; then
    kill "${pid}" 2>/dev/null || true
    sleep 0.2
    if kill -0 "${pid}" 2>/dev/null; then
      kill -9 "${pid}" 2>/dev/null || true
    fi
    echo "${name} 已停止"
  else
    echo "${name} 进程不存在，清理 pid 文件"
  fi
  rm -f "${pid_file}"
}

stop_nginx() {
  local pid_file="${PID_DIR}/nginx.pid"
  if [[ -f "${NGINX_CONF}" ]] && [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
    nginx -s quit -p "${NGINX_PREFIX}" -c "${NGINX_CONF}" 2>/dev/null || true
    sleep 0.2
  fi

  if [[ -f "${pid_file}" ]]; then
    local pid
    pid="$(cat "${pid_file}")"
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      sleep 0.2
      if kill -0 "${pid}" 2>/dev/null; then
        kill -9 "${pid}" 2>/dev/null || true
      fi
    fi
    rm -f "${pid_file}"
    echo "nginx 已停止"
  else
    echo "nginx 未运行"
  fi

  # 兼容旧版 frontend.pid（dev_server.py）
  rm -f "${PID_DIR}/frontend.pid"
}

status_proc() {
  local name="$1"
  local pid_file="${PID_DIR}/${name}.pid"
  if [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
    echo "${name}: running (pid=$(cat "${pid_file}"))"
  else
    echo "${name}: stopped"
  fi
}

cmd_build() {
  echo "==> 编译 backend"
  build_backend
  echo "==> 编译 gateway"
  build_gateway
  echo "==> 编译完成"
  print_paths
}

cmd_up() {
  cmd_build
  echo "==> 启动 backend/gateway/nginx"
  start_proc "backend" "\"${BACKEND_BIN}\" \"${ENV_FILE}\""
  start_proc "gateway" "\"${GATEWAY_BIN}\" \"${ENV_FILE}\""
  start_nginx
  echo
  local nginx_ip
  nginx_ip="$(read_env_var PAYMENT_NGINX_IP 127.0.0.1)"
  local nginx_port
  nginx_port="$(read_env_var PAYMENT_NGINX_PORT 9994)"
  echo "访问地址:"
  echo "  页面A: http://${nginx_ip}:${nginx_port}/payment/confirm?order_sn=你的订单号"
  echo "  页面B: http://${nginx_ip}:${nginx_port}/payment/scan?token=你的token"
  echo "  页面C: http://${nginx_ip}:${nginx_port}/user/real-auth/complete?return_url=%2Fpayment%2Fconfirm%3Forder_sn%3DORD001"
  echo "  登录页: http://${nginx_ip}:${nginx_port}/login"
  print_paths
}

cmd_down() {
  echo "==> 停止服务"
  stop_nginx
  stop_proc "gateway"
  stop_proc "backend"
}

cmd_status() {
  status_proc "backend"
  status_proc "gateway"
  status_proc "nginx"
}

usage() {
  cat <<EOF
用法: ./run_all.sh <命令>

命令:
  build    编译 backend + gateway
  up       编译并启动 backend + gateway + nginx
  down     停止 backend + gateway + nginx
  restart  重启全部服务
  status   查看运行状态
  paths    打印可执行文件路径
EOF
}

main() {
  local cmd="${1:-}"
  case "${cmd}" in
    build) cmd_build ;;
    up) cmd_up ;;
    down) cmd_down ;;
    restart) cmd_down; cmd_up ;;
    status) cmd_status ;;
    paths) print_paths ;;
    *) usage; exit 1 ;;
  esac
}

main "${@}"
