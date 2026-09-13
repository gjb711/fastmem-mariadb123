# FASTMEM 内存表引擎 — 设计文档

## 1. 背景与目标

MariaDB 自带 MEMORY 引擎（heap）存在严重瓶颈：**整个表一把表锁**
（`HP_SHARE->lock` 上的 `THR_LOCK`），任何 DML（含 UPDATE）都会把并发的
读写全部串行化。在"高频更新 + 大量并发读"的场景下，这把锁就是性能天花板。

FASTMEM 的目标：

- **完全没有表级锁**。`store_lock()` 返回零个 `THR_LOCK_DATA`，服务器不会
  为 FASTMEM 表获取/释放任何 thr_lock。
- **读路径无锁**（seqlock 读）；同一行的并发写用**行级（槽级）自旋锁**串行化；
  哈希索引按桶加自旋锁。
- **不需要事务**：语句级语义、最后写入者获胜（last-writer-wins）。
- **性能优先**：热路径（等值查找、原地 UPDATE、扫描）不触碰任何全局锁或
  互斥量。
- 与 MEMORY 相同的 SQL 语义约束：数据不持久化，`max_heap_table_size` 限制
  表大小（沿用同一个变量），`TRUNCATE` 清空表。

适用场景：字典/缓存表、计数器表、"最新状态"表——大量并发读取、持续更新。

## 2. 总体架构

```
SQL 层 (handler: ha_fastmem.cc)
   │ 无 THR_LOCK；语句粒度调用
   ▼
引擎胶水 (fm_create.cc / fm_data.cc / fm_hash.cc)
   │  持有 FM_INFO（句柄态）→ FM_SHARE（共享态）
   ▼
并发核心 (fm_core.h, 无服务器依赖, C++17 + std::atomic)
   ├─ 槽数组：固定大小槽，按 4096 槽一块（chunk）惰性增长
   ├─ 每槽：FM_SLOT { state, wlock, seq, gen, free_next } + 记录镜像
   ├─ 每键：FM_KEY_CORE { nbuckets, head[], locks[] } 链式哈希
   └─ 共享计数器：records(原子), auto_inc(原子), chunk_cnt(释放语义发布)
```

### 2.1 行数据：槽 + 序号锁（seqlock）

每条记录存放在一个固定大小槽的镜像区（`slot = 槽首部 + 记录字节`）。

- 槽状态机：`FREE → ACTIVE → DELETING → FREE`；
- **写镜像协议**（持有 `wlock`）：
  1. `seq` 置奇数（写入进行中）；
  2. `memcpy` 记录镜像；`release` 语义写 `seq` 为偶数（seq+1）；
  3. 释放 `wlock`。
- **读镜像协议**（无锁）：
  1. `acquire` 读 `seq`；若为奇数（写者正在写）→ 重试；
  2. 复制记录；
  3. 再次读 `seq`；若发生变化（写者在此间完成了一轮写）→ 重试；
  4. 第 3 步的 `seq` 用 `acquire` 读，第 2 步的复制在其与第 1/3 步读之间，
     保证看到的是某一轮完整写入。
- 读结果必然是**某一轮完整写入的镜像**：永远不会看到一半旧一半新
  （单条记录 ≤ 128 字节时两个偶数 seq 之间至多发生一次完整写入——
  因为写者在 `wlock` 保护下顺序执行，且 seq 置奇/复原之间没有其他写者）。

### 2.2 位置与代号（generation）

- 位置引用（ref）= **8 字节**：`u32 槽号 + u32 代号`（代号在槽被回收复用
  时递增，见 2.4）。
- 行级操作（UPDATE/DELETE/按 ref 读取）都携带代号校验：槽已复用
  （代号不符）、或状态已不是 ACTIVE → 返回 `HA_ERR_RECORD_DELETED`，
  由服务器按"行被并发删除"处理，绝不会读写到错误的行。

### 2.3 无锁读路径

- **等值查找**：`bucket = hash(键) & (nbuckets-1)`；
  取 `locks[bucket]` 自旋锁 → 沿链表遍历 → 对每个节点做 seqlock 读 +
  键比较 → 释放锁。桶锁只是保护链表指针，不保护行内容。
- **全表扫描**：按槽号线性扫描，跳过非 ACTIVE 槽；每次调用以上限
  `chunk_cnt << 12` 为界 → **松散扫描**：扫描中途插入的行可能出
  现/消失（与 MEMORY 一样的允许行为）。扫描不碰任何锁。
