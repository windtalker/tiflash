# TiFlash 支持 NullEQ Join Key（`<=>` / `tidbNullEQ`）的改动探索

> 目标：让 TiFlash 的 Hash Join 在 **Join Key 使用 Null-safe equal（nulleq）语义**时正确工作：  
> - `NULL <=> NULL` 为 `true`（可匹配）  
> - `NULL <=> non-NULL` 为 `false`  
> - `non-NULL <=> non-NULL` 等价于 `=`
>
> 这与当前 TiFlash Join 的默认语义（`NULL` 不参与等值匹配）不同，因此需要引入一条“NullEQ join key”路径。

---

## 1. 现状：TiFlash Join 对 NULL Join Key 的核心假设

### 1.1 Join 的语义假设写在注释里

`dbms/src/Interpreters/Join.h:153` 起明确写了：

- `NULLs never join to anything, even to each other.`
- Build 阶段跳过含 NULL key 的行；Probe 阶段把含 NULL key 的行当作 not matched。  

这直接与 NullEQ join key 的目标冲突。

### 1.2 关键路径：把 Nullable key “去 Nullable”，并用 null_map 直接跳过

**(A) initBuild 时用于选 JoinMapMethod 的 key_columns 会强制拿 nested column**  
`dbms/src/Interpreters/Join.cpp:54-66`：

- `getKeyColumns()` 会把 `ColumnNullable` 改成 nested column，并写了注释“只 join 非 NULL key”。
- `Join::initBuild()` 会调用 `chooseJoinMapMethod(getKeyColumns(...))`，意味着 join 的 map method 默认建立在“key 不含 NULL”的前提上。

**(B) Build 阶段插入哈希表前，会把所有 key 的 null map OR 到一起，然后跳过这些行**  
`dbms/src/Interpreters/Join.cpp:678-724`：

- `extractNestedColumnsAndNullMap(key_columns, null_map_holder, null_map)`  
  - 会把 key_columns 全部替换成 nested column（丢掉 per-column null 信息）
  - 生成 `null_map`（任何 key 列为 NULL 就置 1）
- 之后 `recordFilteredRows(...)` 复用同一个 `null_map`，把“右表 side-condition 不通过”的行也置 1
- `JoinPartition::insertBlockIntoMaps(..., null_map, ...)` 在插入时遇到 `null_map[i] == 1` 就 `continue`，必要时记录到 `RowsNotInsertToMap`（供 RIGHT/FULL/RightSemi/RightAnti/NullAware 用）

**(C) Probe 阶段也同样做“抽 nested + 复用 null_map”**  
`dbms/src/Interpreters/ProbeProcessInfo.cpp:47-103`：

- `prepareForHashProbe()`：
  - `extractNestedColumnsAndNullMap(...)` 把 key 改成 nested、把 NULL key 行打到 `null_map`
  - `recordFilteredRows(...)` 再把左表 side-condition 不通过的行打到同一张 `null_map`
- `JoinPartition::probeBlockImplTypeCase()`（`dbms/src/Interpreters/JoinPartition.cpp:1490+`）如果 `null_map[i]` 为真，会直接走 `addNotFound()`，不会 probe hash table。

### 1.3 Hash table 目前“默认不支持 Nullable key”

Join 的 KeyGetter 类型定义里，`keys128/keys256` 用的是：

- `ColumnsHashing::HashMethodKeysFixed<..., /*has_nullable_keys=*/false, ...>`  
  `dbms/src/Interpreters/JoinPartition.cpp:432-441`

这与 HashAgg 不同：HashAgg 在存在 nullable group key 时，会选择 `nullable_keys128/256` 或 serialized（见 `dbms/src/Interpreters/Aggregator.cpp:534-548`）。

结论：当前 Join 的实现是“**从 key_columns 层面把 NULL 彻底排除**”，而不是像 group-by 那样“把 NULL 编码进 key 并参与 hash/eq”。

---

## 2. 需求拆解：NullEQ join key 对 Join 引擎意味着什么

为了让 `NULL <=> NULL` 可 join，需要同时满足两件事：

1. **Build 侧含 NULL key 的行必须进入 hash table**（不能再被 `extractNestedColumnsAndNullMap` + `continue` 过滤掉）
2. **key 的 hash/eq 必须把 NULL 性质编码进去**，否则：
   - `NULL` 只是 nested column 的默认值（例如 0 / empty string），会与真实值冲突
   - 或者因为 probe 端也跳过 NULL，根本不会匹配

更具体地说，NullEQ join key 的 key 等价关系是：

- 以每列为单位：`(is_null, value)` 作为 join key component
- 多列时：对每列做上述扩展，再整体作为 key tuple

这和 SQL `GROUP BY` 中 NULL 的处理方式非常接近（group by 会把所有 NULL 分到同一组），因此可以参考 HashAgg 现有的 nullable key 支持路径。

---

## 3. 一个关键现实问题：TiFlash 如何知道“这个 join key 是 NullEQ”？

### 3.1 tipb::Join 当前协议里没有表达 NullEQ join key 的字段

在 `contrib/tipb/proto/executor.proto` 的 `message Join` 里：

