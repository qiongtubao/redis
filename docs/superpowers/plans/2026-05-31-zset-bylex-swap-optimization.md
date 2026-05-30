# ZSet BYLEX Swap 优化实施计划

## 概述

**目标**: 优化 ZSet BYLEX 系列命令的 swap 性能，避免全量 swap-in，改为部分 swap（仅加载范围内的 member）

**方法**:
- 新增 `KEYREQUEST_TYPE_LEX` 请求类型，镜像 `KEYREQUEST_TYPE_SCORE` 的实现
- 新增 `ZSET_SWAP_CTX_TYPE_LEX` 上下文类型
- 在 DATA_CF 上进行范围扫描（而非全量扫描）
- 不使用 EXCLUDE/PREFIX_MATCH flags，全闭区间 swap，命令层过滤

**技术栈**: C (Redis swap layer), RocksDB, Tcl (测试)

**参考文档**: `docs/superpowers/specs/2026-05-31-zset-bylex-swap-optimization-design.md`

---

## 文件结构

### 修改文件

1. **src/ctrip_swap.h**
   - 新增 `KEYREQUEST_TYPE_LEX` 常量
   - `keyRequest` union 新增 `zl` 成员
   - 新增 `ZSET_SWAP_CTX_TYPE_LEX` 常量
   - `zsetDataCtx` union 新增 `zl` 成员
   - 新增 `getKeyRequestsAppendLexResult` 函数声明

2. **src/ctrip_swap_cmd.c**
   - 新增 `getKeyRequestsAppendLexResult` 函数
   - 修改 `getKeyRequestsZrangeGeneric` 添加 `ZRANGE_LEX` case
   - 修改 `copyKeyRequest` 添加 `KEYREQUEST_TYPE_LEX` case
   - 修改 `moveKeyRequest` 添加 `KEYREQUEST_TYPE_LEX` case
   - 修改 `keyRequestDeinit` 添加 `KEYREQUEST_TYPE_LEX` case

3. **src/ctrip_swap_zset.c**
   - 修改 `zsetSwapAna` 添加 `KEYREQUEST_TYPE_LEX` 分支
   - 修改 `zsetEncodeRange` 添加 `ZSET_SWAP_CTX_TYPE_LEX` 分支
   - 修改 `zsetDataCtxDeinit`（或等价清理函数）添加 `ZSET_SWAP_CTX_TYPE_LEX` 释放逻辑
   - **注意**: `zsetDecodeData` 无需修改 — LEX 扫描使用 DATA_CF，自动走 `zsetDecodeBigData` 路径

4. **src/ctrip_swap_rio.c**
   - 修改 `RIODoIterate` 添加反向 seek_for_prev 越界修正

5. **tests/swap/unit/zset.tcl**
   - 新增 BYLEX 优化相关测试用例

---

## 实施任务

### 任务 1: 数据结构定义 (ctrip_swap.h)

**文件**:
- 修改: `src/ctrip_swap.h:182` (KEYREQUEST_TYPE_LEX)
- 修改: `src/ctrip_swap.h:215-220` (keyRequest union zl)
- 修改: `src/ctrip_swap.h:935` (ZSET_SWAP_CTX_TYPE_LEX)
- 修改: `src/ctrip_swap.h:940-946` (zsetDataCtx union zl)

**步骤**:

1. **添加 KEYREQUEST_TYPE_LEX 常量**
   ```c
   #define KEYREQUEST_TYPE_KEY    0
   #define KEYREQUEST_TYPE_SUBKEY 1
   #define KEYREQUEST_TYPE_RANGE  2
   #define KEYREQUEST_TYPE_SCORE  3
   #define KEYREQUEST_TYPE_SAMPLE 4
   #define KEYREQUEST_TYPE_BTIMAP_OFFSET  5
   #define KEYREQUEST_TYPE_BTIMAP_RANGE  6
   #define KEYREQUEST_TYPE_LEX    7
   ```

