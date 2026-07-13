### 支付服务表：`payment_sessions` (支付会话表)

| 字段名                  | 数据类型         | 约束                  | 默认值                                             | 索引      | 描述                                                |
| :------------------- | :----------- | :------------------ | :---------------------------------------------- | :------ | :------------------------------------------------ |
| `id`                 | BIGINT       | PK, AUTO\_INCREMENT | -                                               | PRIMARY | 支付会话内部主键                                          |
| `payment_session_id` | VARCHAR(64)  | UNIQUE, NOT NULL    | -                                               | UNIQUE  | 支付会话 ID，用于串联页面 A、二维码、页面 B、WebSocket 推送            |
| `order_sn`           | VARCHAR(64)  | UNIQUE, NOT NULL    | -                                               | UNIQUE  | 关联订单号。当前版本设计为同一订单只允许存在一个支付会话                      |
| `account_id`         | VARCHAR(64)  | NOT NULL            | -                                               | -       | 关联游戏账号 ID                                         |
| `buyer_id`           | VARCHAR(64)  | NOT NULL            | -                                               | -       | 购买者用户 ID                                         |
| `buyer_email`        | VARCHAR(64)  | NOT NULL            | -                                               | -       | 买家邮箱，仅用于会话信息记录和排查，不作为 WebSocket 主路由键             |
| `status`             | TINYINT      | NOT NULL            | 0                                               | 联合索引    | 支付会话状态：0-待支付，1-已支付，2-已失效，3-已过期                    |
| `expire_at`          | DATETIME     | NOT NULL            | -                                               | 联合索引    | 支付会话过期时间                                          |
| `created_at`         | DATETIME     | NOT NULL            | CURRENT\_TIMESTAMP                              | -       | 支付会话创建时间                                          |
| `updated_at`         | DATETIME     | NOT NULL            | CURRENT\_TIMESTAMP ON UPDATE CURRENT\_TIMESTAMP | -       | 支付会话更新时间                                          |

**索引设计：**

- **`PRIMARY KEY (id)`**：主键索引，支持按内部主键快速定位支付会话。
- **`UNIQUE KEY uk_payment_session_id (payment_session_id)`**：唯一索引，保证支付会话 ID 唯一，并支持支付主流程根据 `payment_session_id` 快速查询支付会话。
- **`UNIQUE KEY uk_order_sn (order_sn)`**：唯一索引，保证同一个订单只对应一个支付会话。重复提交订单确认时，应直接复用已有支付会话，而不是创建新会话。
- **`KEY idx_status_expire_at (status, expire_at)`**：联合索引，支持定时任务扫描已过期的待支付会话，例如查询 `status = 0 AND expire_at < NOW()` 的记录。

**格式：**

- **`payment_session_id`**：建议格式为 `PS{yyyyMMdd}{随机串或雪花ID}`，例如：`PS20260619000123456789`。
- **`order_sn`**：必须与 `orders.order_sn` 对应。
- **`account_id`**：必须与 `accounts.account_id` 对应。
- **`expire_at`**：具体由业务配置决定，在文档中注明：默认设置为有效期 5 分钟。

**状态说明：**

- **`0-待支付`**：支付会话已创建，二维码可使用。
- **`1-已支付`**：该支付会话已完成支付。
- **`2-已失效`**：订单已支付、取消、重新生成会话或被系统主动废弃。
- **`3-已过期`**：超过 `expire_at`，禁止继续支付。

**支付服务使用规则：**

- 页面 A 提交订单确认后，由后端创建或复用 `payment_session_id`。
- 当前版本中，同一个 `order_sn` 只允许存在一个支付会话。
- 页面 A 的 WebSocket 连接必须绑定 `payment_session_id`。
- 二维码 `qr_token` 中必须包含 `payment_session_id`、`order_sn`、`account_id`、`price`、`iat`、`exp`、`nonce` 等字段。
- 页面 B 扫码后提交的 `qr_token` 必须通过 JWT 验签，然后再通过 `payment_session_id` 查询支付会话。
- 支付成功时必须将 `payment_sessions.status` 从 `0` 更新为 `1`。
- 支付会话过期后必须拒绝继续支付。

