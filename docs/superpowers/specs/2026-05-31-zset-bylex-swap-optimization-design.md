# ZSet BYLEX Swap 优化设计文档

**日期**: 2026-05-31
**状态**: 已批准
**作者**: brainstorming session

## 1. 背景与问题

### 1.1 当前行为

Redis on Rocks (ROR) 的 swap 模式下，ZSet 的 BYLEX 系列命令（`ZRANGEBYLEX`、`ZREVRANGEBYLEX`、`ZLEXCOUNT`、`ZREMRANGEBYLEX`）在 swap 层会被解析为 `KEYREQUEST_TYPE_SUBKEY`（num_subkeys=0），触发 **全量 swap-in**：将整个 ZSet 从 RocksDB 加载到内存后再执行命令。

### 1.2 根本原因

`getKeyRequestsZrangeGeneric` 函数的 switch 语句中，`ZRANGE_LEX` 落入 `default` 分支，创建了空 subkey 列表的 `KEYREQUEST_TYPE_SUBKEY` 请求。swap 层没有对应的 `KEYREQUEST_TYPE_LEX` 类型，无法感知字典序范围信息。

### 1.3 优化机会

DATA_CF 中同一 ZSet 的 member 存储格式为 `[dbid][keylen][key][version][flag=SUBKEY][member]`。由于 RocksDB key 天然按字典序排列，且所有 member 共享相同前缀，member 之间已经是字典序排列。BYLEX 的字典序范围查询可以直接映射为 DATA_CF 上的 range scan。

## 2. 设计方案

### 2.1 核心思路

新增 `KEYREQUEST_TYPE_LEX` 请求类型，完全镜像现有 `KEYREQUEST_TYPE_SCORE`（BYSCORE）的 partial swap 路径，但扫描 DATA_CF 而非 SCORE_CF，范围边界由 member 字典序决定而非 score。

### 2.2 数据流对比

```
BYSCORE 命令                          BYLEX 命令
    │                                      │
    ▼                                      ▼
getKeyRequestsZrangeGeneric            getKeyRequestsZrangeGeneric
  type=KEYREQUEST_TYPE_SCORE             type=KEYREQUEST_TYPE_LEX (新增)
  携带 zrangespec                        携带 zlexrangespec
    │                                      │
    ▼                                      ▼
zsetSwapAna                            zsetSwapAna
  ctx_type=ZSET_SWAP_CTX_TYPE_ZS         ctx_type=ZSET_SWAP_CTX_TYPE_LEX (新增)
    │                                      │
    ▼                                      ▼
zsetEncodeRange                        zsetEncodeRange
  cf=SCORE_CF                            cf=DATA_CF
  encodeScoreKey(min/max)                encodeDataKey(min/max member)
    │                                      │
    ▼                                      ▼
zsetDecodeScoreData                    zsetDecodeLexData (新增)
  从 SCORE_CF key 解析                   从 DATA_CF key/value 解析
    │                                      │
    ▼                                      ▼
创建 partial zset 对象                  创建 partial zset 对象
```

### 2.3 边界策略：全闭区间 swap

**设计决策**：所有 BYLEX 请求统一按闭区间 `[min, max]` 构造 swap 范围，不使用任何 EXCLUDE 或 PREFIX_MATCH flags。开闭区间的过滤由 Redis 命令层完成。

**代价**：最多多 swap 2 个边界 member（开区间时），完全可忽略。

**收益**：
- 不需要 `nextMemberKey` 函数
- 不需要 EXCLUDE flags
- 不需要 decode 阶段过滤
- 避免变长 member 的前缀陷阱

**对比**：

| 原始区间 | swap 范围 | 多 swap 的 | 命令层过滤 |
|---------|----------|-----------|-----------|
| `[abc, xyz]` | abc ~ xyz | 无 | 无 |
| `(abc, xyz]` | abc ~ xyz | 多了 abc | 命令层排除 abc |
| `[abc, xyz)` | abc ~ xyz | 多了 xyz | 命令层排除 xyz |
| `(abc, xyz)` | abc ~ xyz | 多了 abc + xyz | 命令层都排除 |

### 2.4 反向扫描的前缀越界修正

`seek_for_prev(encodeDataKey("xyz"))` 可能定位到 member "xyzz"（以 "xyz" 为前缀的更长字符串），因为 "xyzz" > "xyz" 但在 RocksDB 中排在 "xyz" 之后。

**修复**：在 `ctrip_swap_rio.c` 的 `RIODoIterate` 中，反向扫描初始定位后增加单次 `prev()` 修正：