- 有 `left_join_keys` / `right_join_keys`（等值 join key expression）
- 有 `left_conditions` / `right_conditions` / `other_conditions`
- 只有 `is_null_aware_semi_join`（用于 NOT IN 的 NullAware anti/semi 家族），**不是 NullEQ join key**
- 没有类似 `join_key_is_nullsafe` 的字段

因此仅修改 TiFlash Join 引擎还不够：需要**定义 DAGRequest/MPPRequest 如何携带“这个 key 用 `<=>`”的语义**。

### 3.2 两条可行路线（需要 TiDB / 协议配合）

#### 路线 A（推荐）：扩展 tipb::Join，显式标注 join key 的比较语义（按 key 粒度）

例如新增一个数组字段（名称仅示例）：

- `repeated bool is_null_eq = ...;`（长度与 join key 数一致）
  - `is_null_eq[i] = false`：第 `i` 个 key 使用普通 `=` 语义（NULL 不匹配任何行）
  - `is_null_eq[i] = true`：第 `i` 个 key 使用 Null-safe equal 语义（`NULL <=> NULL` 为 true）

也可以用更显式的枚举：

- `repeated JoinKeyCmp join_key_cmp = ...;`
- `enum JoinKeyCmp { EQ = 0; NULL_SAFE_EQ = 1; }`

优点：
- TiFlash 可以按 key 精确实现：支持 “部分 key NullEQ、部分 key EQ” 的混合场景
- 不需要 TiDB 做额外的 key 展开（不会把一个 `<=>` 变成两个 key）

缺点：
- 需要改 kvproto/tipb 并升级上下游

#### 路线 B：TiDB 在计划阶段把 `<=>` 改写成纯 EQ key（引擎不用懂 NullEQ）

路线 B 的核心思想是：**把 NULL 的信息编码进 join key**，从而把
`(a <=> b)` 变成纯等值 join key（TiFlash 继续做普通 `=` join 即可）。

对每个 `<=>` key `(a <=> b)`，展开成两组等值 key（每个 `<=>` 变成 2 个 key）：

1. `isNull(a)` 与 `isNull(b)`（0/1，非 NULL）
2. `ifNull(a, S)` 与 `ifNull(b, S)`（或 `coalesce(a, S)` / `coalesce(b, S)`，保证非 NULL）

这样 TiFlash 仍然可以用当前 “NULL key 不 join” 的实现，因为经过改写后 join key 自身不再产生 NULL。

##### 3.2.B.1 为什么这等价于 `<=>`（直观证明）

令新 key 为二元组：

- `k(a) = (isNull(a), ifNull(a, S))`

那么对任意 `a,b`：

- 若 `a,b` 都非 NULL：`isNull(a)=isNull(b)=0`，且 `ifNull(a,S)=a`、`ifNull(b,S)=b`  
  所以 `k(a)=k(b)` ⇔ `a=b` ⇔ `a <=> b`
- 若 `a` 为 NULL、`b` 非 NULL：`isNull(a)=1, isNull(b)=0` ⇒ 第一维就不相等 ⇒ `k(a)≠k(b)` ⇔ `(a <=> b)=false`
- 若 `a,b` 都为 NULL：`isNull(a)=isNull(b)=1`，且 `ifNull(a,S)=ifNull(b,S)=S`  
  所以 `k(a)=k(b)` ⇔ `(a <=> b)=true`

注意：这里的 `S` **不需要“避开真实值域”**。即使 `S` 恰好等于某些非 NULL 的真实值（例如整型用 `0`），
也不会导致“NULL 与 0 匹配”，因为 `isNull(NULL)=1` 而 `isNull(0)=0`。

##### 3.2.B.2 多列 key / 混合 key 的处理

如果 join 条件是：

- `t1.k1 <=> t2.k1 AND t1.k2 = t2.k2`

则 join key 列表可以变成：

- `k2`（保留原来的 EQ key）
- `isNull(k1)`（新增）
- `ifNull(k1, S1)`（新增）

一般化：

- `=` key：保持不变
- `<=>` key：每个 key 展开成 `isNull(...)` + `ifNull(..., S)` 两个 key

因此 join key 个数会增加，hash key 也会变宽。

##### 3.2.B.3 `S`（sentinel 常量）怎么选

只要满足：

1. **非 NULL**
2. **可被安全表示为 join key 的“公共类型”**（建议先 cast 到 join key 的 common type，再做 ifNull）
3. 对字符串：**确保两边 collation 一致**（通常通过显式 cast/field type 即可）

就可以。常见选择：

- 整型/浮点/decimal：`0`
- 字符串/二进制：`''`
- 日期时间：选择一个稳定合法值（例如 `1970-01-01` / `1970-01-01 00:00:00`），避免依赖 SQL mode 对 “zero date” 的处理差异
- duration：`00:00:00` 或 `0`

##### 3.2.B.4 这个改写为什么能“绕开 TiFlash join 的 NULL 假设”

TiFlash 当前 Hash Join 的关键假设是：key 列是 Nullable 时，会用 `extractNestedColumnsAndNullMap(...)`
把 NULL 行打到 `null_map`，从而 build/probe 时直接跳过。

路线 B 里：

- `isNull(...)` 结果本身不为 NULL
- `ifNull(..., S)`/`coalesce(..., S)` 也保证结果不为 NULL

