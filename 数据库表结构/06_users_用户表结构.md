# 核心业务表：`users`（用户表）

## 1. 字段设计


| 字段名                   | 数据类型         | 约束                | 默认值              | 索引      | 描述                                                       |
| --------------------- | ------------ | ----------------- | ---------------- | ------- | -------------------------------------------------------- |
| `id`                  | BIGINT       | PK, AUTOINCREMENT | -                | PRIMARY | 用户内部主键 ID                                                |
| `user_id`             | VARCHAR(64)  | UNIQUE, NOT NULL  | -                | UNIQUE  | 用户业务 ID（雪花算法生成），供订单、账号、支付等业务表引用                          |
| `username`            | VARCHAR(64)  | UNIQUE, NOT NULL  | -                | UNIQUE  | 用户登录名，要求全局唯一                                             |
| `password_hash`       | VARCHAR(255) | NOT NULL          | -                | -       | 登录密码哈希值，禁止保存明文密码                                         |
| `email_hash`          | CHAR(64)     | NULL              | NULL             | UNIQUE  | 邮箱 HMAC-SHA256 十六进制摘要值，用于唯一性校验和登录匹配                      |
| `email_masked`        | VARCHAR(128) | NULL              | NULL             | -       | 脱敏邮箱号，例如：`use****@example.com`，用于展示和排查                   |
| `id_card_hash`        | CHAR(64)     | NULL              | NULL             | UNIQUE  | 身份证号 HMAC-SHA256 十六进制摘要值，用于实名认证状态判断、一致性校验和风控排查，不保存明文身份证号 |
| `id_card_masked`      | VARCHAR(32)  | NULL              | NULL             | -       | 脱敏身份证号，例如：`4403**************00`，用于后台展示和人工排查             |
| `real_auth_completed` | TINYINT      | NOT NULL          | 0                | -       | 实名认证信息是否已补全：0-未补全，1-已补全                                  |
| `user_type`           | TINYINT      | NOT NULL          | 0                | INDEX   | 用户类型：0-普通用户，1-VIP用户，2-SVIP用户, 3-平台管理员                    |
| `status`              | TINYINT      | NOT NULL          | 0                | -       | 用户状态：0-正常，1-冻结，2-注销，3-风控限制                               |
| `last_login_at`       | DATETIME     | NOT NULL          | -                | -       | 最近登录时间                                                   |
| `created_at`          | DATETIME     | NOT NULL          | CURRENTTIMESTAMP | -       | 用户创建时间                                                   |


---



## 2. 索引设计

- `PRIMARY KEY (id)`：主键索引，支持按内部主键快速定位用户。
- `UNIQUE KEY uk_user_id (user_id)`：唯一索引，保证用户业务 ID 全局唯一，并支持订单、账号、支付等业务表通过 `buyer_id`、`seller_id` 关联用户。
- `UNIQUE KEY uk_username (username)`：唯一索引，保证用户登录名唯一，支持用户名登录和重复注册校验。
- `UNIQUE KEY uk_email_hash (email_hash)`：唯一索引，保证邮箱号唯一，支持邮箱号登录、邮箱号绑定和重复注册校验。
- `UNIQUE KEY uk_id_card_hash (id_card_hash)`：唯一索引，限制同一身份证号只能绑定一个用户账号。
- `KEY idx_user_type (user_type)`：普通索引，支持后台按用户类型筛选普通用户、VIP 用户、SVIP 用户和平台管理员。
- `real_auth_completed` **字段暂不单独建立索引**：支付流程中会先通过 `user_id` 定位用户，再判断实名认证是否已补全，因此当前版本暂不需要为该低区分度字段单独建索引。若后续后台需要批量筛选未实名用户，可再补充 `KEY idx_real_auth_completed (real_auth_completed)`。

---



## 3. 格式说明