```c
if (reverse && !prefix_match) {
    rawkey = rocksdb_iter_key(iter, &klen);
    if (rawkey && klen > end_len && memcmp(rawkey, end, end_len) == 0) {
        rocksdb_iter_prev(iter);
    }
}
```

### 2.5 涉及的四个命令

| 命令 | 类型 | swap 行为 |
|------|------|----------|
| `ZRANGEBYLEX` | 只读 | DATA_CF 范围扫描，返回 partial zset |
| `ZREVRANGEBYLEX` | 只读 | DATA_CF 反向范围扫描，返回 partial zset |
| `ZLEXCOUNT` | 只读 | DATA_CF 范围扫描，仅返回 count |
| `ZREMRANGEBYLEX` | 写 | swap in 范围内 member → 命令层删除 → dirty subkeys 异步淘汰 |

## 3. 各层代码变更

### 3.1 数据结构定义（`src/ctrip_swap.h`）

1. **新增 `KEYREQUEST_TYPE_LEX`**（值为 7）
2. **`keyRequest` union 新增 `zl` 成员**：
   ```c
   struct {
       zlexrangespec* rangespec;
       int reverse;
       int limit;
   } zl; /* zset lex */
   ```
3. **新增 `ZSET_SWAP_CTX_TYPE_LEX`**（值为 2）
4. **`zsetDataCtx` union 新增 `zl` 成员**：
   ```c
   struct {
       zlexrangespec* rangespec;
       int reverse;
       int limit;
   } zl;
   ```

### 3.2 命令解析层（`src/ctrip_swap_cmd.c`）

1. **`getKeyRequestsZrangeGeneric`** 新增 `ZRANGE_LEX` case：
   - 解析 `zlexrangespec`
   - 调用新增的 `getKeyRequestsAppendLexResult`

2. **新增 `getKeyRequestsAppendLexResult` 函数**：
   - 类比 `getKeyRequestsAppendScoreResult`
   - `type = KEYREQUEST_TYPE_LEX`
   - 存储 `zl.reverse`、`zl.rangespec`、`zl.limit`

3. **`keyRequestDeinit`** 新增 `KEYREQUEST_TYPE_LEX` 的 `zlexrangespec` 释放

### 3.3 Swap 分析层（`src/ctrip_swap_zset.c`）

1. **`zsetSwapAna`** 新增 `KEYREQUEST_TYPE_LEX` 分支：
   - 设置 `datactx->type = ZSET_SWAP_CTX_TYPE_LEX`
   - 拷贝 `zl.reverse`、`zl.limit`、`zl.rangespec`
   - 处理 `SWAP_IN_DEL`（ZREMRANGEBYLEX）

2. **`zsetSwapAnaAction`**：不需要改动
   - `ZSET_SWAP_CTX_TYPE_LEX != ZSET_SWAP_CTX_TYPE_NONE`，自动选择 `ROCKS_ITERATE`

3. **`zsetEncodeRange`** 新增 `ZSET_SWAP_CTX_TYPE_LEX` 分支：
   - `*pcf = DATA_CF`
   - `*flags = 0`（无 PREFIX_MATCH、无 EXCLUDE）
   - `*flags |= ROCKS_ITERATE_REVERSE`（如果 reverse）
   - min 边界：`-` 用 `rocksEncodeDataRangeStartKey`，否则 `rocksEncodeDataKey(min_member)`
   - max 边界：`+` 用 `rocksEncodeDataRangeEndKey`，否则 `rocksEncodeDataKey(max_member)`

4. **`zsetDecodeData`** 新增 `ZSET_SWAP_CTX_TYPE_LEX` 分发

5. **新增 `zsetDecodeLexData` 函数**：
   - 从 DATA_CF rawkey 用 `rocksDecodeDataKey` 解析 member
   - 从 rawval 用 `zsetDecodeSubval` 解析 score
   - 构建 zset 对象

6. **`zsetDataCtxDeinit`** 新增 `ZSET_SWAP_CTX_TYPE_LEX` 释放

### 3.4 迭代器修正（`src/ctrip_swap_rio.c`）

在 `RIODoIterate` 函数中，`seek_for_prev` 之后、EXCLUDE 处理之前，增加反向扫描的前缀越界修正（见 2.4 节）。

### 3.5 不需要改动的部分

| 组件 | 原因 |
|------|------|
| `zsetSwapIn` | 通用逻辑，冷键 dbAdd / 温键丢弃 result |
| `zsetSwapOut` | 通用 dirty subkeys 淘汰机制 |
| `zsetSwapAnaAction` | `type != NONE` 自动兼容 |
| `t_zset.c` 命令层 | 命令本身已正确处理 `[`/`(` 过滤 |
| RocksDB 存储格式 | 复用现有 DATA_CF 格式 |
| `swapOnKey` 框架层 | 通用流程 |

