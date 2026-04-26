# ctrip_storage 架构文档

## 概述

ctrip_storage 是 Redis 的冷热分离存储子系统，核心职责是将冷数据从 Redis 内存卸载（swap out）到外部存储引擎（如 RocksDB），并在需要时将其重新加载（swap in）回内存。

---

## 模块分层结构

```
┌─────────────────────────────────────────────────────────────┐
│                  Redis 主进程 (server.h)                     │
│              src/db.c  src/evict.c  src/server.c             │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│               第一层：对外接口层                              │
│  ctrip_storage.h / ctrip_storage.c                           │
│  ctrip_storage_types.h                                       │
│  职责：生命周期管理、engine 注册、命令前/后钩子、deferred 命  │
│        令队列处理、StorageEvictCtx 管理                       │
└──────┬───────────────┬──────────────────┬───────────────────┘
       │               │                  │
┌──────▼──────┐  ┌─────▼──────┐  ┌───────▼───────┐
│  请求层      │  │  对象层     │  │  客户端层      │
│  request.*  │  │  objects.* │  │  client.*     │
└──────┬──────┘  └─────┬──────┘  └───────────────┘
       │               │
┌──────▼──────────────────────────────────────────────────────┐
│               第二层：基础服务层                              │
│  锁：lock.*          Evict：evict.*                          │
│  配置：config.*      线程：thread.*                          │
│  限流：rate_limit.*  指标：metric.*                          │
│  到期：expire.*      过滤：filter.*                          │
│  RIO：rio.*          扫描：scan.h                            │
└──────┬──────────────────────────────────────────────────────┘
       │
┌──────▼──────────────────────────────────────────────────────┐
│               第三层：基础数据结构/工具层                     │
│  absent_cache.*  cuckoo_filter.*  roaring_bitmap.*           │
│  buffered_allocator.*  adlist.*  utils.h  error.h  debug.h  │
└─────────────────────────────────────────────────────────────┘
```

---

## 文件职责清单

### 第一层：对外接口层

| 文件 | 职责 |
|------|------|
| `ctrip_storage_types.h` | **基础类型定义**（无 Redis 内核依赖），定义 `StorageEngine` 函数表、`StorageServerNamespace`、`StorageDBNamespace`、`StorageEvictCtx` 等核心类型。被 `server.h` 包含。 |
| `ctrip_storage.h` | **对外公共接口**，声明初始化（`initStorage`/`initStorageDB`）、引擎注册（`initMemoryStorageEngine`/`initRocksDBStorageEngine`）、命令处理钩子（`serverStorageBeforeProcessCommand`）、evict 上下文等。 |
| `ctrip_storage.c` | **顶层逻辑实现**，连接 lock/evict/request/thread/engine，实现 `processReadyDeferredCommands` 主调度循环。 |

### 第二层：请求层

| 文件 | 职责 |
|------|------|
| `ctrip_storage_request.h` | **请求层核心头**，定义 `swapRequest`、`swapRequestBatch`、`swapCtx`、`asyncCompleteQueue`、`parallelSync` 等中心数据结构和全部 API 声明。 |
| `ctrip_storage_request.c` | **请求调度与清理逻辑**，包含 `swapCtxFree`、`clientReleaseRequestIO`、`dbDeleteMeta`、与 evict/cuckoo/absent-cache 协作的提交逻辑。 |
| `ctrip_storage_request_async.c` | **异步提交路径**，实现 `asyncCompleteQueue` 的 append/notify/process 回调（`asyncSwapRequestBatchSubmit`、`asyncCompleteQueueProcess`）。 |
| `ctrip_storage_request_sync.c` | **并行同步路径**，使用 pipe 通知等待 RIO 完成（`parallelSyncSwapRequestBatchSubmit`、`parallelSwapProcess`）。 |
| `ctrip_storage_request_utils.h/.c` | **请求工具函数**，meta/data key 编解码（`rocksEncodeMetaKey`/`rocksDecodeMetaKey`）、类型字符串转换（`strObjectType`）等。 |
| `ctrip_storage_request_meta_scan.c` | **meta 扫描请求**，处理 meta 层的 scan 请求逻辑。 |
| `ctrip_storage_thread_request.c` | **线程间请求通知**，pipe 通知辅助函数，配合 async/parallel 两种模式。 |
| `ctrip_storage_batch.h` | **批量请求类型定义**，`swapRequestBatch` 的辅助类型。 |