2. **keyRequest union 新增 zl 成员**
   ```c
   union {
     struct {
       int num_subkeys;
       robj **subkeys;
     } b; /* subkey: hash, set */
     struct {
       int num_ranges;
       range *ranges;
     } l; /* range: list */
     struct {
       zrangespec* rangespec;
       int reverse;
       int limit;
     } zs; /* zset score*/
     struct {
       zlexrangespec* rangespec;
       int reverse;
       int limit;
     } zl; /* zset lex */
     struct {
       int count;
     } sp; /* sample */
     struct {
       long long offset;
     } bo; /* bitmap offset*/
     struct {
       long long start;
       long long end;
     } br; /* bitmap range*/
   };
   ```

3. **添加 ZSET_SWAP_CTX_TYPE_LEX 常量**
   ```c
   #define ZSET_SWAP_CTX_TYPE_NONE 0
   #define ZSET_SWAP_CTX_TYPE_ZS 1
   #define ZSET_SWAP_CTX_TYPE_LEX 2
   ```

4. **zsetDataCtx union 新增 zl 成员**
   ```c
   typedef struct zsetDataCtx {
     baseBigDataCtx bdc;
     int type;
     union {
       struct {
         zrangespec* rangespec;
         int reverse;
         int limit;
       } zs;
       struct {
         zlexrangespec* rangespec;
         int reverse;
         int limit;
       } zl;
     };
   } zsetDataCtx;
   ```

5. **添加 getKeyRequestsAppendLexResult 函数声明**
   ```c
   void getKeyRequestsAppendLexResult(getKeyRequestsResult *result, int level,
           robj *key, int reverse, zlexrangespec* rangespec, int limit,
           int cmd_intention, int cmd_intention_flags, uint64_t cmd_flags, int dbid);
   ```

6. **编译验证**
   ```bash
   make clean && make -j$(nproc)
   ```
   预期: 编译通过（仅添加声明，无逻辑变更）

7. **提交**
   ```bash
   git add src/ctrip_swap.h
   git commit -m "feat(swap): add KEYREQUEST_TYPE_LEX and ZSET_SWAP_CTX_TYPE_LEX definitions"
   ```

---

### 任务 2: keyRequest 生命周期管理 (ctrip_swap_cmd.c)

**文件**:
- 新增: `getKeyRequestsAppendLexResult` 函数 (ctrip_swap_cmd.c:1285-1302 附近)
- 修改: `copyKeyRequest` (ctrip_swap_cmd.c:1108-1112)
- 修改: `moveKeyRequest` (ctrip_swap_cmd.c:1158-1163)
- 修改: `keyRequestDeinit` (ctrip_swap_cmd.c:1205-1210)

**步骤**:

1. **新增 getKeyRequestsAppendLexResult 函数**
   ```c
   void getKeyRequestsAppendLexResult(getKeyRequestsResult *result, int level,
           robj *key, int reverse, zlexrangespec* rangespec, int limit, int cmd_intention,
           int cmd_intention_flags, uint64_t cmd_flags, int dbid) {
       expandKeyRequests(result);
       keyRequest *key_request = &result->key_requests[result->num++];
       key_request->level = level;
       key_request->key = key;
       key_request->type = KEYREQUEST_TYPE_LEX;
       key_request->zl.reverse = reverse;
       key_request->zl.rangespec = rangespec;
       key_request->zl.limit = limit;
       key_request->cmd_intention = cmd_intention;
       key_request->cmd_intention_flags = cmd_intention_flags;
       key_request->cmd_flags = cmd_flags;
       key_request->dbid = dbid;
       key_request->trace = NULL;
       key_request->deferred = 0;
   }
   ```

2. **修改 copyKeyRequest 添加 LEX case**
   ```c
   case KEYREQUEST_TYPE_SCORE:
       dst->zs.rangespec = zrangespecdup(src->zs.rangespec);
       dst->zs.reverse = src->zs.reverse;
       dst->zs.limit = src->zs.limit;
       break;
   case KEYREQUEST_TYPE_LEX:
       dst->zl.rangespec = zmalloc(sizeof(zlexrangespec));
       dst->zl.rangespec->min = sdsdup(src->zl.rangespec->min);
       dst->zl.rangespec->max = sdsdup(src->zl.rangespec->max);
       dst->zl.rangespec->minex = src->zl.rangespec->minex;
       dst->zl.rangespec->maxex = src->zl.rangespec->maxex;
       dst->zl.reverse = src->zl.reverse;
       dst->zl.limit = src->zl.limit;
       break;
   ```