因此 join key 列在执行引擎里要么是非 Nullable，要么是 Nullable 但 null-map 全 0；最终不会触发 “跳过 NULL key 行”。
TiFlash 不需要理解 `<=>`，仍然走普通等值 join key 逻辑即可得到 `<=>` 语义。

优点：
- TiFlash Join 引擎改动小（甚至无需改）
- 不需要 hash table 支持 Nullable key

缺点：
- 每个 `<=>` key 会变成 2 个 key，增加计算、内存、hash key 宽度
- 需要确保 `S` 的类型/取值在所有 join key 类型上都合法且稳定（字符串/decimal/collation/时间类型 都要考虑）
- 计划可读性变差，且 runtime filter/key cast 等逻辑会更复杂

本次文档后续默认按路线 A（TiFlash 原生支持 NullEQ join key）进行改动探索。

### 3.3 本文默认的输入契约（来自 TiDB planner / 协议）

为了让 TiFlash 侧实现可控、避免把语义“分散”到 join key / other_conditions / runtime filter 多条路径里，本文后续默认假设：

1. **下发方式固定为 “join key pair + per-key `is_null_eq[]` 标记”**  
   - `<=>` 语义只通过 `is_null_eq[i]=true` 表达，不会把 `<=>` 留在 `other_conditions` 里（否则 TiFlash 侧需要额外实现 join filter 的 `<=>` 语义与 pushdown/性能策略，不在本文范围）。
2. **Join key 下发为列引用（ColumnRef），不会是任意 expression**  
   - TiDB planner 保证 `left_join_keys[i]` / `right_join_keys[i]` 对应的都是 column。  
   - TiFlash 侧可能为了左右两边类型对齐在执行层插入 cast（例如两边列类型不完全一致时），但这属于实现细节：join key 的“对齐顺序/数量”不变，`is_null_eq[i]` 仍按 index 与 key 对齐。
3. **fixed packed keys 路径不涉及 collation**  
   - fixed packed keys 只适用于 fixed-size 的 join key type（数值/时间等），而 collation 仅对 string 生效。  
   - TiFlash 里虽存在 FixedString 类型，但 TiDB 目前没有对应的固定长度 string 类型，因此 TiDB/TiFlash 路径下不会产生 “FixedString join key 走 packed keys” 的场景。  
   - 若未来 TiDB 引入 FixedString（或 TiFlash join key 支持固定长度 string），需要重新评估：collator 是否会影响 hash/eq、以及 packed key 的编码方式是否需要包含 collation 语义。

---

## 4. TiFlash Join 引擎层面的主要改动点（路线 A）

下面用“join key 级别的比较模式”为中心展开。核心思想：

- **把现在的 `null_map` 从“key-NULL + side-filter”的混用，拆成两个概念**  
  - `row_filter_map`：由 left/right side-condition 产生（为 false 或 NULL 的行应该当作 not matched / not inserted）  
  - `key_null_map`：只用于 **EQ key**（非 NullEQ）——这些 key 的 NULL 行不能匹配，应当被过滤  
  - 对 NullEQ key，NULL 不应进入 `key_null_map`
- Build/Probe 传到 JoinPartition 的 `null_map` 参数应当表示 “**这一行无需 probe/insert**”，而不再默认包含“key 是 NULL”。

### 4.1 Join / Planner：增加一个 join key 的“比较模式”配置并向下传递

入口在 `dbms/src/Flash/Planner/Plans/PhysicalJoin.cpp`：

- 目前 `std::make_shared<Join>(probe_key_names, build_key_names, ...)` 没有 NullEQ 信息
- 需要在 JoinInterpreterHelper::TiFlashJoin 或 PhysicalJoin 构造 Join 时，把从 tipb::Join 解析出的 `is_null_eq[]` 传入 Join（按 key 粒度）

建议：
- Join 保存一个 `std::vector<UInt8> is_null_eq;`，与 key_names_left/right 对齐（避免 `std::vector<bool>` 的坑）  
  - `is_null_eq[i] = 0`：第 i 个 key 走普通 `=`（任一侧为 NULL 就不匹配）  
  - `is_null_eq[i] = 1`：第 i 个 key 走 NullEQ（`NULL <=> NULL` 可匹配）  
- 结合 TiDB planner 的输入契约（见 3.3）：
  - join key 视为“列引用”，TiFlash 侧如需类型对齐可插入 cast，但 `is_null_eq` 必须按 key index 保持一致
- 解析/校验策略建议：
  - 若 tipb 未下发该字段：默认全 0（保持现状）
  - 若长度与 join key 数不一致：建议直接报错（更安全），或保守地按 `min(len)` 读取并对缺失部分补 0，同时打 warning

### 4.2 initBuild：chooseJoinMapMethod 需要支持 Nullable key（仅在 NullEQ key 语义下）

当前 `chooseJoinMapMethod`（`dbms/src/Interpreters/JoinHashMap.cpp`）只根据 `key_columns[j]->isFixedAndContiguous()`、`isNumeric()` 等判断。

由于 `ColumnNullable` 往往不满足 fixed/contiguous，它会把很多本可走 `key64/keys128` 的场景降级成 `serialized`，性能可能大幅下降。