- **UPDATE 快路径**（键未变）：槽内 `wlock` + 写镜像 + 释放。不碰哈希链、
  不碰任何桶锁、不碰全局互斥量。并发读要么读到旧镜像要么读到新镜像。
- 统计：`records` 为原子计数器（`HA_STATS_RECORDS_IS_EXACT` 保留）。

### 2.4 槽的分配与回收

- 分配/回收在 `struct_mutex` 下进行（仅 INSERT 的分配、DELETE 的入空闲链、
  TRUNCATE 用到，UPDATE/读热路径永不碰它）。
- 空闲链（`free_head`）复用被删除的槽；槽复用前**代号 +1**，使所有持有
  旧 ref 的线程立刻失效。
- 空闲链为空时按 4096 槽一块增长，新块内除首槽外全部推入空闲链；
  `chunk_cnt` 用 release 语义发布 → 读者每次扫描重新读取它，无需同步。

### 2.5 键变更 UPDATE（复合路径）

键变更（任一字面键有变化）时：

1. 对每个变更的键计算 `(键号, 新桶)`，去重后**按 (键号, 桶) 排序**；
2. 按序获取所有桶锁（固定顺序 → 无死锁）；
3. 在持锁状态下对每个变更键做**只读重复检查**（对比新镜像与链上节点，
   全部通过才继续；任何一处失败即返回 `HA_ERR_FOUND_DUPP_KEY`，尚未修改
   任何内容，无需回滚）；
4. 再取槽 `wlock`，校验 ACTIVE + 代号；
5. 写新镜像（一次）；从旧桶解链、链入新桶；释放全部锁。

并发键迁移的已知竞态：两个键变更 UPDATE 并发作用于同一行时，理论上可能
留下一个"孤儿链节点"（行已在新桶，旧桶里残留一个键不匹配的节点）。
查找会跳过键不匹配的节点（删除节点也被跳过），功能上正确；文档记为
未来可优化点（惰性清理）。

## 3. 锁顺序与无死锁证明

涉及共享状态的临界区只有三类：

| 操作 | 加锁顺序 |
|------|---------|
| INSERT | `struct_mutex`（分配槽）→ 单键桶锁（链入） |
| UPDATE 快路径 | 仅 `wlock` ×1 |
| UPDATE 键变更 | 已排序桶锁集 → `wlock` |
| DELETE | `wlock`（置 DELETING，先释放）→ 各桶锁（解链）→ `struct_mutex`（还槽） |
| TRUNCATE | `struct_mutex` → 各桶锁（清链，逐个释放）→ 各槽 `wlock`（回收） |
| UPDATE/DELETE 语句 | handler 层行锁 `wlock`（3.1 节：跨调用持有）→ 内部再按上表同序 |

- 桶锁之间：键变更路径按 (键,桶) 严格升序获取；DELETE/INSERT 每次只持
  一把桶锁。桶锁顺序一致 ⇒ 桶锁之间不死锁。
- 槽锁（wlock）与桶锁之间：UPDATE 先桶后槽；DELETE 先槽（但完成置
  DELETING 后立即释放槽锁）再桶。锁定方向相反但**从不嵌套持锁等待**
  （DELETE 释放 wlock 后才取桶锁；UPDATE 的 wlock 仅在校验与写镜像期间
  持有，桶锁已持有）⇒ 无环形等待。
- `struct_mutex` 只在分配/回收/清空时短暂持有，且从不与桶锁/槽锁
  同时嵌套（INSERT 先释放 struct 后再链桶；DELETE 先解链再还槽）。

⇒ 无死锁；饥饿可能性（自旋锁）在单行争用极高时有理论概率，实践中
可忽略（行级自旋通常微秒级释放）。

### 3.1 写语句的行级串行化（read-modify-write 原子性）

无锁读只解决"读不加锁"；但 SQL 层的 `UPDATE/DELETE` 是
「读旧图 → 计算 → 写回」三步（跨 handler 两次调用）。若不串行化，
两个并发写者会读到同一旧图、各自算出新值，后写覆盖前写 →
**丢失更新**。

修复（ha_fastmem，三点缺一不可）：

- `external_lock()` 收到 `F_WRLCK` 时置 `write_stmt` 标志，仅翻转句柄
  内标志，**无任何表级锁**。注意：MariaDB 传给 `external_lock` 的是
  **fcntl 风格常量**（`F_RDLCK=1 / F_WRLCK=2 / F_UNLCK=3`，见
  `include/my_global.h` 与 `sql/lock.cc::lock_external`），不是
  `thr_lock_type` 枚举——按后者比较将永远识别不出写语句；
