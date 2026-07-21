### 支付服务表：`payment_attempts` (支付尝试记录表)

| 字段名               | 数据类型     | 约束               | 默认值                                        | 索引     | 描述                                       |
| :------------------- | :----------- | :----------------- | :-------------------------------------------- | :------- | :----------------------------------------- |
| `id`                 | BIGINT       | PK, AUTO_INCREMENT | -                                             | PRIMARY  | 支付尝试记录内部主键                       |
| `request_id`         | VARCHAR(64)  | UNIQUE, NOT NULL   | -                                             | UNIQUE   | 支付请求唯一 ID，用于接口幂等              |
| `payment_session_id` | VARCHAR(64)  | NOT NULL           | -                                             | INDEX    | 支付会话 ID                                |
| `order_sn`           | VARCHAR(64)  | NOT NULL           | -                                             | INDEX    | 订单号                                     |
| `account_id`         | VARCHAR(64)  | NOT NULL           | -                                             | -        | 游戏账号 ID，用于记录支付尝试时的账号快照  |
| `buyer_id`           | VARCHAR(64)  | NOT NULL           | -                                             | -        | 购买者用户 ID，用于记录支付请求来源        |
| `result`             | TINYINT      | NOT NULL           | 0                                             | -        | 支付尝试结果：0-处理中，1-成功，2-失败     |
| `fail_reason`        | VARCHAR(255) | NULL               | NULL                                          | -        | 支付失败原因，不允许记录支付密码等敏感信息 |
| `client_ip`          | VARCHAR(64)  | NULL               | NULL                                          | -        | 发起支付请求的客户端 IP                    |
| `user_agent`         | VARCHAR(512) | NULL               | NULL                                          | -        | 客户端浏览器或设备信息                     |
| `created_at`         | DATETIME     | NOT NULL           | CURRENT_TIMESTAMP                             | 联合索引 | 支付尝试创建时间                           |
| `updated_at`         | DATETIME     | NOT NULL           | CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP | 联合索引 | 支付尝试更新时间                           |

**索引设计：**

- **`PRIMARY KEY (id)`**：主键索引，支持按内部主键快速定位支付尝试记录。
- **`UNIQUE KEY uk_request_id (request_id)`**：唯一索引，保证支付请求幂等，防止同一支付请求被重复处理。
- **`KEY idx_payment_session_id (payment_session_id)`**：普通索引，支持按支付会话查询该会话下的全部支付尝试记录。
- **`KEY idx_order_sn (order_sn)`**：普通索引，支持按订单号查询支付尝试记录，便于订单维度排查。
- **`KEY idx_created_at_updated_at (created_at, updated_at)`**：联合索引。为后台可能需要频繁按时间范围统计支付尝试记录而创建。

**格式：**

- **`request_id`**：建议格式为 `REQ{yyyyMMdd}{随机串或雪花ID}`，例如：`REQ20260619000123456789`。
- **`payment_session_id`**：必须与 `payment_sessions.payment_session_id` 对应。
- **`order_sn`**：必须与 `orders.order_sn` 对应。
- **`account_id`**：必须与 `accounts.account_id` 对应，用于记录本次支付尝试发生时的账号快照。
- **`buyer_id`**：必须与订单中的购买者用户 ID 对应，用于记录本次支付尝试的请求来源。
- **`fail_reason`**：只允许记录错误类型和脱敏描述，例如：`PAY_PASSWORD_ERROR`、`SESSION_EXPIRED`、`ORDER_ALREADY_PAID`、`REQUEST_PROCESSING`。禁止记录支付密码、身份证号、完整 Token 等敏感信息。

**状态说明：**

- **`0-处理中`**：支付请求已接收，正在处理。
- **`1-成功`**：支付请求处理成功。
- **`2-失败`**：支付请求处理失败。

**支付服务使用规则：**

