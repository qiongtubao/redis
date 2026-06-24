# Redis-On-Rocks 内存碎片率处理与 Swap 模式分析

> 适用版本：Redis-On-Rocks 6.x（`make SWAP=1` 编译，即 `ENABLE_SWAP`）

本文档说明本项目中内存碎片率的计算方式、清理机制，以及开启 Swap 模式后 `MEMORY PURGE` 效果不明显的原因与应对建议。

---

## 一、项目里的「碎片率」不止一个

Redis/RoR 把内存拆成多层指标，**不同指标对应不同清理手段**，不能混看。

### 1. 采样与更新（每 100ms）

`cronUpdateMemoryStats()` 在 `serverCron` 中周期性采样：

```c
// src/server.c
void cronUpdateMemoryStats() {
    run_with_period(100) {
        server.cron_malloc_stats.process_rss = zmalloc_get_rss();
        server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        zmalloc_get_allocator_info(&server.cron_malloc_stats.allocator_allocated,
                                   &server.cron_malloc_stats.allocator_active,
                                   &server.cron_malloc_stats.allocator_resident);
    }
}
```

### 2. 各指标含义

| 指标 | 公式 | 含义 |
|------|------|------|
| `mem_fragmentation_ratio` | `process_rss / zmalloc_used` | 进程 RSS 相对 Redis 堆的逻辑用量（**含 Lua、共享库、RocksDB 等**） |
| `allocator_frag_ratio` | `allocator_active / allocator_allocated` | **jemalloc 外部碎片**（active 里有空洞） |
| `allocator_rss_ratio` | `allocator_resident / allocator_active` | jemalloc **保留页/脏页**（可 purge，但不是 bin 级碎片） |
| `rss_overhead_ratio` | `process_rss / allocator_resident` | 非 jemalloc 部分的 RSS（Lua、模块、RocksDB 等） |

计算逻辑在 `getMemoryOverheadData()`（`src/object.c`）：

```c
mh->total_frag =
    (float)server.cron_malloc_stats.process_rss / server.cron_malloc_stats.zmalloc_used;
mh->allocator_frag =
    (float)server.cron_malloc_stats.allocator_active / server.cron_malloc_stats.allocator_allocated;
mh->allocator_rss =
    (float)server.cron_malloc_stats.allocator_resident / server.cron_malloc_stats.allocator_active;
```

### 3. Swap 模式额外指标（`ENABLE_SWAP`）

Swap 模式下，大量冷数据在 RocksDB 磁盘侧，**不在 `zmalloc_used` 里**，但 RSS 仍包含 RocksDB 内存，因此引入「修正碎片率」：

```c
// src/object.c
#ifdef ENABLE_SWAP
    mh->rocks = rocksGetMemoryOverhead(rocks);
    mh->rectified_frag = (float)server.cron_malloc_stats.process_rss /
        (server.cron_malloc_stats.zmalloc_used + mh->rocks->total);
    mh->rectified_frag_bytes = server.cron_malloc_stats.process_rss -
        server.cron_malloc_stats.zmalloc_used - mh->rocks->total;
#endif
```

对应 `INFO memory` 里的：

- `swap_mem_rocksdb`
- `swap_rectified_frag_ratio` / `swap_rectified_frag_bytes`

**Swap 模式下应优先看 `swap_rectified_frag_ratio`，而不是 `mem_fragmentation_ratio`。**

RocksDB 内存统计来自 `rocksGetMemoryOverhead()`（`src/ctrip_swap_rocks.c`），包括 memtable、block cache、index/filter、pinned blocks。

---

## 二、三种碎片清理机制（作用范围不同）

```
Process RSS (process_rss)
├── Allocator Resident (allocator_resident)  ← jemalloc RSS
│   ├── Allocator Active (allocator_active)  ← 含保留页/脏页
│   │   └── Allocator Allocated (allocator_allocated)  ← Redis 逻辑分配
│   └── [Dirty/Retained Pages]  ← MEMORY PURGE 可释放
└── RocksDB Memory (memtable/blockcache/...)  ← PURGE 无法触及
```

### 1. `MEMORY PURGE` — 归还 jemalloc 空闲页

命令入口（`src/object.c`）：

```c
if (jemalloc_purge() == 0)
    addReply(c, shared.ok);
```

底层对所有 arena 执行 purge（`src/zmalloc.c`）：

```c
int jemalloc_purge() {
    /* return all unused (reserved) pages to the OS */
    je_mallctl("arena.%d.purge", narenas, ...);
}
```

**能降**：`allocator_active`、`allocator_resident`、部分 RSS  
**不能降**：jemalloc bin 里的外部碎片（active/allocated 空洞）、RocksDB 内存

`FLUSHDB`/`FLUSHALL` 同步模式也会自动 purge（`src/db.c`）：

```c
#if defined(USE_JEMALLOC)
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
```

