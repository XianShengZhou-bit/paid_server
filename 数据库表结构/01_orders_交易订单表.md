### 核心业务表：`orders` (交易订单表)

| 字段名 | 数据类型 | 约束 | 默认值 | 索引 | 描述 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `id` | BIGINT | PK, AUTO_INCREMENT | - | PRIMARY | 订单内部主键 |
| `order_sn` | VARCHAR(64) | UNIQUE, NOT NULL | - | UNIQUE | 订单号（雪花算法生成） |
| `account_id` | VARCHAR(64) | NOT NULL | - | INDEX | 关联游戏账号 ID |
| `buyer_id` | VARCHAR(64) | NOT NULL | - | INDEX | 购买者用户 ID |
| `price` | INT | NOT NULL | - | - | 交易金额，单位：分 |
| `buyer_email` | VARCHAR(64) | NOT NULL | - | - | 买家预留邮箱 |
| `buyer_id_card_hash` | VARCHAR(128) | NOT NULL | - | - | 买家身份证号哈希值，用于一致性校验，不保存明文身份证号 |
| `buyer_id_card_masked` | VARCHAR(32) | NOT NULL | - | - | 买家身份证号脱敏值，用于后台排查和审计展示 |
| `status` | TINYINT | NOT NULL | 0 | INDEX | 订单状态：0-待支付，1-已支付，2-已取消，3-已过期 |
| `create_time` | DATETIME | NOT NULL | CURRENT_TIMESTAMP | INDEX | 订单创建时间 |
| `paid_at` | DATETIME | NULL | NULL | INDEX | 支付完成时间 |
| `update_time` | DATETIME | NOT NULL | CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP | INDEX | 订单更新时间 |

**索引设计：**

* **`PRIMARY KEY (id)`**：主键索引，支持按内部主键快速定位订单。
* **`UNIQUE KEY uk_order_sn (order_sn)`**：唯一索引，保证订单号唯一性，支持按订单号快速查询。
* **`KEY idx_account_id (account_id)`**：普通索引，支持按游戏账号 ID 查询该账号的交易记录。
* **`KEY idx_buyer_id (buyer_id)`**：普通索引，支持按购买者用户 ID 查询其所有订单。
* **`KEY idx_status (status)`**：普通索引，支持按订单状态筛选。
* **`KEY idx_create_time (create_time)`**：普通索引，支持按订单创建时间排序和范围查询。
* **`KEY idx_paid_at (paid_at)`**：普通索引，支持按支付完成时间查询和对账。
* **`KEY idx_update_time (update_time)`**：普通索引，支持增量同步和排查最近变更订单。

**格式：**

* **`order_sn`**：订单号格式 `ORD{yyyyMMdd}{雪花ID后12位}`，例如：`ORD20260517901234567890`。
* **`price`**：统一使用整数分保存。例如：`9900` 表示 `99.00` 元。禁止使用 `FLOAT` / `DOUBLE` 保存金额。
* **`buyer_id_card_hash`**：建议使用后端服务密钥参与计算的哈希，例如 `HMAC-SHA256(id_card)`。禁止前端生成后直接作为可信值。
* **`buyer_id_card_masked`**：示例：`4403**************00`。

**状态说明：**

* **`0-待支付`**：订单已由外部系统创建，但尚未支付完成。
* **`1-已支付`**：支付服务已完成支付处理，订单进入已支付状态。
* **`2-已取消`**：订单被业务系统取消。
* **`3-已过期`**：订单超过允许支付时间，禁止继续支付。

**支付服务使用规则：**

* 支付服务不负责创建订单，只读取并更新外部系统已创建的订单。
* 支付成功时只能将 `status` 从 `0` 更新为 `1`，并写入 `paid_at` 与 `update_time`。
* 支付事务中必须基于 `order_sn` 唯一索引使用 `SELECT ... FOR UPDATE` 锁定订单行，并结合 `status = 0` 条件更新，防止并发重复支付。
* 页面 A 提交的邮箱和身份证号只能用于校验，最终订单金额、账号 ID、订单状态必须以数据库为准。