- `user_id`：用户业务 ID，建议格式为 `USR{yyyyMMdd}{雪花ID后12位}`，例如：`USR20260619000123456789`。
- `username`：用户登录名，要求全局唯一。建议限制为字母、数字、下划线组合，长度由业务层控制。
- `password_hash`：必须保存登录密码哈希值，禁止保存明文密码。使用统一服务端密钥计算：`password_hash = HMAC-SHA256(plain_password, PASSWORD_HASH_SECRET)`，得到 64 位十六进制摘要值写入本字段。`PASSWORD_HASH_SECRET` 仅保存在服务端环境变量中，禁止落库、禁止下发前端。`plain:` 前缀仅用于本地开发测试。该字段不支持反推出原始密码。
- `email_hash`：当前阶段实际用于保存邮箱哈希值。计算前应先对邮箱进行标准化处理，例如去除首尾空格并统一转小写，然后使用 `HMAC-SHA256(email_normalized, USER_EMAIL_HASH_SECRET)` 计算得到十六进制摘要值。该字段用于邮箱唯一性校验和登录匹配，禁止直接使用明文邮箱作为唯一校验字段。
- `email_masked`：当前阶段实际用于保存邮箱展示值。建议保存脱敏邮箱，例如：`use****@example.com`，用于前端展示、后台展示和人工排查。
- `id_card_hash`：身份证号哈希值。计算前应先对身份证号进行标准化处理，例如去除首尾空格并统一转大写，然后使用 `HMAC-SHA256(id_card_normalized, USER_ID_CARD_HASH_SECRET)` 计算得到十六进制摘要值。该字段用于实名认证状态判断、一致性校验和风控排查，禁止直接保存明文身份证号。
- `id_card_masked`：脱敏身份证号，例如：`4403**************00`，只用于后台展示和人工排查。
- `real_auth_completed`：实名认证信息是否已补全。当前版本中，当 `email_hash` 和 `id_card_hash` 均存在时，可设置为 `1-已补全`；否则为 `0-未补全`。
- `user_type`：用于区分普通用户、VIP 用户、SVIP 用户和平台管理员。不同类型用户在业务权限上应进行隔离。
- `status`：用于控制用户账号是否允许登录、下单、支付、上架商品等操作。
- `last_login_at`：用户成功登录后更新，用于安全排查和用户活跃度分析。
- `created_at`：用户注册或创建账号时写入，用于记录用户创建时间。

---



## 4. 用户类型说明

- `0-普通用户`：普通用户，可浏览账号、下单、支付。
- `1-VIP用户`：vip用户，在普通用户权限的基础上，可售卖账号。
- `2-SVIP用户`：SVIP用户，在vip用户权限的基础上，具有额外功能，如在物品公示期即可下单完成支付，同时还有10%的购买账号力度优惠等。
- `3-平台管理员`：平台后台管理用户，具备运营、审核或风控管理权限。

---



## 5. 用户状态说明

- `0-正常`：用户账号可正常登录和使用。
- `1-冻结`：用户账号被平台冻结，禁止登录或禁止交易。
- `2-注销`：用户账号已注销，不允许继续使用。
- `3-风控限制`：用户账号触发风控策略，限制支付、下单、上架等高风险操作。

---



## 6. 业务使用规则

- `orders.buyer_id` 应对应 `users.user_id`。
- `accounts.seller_id` 应对应 `users.user_id`。
- 支付服务在处理支付请求时，应根据 `buyer_id` 查询用户并校验用户是否存在、状态是否正常。
- 如果用户状态不是 `0-正常`，支付服务应拒绝继续支付。
- 上架账号时，应校验 `seller_id` 是否对应有效用户，并校验其 `user_type` 判断其是否拥有上架账号权限。
- 平台后台管理操作应校验用户是否为 `3-平台管理员`，并结合权限系统进一步控制具体操作范围。
- 用户密码只能在注册、登录、修改密码等认证服务中处理，支付服务不应读取或记录明文密码。
- 邮箱号等敏感信息应避免明文写入日志。
- 当前阶段实名认证采用“邮箱号 + 身份证号”完成。`email_hash` 当前实际用于邮箱唯一性校验和登录匹配，`email_masked` 当前实际用于脱敏邮箱展示。
- `id_card_hash` 用于身份证号一致性校验、实名状态判断和风控排查，`id_card_masked` 用于后台展示和人工排查，禁止保存明文身份证号。
- 当用户已存在 `email_hash` 且补全 `id_card_hash` 后，可将 `real_auth_completed` 设置为 `1-已补全`。
- 支付服务在页面 A 继续支付前，应校验 `real_auth_completed == 1`；如果未补全，应返回信息补全链接，不得继续创建支付会话和二维码。

---