### 2. `activedefrag` — 整理 jemalloc 外部碎片

通过 `activeDefragAlloc()` 把可移动的 allocation 搬到新地址，减少 `active - allocated`（`src/defrag.c`）。

启动条件（默认）：

- 碎片率 ≥ `active-defrag-threshold-lower`（默认 10%）
- 且碎片字节 ≥ `active-defrag-ignore-bytes`（默认 100MB）

**只扫描 `db->dict` 里仍在内存中的 key 对象**，对 Swap 冷数据（只剩 meta、值在磁盘）几乎无作用。

### 3. `jemalloc-bg-thread` — 异步 purge

```c
// src/zmalloc.c
void set_jemalloc_bg_thread(int enable) {
    /* let jemalloc do purging asynchronously, required when there's no traffic
     * after flushdb */
    je_mallctl("background_thread", NULL, 0, &val, 1);
}
```

Swap 模式额外有 `jemalloc-max-bg-threads`（默认 4，IMMUTABLE），在 `InitServerLast()` 里设置：

```c
// src/server.c
#ifdef ENABLE_SWAP
    set_jemalloc_max_bg_threads(server.jemalloc_max_bg_threads);
#endif
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
```

本质仍是 **jemalloc arena purge**，不碰 RocksDB。

---

## 三、开启 `ENABLE_SWAP` 后，PURGE 效果不明显的原因

### 1. 指标「虚高」：分母变小了

Swap 把大 value 换到 RocksDB，`zmalloc_used` 大幅下降，但 RSS 里还有：

- RocksDB memtable / block cache / index&filter
- key meta（`objectMeta`、dirty_subkeys 等）
- 客户端 buffer、复制 backlog 等

于是：

```
mem_fragmentation_ratio = RSS / zmalloc_used  →  很容易 > 2.0
```

这**不等于** jemalloc 碎片严重，而是 **RocksDB 内存没进分母**。  
应看 `swap_rectified_frag_ratio`，它把 RocksDB 算进分母。

### 2. PURGE 只管 jemalloc，不管 RocksDB

RocksDB 内存来自自己的 block cache / memtable 分配，**不受 `MEMORY PURGE` 影响**。  
即使 jemalloc 侧 purge 成功，RSS 也可能几乎不变——因为大头在 RocksDB。

Swap 路径里也没有在 swap-out 后调用 `jemalloc_purge()`。

### 3. jemalloc 侧碎片本身可能就不高

Swap 的设计是 **热数据留内存、冷数据落盘**：

- Redis 堆里主要是 meta + 热 key 片段
- 对象规模小、生命周期相对稳定
- `allocator_frag_ratio` 可能长期 < 1.1

此时：

- `activedefrag` 不会启动（默认阈值 10% + 100MB）
- `MEMORY PURGE` 可释放的 dirty/retained 页很少
- 项目自测也验证了这一点（`MEMORY_PURGE_TEST_SUMMARY.md`：~70MB 数据下 PURGE 前后几乎无变化）

### 4. Swap-out 释放 value，但不自动归还物理页

Key 被 swap 到磁盘时，value 从 jemalloc 释放，但 jemalloc 5 **不会在无流量时主动把页还给 OS**（代码注释明确说明）。  
需要 purge 或 bg-thread 触发——但若堆里仍有活跃分配（meta、热数据、RocksDB 线程分配），可 purge 的页有限。

### 5. Active Defrag 对 Swap 场景帮助有限

Defrag 扫描 `dictScan(db->dict, ...)`，只整理**仍在内存中的对象结构**。  
冷 key 只剩 meta，**可移动的 allocation 很少**，defrag hit 低，对整体 RSS 贡献小。

### 6. 两种「碎片」被混在一起

| 你看到的 | 实际原因 | 有效手段 |
|---------|---------|---------|
| `mem_fragmentation_ratio` 高 | RocksDB + meta 开销 | 调 RocksDB cache、swap 策略；看 `swap_rectified_frag_ratio` |
| `allocator_frag_ratio` 高 | jemalloc bin 空洞 | `activedefrag yes` |
| `allocator_rss_ratio` 高 | jemalloc 保留页 | `MEMORY PURGE` / `jemalloc-bg-thread yes` |
| `rss_overhead_ratio` 高 | Lua/模块/非堆内存 | 与 PURGE 无关 |

Swap 模式下常见的是第一种（指标虚高），而 PURGE 只能处理第三种。

---

## 四、实践建议（Swap 模式）

### 监控：看对指标

```bash
redis-cli INFO memory | grep -E \
  "used_memory|used_memory_rss|allocator_frag|mem_fragmentation|swap_mem_rocksdb|swap_rectified"
redis-cli MEMORY STATS | grep -E "allocator\.|rectified|rocksdb"
```