### 第二层：对象层

| 文件 | 职责 |
|------|------|
| `ctrip_storage_data.h` | **核心 swap 数据类型**，定义 `swapData`、`swapDataType`（函数指针表），是对象层的基础。 |
| `ctrip_storage_objects.h` | **对象层聚合头**，定义各数据类型的 swapDataCtx（`bitmapDataCtx`/`listDataCtx`/`hashDataCtx`/`zsetDataCtx`/`setDataCtx`/`wholeKeySwapData`），以及 `swapDataSetupX` 等工厂函数声明。 |
| `ctrip_storage_objects.c` | **对象层聚合实现**，各类型 `swapDataSetupX` 的实现（分发到具体类型文件），以及 `persistingKeys` 管理（`persistingKeysPut`/`swapPersistCtxAddKey`）。 |
| `ctrip_storage_object_meta.h/.c` | **对象 meta 系统**，定义 `objectMeta`/`objectMetaType`（函数指针表），实现 meta 的 encode/decode/build/dup/free，用于 RocksDB 中对象元数据的序列化。 |
| `ctrip_storage_object_wholekey.c` | **整键类型**（string 等），实现 `wholeKeySwapAna`（swap 意图分析）。 |
| `ctrip_storage_object_hash.c` | **hash 类型**，实现 `swapDataSetupHash`，定义 hash 的 swapDataType。 |
| `ctrip_storage_object_set.c` | **set 类型**，实现 `swapDataSetupSet`，定义 set 的 swapDataType。 |
| `ctrip_storage_object_list.c` | **list 类型**，实现 `swapDataSetupList`，定义 list 的 swapDataType 与 `listObjectMetaType`。 |
| `ctrip_storage_object_zset.c` | **zset 类型**，实现 `swapDataSetupZSet`，定义 zset 的 swapDataType。 |
| `ctrip_storage_object_bitmap.c` | **bitmap 类型**，实现 `swapDataSetupBitmap`，定义 bitmap 的 swapDataType 与 bitmapMeta 操作。 |
| `ctrip_storage_object.h` | **对象层辅助头**，补充对象相关定义。 |
| `ctrip_storage_persist.h` | **持久化上下文类型**，定义持久化/swap persist 相关结构。 |

### 第二层：客户端层

| 文件 | 职责 |
|------|------|
| `ctrip_storage_client.h` | **客户端 swap 状态**，定义 `deferredCommand`、`swapUnblockCtx`、`argRewrite`/`argRewrites`、`swapMstate` 等，描述每个 client 的 swap 生命周期。 |
| `ctrip_storage_client.c` | **客户端状态实现**，实现 `initDeferredCommand`、`argRewrites` 初始化/重置。 |

### 第二层：基础服务层

