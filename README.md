# paid_server — 游戏账号交易支付系统

基于 **C++17** 自研 HTTP/WebSocket 服务与 **Nginx** 静态前端，实现游戏账号交易场景下的订单确认、实名补全、扫码支付与实时结果推送。采用 **网关 + 后端** 分层架构，网关负责鉴权与会话管理，后端专注支付业务与数据库事务。

## 技术栈

| 层级 | 技术 |
|------|------|
| 前端 | HTML / JavaScript，Nginx 托管静态页面 |
| 网关 | C++17，自研 HTTP + WebSocket 服务（`payment_gateway`） |
| 后端 | C++17，自研 HTTP 服务（`payment_backend`） |
| 数据 | MySQL（连接池 + 事务） |
| 安全 | OpenSSL（JWT 签发/验签、HMAC-SHA256 密码与敏感字段哈希） |
| 日志 | spdlog，敏感字段脱敏 |
| 构建 | CMake 3.16+ |
| 编排 | Bash 脚本 `run_all.sh` 一键编译启停 |

## 系统架构

```text
浏览器
  │
  ▼
Nginx :9994          静态页面（登录 / 订单确认 / 扫码支付 / 实名补全）
  │ proxy /api/*  →  Gateway HTTP :9993
  │ proxy /ws/*   →  Gateway WS   :9992
  │
  ▼
Gateway（鉴权、Session、WebSocket 推送）
  │ 转发 /api/*        →  Backend HTTP :9990
  │ 接收支付结果回调    ←  Backend POST /internal/payment/result
  │
  ▼
Backend（支付业务、MySQL 事务）
  │
  ▼
MySQL（users / orders / accounts / payment_sessions / payment_attempts）
```

### 核心业务流程

1. **页面 A（订单确认）**：卖家登录 → 查看订单 → 勾选协议继续支付 → 建立 WebSocket → 展示支付二维码
2. **页面 B（扫码支付）**：买家手机扫码 → 校验 `qr_token` → 输入登录密码完成支付
3. **页面 C（实名补全）**：未完成实名的卖家补全邮箱与身份证后跳回原页面
4. **实时通知**：后端支付成功后回调网关，网关经 WebSocket 向页面 A 推送 `PAYMENT_RESULT`

## 目录结构

```text
paid_http/
├── backend/                    # 支付后端服务
│   ├── common/                 # 配置、JWT、日志、MySQL 连接池、密码校验
│   ├── payment_impl.hpp        # 6 个支付 API 业务实现
│   ├── main.cpp                # HTTP 服务入口
│   └── 后端业务流程文档.md
├── gateway/                    # API 网关
│   ├── common/                 # 配置、日志、会话存储、HTTP 转发器
│   ├── gateway_impl.hpp        # 登录鉴权、路由转发、WebSocket 推送
│   ├── main.cpp                # HTTP + WebSocket 双端口入口
│   └── 网关业务流程文档.md
├── frontend/                   # 前端静态资源
│   ├── pages/                  # login / payment_confirm / payment_scan / real_auth_complete
│   ├── common/                 # api.js、config.js、二维码渲染
│   ├── nginx.conf.template     # Nginx 配置模板
│   └── 前端业务流程文档.md
├── 数据库表结构/                # 5 张业务表设计文档
├── sql_create_and_add/         # MySQL 建表与测试数据脚本
├── run_all.sh                  # 一键编译 / 启停 / 状态查看
├── .env.example                # 环境变量模板
└── .gitignore
```

## 快速开始

### 1. 安装依赖

```bash
# Ubuntu / Debian
sudo apt install -y build-essential cmake libssl-dev libmysqlclient-dev \
  libspdlog-dev nlohmann-json3-dev nginx mysql-client
```

### 2. 配置环境

```bash
cp .env.example .env
# 编辑 .env，填写 MySQL 连接信息与各服务密钥
```

### 3. 初始化数据库

```bash
# 需已启动 MySQL，且 .env 中数据库账号有建库权限
bash sql_create_and_add/setup_mysql_test_data.sh
```

### 4. 编译并启动

```bash
./run_all.sh up
```

启动后默认访问地址（IP/端口以 `.env` 为准）：

