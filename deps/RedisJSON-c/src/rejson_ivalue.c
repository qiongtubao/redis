// =============================================================================
// rejson_ivalue.c —— IValue 包装层
//
// IValue 是 cJSON* 的类型别名。本文件对 cJSON 提供：
//   - 解析/深拷贝/释放
//   - 类型检测（区分 integer 和 number）
//   - 类型安全的值提取（long / double / bool / string）
//   - JSON 序列化
//   - 空容器判断
//
// 所有内存分配通过 cJSON_malloc/cJSON_free 进行，
// 已挂钩到 RedisModule_Alloc/RedisModule_Free（见 rejson_module.c 的 OnLoad）。
// =============================================================================

#include <stdlib.h>   // NULL, size_t
#include <string.h>   // strlen, memcpy (cJSON 内部使用)
#include <stdio.h>    // (预留，当前无输出)
#include "rejson.h"   // IValue, ReJSONType, ivalue_* 声明

// =============================================================================
// 解析 JSON 字符串 -> IValue 树
//
// 参数:  json_str —— 以 '\0' 结尾的 JSON 文本（如 "{\"a\":1}"）
// 返回:  新分配的 IValue*，解析失败返回 NULL
// 注意:  调用者必须用 ivalue_free() 释放返回值
// 对比:  Rust 版 serde_json::from_str()
// =============================================================================
IValue *ivalue_parse(const char *json_str) {
    // 转发给 cJSON_Parse，它是 cJSON 的核心 JSON 解析器
    return cJSON_Parse(json_str);
}

// =============================================================================
// 深拷贝 —— 递归复制整棵 IValue 树
//
// 参数:  src —— 源树根节点，允许 NULL
// 返回:  新分配的独立副本，src=NULL 时返回 NULL
// 注意:  副本与源树完全独立，修改副本不影响源树
// 对比:  Rust 版 Clone trait 的 clone() 方法
// =============================================================================
IValue *ivalue_dup(const IValue *src) {
    // src 为 NULL 时直接返回（Rust 版 Option::map 的 None 分支）
    if (!src) return NULL;
    // cJSON_Duplicate(v, recurse=1) 递归复制所有子节点
    return cJSON_Duplicate(src, 1);
}

// =============================================================================
// 释放 —— 递归释放整棵 IValue 树及其所有子节点
//
// 参数:  v —— 要释放的树根节点，允许 NULL
// 对比:  Rust 版的 Drop trait (drop)
// =============================================================================
void ivalue_free(IValue *v) {
    // cJSON_Delete 递归遍历链表释放所有子节点
    cJSON_Delete(v);
}

// =============================================================================
// 类型检测 —— 返回 ReJSONType 枚举值
//
// 核心逻辑：区分 REJSON_LONG（整数）和 REJSON_DOUBLE（浮点数）。
// cJSON 使用同一 cJSON_Number 类型表示整数和浮点，区别在 valueint 和
// valuedouble 两个字段：
//   - 整数 （如 42）：valueint=42, valuedouble=42.0（两者一致）
//   - 浮点（如 3.14）：valueint=3,  valuedouble=3.14（两者不一致）
//
// 此逻辑与 Rust 版 serde_json::Value 的 is_i64()/is_f64() 判断一致。
//
// 对比:  Rust 版 IValue::get_type() -> SelectValueType
// =============================================================================
ReJSONType ivalue_type(const IValue *v) {
    // 空指针视为 null
    if (!v) return REJSON_NULL;

    // cJSON 的类型标记在 v->type 的低位（cJSON_IsReference 等标志用高位）
    switch ((int)v->type) {
        // cJSON_False 和 cJSON_True 是两个独立常量，都映射到 boolean
        case cJSON_False:
        case cJSON_True:
            return REJSON_BOOL;

        // JSON null
        case cJSON_NULL:
            return REJSON_NULL;

        // 数字 —— 需要区分整数/浮点
        case cJSON_Number: {
            // 判断条件：valuedouble 与 valueint 不同 → 浮点
            // 或者 valuedouble 本身有小数部分（如 3.14 的 (int64_t)3.14 = 3 ≠ 3.14）
            if (v->valuedouble != (double)v->valueint ||
                v->valuedouble != (int64_t)v->valuedouble) {
                return REJSON_DOUBLE;  // 浮点数
            }
            return REJSON_LONG;        // 整数（valuedouble == valueint）
        }

        // 字符串
        case cJSON_String:
            return REJSON_STRING;

        // 数组
        case cJSON_Array:
            return REJSON_ARRAY;

        // 对象
        case cJSON_Object:
            return REJSON_OBJECT;

        // 未知类型（理论上不会走到）
        default:
            return REJSON_NULL;
    }
}