| 文件 | 职责 |
|------|------|
| `ctrip_storage_lock.h/.c` | **swap 锁系统**，定义 `lock`/`locks`/`lockLink`/`lockLinks` 多级锁，实现 `lockLock`/`lockUnlock`/`lockWouldBlock`/`lockProceeded`，保证 key/db 级别的请求互斥与顺序。 |
| `ctrip_storage_evict.h/.c` | **驱逐系统**，定义 `swapEvictionCtx`，实现 `tryEvictKey`、`swapEvictAsap`、in-progress 限制判断，触发将冷键提交 swap-out 请求。 |
| `ctrip_storage_thread.h/.c` | **IO 线程管理**，定义 `swapThread`/`swapThreadCpuUsage`，实现 `swapThreadMain`（IO 线程主循环）、`swapThreadsDispatch`（任务分发）。 |
| `ctrip_storage_config.h/.c` | **配置系统**，定义 `StorageConfigDesc`/`StorageConfType`，通过 `storageGetConfigDescriptors` 将存储相关配置项暴露给 Redis `CONFIG GET/SET`。 |
| `ctrip_storage_rate_limit.h/.c` | **请求限流**，控制 swap 请求的发起速率，防止 IO 线程过载。 |
| `ctrip_storage_metric.h/.c` | **监控指标**，定义并收集锁/请求/缓存命中等统计数据。 |
| `ctrip_storage_expire.h/.c` | **过期管理**，处理存储层对象的过期逻辑。 |
| `ctrip_storage_filter.h/.c` | **冷热过滤**，整合 cuckoo filter 与 absent cache，对"肯定不存在"的 key/subkey 进行快速过滤以避免无效 IO。 |
| `ctrip_storage_commands.h/.c` | **命令意图定义**，定义各 Redis 命令对应的 swap 意图（swap-in/swap-out/del/nop），供 swapAna 使用。 |
| `ctrip_storage_rio.h/.c` | **RIO 接口封装**，对底层 StorageEngine 的读写操作进行统一封装（get/put/del/scan 等）。 |
| `ctrip_storage_scan.h` | **scan 类型定义**，定义 scan session、expire scan 等相关类型。 |
| `ctrip_storage_repl.c` | **复制支持**，处理主从复制场景下的存储同步逻辑。 |
| `ctrip_storage_trace.h/.c` | **追踪/诊断**，提供请求链路追踪与诊断信息的打印结构。 |
| `ctrip_storage_debug.h` | **调试宏/工具**，条件编译的调试辅助定义。 |
| `ctrip_storage_error.h` | **错误码定义**，存储子系统的错误码枚举。 |
| `ctrip_storage_utils.h` | **通用工具**，字符串/编码/解码/内存等通用 helper 声明。 |

### 第三层：基础数据结构/工具层

| 文件 | 职责 |
|------|------|
| `absent_cache.h/.c` | **缺席缓存**，记录确定不在外部存储中的 key/subkey，避免无效 IO 查询（LRU + HashMap 实现）。 |
| `cuckoo_filter.h/.c` | **布谷鸟过滤器**，概率型集合结构，用于冷键过滤（判断 key 是否可能在外部存储中）。 |
| `roaring_bitmap.h/.c` | **Roaring Bitmap**，高效压缩位图，用于 bitmap 类型对象的 subkey 范围存储与查询。 |
| `buffered_allocator.h/.c` | **缓冲分配器**，小对象预分配/复用池，减少高频 malloc/free 开销，被 lock/request 等频繁分配路径使用。 |
| `ctrip_storage_adlist.h/.c` | **双向链表**，独立实现的 adlist（兼容 Redis adlist 接口），供 absent_cache 等使用。 |

### 存储引擎实现

| 文件 | 职责 |
|------|------|
| `memory_storage_engine.c` | **内存存储引擎**，基于内存 dict 的引擎实现，用于测试和开发。 |
| `rocksdb_storage_engine.c` | **RocksDB 存储引擎**，生产级持久化引擎，实现 StorageEngine 函数表中的 get/put/del/scan 等。 |

---

## 文件依赖关系图

```
ctrip_storage_types.h          ← server.h（被集成到 Redis）
        ↑
ctrip_storage.h
  ├── ctrip_storage_request.h
  │     ├── ctrip_storage_trace.h
  │     ├── ctrip_storage_object_meta.h
  │     │     ├── ctrip_storage_utils.h
  │     │     └── ctrip_storage_scan.h
  │     ├── buffered_allocator.h
  │     ├── ctrip_storage_objects.h
  │     │     ├── ctrip_storage_data.h
  │     │     ├── roaring_bitmap.h
  │     │     ├── ctrip_storage_persist.h
  │     │     └── ctrip_storage_commands.h
  │     ├── ctrip_storage_error.h
  │     └── ctrip_storage_utils.h
  ├── ctrip_storage_client.h
  │     ├── server.h
  │     └── ctrip_storage_data.h
  ├── ctrip_storage_error.h
  ├── ctrip_storage_lock.h
  │     └── ctrip_storage_request.h（引用请求层常量）
  └── ctrip_storage_expire.h

ctrip_storage.c
  ├── ctrip_storage.h
  ├── ctrip_storage_evict.h
  ├── ctrip_storage_lock.h
  ├── ctrip_storage_request.h
  ├── ctrip_storage_object_meta.h
  ├── buffered_allocator.h
  └── ctrip_storage_rate_limit.h

ctrip_storage_evict.c
  ├── ctrip_storage.h
  ├── ctrip_storage_evict.h
  ├── ctrip_storage_lock.h
  └── ctrip_storage_request.h

ctrip_storage_lock.c
  ├── ctrip_storage_lock.h
  ├── buffered_allocator.h
  └── ctrip_storage_metric.h

ctrip_storage_filter.c
  └── ctrip_storage_filter.h
        └── （引用 cuckoo_filter、absent_cache）

absent_cache.c
  └── absent_cache.h
        ├── server.h
        └── ctrip_storage_adlist.h

cuckoo_filter.c
  ├── cuckoo_filter.h
  └── zmalloc.h（Redis 内存分配）

roaring_bitmap.c
  ├── roaring_bitmap.h
  └── zmalloc.h
```