- 写语句期间，每次读行（`index_read*` / `index_next` / `rnd_next` /
  `rnd_pos` / `find_unique_row`）先对命中的行槽取 `wlock`，**再在锁内
  重读该行**（`fm_reread_locked`）。定位/预读发生在取锁之前，读到的
  可能已是旧图；锁内重读保证后续计算基于锁内的最新值。若行在预读与
  取锁之间被删除/回收，重读失败并按"行不存在"返回；
- `update_row()` / `delete_row()` 在锁内执行（`fm_update_locked` /
  `fm_delete_locked` → `fm_core_update_row_locked` /
  `fm_core_delete_row_locked`，不重复加锁）随后释放；
- 效果：写者之间的整个「锁 → 重读 → 算 → 写」原子化（等价行锁且逐行
  释放，天然无死锁、无锁序依赖）；**纯 SELECT 语句完全不进锁路径**，
  无锁读保持。实测 8 进程 × 2000 条并发热行 UPDATE 零丢失
  （见 `docs/BENCHMARK.md`）。

辅助防线：`READ_CHECK_USED`（写前回读比对）保持开启——
`fm_extra(HA_EXTRA_RESET_STATE)` 不得清除它（那是语句间的常规重置，
不是显式的 `HA_EXTRA_NO_READCHECK`）。

锁序影响：`wlock` 在 handler 层最先获取、最后释放，内部路径保持
3 节表格原有顺序 ⇒ 无死锁证明不变。

## 4. 自增列

- 表级共享计数器 `auto_inc` 存储**最后发放值**；`get_auto_increment`
  用 CAS 循环一次性预留 `[first, last]` 区间，并发语句拿到互不相交的
  区间（替代 MEMORY 的"反正整表被锁，直接返回 next"做法）。
- `fm_update_auto_increment` 观察显式写入的值并抬升计数器（移植自
  heap 的 `heap_update_auto_increment` 类型开关）。
- 溢出时返回 `ULONGLONG_MAX` → 服务器映射为 `HA_ERR_AUTOINC_READ_FAILED`。

## 5. 与 MEMORY 的行为差异（有意为之）

| 维度 | MEMORY (heap) | FASTMEM |
|------|---------------|---------|
| 表锁 | THR_LOCK 整表 | **无**（store_lock 返回空） |
| 并发写 | 串行（整表互斥） | 行级：写语句全程持有行槽锁（读-算-写原子，3.1 节），读不阻塞 |
| 并发读 | 与写互斥 | 无锁 seqlock 复制 |
| 位置 ref | 内存指针 | 槽号+代号(8B)，防悬垂 |
| 索引 | HASH 或 BTREE | **仅 HASH**（CREATE 显式 BTREE 报错） |
| 事务 | 无 | 无（HA_NO_TRANSACTIONS） |
| 扫描 | 记录链表 | 槽位线性扫描（松散） |
| 排序键遍历 | BTREE 时支持 prev/first/last | 不支持（只等值+next 链） |
| `max_heap_table_size` | 限制表大小 | 沿用同一变量 |

## 6. 关键性能决策

1. **零共享写路径**：原地 UPDATE（无键变更）只碰目标槽——这是
   "不停更新"工作负载的核心收益。
2. **无锁读**：等值查找只取桶自旋锁保护链表指针；行内容复制用
   seqlock 重试，重试率在实际负载下趋近于零。
3. **无全局计数器热争用**：`records`/`auto_inc` 各自独立原子，
   UPDATE/DELETE 不读写 `records`（只在 INSERT/DELETE 时增减）。
4. **固定大小槽**：零分配、零碎片、`memcpy` 平铺；槽号即地址。
5. **紧凑 ref**：8 字节（槽号+代号），文件排序/嵌套循环成本极低。

## 7. 正确性约束与取舍

- **无事务**：违反唯一键的并发竞态按"后者失败"处理，No 回滚；
- **松散扫描**：`can_continue_handler_scan() == 0`，允许扫描中行集变化；
- **TRUNCATE 会失效所有游标/ref**（代号统一递增）；
- ref 在进程级有效（数据在内存中，重启即空，与 MEMORY 一致）。

## 8. 未来工作

- 孤儿链节点惰性回收（见 2.5）；
- 按负载自适应 `nbuckets`（当前按 max_records/4 取 2 的幂，上限 1M）；
- 可选：为纯非唯一键负载去掉桶锁（发布式链表）;
- 压测基准（bench/ 目录）用于与 MEMORY 对比。