建议参考 Aggregator 的做法（`dbms/src/Interpreters/Aggregator.cpp:534-548`）：

- 对 NullEQ key：
  - 先“按 removeNullable 的逻辑”统计 `keys_bytes`（基于 nested type/column 的 size）
  - 如果所有 key 都是 fixed 且无 collator（与 HashAgg 逻辑一致），并且 `bitmap + keys_bytes <= 16/32`：
    - 选择 `JoinMapMethod::keys128` / `keys256`（但后面 KeyGetter 要用 `has_nullable_keys = true`）
  - 否则 fallback 到 `JoinMapMethod::serialized`
- 说明：上面的 “无 collator” 在 TiDB/TiFlash 的 join key 场景下基本等价于 “非 string 的 fixed-size type”  
  - collation 只对 string 生效，而 TiDB 下发的 string join key 也不是 fixed-size；因此 string key 不会走 `keys128/keys256` packed 路径。  
  - TiFlash 的 FixedString 类型是 ClickHouse 遗留类型，但 TiDB 当前不会产生该类型的 join key；如果未来引入 FixedString，需要重新审视 packed keys 与 collator 的约束。
- 重要：对 nullable numeric / datetime / fixed-size key，不要再选 `key8/key16/key32/key64`，因为这些 KeyGetter 无法编码 NULL bitmap（应使用 `keys128/keys256` + bitmap 或 serialized）

对应代码点：
- `Join::initBuild()` 调用 `chooseJoinMapMethod(getKeyColumns(...))`  
  `dbms/src/Interpreters/Join.cpp:392-404`
- `getKeyColumns()` 目前强制取 nested，需要在 NullEQ 模式下改成“保留 Nullable column”，或者重写一个 `getKeyColumnsForJoinMapMethod(...)`。

### 4.3 Build 阶段：不再对 NullEQ key 调用 extractNestedColumnsAndNullMap

现在 build 路径（`Join::insertFromBlockInternal`）的 `null_map` 同时承载：

- key NULL（来自 `extractNestedColumnsAndNullMap`）
- right side-condition false/NULL（来自 `recordFilteredRows`）

对 NullEQ join key，这会把 NULL key 行过滤掉，直接错误。

建议改造为：

1. `key_columns = extractAndMaterializeKeyColumns(...)` 后 **不要**调用 `extractNestedColumnsAndNullMap`（至少对 NullEQ key 不调用）
2. 单独计算 `row_filter_map`：
   - 用全新的 `ColumnPtr row_filter_map_holder; ConstNullMapPtr row_filter_map;`
   - 只调用 `recordFilteredRows(block, right_filter_column, row_filter_map_holder, row_filter_map);`
3. 对混合 EQ/NullEQ key（如果支持 per-key）：
   - 仅对 EQ key 的 nullable 列 OR null map 到 `row_filter_map`
   - NullEQ key 的 null map 不进入 `row_filter_map`
4. 传给 `JoinPartition::insertBlockIntoMaps(..., null_map= row_filter_map, ...)`
5. `RowsNotInsertToMap` 只应记录：
   - right/full/right-anti 等需要保留 build side 行时：右表 side-condition 不通过的行（这些行不会进入 map）
   - 如果有 EQ key：EQ key 为 NULL 的行（不会 join）
   - **不应**记录 NullEQ key 为 NULL 的行（它们应该入 map 并可被 matched）

相关代码点：
- `dbms/src/Interpreters/Join.cpp:678-685`（当前会抽 nested + 复用 null_map）
- `dbms/src/Interpreters/JoinPartition.cpp:531+`（insert 时 `if ((*null_map)[i]) continue;`）
- `dbms/src/DataStreams/ScanHashMapAfterProbeBlockInputStream.cpp:398-427`（scan 时先输出 rows_not_inserted_to_map 的行，这一块一旦混入 NullEQ NULL-key 行就会产生错误输出）

### 4.4 Probe 阶段：prepareForHashProbe 不再把 NullEQ key 的 NULL 行打进 null_map

Probe 侧同理：

- `ProbeProcessInfo::prepareForHashProbe()` 当前在 `extractNestedColumnsAndNullMap` 后把 key NULL 行置 1，并且在 `JoinPartition::probeBlockImplTypeCase` 直接走 `addNotFound()`，不会 probe map。
- NullEQ join key 下必须允许 NULL key 行 probe map。

建议与 build 同样拆分：

1. `hash_join_data->key_columns = extractAndMaterializeKeyColumns(...)`
2. 计算 `row_filter_map` 只来自 left side-condition（`recordFilteredRows`），以及 EQ key 的 NULL（如有）
3. `computeDispatchHash()`（在 spill enabled & not spilled 或 fine-grained shuffle 的 “virtual dispatch” 路径下需要）必须对 NullEQ key **包含 NULL 信息**，因此不能在此之前把 Nullable key 强行替换成 nested column

相关代码点：
- `dbms/src/Interpreters/ProbeProcessInfo.cpp:62-101`
- `computeDispatchHash()` 使用 `Column::updateHashWithValue()`，对 `ColumnNullable` 理论上会把 NULL 也编码进 hash（所以关键在于：不要提前把 key_columns 改成 nested）。

### 4.5 JoinPartition：KeyGetter 要能把 NULL 编码进 key