| 页面 | 地址 |
|------|------|
| 登录 | `http://<PAYMENT_NGINX_IP>:9994/login` |
| 订单确认（页面 A） | `http://<IP>:9994/payment/confirm?order_sn=<订单号>` |
| 扫码支付（页面 B） | `http://<IP>:9994/payment/scan?token=<qr_token>` |
| 实名补全（页面 C） | `http://<IP>:9994/user/real-auth/complete?return_url=...` |

### 5. 常用命令

```bash
./run_all.sh build     # 仅编译
./run_all.sh status    # 查看运行状态
./run_all.sh down      # 停止全部服务
./run_all.sh restart   # 重启
```

日志目录：`.run/logs/`（`backend.log`、`gateway.log`、`nginx_*.log`）

## API 一览

统一响应格式：`{"code": 0, "message": "", "data": {}}`

| 方法 | 路径 | 处理方 | 说明 |
|------|------|--------|------|
| POST | `/api/auth/login` | 网关 | 登录，写入 `session_id` Cookie |
| POST | `/api/auth/logout` | 网关 | 登出，清除 Cookie |
| GET | `/api/payment/orders/{order_sn}` | 网关 → 后端 | 查询订单（需登录） |
| POST | `/api/payment/confirm` | 网关 → 后端 | 确认支付，创建会话与二维码（需登录） |
| POST | `/api/users/real-auth/complete` | 网关 → 后端 | 实名信息补全（需登录） |
| GET | `/api/payment/page` | 网关 → 后端 | 扫码支付页数据（凭 `qr_token`） |
| POST | `/api/payment/attempt/init` | 网关 → 后端 | 初始化支付尝试 |
| POST | `/api/payment/pay` | 网关 → 后端 | 提交登录密码完成支付 |
| WS | `/ws/payment` | 网关 | 页面 A 绑定会话，接收支付结果推送 |
| POST | `/internal/payment/result` | 网关（内网） | 后端回调，触发 WebSocket 推送 |

详细请求体、错误码与逐步处理流程见各模块文档：

- [网关业务流程](gateway/网关业务流程文档.md)
- [后端业务流程](backend/后端业务流程文档.md)
- [前端业务流程](frontend/前端业务流程文档.md)

## 数据库

| 表 | 说明 |
|----|------|
| `users` | 用户账号、密码哈希、实名状态 |
| `orders` | 交易订单 |
| `accounts` | 游戏账号商品 |
| `payment_sessions` | 支付会话（关联订单与二维码） |
| `payment_attempts` | 支付尝试记录（幂等、失败原因） |

表结构详见 [数据库表结构/](数据库表结构/) 目录。

## 安全设计

- **登录密码**：`HMAC-SHA256(plain_password, PASSWORD_HASH_SECRET)` 存库；明文仅在请求内使用，不落库、不写日志
- **敏感个人信息**：邮箱、身份证号明文不落库，仅存 HMAC 哈希与脱敏展示值
- **支付二维码**：`qr_token` 为 JWT，不落库；合法性通过验签 + 会话查询双重校验
- **会话隔离**：需登录接口由网关校验 Cookie，向后端注入 `X-User-Id` 请求头
- **日志脱敏**：密码、token 等字段在日志中自动屏蔽

## 服务端口（默认）

| 服务 | 端口 | 说明 |
|------|------|------|
| Backend HTTP | 9990 | 内网支付 API |
| Backend WebSocket | 9991 | 预留 |
| Gateway WebSocket | 9992 | 支付结果推送 |
| Gateway HTTP | 9993 | 对外 API 入口 |
| Nginx | 9994 | 静态页面 + 反向代理 |

端口均在 `.env` 中可配置。

## 项目亮点（求职向）

- **全栈 C++ 服务**：自研轻量 HTTP/WebSocket 服务，无第三方 Web 框架依赖
- **网关分层**：鉴权、转发、推送与业务逻辑解耦，后端可独立扩展
- **完整支付闭环**：订单确认 → 二维码生成 → 扫码支付 → 事务落库 → WebSocket 实时通知
- **安全意识**：密码哈希、JWT、敏感字段脱敏、幂等支付尝试均有落地实现
- **可运行交付**：一键脚本编译部署，附带建表脚本与测试数据

## 后续规划

- [ ] 会话存储从内存 Map 迁移至 Redis
- [ ] 网关限流与更完善的错误码体系
- [ ] Backend WebSocket 端口（9991）启用

## License

暂未指定开源协议。如需引用或二次开发，请联系作者。
