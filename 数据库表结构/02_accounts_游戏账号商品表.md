### 核心业务表：`accounts` (游戏账号商品表)

| 字段名             | 数据类型        | 约束                 | 默认值                                           | 索引           | 描述                                                |
| :-------------- | :---------- | :----------------- | :-------------------------------------------- | :----------- | :------------------------------------------------ |
| `id`            | BIGINT      | PK, AUTO_INCREMENT | -                                             | PRIMARY      | 账号内部主键 ID                                         |
| `account_id`    | VARCHAR(64) | UNIQUE, NOT NULL   | -                                             | UNIQUE       | 游戏账号编号（雪花算法生成）                                    |
| `seller_id`     | VARCHAR(64) | NOT NULL           | -                                             | INDEX        | 卖家用户 ID                                           |
| `game_name`     | VARCHAR(64) | NOT NULL           | -                                             | 联合索引         | 游戏名称                                              |
| `server_area`   | VARCHAR(64) | NOT NULL           | -                                             | 联合索引         | 区服名称                                              |
| `price`         | INT         | NOT NULL           | -                                             | 联合索引         | 价格，单位：分                                           |
| `account_level` | INT         | NOT NULL           | 0                                             | 联合索引         | 账号等级                                              |
| `hero_count`    | INT         | NOT NULL           | 0                                             | 联合索引         | 英雄 / 角色数量                                         |
| `skin_count`    | INT         | NOT NULL           | 0                                             | 联合索引         | 皮肤数量                                              |
| `rare_items`    | JSON        | NOT NULL           | `'[]'`                                        | -            | 珍稀道具展示快照，例如：`["龙狙", "火麒麟"]`，用于列表页和详情页展示，不作为核心检索依据 |
| `status`        | TINYINT     | NOT NULL           | 0                                             | INDEX / 联合索引 | 账号状态：0-在售，1-交易中，2-已售出，3-已下架                       |
| `version`       | INT         | NOT NULL           | 0                                             | -            | 乐观锁版本号，用于防并发超卖和买卖双方同时修改账号状态                       |
| `created_at`    | TIMESTAMP   | NOT NULL           | CURRENT_TIMESTAMP                             | INDEX        | 上架时间                                              |
| `updated_at`    | TIMESTAMP   | NOT NULL           | CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP | INDEX        | 最近更新时间                                            |

**索引设计：**

* **`PRIMARY KEY (id)`**：主键索引，支持按内部主键快速定位账号。
* **`UNIQUE KEY uk_account_id (account_id)`**：唯一索引，保证游戏账号编号唯一性，支持按账号编号查询账号详情。
* **`KEY idx_seller_id (seller_id)`**：普通索引，支持按卖家用户 ID 查询其上架账号。
* **`KEY idx_game_server_status_price (game_name, server_area, status, price)`**：联合索引，支持用户选择游戏名、区服、账号状态后按价格筛选、排序与分页。例如查询某游戏某区服下所有在售账号，并按价格升序展示。
* **`KEY idx_game_status_price (game_name, status, price)`**：联合索引，支持用户只选择游戏名、不限制区服时，按账号状态和价格筛选、排序与分页。例如查询某游戏下所有区服的在售账号，并按价格升序展示。
* **`KEY idx_game_server_status_skin_level_hero (game_name, server_area, status, skin_count, account_level, hero_count)`**：联合索引，支持账号列表页按游戏名、区服、账号状态、皮肤数量、账号等级、英雄 / 角色数量进行组合筛选。
* **`KEY idx_status (status)`**：普通索引，支持后台管理、定时任务或运营统计按账号状态筛选数据。
* **`KEY idx_created_at (created_at)`**：普通索引，支持按上架时间排序和范围查询。
* **`KEY idx_updated_at (updated_at)`**：普通索引，支持按最后更新时间查询最近变更账号，用于后台排查、缓存刷新或后续数据同步任务。
* **`rare_items` 字段不建立核心检索索引**：该字段主要用于账号列表页和详情页展示珍稀道具快照。珍稀道具筛选、组合查询、统计和风控应通过 `account_rare_items` 明细表实现。

**格式：**

* **`account_id`**：账号编号格式建议为 `ACC{yyyyMMdd}{雪花ID后12位}`，例如：`ACC20260517901234567890`。
* **`price`**：统一使用整数分保存。例如：`9900` 表示 `99.00` 元。禁止使用 `FLOAT` / `DOUBLE` 保存金额。
* **`rare_items`**：必须是合法 JSON 数组。例如：`["龙狙", "火麒麟"]`。该字段仅作为展示快照，不作为核心检索依据。
* **`account_level`**：账号等级，建议使用非负整数。
* **`hero_count`**：英雄 / 角色数量，建议使用非负整数。
* **`skin_count`**：皮肤数量，建议使用非负整数。
* **`version`**：乐观锁版本号，每次需要并发保护的账号状态修改成功后递增。
* **`rare_items`**：文档默认值记为 `'[]'`，实际 MySQL DDL 建议使用 `DEFAULT (JSON_ARRAY())`，或由业务层在插入账号时显式写入空 JSON 数组。

**状态说明：**

* **`0-在售`**：账号可被购买。
* **`1-交易中`**：账号已被订单锁定，正在支付或交易处理中。
* **`2-已售出`**：账号已完成交易，不允许再次购买。
* **`3-已下架`**：账号被卖家或平台下架，不允许购买。

**支付服务使用规则：**

* 页面 A 展示的账号价格必须以 `orders.price` 为最终交易价，以 `accounts.price` 作为账号当前标价或订单创建时的价格来源之一。
* 支付成功时，支付服务应在同一事务中将 `orders.status` 更新为 `1-已支付`，并将 `accounts.status` 更新为 `2-已售出`。
* 如果订单创建阶段已经将账号置为 `1-交易中`，支付服务不应再次把其他订单绑定到同一个 `account_id`。
* 支付事务中应基于唯一索引锁定 `orders`、`accounts`、`payment_sessions` 相关记录，保证订单状态、账号状态和支付会话状态一致。
* 支付服务更新账号状态时，应结合 `status` 条件更新，防止已售出、已下架或异常状态账号被重复交易。
* `accounts.rare_items` 只用于展示，不用于核心搜索。按珍稀道具搜索账号时，应查询 `account_rare_items` 明细表。