核心难点是 `JoinPartition.cpp` 里 KeyGetter 类型是 compile-time 绑定的：

- `keys128/keys256` 目前固定用 `HashMethodKeysFixed<..., has_nullable_keys=false, ...>`  
  `dbms/src/Interpreters/JoinPartition.cpp:432-441`

要支持 NullEQ key，有两种实现策略：

#### 策略 1（MVP，最小侵入）：NullEQ 模式统一走 `JoinMapMethod::serialized`

只要确保：
- key_columns 保留 `ColumnNullable`
- 不再过滤 NULL key 行

则 `HashMethodSerialized` 会通过 `serializeValueIntoArena()` 把 NULL 信息编码进 key（`ColumnNullable` 的 serialize 会写入 nullness），自然实现 `NULL == NULL` 的 key 等价。

优点：
- 不需要改 KeyGetterForTypeImpl
- 不需要改 chooseJoinMapMethod（可以强制覆盖为 serialized）

缺点：
- 性能风险最大（大量 arena 分配和序列化）

#### 策略 2（推荐/优化）：对 fixed keys 走 `keys128/keys256 + has_nullable_keys=true`

复用 HashAgg 的 key 打包逻辑：

- `ColumnsHashing::HashMethodKeysFixed<Value, UInt128/UInt256, Mapped, /*has_nullable_keys=*/true, ...>`
  - 内部会生成 bitmap（`BaseStateKeysFixed::createBitmap`）并用 `packFixed(..., bitmap)` 把 nullness 编进 key blob（见 `dbms/src/Common/ColumnsHashingImpl.h:295-352` 与 `dbms/src/Interpreters/AggregationCommon.h` 的 bitmap packing）

实现上有两种落地方式：

- 2.1 保持 `JoinMapMethod` 不变，但在 insert/probe 时根据 `Join::key_cmp`（或 `Join::has_nullable_key_for_key_getter`）选择不同的 KeyGetter 类型分支
- 2.2 新增 JoinMapMethod 枚举值（类似 Aggregator 的 `nullable_keys128/nullable_keys256`），用 method 区分 key getter（但会波及 `APPLY_FOR_JOIN_VARIANTS` 的所有 switch）

建议按侵入性选择：
- MVP 先做策略 1 保正确性
- 后续再做策略 2 把常见 nullable numeric / datetime / fixedstring key 拉回高性能路径

### 4.6 RowsNotInsertToMap 与 scan-after-probe：语义要重新校准

目前 `RowsNotInsertToMap` 的定位（`dbms/src/Interpreters/JoinPartition.h:225+` 注释）包含：

1. NULL join key rows
2. 被 right join conditions 过滤的 rows

在 NullEQ join key 下：

- “NULL join key rows” **不再默认属于 not-inserted**，它们应该入 map
- not-inserted 应该只剩下：
  - build side-condition 不通过（保留侧 outer join 需要输出的行）
  - EQ key 的 NULL 行（如果混合 key 比较模式）

如果不调整，`ScanHashMapAfterProbeBlockInputStream` 会在 `fillColumns()` 里首先输出这些 not-inserted rows（`dbms/src/DataStreams/ScanHashMapAfterProbeBlockInputStream.cpp:398-427`），导致 NullEQ NULL-key 行被当作“必然 unmatched”输出，错误。

---

## 5. RuntimeFilter：NullEQ join key 下需要特别小心（可能直接禁用）

### 5.1 NullAware Join（NOT IN）与 NullEQ 的交互（需要单独梳理）

TiFlash 里已有 `is_null_aware_semi_join`（用于 NOT IN 的 NullAware semi/anti 家族）。这条路径与 “NullEQ join key” 是两种不同的语义：

- NullAware join 的关注点是 **`NOT IN` 的三值逻辑**：NULL 的存在会影响结果（例如 build side 出现 NULL 可能导致 probe side 某些行返回 NULL/unknown，而不是简单的 true/false）。
- NullEQ join key 的关注点是 **等值 join key 的比较语义**：`NULL <=> NULL` 需要可匹配。

两者在实现上最容易冲突的点是：历史 Join 代码把 “key NULL 行” 作为一类特殊行处理（不入 map，并通过 `RowsNotInsertToMap` 等机制在 outer/scan-after-probe 或 NullAware 分支里兜底）。而 NullEQ 恰恰要求 “NULL key 行可入 map 并参与匹配”。

因此实现建议（至少在 MVP 阶段）要做到：

1. **严格按开关隔离语义**：仅在 `is_null_eq[i]=1` 的 join 上启用 NullEQ 路径，不要“顺手”改动 NullAware join 的历史假设。
2. **明确互斥/优先级**：如果理论上 planner 可能同时下发 NullAware + NullEQ（例如 `NOT IN` + `<=>` 的混合语义），需要在协议/Planner 侧定义是否允许；TiFlash 侧建议 fail-fast 或定义清晰的优先级（否则非常容易出现 silent wrong result）。
3. **重新校准 shared data structure 的语义**：如 `null_map`、`RowsNotInsertToMap`、scan-after-probe 的 used flag 等，要避免把 “NullEQ 的 NULL key 行” 误当成 “必然 unmatched / not inserted”。