---

## 核心数据流

### swap-out（冷却/驱逐）流程

```
Redis 主线程
  │
  ├── [evict触发] tryEvictKey()
  │     ├── 判断 lock 是否阻塞
  │     ├── 构造 swapCtx + swapRequest
  │     └── submitEvictClientRequest()
  │           └── swapRequestBatchSubmit()
  │                 ├── [async模式] asyncSwapRequestBatchSubmit()
  │                 │     └── swapThreadsDispatch() → IO线程
  │                 └── [sync模式]  parallelSyncSwapRequestBatchSubmit()
  │                                 └── pipe 等待 → IO线程
  │
IO 线程 (swapThreadMain)
  │── 执行 StorageEngine.put()/del()（通过 ctrip_storage_rio）
  └── 完成后通知主线程（asyncCompleteQueue / pipe）

主线程
  └── processReadyDeferredCommands()
        ├── asyncCompleteQueueProcess()
        └── 释放内存对象，更新 objectMeta
```

### swap-in（加热/读取）流程

```
Redis 主线程
  │
  ├── [命令执行] serverStorageBeforeProcessCommand()
  │     ├── 检查 cuckoo filter（key 是否在外部存储中）
  │     ├── 检查 absent cache（key 是否确定不存在）
  │     ├── 构造 swapCtx（swapAna 分析意图）
  │     │     └── 各类型 swapDataSetupX() → swapDataType.swapAna()
  │     ├── lockLock()（获取 key 级锁）
  │     └── submitNormalClientRequests()
  │           └── swapRequestBatchSubmit() → IO线程
  │
IO 线程
  │── 执行 StorageEngine.get()（通过 ctrip_storage_rio）
  └── 解码 RDB 格式 → Redis 对象，通知主线程

主线程
  └── processReadyDeferredCommands()
        ├── 将对象写回 Redis dict
        ├── lockUnlock()
        └── 继续执行被 defer 的客户端命令
```

---

## 命名规范现状与建议

### 现有命名问题

| 问题 | 受影响文件 | 建议 |
|------|-----------|------|
| `absent_cache.h/.c` 没有 `ctrip_storage_` 前缀 | `absent_cache.*` | 重命名为 `ctrip_storage_absent_cache.*` |
| `buffered_allocator.h/.c` 没有 `ctrip_storage_` 前缀 | `buffered_allocator.*` | 重命名为 `ctrip_storage_buffered_allocator.*` |
| `cuckoo_filter.h/.c` 没有 `ctrip_storage_` 前缀 | `cuckoo_filter.*` | 重命名为 `ctrip_storage_cuckoo_filter.*` |
| `roaring_bitmap.h/.c` 没有 `ctrip_storage_` 前缀 | `roaring_bitmap.*` | 重命名为 `ctrip_storage_roaring_bitmap.*` |
| `ctrip_storage_absent_cache.c` 与 `absent_cache.c` 并存 | `ctrip_storage_absent_cache.c` | 检查是否重复，合并或删除其中一个 |
| `ctrip_storage_object.h` 与 `ctrip_storage_objects.h` 混淆 | `ctrip_storage_object.h` | 明确区分职责（meta 对象 vs 数据类型集合） |

### 合理的文件分组