// =============================================================================
// 类型安全的值提取 —— int64
//
// 参数:  ok —— 可选的输出参数，类型匹配时设为 true，否则 false
// 返回:  成功则返回 valueint，失败返回 0（调用者应检查 *ok）
// 对比:  Rust 版的 try_i64() -> Option<i64>
// =============================================================================
int64_t ivalue_get_long(const IValue *v, bool *ok) {
    // 先置失败标志；后面成功了再覆盖
    if (ok) *ok = false;
    // 空指针或非数字 → 返回 0 并保持 *ok=false
    if (!v || v->type != cJSON_Number) return 0;
    // 类型匹配，置成功标志
    if (ok) *ok = true;
    // 直接取出 valueint（cJSON 保证对数字节点此字段有效）
    return (int64_t)v->valueint;
}

// =============================================================================
// 类型安全的值提取 —— double
//
// 对比:  Rust 版的 try_f64() -> Option<f64>
// =============================================================================
double ivalue_get_double(const IValue *v, bool *ok) {
    if (ok) *ok = false;
    if (!v || v->type != cJSON_Number) return 0.0;
    if (ok) *ok = true;
    // 浮点值直接用 valuedouble
    return v->valuedouble;
}

// =============================================================================
// 类型安全的值提取 —— boolean
//
// 对比:  Rust 版的 as_bool() -> Option<bool>
// cJSON 用两个独立类型表示布尔：cJSON_True / cJSON_False
// =============================================================================
bool ivalue_get_bool(const IValue *v, bool *ok) {
    // 前置失败标志
    if (ok) *ok = false;
    // 空指针直接返回 false
    if (!v) return false;
    // cJSON_True → true
    if (v->type == cJSON_True) {
        if (ok) *ok = true;
        return true;
    }
    // cJSON_False → false
    if (v->type == cJSON_False) {
        if (ok) *ok = true;
        return false;
    }
    // 其他类型（数字/字符串/数组等）→ 返回 false 且 *ok=false
    return false;
}

// =============================================================================
// 类型安全的值提取 —— const char*
//
// 返回:  字符串的 C 指针（不拷贝，cJSON 生命期内有效），非字符串返回 NULL
// 对比:  Rust 版的 as_str() -> Option<&str>
// =============================================================================
const char *ivalue_get_str(const IValue *v) {
    // 空指针或非字符串 → NULL
    if (!v || v->type != cJSON_String) return NULL;
    // 直接返回 cJSON 内部的 valuestring 指针
    return v->valuestring;
}

// =============================================================================
// 序列化为 JSON 文本
//
// 参数:  indent —— 预留参数，当前忽略（始终紧凑格式）
// 返回:  新分配的 C 字符串，调用者需用 cJSON_free() 释放
//
// 特殊处理：v 为 NULL 时返回字面字符串 "null"（而非崩溃），
// 方便 JSON.GET 在路径不存在时统一处理。
//
// 对比:  Rust 版的 serde_json::to_string() + to_string_pretty()
// =============================================================================
char *ivalue_to_string(const IValue *v, int indent) {
    // v 为 NULL → 返回 "null" 字符串
    if (!v) {
        // 分配 5 字节：'n' 'u' 'l' 'l' '\0'
        char *s = cJSON_malloc(5);
        if (s) {
            s[0] = 'n';
            s[1] = 'u';
            s[2] = 'l';
            s[3] = 'l';
            s[4] = '\0';
        }
        return s;
    }

    // indent 参数预留，当前未实现格式化
    (void)indent;

    // cJSON_PrintUnformatted 输出紧凑 JSON（无换行/缩进）
    // 返回的内存由 cJSON_malloc 分配，调用者用 cJSON_free 释放
    return cJSON_PrintUnformatted(v);
}

// =============================================================================
// 判断 IValue 是否为"空"容器
//
// 用于 JSON.CLEAR 和 JSON.DEL 后判断文档是否应被 RedisModule_DeleteKey 自动删除。
//
// 空定义：
//   - 对象无子节点（child == NULL）→ 空
//   - 数组无子节点（child == NULL）→ 空
//   - 其他类型 → 非空（即使值是 null）
//
// 对比:  Rust 版 IValue::is_empty() 方法
// =============================================================================
bool ivalue_is_empty(const IValue *v) {
    // NULL 指针视为空（便于调用方统一处理）
    if (!v) return true;

    // 对象且无子节点 → 空
    if (v->type == cJSON_Object && v->child == NULL) return true;

    // 数组且无子节点 → 空
    if (v->type == cJSON_Array && v->child == NULL) return true;

    // 非容器类型（数字/字符串/布尔/null）→ 非空
    return false;
}