## 4. 边界 Case

### 4.1 数据层面

| Case | 处理 |
|------|------|
| ZSet 不存在 | `SWAP_NOP`，命令层返回空集/0 |
| 冷键 | LEX 范围扫描 → partial zset → dbAdd |
| 温键 | result 被丢弃，命令在完整对象上执行 |
| 范围 min > max | `zslParseLexRange` 返回 `C_ERR` |
| `[- +]` 全量 | 退化到全量 swap（功能正确，性能退化） |
| 空 member `""` | encodeDataKey subkey 为空，Seek 正确 |
| 反向越界 | 单次 prev() 修正 |
| 前缀关系 member（"abc"/"abcd"） | 闭区间策略，前缀不会造成误排除 |

### 4.2 内存与性能

| 关注点 | 处理 |
|--------|------|
| 范围特别大 | limit 参数限制最多加载量；无 limit 时退化到当前行为 |
| OOM 保护 | 现有 `rioMayOOM` 检查集成在迭代器主循环中 |
| 反向扫描开销 | 单次 prev() 修正，O(1) |

### 4.3 ZREMRANGEBYLEX 一致性

写命令执行流程：swap in 范围内 member → 命令层删除 → dirty subkeys 记录 → 异步淘汰。与 BYSCORE 的 `ZREMRANGEBYSCORE` 路径完全一致。

## 5. 测试策略

### 5.1 基础功能测试

| 测试 | 验证内容 |
|------|---------|
| `ZRANGEBYLEX key [a [z` | 闭区间 partial swap |
| `ZRANGEBYLEX key (a (z` | 开区间 partial swap，命令层过滤 |
| `ZRANGEBYLEX key [a (z` | 混合区间 |
| `ZRANGEBYLEX key - +` | 全量范围退化 |
| `ZRANGEBYLEX key - [m` | 负无穷 |
| `ZRANGEBYLEX key (m +` | 正无穷开区间 |

### 5.2 反向扫描测试

| 测试 | 验证内容 |
|------|---------|
| `ZREVRANGEBYLEX key [z [a` | 反向闭区间 |
| `ZREVRANGEBYLEX key (z (a` | 反向开区间 |
| `ZREVRANGEBYLEX key + -` | 反向全量 |

### 5.3 计数与删除测试

| 测试 | 验证内容 |
|------|---------|
| `ZLEXCOUNT key [a [z` | 计数正确 |
| `ZREMRANGEBYLEX key [a [z` | 删除正确，dirty subkeys 写回 |
| `ZREMRANGEBYLEX key - +` | 全量删除后 key 变冷 |

### 5.4 边界 Case 测试

| 测试 | 验证内容 |
|------|---------|
| 前缀关系 member（"abc"/"abcd"/"abcde"） | 不被误排除 |
| 空 member `""` | 正确处理 |
| 空 ZSet | 返回空结果 |
| 单 member ZSet | 范围正确 |
| LIMIT 参数 | 正确传递和生效 |
| 冷键 vs 温键 | 行为差异正确 |

### 5.5 验证方法

对每个测试 case：
1. 纯内存模式执行 BYLEX → expected_result
2. swap 模式执行相同命令 → actual_result
3. 断言 expected_result == actual_result

## 6. 设计决策汇总

| 决策 | 选择 | 理由 |
|------|------|------|
| 方案 | 镜像 BYSCORE 路径 | 与现有架构完全对称 |
| 边界策略 | 全闭区间 swap | 最简化逻辑，最多 2 个多余 member |
| EXCLUDE flags | 不使用 | 避免变长 member 前缀陷阱 |
| PREFIX_MATCH | 不使用 | 同上 |
| 反向越界 | 单次 prev() 修正 | O(1) 开销 |
| ZREMRANGEBYLEX | swap in 后走 dirty 流程 | 复用现有机制 |
| decode | 新增 zsetDecodeLexData | DATA_CF 与 SCORE_CF 格式不同 |

## 7. 变更文件汇总

| 文件 | 变更类型 |
|------|---------|
| `src/ctrip_swap.h` | 新增 type 定义、union 成员、ctx type |
| `src/ctrip_swap_cmd.c` | 新增 LEX case、appendLexResult、释放逻辑 |
| `src/ctrip_swap_zset.c` | zsetSwapAna 新增分支、zsetEncodeRange 新增分支、zsetDecodeLexData 新函数、释放逻辑 |
| `src/ctrip_swap_rio.c` | 反向 seek_for_prev 前缀越界修正 |