3. **修改 moveKeyRequest 添加 LEX case**
   ```c
   case KEYREQUEST_TYPE_SCORE:
       dst->zs.rangespec = src->zs.rangespec;
       src->zs.rangespec = NULL;
       dst->zs.reverse = src->zs.reverse;
       dst->zs.limit = src->zs.limit;
       break;
   case KEYREQUEST_TYPE_LEX:
       dst->zl.rangespec = src->zl.rangespec;
       src->zl.rangespec = NULL;
       dst->zl.reverse = src->zl.reverse;
       dst->zl.limit = src->zl.limit;
       break;
   ```

4. **修改 keyRequestDeinit 添加 LEX case**
   ```c
   case KEYREQUEST_TYPE_SCORE:
       if (key_request->zs.rangespec != NULL) {
           zfree(key_request->zs.rangespec);
           key_request->zs.rangespec = NULL;
       }
       break;
   case KEYREQUEST_TYPE_LEX:
       if (key_request->zl.rangespec != NULL) {
           zslFreeLexRange(key_request->zl.rangespec);
           zfree(key_request->zl.rangespec);
           key_request->zl.rangespec = NULL;
       }
       break;
   ```

5. **编译验证**
   ```bash
   make -j$(nproc)
   ```
   预期: 编译通过

6. **提交**
   ```bash
   git add src/ctrip_swap_cmd.c
   git commit -m "feat(swap): implement keyRequest lifecycle for KEYREQUEST_TYPE_LEX"
   ```

---

### 任务 3: 命令解析层集成 (ctrip_swap_cmd.c)

**文件**:
- 修改: `getKeyRequestsZrangeGeneric` (ctrip_swap_cmd.c:2034-2100)

**步骤**:

1. **修改 getKeyRequestsZrangeGeneric 添加 ZRANGE_LEX case**

   在 switch 语句中添加 ZRANGE_LEX 分支：
   ```c
   switch (rangetype) {
       case ZRANGE_SCORE: {
           zrangespec* spec = zmalloc(sizeof(zrangespec));
           if (zslParseRange(minobj, maxobj, spec) != C_OK) {
               decrRefCount(key);
               zfree(spec);
               return C_ERR;
           }
           getKeyRequestsAppendScoreResult(result, REQUEST_LEVEL_KEY, key,
               direction == ZRANGE_DIRECTION_REVERSE, spec, opt_offset + opt_limit,
               cmd->intention, cmd->intention_flags, cmd->flags, dbid);
           break;
       }
       case ZRANGE_LEX: {
           zlexrangespec* spec = zmalloc(sizeof(zlexrangespec));
           if (zslParseLexRange(minobj, maxobj, spec) != C_OK) {
               decrRefCount(key);
               zfree(spec);
               return C_ERR;
           }
           getKeyRequestsAppendLexResult(result, REQUEST_LEVEL_KEY, key,
               direction == ZRANGE_DIRECTION_REVERSE, spec, opt_offset + opt_limit,
               cmd->intention, cmd->intention_flags, cmd->flags, dbid);
           break;
       }
       default:
           getKeyRequestsAppendSubkeyResult(result, REQUEST_LEVEL_KEY, key,
               0, NULL, cmd->intention, cmd->intention_flags, cmd->flags, dbid);
           break;
   }
   ```

2. **编译验证**
   ```bash
   make -j$(nproc)
   ```
   预期: 编译通过

3. **提交**
   ```bash
   git add src/ctrip_swap_cmd.c
   git commit -m "feat(swap): parse BYLEX commands to KEYREQUEST_TYPE_LEX"
   ```

---

### 任务 4: Swap 分析层集成 (ctrip_swap_zset.c)

**文件**:
- 修改: `zsetSwapAna` (ctrip_swap_zset.c:189-250)

**步骤**:

1. **修改 zsetSwapAna 添加 KEYREQUEST_TYPE_LEX 分支**

   在 SWAP_IN case 中，紧跟 KEYREQUEST_TYPE_SCORE 分支后添加：
   ```c
   } else if (req->type == KEYREQUEST_TYPE_LEX) {
       datactx->type = ZSET_SWAP_CTX_TYPE_LEX;
       datactx->zl.reverse = req->zl.reverse;
       datactx->zl.limit = req->zl.limit;
       datactx->zl.rangespec = req->zl.rangespec;
       req->zl.rangespec = NULL; /* 转移所有权 */
       *intention = SWAP_IN;
       *intention_flags = 0;

       if (cmd_intention_flags == SWAP_IN_DEL
           || cmd_intention_flags & SWAP_IN_OVERWRITE) {
           objectMeta *meta = swapDataObjectMeta(data);
           if (meta->len == 0) {
               *intention = SWAP_DEL;
               *intention_flags = SWAP_FIN_DEL_SKIP;
           } else {
               *intention = SWAP_IN;
               *intention_flags = SWAP_EXEC_IN_DEL;
           }
       }
   }
   ```

2. **编译验证**
   ```bash
   make -j$(nproc)
   ```
   预期: 编译通过

3. **提交**
   ```bash
   git add src/ctrip_swap_zset.c
   git commit -m "feat(swap): handle KEYREQUEST_TYPE_LEX in zsetSwapAna"
   ```

---

### 任务 5: 编码范围逻辑 (ctrip_swap_zset.c)

**文件**:
- 修改: `zsetEncodeRange` (ctrip_swap_zset.c:498-533)

**步骤**:

1. **修改 zsetEncodeRange 添加 ZSET_SWAP_CTX_TYPE_LEX 分支**

   在 `if (datactx->type == ZSET_SWAP_CTX_TYPE_ZS)` 后添加：
   ```c
   } else if (datactx->type == ZSET_SWAP_CTX_TYPE_LEX) {
       *pcf = DATA_CF;
       *limit = datactx->zl.limit;
       *flags = 0;
       if (datactx->zl.reverse) *flags |= ROCKS_ITERATE_REVERSE;

       /* min 边界 */
       if (datactx->zl.rangespec->min == shared.minstring) {
           *start = rocksEncodeDataRangeStartKey(data->db, data->key->ptr, version);
       } else {
           *start = rocksEncodeDataKey(data->db, data->key->ptr, version,
               datactx->zl.rangespec->min);
       }

       /* max 边界 */
       if (datactx->zl.rangespec->max == shared.maxstring) {
           *end = rocksEncodeDataRangeEndKey(data->db, data->key->ptr, version);
       } else {
           *end = rocksEncodeDataKey(data->db, data->key->ptr, version,
               datactx->zl.rangespec->max);
       }
   }
   ```

   **注意**: 不设置 `ROCKS_ITERATE_PREFIX_MATCH`、`LOW_BOUND_EXCLUDE`、`HIGH_BOUND_EXCLUDE`，全闭区间 swap。

2. **编译验证**
   ```bash
   make -j$(nproc)
   ```
   预期: 编译通过

3. **提交**
   ```bash
   git add src/ctrip_swap_zset.c
   git commit -m "feat(swap): encode DATA_CF range for ZSET_SWAP_CTX_TYPE_LEX"
   ```

---

### 任务 6: 解码逻辑验证（无需代码变更）

**文件**: `src/ctrip_swap_zset.c`

**分析**:

`zsetDecodeData`（第 628 行）根据 CF 类型分发：
- `DATA_CF` → `zsetDecodeBigData`（从 DATA_CF key 解析 member + 从 value 解析 score）
- `SCORE_CF` → `zsetDecodeScoreData`

LEX 扫描也使用 DATA_CF，key 格式 `[dbid][keylen][key][version][flag][member]` 与现有全量 swap 完全相同。`zsetDecodeBigData` 已经正确处理：
1. `rocksDecodeDataKey` 解析出 member（subkeystr/slen）
2. `zsetDecodeSubval` 从 rawval 解析 score
3. `zsetAdd` 插入 zset 对象

**结论**: LEX 的 DATA_CF 扫描结果自动走 `zsetDecodeBigData` 路径，无需新增 `zsetDecodeLexData`，无需修改 `swapDataDecodeData` 接口。

