#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="${ROOT_DIR}/.env"

if [[ ! -f "${ENV_FILE}" ]]; then
  echo "未找到 .env 文件: ${ENV_FILE}" >&2
  exit 1
fi

if ! command -v mysql >/dev/null 2>&1; then
  echo "未找到 mysql 客户端，请先安装 mysql-client。" >&2
  exit 1
fi

read_env() {
  local key="$1"
  local line value found=0

  while IFS= read -r line || [[ -n "${line}" ]]; do
    [[ -z "${line}" ]] && continue
    [[ "${line}" =~ ^[[:space:]]*# ]] && continue
    if [[ "${line}" == "${key}="* ]]; then
      value="${line#*=}"
      value="${value%%#*}"
      value="${value%"${value##*[![:space:]]}"}"
      value="${value#"${value%%[![:space:]]*}"}"
      printf "%s" "${value}"
      found=1
      break
    fi
  done < "${ENV_FILE}"

  if [[ "${found}" -eq 0 ]]; then
    echo "缺少配置: ${key}" >&2
    exit 1
  fi
}

MYSQL_HOST="$(read_env MYSQL_HOST)"
MYSQL_PORT="$(read_env MYSQL_PORT)"
MYSQL_USER="$(read_env MYSQL_USER)"
MYSQL_PASSWORD="$(read_env MYSQL_PASSWORD)"
MYSQL_DATABASE="$(read_env MYSQL_DATABASE)"

echo "连接 MySQL: ${MYSQL_HOST}:${MYSQL_PORT}, database=${MYSQL_DATABASE}"

if ! mysql \
  --host="${MYSQL_HOST}" \
  --port="${MYSQL_PORT}" \
  --user="${MYSQL_USER}" \
  --password="${MYSQL_PASSWORD}" \
  --default-character-set=utf8mb4 \
  -e "USE \`${MYSQL_DATABASE}\`;" >/dev/null 2>&1; then
  cat >&2 <<EOF
无权访问数据库 ${MYSQL_DATABASE}（用户: ${MYSQL_USER}@${MYSQL_HOST}）。
请用有权限账号先执行授权，例如：

  CREATE DATABASE IF NOT EXISTS \`${MYSQL_DATABASE}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
  GRANT ALL PRIVILEGES ON \`${MYSQL_DATABASE}\`.* TO '${MYSQL_USER}'@'${MYSQL_HOST}';
  FLUSH PRIVILEGES;
EOF
  exit 1
fi

mysql \
  --host="${MYSQL_HOST}" \
  --port="${MYSQL_PORT}" \
  --user="${MYSQL_USER}" \
  --password="${MYSQL_PASSWORD}" \
  --default-character-set=utf8mb4 \
  <<SQL
USE \`${MYSQL_DATABASE}\`;

SET FOREIGN_KEY_CHECKS = 0;

DROP TABLE IF EXISTS account_rare_items;
DROP TABLE IF EXISTS payment_attempts;
DROP TABLE IF EXISTS payment_sessions;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS accounts;
DROP TABLE IF EXISTS users;

CREATE TABLE users (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  user_id VARCHAR(64) NOT NULL UNIQUE,
  username VARCHAR(64) NOT NULL UNIQUE,
  password_hash VARCHAR(255) NOT NULL,
  email_hash CHAR(64) NULL UNIQUE,
  email_masked VARCHAR(128) NULL,
  id_card_hash CHAR(64) NULL UNIQUE,
  id_card_masked VARCHAR(32) NULL,
  real_auth_completed TINYINT NOT NULL DEFAULT 0,
  user_type TINYINT NOT NULL DEFAULT 0,
  status TINYINT NOT NULL DEFAULT 0,
  last_login_at DATETIME NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY idx_user_type (user_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE accounts (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  account_id VARCHAR(64) NOT NULL UNIQUE,
  seller_id VARCHAR(64) NOT NULL,
  game_name VARCHAR(64) NOT NULL,
  server_area VARCHAR(64) NOT NULL,
  price INT NOT NULL,
  account_level INT NOT NULL DEFAULT 0,
  hero_count INT NOT NULL DEFAULT 0,
  skin_count INT NOT NULL DEFAULT 0,
  rare_items JSON NOT NULL,
  status TINYINT NOT NULL DEFAULT 0,
  version INT NOT NULL DEFAULT 0,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_seller_id (seller_id),
  KEY idx_game_server_status_price (game_name, server_area, status, price),
  KEY idx_game_status_price (game_name, status, price),
  KEY idx_game_server_status_skin_level_hero (game_name, server_area, status, skin_count, account_level, hero_count),
  KEY idx_status (status),
  KEY idx_created_at (created_at),
  KEY idx_updated_at (updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE orders (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  order_sn VARCHAR(64) NOT NULL UNIQUE,
  account_id VARCHAR(64) NOT NULL,
  buyer_id VARCHAR(64) NOT NULL,
  price INT NOT NULL,
  buyer_email VARCHAR(64) NOT NULL,
  buyer_id_card_hash VARCHAR(128) NOT NULL,
  buyer_id_card_masked VARCHAR(32) NOT NULL,
  status TINYINT NOT NULL DEFAULT 0,
  create_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  paid_at DATETIME NULL,
  update_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_account_id (account_id),
  KEY idx_buyer_id (buyer_id),
  KEY idx_status (status),
  KEY idx_create_time (create_time),
  KEY idx_paid_at (paid_at),
  KEY idx_update_time (update_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE payment_sessions (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  payment_session_id VARCHAR(64) NOT NULL UNIQUE,
  order_sn VARCHAR(64) NOT NULL UNIQUE,
  account_id VARCHAR(64) NOT NULL,
  buyer_id VARCHAR(64) NOT NULL,
  buyer_email VARCHAR(64) NOT NULL,
  status TINYINT NOT NULL DEFAULT 0,
  expire_at DATETIME NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_status_expire_at (status, expire_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE payment_attempts (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  request_id VARCHAR(64) NOT NULL UNIQUE,
  payment_session_id VARCHAR(64) NOT NULL,
  order_sn VARCHAR(64) NOT NULL,
  account_id VARCHAR(64) NOT NULL,
  buyer_id VARCHAR(64) NOT NULL,
  result TINYINT NOT NULL DEFAULT 0,
  fail_reason VARCHAR(255) NULL,
  client_ip VARCHAR(64) NULL,
  user_agent VARCHAR(255) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_payment_session_id (payment_session_id),
  KEY idx_order_sn (order_sn),
  KEY idx_created_at_updated_at (created_at, updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE account_rare_items (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  account_id VARCHAR(64) NOT NULL,
  item_code VARCHAR(64) NOT NULL,
  item_name VARCHAR(64) NOT NULL,
  KEY idx_account_id (account_id),
  KEY idx_item_code_account (item_code, account_id),
  KEY idx_item_name (item_name),
  UNIQUE KEY uk_account_item_code (account_id, item_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TEMPORARY TABLE seq_100 (
  n INT NOT NULL PRIMARY KEY
) ENGINE=Memory;

INSERT INTO seq_100 (n)
SELECT t.n + 1
FROM (
  SELECT d1.d + d2.d * 10 AS n
  FROM
    (SELECT 0 AS d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
     UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d1
    CROSS JOIN
    (SELECT 0 AS d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
     UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d2
) AS t
ORDER BY t.n;

INSERT INTO users (
  user_id, username, password_hash, email_hash, email_masked,
  id_card_hash, id_card_masked, real_auth_completed, user_type, status, last_login_at, created_at
)
SELECT
  CONCAT('USR20260708', LPAD(n, 12, '0')),
  CONCAT('user', LPAD(n, 3, '0')),
  SHA2(CONCAT('password-', n), 256),
  SHA2(CONCAT('user', LPAD(n, 3, '0'), '@example.com'), 256),
  CONCAT('user', LPAD(n, 3, '0'), '****@example.com'),
  SHA2(CONCAT('44030019900101', LPAD(n, 4, '0')), 256),
  CONCAT('440300********', LPAD(n, 4, '0')),
  1,
  CASE WHEN MOD(n, 20) = 0 THEN 1 ELSE 0 END,
  0,
  NOW() - INTERVAL n HOUR,
  NOW() - INTERVAL (100 + n) HOUR
FROM seq_100;

INSERT INTO accounts (
  account_id, seller_id, game_name, server_area, price,
  account_level, hero_count, skin_count, rare_items, status, version, created_at, updated_at
)
SELECT
  CONCAT('ACC20260708', LPAD(n, 12, '0')),
  CONCAT('USR20260708', LPAD(n, 12, '0')),
  ELT(MOD(n, 3) + 1, '王者荣耀', '和平精英', '原神'),
  ELT(MOD(n, 4) + 1, '微信区', 'QQ区', '安卓服', 'iOS服'),
  5000 + n * 100,
  10 + MOD(n, 60),
  20 + MOD(n, 100),
  30 + MOD(n, 150),
  JSON_ARRAY(CONCAT('RARE_', LPAD(n, 4, '0'))),
  CASE
    WHEN n <= 70 THEN 0
    WHEN n <= 85 THEN 1
    WHEN n <= 95 THEN 2
    ELSE 3
  END,
  0,
  NOW() - INTERVAL n DAY,
  NOW() - INTERVAL MOD(n, 10) DAY
FROM seq_100;

INSERT INTO orders (
  order_sn, account_id, buyer_id, price, buyer_email, buyer_id_card_hash,
  buyer_id_card_masked, status, create_time, paid_at, update_time
)
SELECT
  CONCAT('ORD20260708', LPAD(n, 12, '0')),
  CONCAT('ACC20260708', LPAD(n, 12, '0')),
  CONCAT('USR20260708', LPAD(MOD(n, 100) + 1, 12, '0')),
  5000 + n * 100,
  CONCAT('buyer', LPAD(MOD(n, 100) + 1, 3, '0'), '@example.com'),
  SHA2(CONCAT('buyer-card-', MOD(n, 100) + 1), 256),
  CONCAT('440300********', LPAD(MOD(n, 100) + 1, 4, '0')),
  CASE
    WHEN n <= 60 THEN 0
    WHEN n <= 80 THEN 1
    WHEN n <= 90 THEN 2
    ELSE 3
  END,
  NOW() - INTERVAL n DAY,
  CASE WHEN n > 60 AND n <= 80 THEN NOW() - INTERVAL n HOUR ELSE NULL END,
  NOW() - INTERVAL MOD(n, 5) DAY
FROM seq_100;

INSERT INTO payment_sessions (
  payment_session_id, order_sn, account_id, buyer_id, buyer_email, status,
  expire_at, created_at, updated_at
)
SELECT
  CONCAT('PS20260708', LPAD(n, 12, '0')),
  CONCAT('ORD20260708', LPAD(n, 12, '0')),
  CONCAT('ACC20260708', LPAD(n, 12, '0')),
  CONCAT('USR20260708', LPAD(MOD(n, 100) + 1, 12, '0')),
  CONCAT('buyer', LPAD(MOD(n, 100) + 1, 3, '0'), '@example.com'),
  CASE
    WHEN n <= 60 THEN 0
    WHEN n <= 80 THEN 1
    WHEN n <= 90 THEN 2
    ELSE 3
  END,
  CASE
    WHEN n <= 60 THEN NOW() + INTERVAL (120 - n) MINUTE
    WHEN n <= 80 THEN NOW() - INTERVAL n MINUTE
    WHEN n <= 90 THEN NOW() + INTERVAL 10 MINUTE
    ELSE NOW() - INTERVAL 10 MINUTE
  END,
  NOW() - INTERVAL n DAY,
  NOW() - INTERVAL MOD(n, 7) DAY
FROM seq_100;

INSERT INTO payment_attempts (
  request_id, payment_session_id, order_sn, account_id, buyer_id,
  result, fail_reason, client_ip, user_agent, created_at, updated_at
)
SELECT
  CONCAT('REQ20260708', LPAD(n, 12, '0')),
  CONCAT('PS20260708', LPAD(n, 12, '0')),
  CONCAT('ORD20260708', LPAD(n, 12, '0')),
  CONCAT('ACC20260708', LPAD(n, 12, '0')),
  CONCAT('USR20260708', LPAD(MOD(n, 100) + 1, 12, '0')),
  CASE
    WHEN n <= 30 THEN 1
    WHEN n <= 60 THEN 2
    ELSE 0
  END,
  CASE WHEN n <= 30 THEN NULL WHEN n <= 60 THEN 'PASSWORD_INVALID' ELSE NULL END,
  CONCAT('192.168.1.', MOD(n, 254) + 1),
  CONCAT('Mozilla/5.0 TestAgent/', n),
  NOW() - INTERVAL n HOUR,
  NOW() - INTERVAL MOD(n, 30) MINUTE
FROM seq_100;

INSERT INTO account_rare_items (account_id, item_code, item_name)
SELECT
  CONCAT('ACC20260708', LPAD(n, 12, '0')),
  CONCAT('RARE_', LPAD(n, 4, '0')),
  CONCAT('珍稀道具', n)
FROM seq_100;

SET FOREIGN_KEY_CHECKS = 1;

SELECT 'users' AS table_name, COUNT(*) AS row_count FROM users
UNION ALL SELECT 'accounts', COUNT(*) FROM accounts
UNION ALL SELECT 'orders', COUNT(*) FROM orders
UNION ALL SELECT 'payment_sessions', COUNT(*) FROM payment_sessions
UNION ALL SELECT 'payment_attempts', COUNT(*) FROM payment_attempts
UNION ALL SELECT 'account_rare_items', COUNT(*) FROM account_rare_items;
SQL

echo "完成：6 张表已创建并各插入 100 条测试数据。"