当前 runtime filter（IN 类型）依赖 `Set`：

- `Set::insertFromBlock()` 会调用 `extractNestedColumnsAndNullMap` 并“只插入全非 NULL key”  
  `dbms/src/Interpreters/Set.cpp:101-120, 145-164`

对于普通 EQ join，这没问题，因为 build side NULL key 本就不入 join map。

但对 NullEQ join key：

- build side 的 NULL key **会参与 join**，因此 runtime filter 如果丢掉 NULL 值，会在 probe 侧把 “key 为 NULL 的行” 当作不可能匹配，从而错误过滤。

处理建议：

1. **MVP：当 join 处于 NullEQ 模式且 key 可能为 Nullable 时，禁用 runtime filter**  
   - 可以在 `PhysicalJoin::build()` 处不要注册 runtime_filter_list，或在 `Join::workAfterBuildFinish()` 处直接 cancel
2. 优化方案：扩展 runtime filter 表达式语义为 NullEQ 版本  
   - Set 侧：除了非 NULL values 的 set 外，再维护一个 `has_null` 标记
   - 应用侧：predicate 变成 `if isNull(x) then has_null else (x IN set)`
   - 注意：目前 runtime filter pb 限制 source/target expr_list size == 1（`RuntimeFilter` 构造函数里检查），多列 join key 不适用

---

## 6. 建议的分阶段落地计划（尽量可控）

下面给一个“可以按 PR/patch 切分”的更细开发计划。总体原则：

- **先正确性**：优先让结果 100% 正确，即使性能临时退化到 serialized，也要先把语义跑通
- **不破坏现有默认行为**：只有在 `is_null_eq[i]=1` 且 join key 真实可能为 NULL 时才启用 NullEQ 路径
- **支持 key 粒度混合**：同一个 join 里既可以有 `=` key，也可以有 `<=>` key

### Milestone 0：协议/Plumbing（不改语义，先把信息传到底）

目标：TiFlash 能在 Join 对象里拿到 per-key 的 `is_null_eq[]`，并做到“没下发就完全不影响现有 join”。

1. 协议侧（由 TiDB/kvproto 侧提供）
   - `tipb::Join` 新增 `repeated bool is_null_eq = ...;`
   - 约束：长度与 join key 数一致（`left_join_keys.size == right_join_keys.size == is_null_eq.size`）
2. TiFlash 解析与透传
   - `PhysicalJoin`/`JoinInterpreterHelper` 解析 `is_null_eq[]` 并传入 `Join`
   - Join 增加成员 `std::vector<UInt8> is_null_eq`
   - 加入一致性校验与 fail-fast：长度不一致直接报错（更利于早期定位）
3. 可观测性（便于调试）
   - `Join` 的 debug string / log 里打印 `is_null_eq`（至少打印“有无 NullEQ key”以及 index 列表）
   - 解释计划（Explain/diagnostic）里能看到该 join 使用了 NullEQ（可选）

完成标志：
- 只做 plumbing 的 patch 合入后，所有现有 join 测试应 **零变化**。

### Milestone 1：正确性 MVP（serialized 兜底，先跑通所有 join kind）

目标：在存在 Nullable 的 NullEQ key 时，让 HashJoin 的 build/probe 都允许 NULL key 参与匹配，并修复 “null_map 混用” 带来的错误跳过。

#### 1.1 抽象一个“按 key 语义处理 Nullable + 生成 row_filter_map”的公共步骤

在 build/probe 两侧都需要同样的逻辑，建议抽一个 helper（伪接口）：

- 输入：
  - `std::vector<ColumnPtr> key_columns`（已 extract/materialize 完成）
  - `std::vector<UInt8> is_null_eq`（与 keys 对齐）
  - `ConstNullMapPtr * row_filter_map`（输出：需要跳过 insert/probe 的行）
  - `ColumnPtr * row_filter_map_holder`（输出：map 的 owning column）
  - `ColumnPtr side_filter_column`（可选：left/right condition 结果列）
- 输出（对 key_columns 原地改写）：
  - 对 `=` key：若 column 是 `ColumnNullable`，将它替换成 nested column，并把其 null map OR 进 `row_filter_map`
  - 对 `<=>` key：保留 `ColumnNullable`（不能 removeNullable），并且 **不**把 null map OR 进 `row_filter_map`
  - 最后再把 `side_filter_column` 的 false/NULL 结果 OR 进 `row_filter_map`

这样可以得到一个明确的语义：
- `row_filter_map[i]=1` 表示这行 **一定不会参与 join key 的等值匹配**（要么 side-condition 不通过，要么存在 EQ key 的 NULL）
- NullEQ key 的 NULL 行不会被过滤（因为它们“可能匹配”）

注意：这一步是整个功能正确性的核心，因为它把“key NULL”从历史的全局 null_map 里拆出来，并且做到了 per-key 语义。

#### 1.2 Build 侧：insertBlockIntoMaps 不再无条件跳过 NullEQ NULL 行

落点：`Join::insertFromBlockInternal(...)`（以及其周边的 key_columns 处理）。

