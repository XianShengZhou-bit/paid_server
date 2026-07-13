### 扩展业务表：`account_rare_items` (账号珍稀道具明细表)

| 字段名 | 数据类型 | 约束 | 默认值 | 索引 | 描述 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `id` | BIGINT | PK, AUTO_INCREMENT | - | PRIMARY | 道具明细内部主键 |
| `account_id` | VARCHAR(64) | NOT NULL | - | INDEX / UNIQUE 联合索引 | 游戏账号 ID |
| `item_code` | VARCHAR(64) | NOT NULL | - | INDEX / UNIQUE 联合索引 | 道具稳定编码 |
| `item_name` | VARCHAR(64) | NOT NULL | - | INDEX | 道具展示名称 |

**索引设计：**

* **`PRIMARY KEY (id)`**：主键索引，支持按内部主键定位。
* **`KEY idx_account_id (account_id)`**：普通索引，支持查询某个账号拥有的全部珍稀道具。
* **`KEY idx_item_code_account (item_code, account_id)`**：联合索引，支持按道具编码反查拥有该道具的账号，并支持多个珍稀道具组合筛选。
* **`KEY idx_item_name (item_name)`**：普通索引，支持按道具展示名称进行辅助查询。正式检索建议优先使用 `item_code`。
* **`UNIQUE KEY uk_account_item_code (account_id, item_code)`**：唯一索引，防止同一账号重复写入同一个珍稀道具。

**格式：**

* **`account_id`**：必须与 `accounts.account_id` 对应。
* **`item_code`**：道具稳定编码，例如：`DRAGON_AWP`、`FIRE_QILIN`。后端检索和唯一性约束应优先使用该字段。
* **`item_name`**：道具展示名称，例如：`龙狙`、`火麒麟`。该字段用于展示和辅助查询，不建议作为唯一业务标识。

**使用规则：**

* 账号上架或更新时，应同步写入 `accounts` 和 `account_rare_items`。
* `accounts.rare_items` 只作为展示快照，不作为核心检索依据。
* 按珍稀道具搜索账号时，应优先使用 `account_rare_items.item_code` 查询。
* 同一账号同一道具只允许存在一条记录，通过 `uk_account_item_code (account_id, item_code)` 保证唯一性。
* 如果前端按道具名称搜索，应先将 `item_name` 映射为稳定的 `item_code`，再执行账号筛选查询。