```
按功能模块分组（建议目录结构）：
ctrip_storage/
├── core/              # 顶层接口与类型
│   ├── ctrip_storage_types.h
│   ├── ctrip_storage.h / .c
│   └── ctrip_storage_client.h / .c
├── request/           # 请求调度层
│   ├── ctrip_storage_request.h / .c
│   ├── ctrip_storage_request_async.c
│   ├── ctrip_storage_request_sync.c
│   ├── ctrip_storage_request_utils.h / .c
│   ├── ctrip_storage_request_meta_scan.c
│   ├── ctrip_storage_thread_request.c
│   └── ctrip_storage_batch.h
├── objects/           # 各数据类型实现
│   ├── ctrip_storage_data.h
│   ├── ctrip_storage_objects.h / .c
│   ├── ctrip_storage_object_meta.h / .c
│   ├── ctrip_storage_object_wholekey.c
│   ├── ctrip_storage_object_hash.c
│   ├── ctrip_storage_object_set.c
│   ├── ctrip_storage_object_list.c
│   ├── ctrip_storage_object_zset.c
│   ├── ctrip_storage_object_bitmap.c
│   ├── ctrip_storage_persist.h
│   └── ctrip_storage_scan.h
├── infra/             # 基础服务
│   ├── ctrip_storage_lock.h / .c
│   ├── ctrip_storage_evict.h / .c
│   ├── ctrip_storage_thread.h / .c
│   ├── ctrip_storage_config.h / .c
│   ├── ctrip_storage_rate_limit.h / .c
│   ├── ctrip_storage_metric.h / .c
│   ├── ctrip_storage_expire.h / .c
│   ├── ctrip_storage_filter.h / .c
│   ├── ctrip_storage_commands.h / .c
│   ├── ctrip_storage_rio.h / .c
│   ├── ctrip_storage_repl.c
│   ├── ctrip_storage_trace.h / .c
│   ├── ctrip_storage_debug.h
│   ├── ctrip_storage_error.h
│   └── ctrip_storage_utils.h
├── ds/                # 基础数据结构
│   ├── absent_cache.h / .c      → ctrip_storage_absent_cache
│   ├── buffered_allocator.h / .c → ctrip_storage_buffered_allocator
│   ├── cuckoo_filter.h / .c     → ctrip_storage_cuckoo_filter
│   ├── roaring_bitmap.h / .c    → ctrip_storage_roaring_bitmap
│   └── ctrip_storage_adlist.h / .c
└── engines/           # 存储引擎实现
    ├── memory_storage_engine.c
    └── rocksdb_storage_engine.c
```

> **注意**：以上目录结构为逻辑分组建议，实际是否拆分子目录需要综合评估 Makefile 改动成本。最小化变更方案是保持扁平结构，仅统一文件命名前缀。

---

## 关键接口说明

### StorageEngine 函数表（`ctrip_storage_types.h`）

```c
typedef struct StorageEngine {
    /* 引擎生命周期 */
    int (*open)(void *ctx, ...);
    void (*close)(void *ctx);
    /* 数据操作 */
    int (*get)(void *ctx, sds key, sds *val);
    int (*put)(void *ctx, sds key, sds val);
    int (*del)(void *ctx, sds key);
    /* scan 操作 */
    void *(*scanOpen)(void *ctx, sds cursor);
    int   (*scanNext)(void *handle, sds *key, sds *val);
    void  (*scanClose)(void *handle);
} StorageEngine;
```

### swapDataType 函数表（`ctrip_storage_data.h`）

每种 Redis 数据类型（string/hash/set/list/zset/bitmap）实现此接口：

```c
typedef struct swapDataType {
    char *name;
    /* swap 意图分析：决定是 NOP/SWAP_IN/SWAP_OUT/DEL */
    int (*swapAna)(swapData *d, int thd, struct keyRequest *req, int *intention, ...);
    /* 编码：Redis 对象 → RocksDB value */
    int (*encodeKeys)(swapData *d, int intention, void *datactx, ...);
    int (*encodeData)(swapData *d, int intention, void *datactx, ...);
    /* 解码：RocksDB value → Redis 对象 */
    int (*decodeData)(swapData *d, int num, ...);
    /* swap 完成后的内存操作 */
    int (*swapIn)(swapData *d, void *datactx);
    int (*swapOut)(swapData *d, void *datactx);
    int (*swapDel)(swapData *d, void *datactx);
    /* 清理 */
    void (*free)(swapData *d, void *datactx);
} swapDataType;
```

---

*文档生成时间：2026-04-24*
*基于代码分支：latte/8.6.2*