改动点（MVP）：
- 替换现有 `extractNestedColumnsAndNullMap(key_columns, ...)` 的用法，改用 Milestone 1.1 的 helper 生成 `row_filter_map`
- 传给 `JoinPartition::insertBlockIntoMaps(..., null_map=row_filter_map, ...)`
- 验证 `RowsNotInsertToMap` 的语义：
  - 应该包含：build side-condition 不通过的行、以及 EQ key 为 NULL 的行（它们不入 map）
  - 不应包含：NullEQ key 为 NULL 的行（它们应该入 map）

完成标志：
- 对 RIGHT/FULL/RightSemi/RightAnti 等需要 scan-after-probe 的 join kind，build side NULL-key 行在 NullEQ 模式下不再被错误归入 `rows_not_inserted_to_map`。

#### 1.3 Probe 侧：prepareForHashProbe 不再把 NullEQ NULL 行当作 not found 直接短路

落点：`ProbeProcessInfo::prepareForHashProbe(...)`、`JoinPartition::probeBlockImplTypeCase(...)`。

改动点（MVP）：
- 替换 probe 侧 `extractNestedColumnsAndNullMap(...)` 的使用方式（同 build：用 helper 生成 row_filter_map）
- Probe 阶段对 `row_filter_map[i]=1` 的行仍然走 `addNotFound()`（保持现有行为）
- 对 NullEQ key NULL 行：`row_filter_map[i]=0`，必须进入 hash probe（可能命中 build side 的 NULL key）

完成标志：
- `NULL <=> NULL` 能在 inner/outer join 中命中；`NULL <=> non-NULL` 不命中。

#### 1.4 JoinMapMethod 选择：MVP 强制 serialized（仅在“确实需要”时）

这一步的目的不是优化，而是避免 `keys128/keys256` 的 KeyGetter 在面对 `ColumnNullable` 时直接 UB 或算错 key。

策略（建议）：
- 若 join 中 **存在至少一个** `is_null_eq[i]=1` 且该 key 在 build 侧的物理列类型是 Nullable：
  - 强制 `JoinMapMethod::serialized`
  - key_columns 保留 `ColumnNullable`（让 `serializeValueIntoArena` 编码 nullness）
- 否则（所有 NullEQ key 都非 Nullable）：
  - 可保持现有 map method 选择与 key getter（因为 `<=>` 与 `=` 等价）

这个判断放在 `Join::initBuild()` 最稳妥（method 只需决定一次）。

#### 1.5 RuntimeFilter：MVP 先禁用（只要会影响正确性就禁）

规则建议（够保守但简单）：
- 若 join 使用 NullEQ 且存在 Nullable 的 NullEQ key：禁用 runtime filter（防止 Set 丢 NULL 导致错误过滤）

完成标志：
- 同一条查询在 TiDB 上启用 runtime filter 与否，都不应因为 NullEQ join 而出现结果差异（只是性能差异）。

#### 1.6 角落路径：spill / fine-grained shuffle / dispatch hash

需要在 MVP 阶段就明确做对，否则线上会出现“不开 spill 正确，一开 spill 错”的情况：

- dispatch hash 的输入 key_columns **必须**与 join hashing 的 key_columns 语义一致  
  - 对 NullEQ key：hash 需要包含 nullness（ColumnNullable 必须保留）
  - 对 EQ key：因为 NULL 行已经被 row_filter_map 过滤，removeNullable 后不影响正确性

完成标志：
- 打开 spill/fine-grained shuffle 的测试用例能正确跑过（至少覆盖 NULL<=>NULL 的命中）。

### Milestone 2：测试与回归（在 TiFlash 层把语义覆盖住）

目标：把 NullEQ join 的语义固定下来，防止后续 refactor/runtimfilter/packed keys 反复引入回归。

建议按“从小到大”的顺序补测试：

1. Join 语义最小集（单列 key）
   - inner：`(NULL) <=> (NULL)` 命中
   - inner：`(NULL) <=> (1)` 不命中
   - inner：`(1) <=> (1)` 命中（与 `=` 一致）
2. Outer join（确保 build/probe NULL 行不会被提前丢）
   - left outer：probe side NULL key 行正确输出（匹配/不匹配两种）
   - right/full：build side NULL key 行可被匹配；未匹配时 scan-after-probe 输出
3. Semi / Anti（关注“是否短路成 not found”）
   - semi：NULL<=>NULL 应返回存在
   - anti：NULL<=>NULL 应返回不存在
4. 多列 key + 混合语义（关键！）
   - `k1 <=> k1 AND k2 = k2`：当 k2 为 NULL 时应 **不匹配**（因为 EQ key NULL 过滤）
   - `k1 <=> k1 AND k2 <=> k2`：两列都 NULL 时可匹配
5. side-condition 交互
   - right_conditions/left_conditions 过滤为 false/NULL 时，row_filter_map 应屏蔽该行（不参与 join），但 outer join 仍需按语义输出
6. spill / fine-grained shuffle（如果现有测试框架能触发）

落地位置建议：
- 优先在 `dbms/src/Flash/tests/gtest_join_executor.cpp` 增加用例，并扩展构造 join DAG 的 helper 支持下发 `is_null_eq[]`。

完成标志：
- 新增用例能在本地稳定复现：有 NullEQ 时正确；没有 NullEQ 时行为与历史完全一致。

### Milestone 3：性能优化（把常见 nullable key 拉回 packed keys）

