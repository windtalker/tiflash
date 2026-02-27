# TiFlash NullEQ Join Key（`<=>` / `tidbNullEQ`）开发追踪

本文是 `nulleq_join.md`（工作量评估/方案探索）的 **实现追踪版**，用于把关键约定固化在 repo 里，便于分批开发与 review，避免长 session 上下文被压缩后“走偏”。

## How to continue（断点续开发 / 防 context 丢失）

本实现以本文件为事实来源（source of truth）。不论是否重开 session，继续开发前都遵循以下步骤：

1) 重新阅读本文件（必要时补读 `nulleq_join.md` 的背景段落）
2) 运行 `git status` / `git diff --stat` 确认当前 workspace 所处的 checkpoint
3) 明确本次要推进的 checkpoint（CP0/CP1/CP2...）并保持 patch 小而可 review
4) 每完成一个 checkpoint，更新本文的 “当前进度” 勾选项，并停下来让 reviewer 做一次集中 review

建议在每次“继续”时的指令里带上类似信息，避免依赖对话上下文：
- “以 `docs/design/nulleq_join_dev.md` 为准，从 CP2 开始继续”
- “先读 dev 文档 + `git status`，再改代码”

## 目标与非目标

### 目标
- TiFlash 的 Hash Join 在 **Join Key** 使用 null-safe equal（NullEQ）语义时结果正确：
  - `NULL <=> NULL` 为 `true`（可匹配）
  - `NULL <=> non-NULL` 为 `false`
  - `non-NULL <=> non-NULL` 等价于 `=`
- 支持 **key 粒度混合语义**：同一个 join 内允许部分 key 为 `=`、部分 key 为 `<=>`。
- 默认行为不变：未下发 NullEQ 标记时，保持现有 `NULL` 不参与等值 join 的语义。

### 非目标（至少 MVP 阶段）
- 不实现“把 `<=>` 留在 other_conditions 里”的语义（仅在 join key 里表达 NullEQ）。
- 不保证性能最优（MVP 可强制走 `serialized` 以换取正确性）。
- 不扩大 NullAware join（`NOT IN` 家族）的语义覆盖范围；必要时 fail-fast。

## 输入契约（TiDB Planner / tipb / TiFlash）

### tipb 协议
- `tipb::Join` 新增字段：`repeated bool is_null_eq = ...;`
  - `is_null_eq[i] = false`：第 i 个 join key 使用普通 `=` 语义（NULL 不匹配任何行）
  - `is_null_eq[i] = true`：第 i 个 join key 使用 `<=>` 语义（NULL 可与 NULL 匹配）
- 长度约束：
  - `is_null_eq_size == 0`：视为全 `false`（兼容旧版本下发）
  - 否则必须满足：`is_null_eq_size == left_join_keys_size == right_join_keys_size`

### Join key 的表达形式（MVP 假设）
- Join key 由 planner 下发为列引用（ColumnRef），不下发任意表达式。
  - TiFlash 执行层若需要类型对齐可插入 cast，但 **key 的“对齐顺序/数量”不变**，`is_null_eq[i]` 仍按 index 对齐。

### 语义边界
- NullEQ 语义只通过 `is_null_eq[]` 表达；不把 `<=>` 留在 `other_conditions`。
- 混合 key：对 `=` key，NULL 仍视为不可匹配；对 `<=>` key，NULL 参与匹配。

### 与 NullAware join 的关系
- `is_null_aware_semi_join`（NOT IN 的 NullAware）与 NullEQ 是两套不同语义。
- MVP 建议策略：**不允许同时启用**（任一 `is_null_eq[i]=true` 且 `is_null_aware_semi_join=true` 时 fail-fast）。
  - 目的：避免历史 “NULL key 行特殊处理” 与 NullEQ “NULL key 行可入 map” 的隐式冲突导致 silent wrong result。

## MVP 设计选择（正确性优先）

### 核心原则
- 把当前 join pipeline 中 “key-NULL + side-condition filter 共用一张 null_map” 拆开：
  - `row_filter_map`：由 left/right side-condition 产生（false/NULL 行不参与 insert/probe）
  - `key_null_map`：仅用于普通 `=` key 的 NULL 行过滤
  - NullEQ key 不应写入 `key_null_map`