- [ ] **Step 1: 验证编译**
  ```bash
  make -j$(nproc)
  ```
  预期: 编译通过（前面任务的变更已包含所有必要的修改）

- [ ] **Step 2: 确认 DATA_CF 解码路径正确**

  确认 `zsetEncodeRange` 中 LEX 分支设置 `*pcf = DATA_CF`，`zsetDecodeData` 中 `cfs[0] == DATA_CF` 条件命中 `zsetDecodeBigData`。无需代码变更。

- [ ] **Step 3: 提交（空提交标记验证完成）**
  ```bash
  git commit --allow-empty -m "verify(swap): DATA_CF decode path works for LEX without changes"
  ```

---

### 任务 7: 反向扫描越界修正 (ctrip_swap_rio.c)

**文件**:
- 修改: `RIODoIterate` (ctrip_swap_rio.c:268-271)

**步骤**:

1. **修改 RIODoIterate 添加反向 seek_for_prev 越界修正**

   在 `rocksdb_iter_seek_for_prev` 后添加：
   ```c
   if (reverse) rocksdb_iter_seek_for_prev(iter, end, end_len);
   else rocksdb_iter_seek(iter, start, start_len);
   if (!rocksdb_iter_valid(iter)) goto end;

   /* 修正反向扫描的越界问题:
    * seek_for_prev 可能定位到 end_key 的前缀扩展（如 end="xyz" 定位到 "xyzz"）
    * 需要回退到真正的 <= end 的位置 */
   if (reverse && !prefix_match) {
       rawkey = rocksdb_iter_key(iter, &klen);
       if (rawkey && klen > end_len && memcmp(rawkey, end, end_len) == 0) {
           rocksdb_iter_prev(iter);
           if (!rocksdb_iter_valid(iter)) goto end;
       }
   }
   ```

2. **编译验证**
   ```bash
   make -j$(nproc)
   ```
   预期: 编译通过

3. **提交**
   ```bash
   git add src/ctrip_swap_rio.c
   git commit -m "fix(swap): correct reverse seek_for_prev prefix overflow in iterator"
   ```

---

### 任务 8: 上下文释放逻辑 (ctrip_swap_zset.c)

**文件**:
- 修改: `zsetDataCtxDeinit` (ctrip_swap_zset.c)

**步骤**:

1. **查找 zsetDataCtxDeinit 函数位置**
   ```bash
   grep -n "zsetDataCtxDeinit\|zsetDataCtxDestroy" src/ctrip_swap_zset.c
   ```

2. **如果函数不存在，查找 swapDataSetupZSet 中的释放逻辑**
   ```bash
   grep -A 30 "swapDataSetupZSet" src/ctrip_swap_zset.c
   ```

3. **添加 ZSET_SWAP_CTX_TYPE_LEX 释放逻辑**

   在 datactx 清理函数中添加：
   ```c
   if (datactx->type == ZSET_SWAP_CTX_TYPE_ZS) {
       if (datactx->zs.rangespec != NULL) {
           zfree(datactx->zs.rangespec);
           datactx->zs.rangespec = NULL;
       }
   } else if (datactx->type == ZSET_SWAP_CTX_TYPE_LEX) {
       if (datactx->zl.rangespec != NULL) {
           zslFreeLexRange(datactx->zl.rangespec);
           zfree(datactx->zl.rangespec);
           datactx->zl.rangespec = NULL;
       }
   }
   ```

4. **编译验证**
   ```bash
   make -j$(nproc)
   ```
   预期: 编译通过

5. **提交**
   ```bash
   git add src/ctrip_swap_zset.c
   git commit -m "feat(swap): cleanup zsetDataCtx for LEX type"
   ```

---

### 任务 9: 基础功能测试 (tests/swap/unit/zset.tcl)

**文件**:
- 新增: BYLEX 优化测试用例 (tests/swap/unit/zset.tcl:504 附近)

**步骤**:

1. **添加 BYLEX 冷键测试用例**

   在现有 BYLEX 测试后添加：
   ```tcl
   test "ZRANGEBYLEX with cold key - closed interval" {
       # 创建 zset 并驱逐到冷键
       r del zset
       r zadd zset 0 alpha 0 bar 0 cool 0 down 0 elephant 0 foo 0 great 0 hill 0 omega
       r swap.evict zset
       wait_key_cold r zset

       # 测试闭区间
       assert_equal {alpha bar cool} [r zrangebylex zset - \[cool]
       assert_equal {bar cool down} [r zrangebylex zset \[bar \[down]
       assert_equal {great hill omega} [r zrangebylex zset \[g +]
   }

   test "ZRANGEBYLEX with cold key - open interval" {
       r del zset
       r zadd zset 0 alpha 0 bar 0 cool 0 down 0 elephant
       r swap.evict zset
       wait_key_cold r zset

       # 测试开区间
       assert_equal {alpha bar} [r zrangebylex zset - (cool]
       assert_equal {cool} [r zrangebylex zset (bar (down]
       assert_equal {down elephant} [r zrangebylex zset (cool +]
   }

   test "ZREVRANGEBYLEX with cold key" {
       r del zset
       r zadd zset 0 alpha 0 bar 0 cool 0 down 0 elephant
       r swap.evict zset
       wait_key_cold r zset

       # 测试反向扫描
       assert_equal {cool bar alpha} [r zrevrangebylex zset \[cool -]
       assert_equal {down cool bar} [r zrevrangebylex zset \[down \[bar]
       assert_equal {elephant down} [r zrevrangebylex zset + (cool]
   }

   test "ZLEXCOUNT with cold key" {
       r del zset
       r zadd zset 0 alpha 0 bar 0 cool 0 down 0 elephant 0 foo 0 great 0 hill 0 omega
       r swap.evict zset
       wait_key_cold r zset

       # 测试计数
       assert_equal 9 [r zlexcount zset - +]
       assert_equal 3 [r zlexcount zset \[ele \[h]
       assert_equal 2 [r zlexcount zset (ele (great]
   }

   test "ZRANGEBYLEX with cold key - prefix members" {
       # 测试前缀关系 member 不被误排除
       r del zset
       r zadd zset 0 abc 0 abcd 0 abcde 0 xyz
       r swap.evict zset
       wait_key_cold r zset

       assert_equal {abc abcd abcde} [r zrangebylex zset \[abc \[abcde]
       assert_equal {abcd abcde} [r zrangebylex zset (abc \[abcde]
       assert_equal {xyz} [r zrangebylex zset \[xyz \[xyz]
   }

   test "ZRANGEBYLEX with cold key - LIMIT" {
       r del zset
       r zadd zset 0 alpha 0 bar 0 cool 0 down 0 elephant
       r swap.evict zset
       wait_key_cold r zset

       # 测试 LIMIT
       assert_equal {alpha bar} [r zrangebylex zset - \[cool LIMIT 0 2]
       assert_equal {bar cool} [r zrangebylex zset - \[cool LIMIT 1 2]
       assert_equal {omega hill great foo elephant} [r zrevrangebylex zset + \[d LIMIT 0 5]
   }
   ```

2. **运行测试**
   ```bash
   ./runtest-swap --single unit/zset --only "ZRANGEBYLEX with cold key*"
   ```
   预期: 所有新增测试通过

3. **提交**
   ```bash
   git add tests/swap/unit/zset.tcl
   git commit -m "test(swap): add BYLEX optimization tests for cold keys"
   ```

---

### 任务 10: 写命令测试 (tests/swap/unit/zset.tcl)

**文件**:
- 新增: ZREMRANGEBYLEX 测试用例

**步骤**:

1. **添加 ZREMRANGEBYLEX 测试用例**
   ```tcl
   test "ZREMRANGEBYLEX with cold key" {
       r del zset
       r zadd zset 0 alpha 0 bar 0 cool 0 down 0 elephant 0 foo 0 great 0 hill 0 omega
       r swap.evict zset
       wait_key_cold r zset

       # 删除部分范围
       assert_equal 3 [r zremrangebylex zset \[bar \[down]
       assert_equal {alpha elephant foo great hill omega} [r zrange zset 0 -1]

       # 验证 RocksDB 数据一致性
       r swap.evict zset
       wait_key_cold r zset
       assert_equal {alpha elephant foo great hill omega} [r zrange zset 0 -1]
   }

   test "ZREMRANGEBYLEX with cold key - full range" {
       r del zset
       r zadd zset 0 alpha 0 bar 0 cool
       r swap.evict zset
       wait_key_cold r zset

       # 删除全部范围
       assert_equal 3 [r zremrangebylex zset - +]
       assert_equal 0 [r exists zset]
   }
   ```