目标：在 Nullable + NullEQ 的常见场景下，避免 serialized 的巨大开销。

建议路线（参考 HashAgg）：

1. 新增/复用 join 的 nullable fixed key hash method
   - 引入 `HashMethodKeysFixed<..., has_nullable_keys=true, ...>` 分支（如 HashAgg）
   - 支持 `UInt128/UInt256` packed key（bitmap + values）
2. `chooseJoinMapMethod` 支持选择 nullable-packed
   - 当存在 Nullable NullEQ key，且所有 key 都是 fixed（numeric/datetime/fixedstring 等）、且无 collator：选择 packed
   - 否则 fallback serialized（string/collation/复杂表达式）
3. 渐进式覆盖
   - 先支持 `keys128/keys256`（收益最大、侵入可控）
   - 再考虑 `key64` 系列（需要为 nullable 引入 bitmap，会更麻烦，收益也相对小）

完成标志：
- 对典型 schema（nullable int/datetime join key）性能回到接近普通 EQ join。

### Milestone 4：RuntimeFilter（可选增强）与工程化收尾

1. RuntimeFilter 的 NullEQ 语义（可选）
   - Set 增加 `has_null` 标记
   - 应用侧 predicate 变成：`isNull(x) ? has_null : (x IN set)`
   - 只在单列 key（pb 已限制）且该 key 为 NullEQ 时启用
2. 更完整的 corner case 覆盖
   - spill/restore、fine-grained shuffle、并发 build/probe 的稳定性回归
3. 可观测性与文档
   - 增加 profile 计数器：NullEQ join 次数、forced-serialized 次数、runtime filter 被禁用次数
   - 更新开发文档：明确哪些 join key 类型/场景会 fallback serialized

完成标志：
- 功能默认可用、性能可控、出现问题时可通过指标快速定位是否走了 serialized 或禁用了 runtime filter。

---

## 7. 测试建议（覆盖语义 + 覆盖关键 join kind）

至少需要覆盖：

1. Inner join：两边 key 都为 NULL 时能匹配
2. Left outer join：左表 NULL key 行在右表也有 NULL 时应匹配，否则输出 NULL 右列
3. Right outer / Full join：build side 的 NULL key 行能够被匹配并正确标记 used；未匹配则在 scan-after-probe 输出
4. Semi / Anti join：NULL key 在 NullEQ 模式下参与匹配（而不是直接 false）
5. 多列 key：其中某列为 NULL 时只有两边都 NULL 才能匹配
6. Collation / decimal-format-string 等特殊类型（尽量覆盖 `join_key_types[i].is_incompatible_decimal` 的路径）
7. spill enabled + not spilled（触发 “virtual dispatch”），验证 computeDispatchHash 对 NULL 的一致性

形式上可以：

- 增加 gtest（类似已有的 join spill / runtime filter gtest）
- 或者在 executor test framework 中构造 join DAG request

---

## 8. 代码热点清单（落地时优先看的文件）

- Join 语义与初始化
  - `dbms/src/Interpreters/Join.h`（Nullable key 语义注释、Join 成员扩展）
  - `dbms/src/Interpreters/Join.cpp`（`getKeyColumns()`、`initBuild()`、`insertFromBlockInternal()`）
  - `dbms/src/Interpreters/JoinHashMap.cpp`（`chooseJoinMapMethod()`）
- Probe 侧准备与 NULL/过滤逻辑
  - `dbms/src/Interpreters/ProbeProcessInfo.cpp`（`prepareForHashProbe()`）
  - `dbms/src/Interpreters/JoinUtils.cpp`（`recordFilteredRows()`）
  - `dbms/src/Interpreters/NullableUtils.cpp`（`extractNestedColumnsAndNullMap()`）
- Hash 表插入 / probe
  - `dbms/src/Interpreters/JoinPartition.cpp`（KeyGetterForTypeImpl、insert/probe 逻辑）
  - `dbms/src/Common/ColumnsHashing.h` / `dbms/src/Common/ColumnsHashingImpl.h`（nullable packed key 支持）
  - `dbms/src/Interpreters/Aggregator.cpp`（nullable_keys128/256 的选择策略可借鉴）
- scan-after-probe
  - `dbms/src/DataStreams/ScanHashMapAfterProbeBlockInputStream.cpp`（RowsNotInsertToMap 与 used flag 输出逻辑）
- runtime filter / set
  - `dbms/src/DataStreams/RuntimeFilter.h`
  - `dbms/src/Interpreters/Set.cpp`

---

## 9. 总结

TiFlash 现有 Join 的 NULL 处理方式是“**直接把 NULL key 行排除出 hash join**”，因此要支持 NullEQ join key，必然需要：

- 在 build/probe 两侧把 “key-NULL” 与 “side-condition filter” 解耦
- 允许 NULL key 行进入 hash map 并参与 probe
- 让 hash key 编码/比较包含 nullness（参考 HashAgg 的 nullable key 打包逻辑，或先用 serialized 兜底）
- 同时解决 runtime filter 与 scan-after-probe 这两条“默认认为 NULL key 一定 unmatched”的隐含逻辑

这不是单点改动，而是 join pipeline 中几处“默认跳过 NULL key”的假设需要系统性拆解与替换。