- 每次页面 B 提交支付请求时，必须生成或携带 `request_id`。
- 后端必须基于 `request_id` 做幂等控制。
- 后端接收到支付请求后，应优先查询或插入 `payment_attempts` 记录，确认当前 `request_id` 是否已经被处理。
- 如果重复收到相同 `request_id`：
  - 已成功则直接返回成功结果；
  - 已失败则返回失败原因；
  - 处理中则返回处理中、拒绝重复提交。
- `payment_attempts` 只保存支付尝试记录，不保存支付密码。
- 支付成功时，应在同一事务中更新 `orders`、`accounts`、`payment_sessions` 和当前 `payment_attempts` 记录，保证订单状态、账号状态、支付会话状态和支付尝试结果一致。
- 支付失败时，应记录 `result = 2` 和脱敏后的 `fail_reason`，便于后续排查。
- 该表用于支付请求幂等控制、失败排查、重复提交处理、审计记录和后续风控分析。

**`request_id` 生成方法和流程：**

- **生成主体**：`request_id` 由后端支付服务生成，前端不负责生成。前端只负责在支付请求中携带后端签发的 `request_id`。后端通过数据库唯一索引 `uk_request_id (request_id)` 对支付请求进行幂等控制。

- **生成格式**：建议格式为 `REQ{yyyyMMdd}{雪花ID}`，例如：`REQ20260619000123456789`。其中 `REQ` 表示支付请求，`yyyyMMdd` 表示生成日期，后半部分由后端使用雪花 ID 生成，保证全局唯一。

- **生成时机**：页面 B 扫码进入支付页后，前端应先调用后端接口初始化支付请求，例如 `POST /api/payment/attempt/init`。后端在校验 `qr_token`、`payment_session_id`、订单状态、支付会话状态和过期时间无误后，生成本次支付请求对应的 `request_id`，并返回给页面 B。

- **前端保存规则**：前端收到后端返回的 `request_id` 后，应将其与当前 `payment_session_id` 绑定并暂存到 `sessionStorage` 中，例如使用 `pay_request_id:{payment_session_id}` 作为本地存储键。前端后续点击“确认支付”时，应携带该 `request_id` 提交支付请求。

- **支付提交流程**：用户在页面 B 输入支付密码并点击“确认支付”后，前端向后端提交 `request_id`、`qr_token`、支付密码等必要参数。后端收到请求后，基于 `request_id` 查询或插入 `payment_attempts` 记录，并通过 `uk_request_id (request_id)` 唯一索引保证同一支付请求不会被重复处理。

- **重试复用规则**：如果支付请求已经发出，但出现网络超时、页面刷新、响应丢失、用户重复点击等情况，前端应继续复用同一个 `request_id` 重新提交。这样后端可以识别为同一次支付请求的重试，避免重复处理。

- **重新生成规则**：如果后端已明确返回支付失败，并且允许用户修改支付信息后重新发起一次新的支付尝试，则前端应重新调用 `POST /api/payment/attempt/init` 获取新的 `request_id`。如果后端返回支付成功，应清除前端本地暂存的 `request_id`。

- **后端校验规则**：后端处理支付请求时，不能只信任前端提交的 `request_id`。后端必须同时校验 `qr_token`、`payment_session_id`、`order_sn`、`account_id`、订单状态、支付会话状态和过期时间。`request_id` 只用于识别“同一次支付提交请求”，不作为订单、金额、账号或支付权限的可信依据。

- **篡改处理规则**：如果前端提交的 `request_id` 不存在、格式非法、已绑定其他支付会话，或与当前 `payment_session_id` 不匹配，后端应拒绝本次支付请求，并返回参数非法或支付请求无效的错误结果。

- **重复请求处理规则**：如果重复收到相同 `request_id`，当原记录 `result = 1` 时直接返回支付成功；当 `result = 2` 时返回原失败原因；当 `result = 0` 时返回处理中或拒绝重复提交。

- **核心原则**：`request_id` 由后端生成，前端只负责携带；同一次已发出的支付请求重试时，`request_id` 必须相同；新的支付尝试，必须重新向后端申请新的 `request_id`。