2. **运行测试**
   ```bash
   ./runtest-swap --single unit/zset --only "ZREMRANGEBYLEX with cold key*"
   ```
   预期: 所有测试通过

3. **提交**
   ```bash
   git add tests/swap/unit/zset.tcl
   git commit -m "test(swap): add ZREMRANGEBYLEX tests for cold keys"
   ```

---

### 任务 11: 全量测试验证

**步骤**:

1. **运行完整的 zset 单元测试**
   ```bash
   ./runtest-swap --single unit/zset
   ```
   预期: 所有测试通过（包括原有和新增）

2. **运行 zset 集成测试**
   ```bash
   ./runtest-swap --single integration/zset
   ```
   预期: 所有测试通过

3. **提交最终验证**
   ```bash
   git commit --allow-empty -m "test(swap): BYLEX optimization all tests pass"
   ```

---

### 任务 12: 性能对比测试

**步骤**:

1. **编写性能测试脚本**
   ```bash
   cat > /tmp/bylex_perf_test.sh << 'EOF'
   #!/bin/bash

   # 启动 Redis swap 实例
   ./src/redis-server --swap-mode yes --rocksdb.maxmemory 256mb &
   REDIS_PID=$!
   sleep 2

   # 创建大 zset（10000 个 member）
   echo "Creating large zset..."
   for i in $(seq 1 10000); do
       ./src/redis-cli zadd bigzset 0 "member$(printf '%05d' $i)" > /dev/null
   done

   # 驱逐到冷键
   echo "Evicting to cold key..."
   ./src/redis-cli swap.evict bigzset
   sleep 1

   # 测试 ZRANGEBYLEX 性能
   echo "Testing ZRANGEBYLEX..."
   time ./src/redis-cli zrangebylex bigzset \[member00100 \[member00200 > /dev/null

   # 清理
   kill $REDIS_PID
   wait $REDIS_PID 2>/dev/null
   EOF
   chmod +x /tmp/bylex_perf_test.sh
   ```

2. **运行性能测试**
   ```bash
   /tmp/bylex_perf_test.sh
   ```
   预期: 相比优化前（全量 swap），BYLEX 部分 swap 应该显著提升性能

3. **记录性能数据**
   - 优化前: 全量 swap 10000 个 member 的时间
   - 优化后: 部分 swap 100 个 member 的时间
   - 预期加速比: 100x

---

## 验收标准

1. **功能正确性**: 所有 BYLEX 命令在冷键场景下返回正确结果
2. **性能提升**: 部分范围查询避免全量 swap，性能显著提升
3. **测试覆盖**: 新增测试用例全部通过，原有测试不受影响
4. **代码质量**: 编译无警告，代码风格与现有代码一致
5. **内存安全**: 无内存泄漏（通过 valgrind 验证）

---

## 风险与缓解

1. **反向扫描越界**: seek_for_prev 可能定位到错误位置
   - 缓解: 添加单次 prev() 修正逻辑，覆盖所有反向扫描场景

2. **前缀 member 误排除**: 如 "xyz" 和 "xyzz" 的边界问题
   - 缓解: 不使用 PREFIX_MATCH，全闭区间 swap，命令层过滤

3. **内存泄漏**: zlexrangespec 需要正确释放
   - 缓解: 在 keyRequestDeinit 和 zsetDataCtxDeinit 中添加释放逻辑

---

## 参考资料

- 设计文档: `docs/superpowers/specs/2026-05-31-zset-bylex-swap-optimization-design.md`
- BYSCORE 实现: `src/ctrip_swap_zset.c` (ZSET_SWAP_CTX_TYPE_ZS 相关代码)
- 现有 BYLEX 测试: `tests/swap/unit/zset.tcl:504-567`