- Build/Probe 传入 JoinPartition 的过滤 map 语义：表示“这一行无需 insert/probe”，不再默认包含“key 是 NULL”。

### Hash key 编码（MVP）
- MVP 阶段：当存在 Nullable 的 NullEQ key 时，强制走 `JoinMapMethod::serialized`（保证 NULL 信息进入 key 序列化）。
- 后续优化：对 fixed-size key 引入 nullable-packed（参考 HashAgg 的 `has_nullable_keys=true` packed keys）。

### RuntimeFilter（MVP）
- 当 join 含 NullEQ 且 key 可能为 Nullable：MVP **禁用 runtime filter**（避免 Set 丢 NULL 导致错误过滤）。

## 里程碑与 Done 标准

### Milestone 0：协议/Plumbing（不改语义）
Done 标准：
- TiFlash 能解析 `is_null_eq[]` 并透传到 `DB::Join`（或等价执行层结构）。
- 未下发 `is_null_eq` 时行为零变化；现有测试零变化。

### Milestone 1：正确性 MVP（serialized 兜底）
Done 标准：
- 含 Nullable NullEQ key 时：
  - build 侧 NULL key 行可入 map
  - probe 侧 NULL key 行可 probe 并命中（`NULL<=>NULL`）
  - outer join / scan-after-probe 不会把 NullEQ 的 NULL-key 行误当作“必然 unmatched”
- runtime filter 在该模式下被禁用（可观测/可解释）。
- 增加最小 gtest 覆盖（见下）。

### Milestone 2：测试矩阵（语义覆盖）
Done 标准：至少覆盖
- inner / left outer / right outer / full
- semi / anti
- 单列 NullEQ
- 多列混合：`k1 <=> k1 AND k2 = k2`
- side-condition 交互（left/right conditions）

### Milestone 3：性能优化（nullable packed keys）
Done 标准：
- nullable numeric/datetime 的 NullEQ join key 不再强制 serialized（回归 packed keys 路径）。

### Milestone 4：RuntimeFilter（可选）
Done 标准：
- 单列 NullEQ key 支持正确的 runtime filter（Set + has_null 语义），或明确长期禁用并有指标。

## Review 检查点建议（分批开发）
- CP0：tipb 字段 + TiFlash 解析（plumbing 起步）
- CP1：`DB::Join` 保存/打印 `is_null_eq`（仍不改语义）
- CP2：MVP 正确性（建议再拆小）
  - CP2.1：JoinMapMethod 选择策略（nullable NullEQ 强制 serialized）+ NullAware 互斥检查
  - CP2.2：build/probe 的 `null_map` 语义拆分（EQ key 才过滤 NULL；NullEQ key 不过滤）
  - CP2.3：MVP 禁用 runtime filter（nullable NullEQ 下）
  - CP2.4：补测试（inner/outer/semi/anti + 混合 key + side-condition）
- CP3：补测试矩阵（防回归）
- CP4：性能优化（nullable packed keys）

## 当前进度（持续更新）
- [x] tipb: `Join.is_null_eq` 字段定义（TiFlash repo 内）
- [x] TiFlash: `JoinInterpreterHelper::TiFlashJoin` 解析 `is_null_eq[]`
- [x] TiFlash: 透传至 `DB::Join` 并加 debug/log
- [x] TiFlash: CP2.1（nullable NullEQ 强制 serialized + NullAware 互斥 fail-fast）
- [ ] TiFlash: MVP 语义改动（serialized 兜底 + 拆 null_map）
- [ ] TiFlash: gtest 覆盖
- [ ] TiFlash: packed keys 优化

## 已知风险 / Open Questions
- 版本兼容：TiDB/kvproto 是否已同步该字段；TiFlash 需兼容 `is_null_eq` 缺失（size=0）。
- 类型对齐/cast：join key 若未来允许表达式或额外 cast，`is_null_eq[i]` 如何稳定对应。
- Collation：string key + collation 的 hash/eq 路径与 serialized fallback 的性能影响。
- spill / fine-grained shuffle：`computeDispatchHash()` 必须包含 NULL 信息；MVP 测试是否能覆盖该路径。
- NullAware join 交互：是否允许组合语义；若允许，优先级/行为如何定义。