- 判断 jemalloc 碎片 → `allocator_frag_ratio` / `allocator-fragmentation.ratio`
- 判断是否需要 PURGE → `allocator_rss_ratio` 或 `allocator.active` vs `allocator.allocated` 差距
- Swap 整体健康度 → **`swap_rectified_frag_ratio`**（不是 `mem_fragmentation_ratio`）

### 推荐配置

```bash
# jemalloc 保留页清理（对 Swap 仍有效，但只作用于 Redis 堆）
CONFIG SET jemalloc-bg-thread yes

# jemalloc 外部碎片（对 Swap 热数据有效）
CONFIG SET activedefrag yes
CONFIG SET active-defrag-threshold-lower 10
CONFIG SET active-defrag-ignore-bytes 104857600

# Swap 编译时默认 jemalloc-max-bg-threads 4（IMMUTABLE，需重启改）
```

RocksDB RSS 偏高时，需要从 Swap/RocksDB 侧治理（block cache 上限、swap 淘汰策略等），**不要指望 `MEMORY PURGE` 解决**。

### 何时手动 PURGE 仍有意义

- 大量 **热数据删除 / FLUSHDB** 后，`allocator_active` 仍明显高于 `allocator_allocated`
- `allocator_rss_ratio > 1.1` 且 `allocator_frag_ratio` 正常（说明是保留页问题，不是 bin 碎片）
- 需要 **GB 级** 内存释放；小规模测试（项目里 ~70MB）基本看不到效果

---

## 五、相关源码索引

| 功能 | 文件 |
|------|------|
| 内存指标采样 | `src/server.c` — `cronUpdateMemoryStats()` |
| 碎片率计算 | `src/object.c` — `getMemoryOverheadData()` |
| MEMORY PURGE | `src/object.c` — `memoryCommand()`; `src/zmalloc.c` — `jemalloc_purge()` |
| FLUSHDB 自动 purge | `src/db.c` — `flushdbCommand()` |
| Active Defrag | `src/defrag.c` — `activeDefragCycle()`, `computeDefragCycles()` |
| jemalloc 后台线程 | `src/zmalloc.c` — `set_jemalloc_bg_thread()` |
| Swap 额外 bg 线程 | `src/server.c` — `InitServerLast()` |
| RocksDB 内存统计 | `src/ctrip_swap_rocks.c` — `rocksGetMemoryOverhead()` |
| Swap 修正碎片率 | `src/object.c` — `getMemoryOverheadData()` (`ENABLE_SWAP`) |
| 配置项 | `src/config.c` — `activedefrag`, `jemalloc-bg-thread`, `jemalloc-max-bg-threads` |

---

## 六、结论

1. **碎片率是多层概念**：`mem_fragmentation_ratio`（总 RSS 开销）、`allocator_frag_ratio`（jemalloc 外部碎片）、`allocator_rss_ratio`（可 purge 的保留页）；Swap 模式还有 `swap_rectified_frag_ratio`。

2. **`MEMORY PURGE` 只做 jemalloc arena purge**，释放 dirty/retained 页，**不整理 bin 级外部碎片，也不释放 RocksDB 内存**。

3. **开启 Swap 后效果不明显**，主要是因为：
   - 大量内存转移到 RocksDB，PURGE 触达不到；
   - `mem_fragmentation_ratio` 因分母变小而虚高，容易误判；
   - Redis 堆本身碎片可能不高，PURGE 可释放页很少；
   - Swap-out 不会自动触发 purge，需手动或依赖 bg-thread。

4. **Swap 模式下**：用 `swap_rectified_frag_ratio` 评估整体；用 `allocator_frag_ratio` 决定是否开 `activedefrag`；用 `allocator_rss_ratio` 判断 `MEMORY PURGE` 是否值得做；RocksDB 内存需单独治理。

---

## 七、Active Defrag 与 Swap 并发

在 Swap 模式下开启 `activedefrag yes` 时，主线程 Defrag 与 Swap 工作线程可能并发修改同一 key 的内存对象；此外 Defrag 移动 key 名时须同步 `db->meta`（与 `db->dict` 共享 sds 指针）。

**应对方案**（参考 expire 的 `lockWouldBlock` 模式）：Swap 锁占用中的 key 跳过碎片整理；移动 key 时同步 `db->meta` / `db->dirty_subkeys`。

详见：[activedefrag-swap-concurrency-design.md](./activedefrag-swap-concurrency-design.md)

---

## 参考资料

- [activedefrag-swap-concurrency-design.md](./activedefrag-swap-concurrency-design.md) — Active Defrag × Swap 并发分析与设计方案
- `MEMORY_PURGE_TEST_GUIDE.md` — 测试指南
- `MEMORY_PURGE_TEST_SUMMARY.md` — 测试结果与 jemalloc 行为分析
- `tasks/redis_defrag_analysis.md` — Active Defrag 与 PURGE 深度分析
- `tests/integration/memory-fragmentation-purge.tcl` — 集成测试
