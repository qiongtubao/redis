# Swap 并发竞态修复报告

## 问题摘要

**崩溃位置**: `src/ctrip_swap.h:679` - `serverAssert(object_meta->len >= 0)`

**根本原因**: 编译器优化导致防御性检查被移除，在并发场景下 `object_meta->len` 变成负数。

## 详细分析

### 1. 崩溃场景

从 core dump 分析得出：
```
#9 setCreateOrMergeObject at ctrip_swap_set.c:562
   swapDataObjectMetaModifyLen(data,-setTypeSize(decoded));

data->object_meta->len = -1  (崩溃前)
data->value = 0x7cf87dc105c0 (非 NULL)
decoded = 0x7cf87dc107e0     (非 NULL)
```

### 2. 代码逻辑分析

原始代码结构：
```c
if (swapDataIsCold(data) || decoded == NULL) {  // line 535
    // cold 分支
    if (decoded) {
        if (data->value != NULL) {  // line 544 - 防御性检查
            // merge 处理...
        }
        swapDataObjectMetaModifyLen(data,-setTypeSize(decoded));  // line 562
    }
}
```

`swapDataIsCold` 定义：
```c
static inline int swapDataIsCold(swapData *data) {
  return data->value == NULL;  // 只检查 value 是否为 NULL
}
```

### 3. 编译器优化问题

汇编代码显示：
```assembly
3eee05:  cmpq   $0x0,0x20(%rbx)    # 检查 data->value == NULL
3eee0a:  jne    3eee80              # 如果 != NULL，跳转到 merge 分支
3eee0c:  call   setTypeSize         # cold 分支：调用 setTypeSize
...
3eee60:  mov    %rdx,0x10(%rcx)     # 修改 object_meta->len (line 562)
3eee67:  js     3eef0a              # 如果 len < 0，触发断言
```

**关键问题**：
1. 编译器看到 `swapDataIsCold(data)` 检查 `data->value == NULL`
2. 进入 cold 分支后，编译器认为 `data->value` 必定为 NULL
3. 因此 line 544 的 `if (data->value != NULL)` 检查被认为永远为 false
4. **编译器优化掉了整个防御代码块**（line 544-560）

### 4. 并发竞态条件

```
线程 A (swap 线程):
  T1: 检查 swapDataIsCold(data) → true (data->value == NULL)
  T2: 进入 cold 分支
  T3: (此时 data->value 被其他线程修改为非 NULL)
  T4: 防御检查被优化掉，直接执行 line 562
  T5: object_meta->len 变成负数 → crash

线程 B (其他线程):
  T3: 修改 data->value 为非 NULL
```

## 修复方案

### 核心思路

使用 `volatile` 关键字防止编译器优化掉防御检查：
```c
robj *value_check = *(volatile robj **)&data->value;
```

### 修复代码

#### 1. Set (ctrip_swap_set.c)
```c
void *setCreateOrMergeObject(swapData *data, void *decoded_, void *datactx) {
    robj *result, *decoded = decoded_;
    UNUSED(datactx);
    serverAssert(decoded == NULL || decoded->type == OBJ_SET);

    /* 防御性检查：在并发场景下，data->value 可能在执行过程中被修改。
     * 使用 volatile 防止编译器优化掉此检查。 */
    robj *value_check = *(volatile robj **)&data->value;

    if (swapDataIsCold(data) || decoded == NULL) {
        result = decoded;
        if (decoded) {
            if (value_check != NULL) {
                serverLog(LL_WARNING,
                    "[swap] setCreateOrMergeObject: data->value changed during execution");
                // merge 处理...
                return NULL;
            }
            swapDataObjectMetaModifyLen(data,-setTypeSize(decoded));
        }
    }
    // ...
}
```

#### 2. Hash (ctrip_swap_hash.c) 和 Zset (ctrip_swap_zset.c)
应用相同的修复模式。

## 测试验证

### 测试结果
```
=== 测试 SET ===
SUNIONSTORE 结果: 500
✓ SET 测试通过！

=== 测试 HASH ===
HASH 大小: 100
✓ HASH 测试通过！

=== 测试 ZSET ===
ZUNIONSTORE 结果: 200
✓ ZSET 测试通过！

=== 所有测试通过！ ===
```

## 影响范围

- **修复文件**:
  - src/ctrip_swap_set.c
  - src/ctrip_swap_hash.c
  - src/ctrip_swap_zset.c

- **修复场景**:
  - 并发 swap 操作
  - swap 执行过程中数据状态变化
  - object_meta->len 维护

## 经验教训

1. **volatile 的重要性**: 在多线程环境下，防御性检查必须用 `volatile` 保护
2. **编译器优化陷阱**: 编译器会基于"不可能发生"的假设进行优化
3. **并发编程难度**: 即使有防御代码，也可能被优化掉

## 未来改进建议

1. 考虑使用原子操作或内存屏障
2. 添加更多的并发测试用例
3. 代码审查时注意编译器优化问题
