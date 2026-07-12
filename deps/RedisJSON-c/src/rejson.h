// =============================================================================
// rejson.h —— ReJSON-c 模块公共头文件
//
// 定义 C 模块的全部公共类型、枚举、常量和 API 函数声明。
// 对应 Rust 版 manager.rs 的 trait 定义 + rejson_api.h 的导出接口。
// =============================================================================

#ifndef REJSON_H
#define REJSON_H

#include <stdint.h>   // int64_t, uint64_t
#include <stdbool.h>  // bool, true, false
#include "cJSON.h"    // cJSON 结构体定义（IValue 的底层存储）

// =============================================================================
// IValue —— 围绕 cJSON 的轻量封装层
//
// IValue 是 cJSON* 的类型别名（typedef），所有 JSON 操作最终通过 cJSON API
// 完成。本层封装解决两个问题：
//   1. 统一类型系统：将 cJSON 的类型标记映射为 ReJSON 的类型枚举
//   2. 整数/浮点区分：cJSON 用同一标记表示二者，由 ivalue_type() 区分
// =============================================================================

// ---------------------------------------------------------------------------
// ReJSONType —— 类型标签枚举
//
// 与 Rust 版 manager.rs 的 SelectValueType 一一对应。顺序无关。
// JSON.TYPE 命令的返回值由 rejson_type_name() 映射为字符串。
//
// 关键设计：LONG(2) 和 DOUBLE(3) 分别对应 "integer" 和 "number"，
// 与 Rust 版 serde_json::Value 的 is_i64()/is_f64() 一致。
// ---------------------------------------------------------------------------
typedef enum {
    REJSON_NULL    = 0,   // JSON null 值，类型名 "null"
    REJSON_BOOL    = 1,   // true / false，类型名 "boolean"
    REJSON_LONG    = 2,   // i64 范围内的整数，类型名 "integer"
    REJSON_DOUBLE  = 3,   // 浮点数（含小数部分），类型名 "number"
    REJSON_STRING  = 4,   // 字符串，类型名 "string"
    REJSON_ARRAY   = 5,   // 数组，类型名 "array"
    REJSON_OBJECT  = 6,   // 对象（键值对集合），类型名 "object"
} ReJSONType;

// IValue 是 cJSON 指针的类型别名
// ivalue_parse() 创建新树，ivalue_free() 释放
typedef cJSON IValue;

// ---------------------------------------------------------------------------
// PathResult —— 路径解析结果码
//
// path_resolve() 在 JSON 文档中按路径表达式查找节点。
// 结果码让调用方可以精确区分错误类型，以便返回不同的 Redis 错误消息。
// ---------------------------------------------------------------------------
typedef enum {
    PATH_OK,             // 路径成功匹配到至少一个节点
    PATH_ERR_SYNTAX,     // 路径表达式语法错误（如不成对的括号）
    PATH_ERR_TYPE,       // 路径中有类型不匹配（如对数字取子字段 .foo）
    PATH_ERR_OOB,        // 数组索引越界（负索引调整后仍出界）
    PATH_ERR_NO_MATCH,   // 路径未匹配到任何节点（如键不存在）
} PathResult;

// ---------------------------------------------------------------------------
// ResolvedPath —— 路径的分段表示
//
// parse_path() 将 ".a.b[0]" 等路径分解为分段列表。
// keys[i] 对应对象键的段，indices[i] 对应数组索引的段（对象键处为 -1）。
// 例如 ".a.b[0]" 的三段：a / b / [0]。
// depth 是总段数，cap 是已分配容量（用于 realloc）。
// ---------------------------------------------------------------------------
typedef struct {
    char **keys;          // 每段的键名（对数组段为 ""）
    int *indices;         // 每段的数组索引（对对象段为 -1）
    int depth;            // 总段数
    int cap;              // 分配容量
} ResolvedPath;

// ---------------------------------------------------------------------------
// PathCtx —— 路径解析的完整输出上下文
//
// path_resolve() 的返回值包装。node 指向匹配到的 IValue 节点。
// 对于 JSONPath 多匹配，node 可能指向一个新分配的 JSON 数组（包含所有
// 匹配节点的深拷贝），调用者需调用 path_ctx_cleanup() 释放。
// 对于单匹配（legacy 或 JSONPath 单结果），node 指向文档树内的节点，
// 不需要清理。
// ---------------------------------------------------------------------------
typedef struct {
    ResolvedPath path;    // 路径的分段表示（调试用，调用方通常不需要）
    IValue *node;         // 匹配到的 IValue 节点（NULL = 匹配失败）
    PathResult result;    // 匹配结果码
    char err_msg[256];    // 人类可读的错误描述（仅在失败时有效）
} PathCtx;

// =============================================================================
// IValue 基本操作 API
//
// 这些函数封装了 cJSON 的底层操作，提供 ReJSON 模块需要的语义。
// 所有返回值如果分配内存，调用者需用 ivalue_free() 或 cJSON_free() 释放。
// =============================================================================

// 解析 JSON 字符串 → IValue 树。返回新分配树，失败返回 NULL。
// 对应 Rust 版的 serde_json::from_str()
IValue *ivalue_parse(const char *json_str);

// 深拷贝整个 IValue 树（递归复制所有子节点）。
// src 为 NULL 时返回 NULL。
// 对应 Rust 版的 Clone::clone()
IValue *ivalue_dup(const IValue *src);

// 递归释放 IValue 树。
// 对应 Rust 版的 Drop::drop()
void ivalue_free(IValue *v);

// 获取类型标签。区分 REJSON_LONG（整数）和 REJSON_DOUBLE（浮点）。
// 对应 Rust 版的 get_type() -> SelectValueType
ReJSONType ivalue_type(const IValue *v);

// 提取 int64 值。ok 可空，类型不匹配时设 *ok=false 并返回 0。
int64_t ivalue_get_long(const IValue *v, bool *ok);

// 提取 double 值。
double  ivalue_get_double(const IValue *v, bool *ok);

// 提取 bool 值。
bool    ivalue_get_bool(const IValue *v, bool *ok);

// 提取字符串指针（指向 cJSON 内部 buffer，不拷贝）。
const char *ivalue_get_str(const IValue *v);

// 序列化为紧凑 JSON 文本。调用者用 cJSON_free() 释放。
// indent 参数预留（当前未实现格式化）。
char *ivalue_to_string(const IValue *v, int indent);

// =============================================================================
// 路径匹配 API
//
// path_resolve() 是对外的唯一入口，同时支持：
//   - legacy 语法：.a.b[0]（以点开头，裸值返回）
//   - JSONPath 语法：$.a.b、$..*、$..[?(@.n>1)]（以 $ 开头）
//
// 响应格式差异：
//   - Legacy 单匹配 → 返回裸 JSON 值（不包装数组）
//   - JSONPath 单匹配 → 返回 [value]（数组包裹）
//   - JSONPath 多匹配 → 内部已包裹数组，resolve_to_json 不再加外层
// =============================================================================

// 编译并执行路径表达式。
// ctx      —— [out] 匹配结果。result==PATH_OK 时 node 有效。
// doc      —— JSON 文档根节点
// path_str —— 路径表达式。空字符串或 "."/"$" 返回根节点。
void path_resolve(PathCtx *ctx, const IValue *doc, const char *path_str);

// 释放 PathCtx 中可能分配的多匹配数组。
// 对单匹配结果是空操作。
void path_ctx_cleanup(PathCtx *ctx);

// =============================================================================
// NX / XX 常量
//
// JSON.SET 命令支持的条件写入模式：
//   NX = Not-eXists — 仅当路径不存在时才写入
//   XX = eXists     — 仅当路径存在时才写入
// 两者互斥，同时指定返回错误。
// =============================================================================
#define REJSON_NX 1
#define REJSON_XX 2

// =============================================================================
// 类型名映射函数
//
// 将 ReJSONType 枚举值映射为 JSON.TYPE 命令返回的字符串。
// 映射表与 Rust 版 json_type_command_impl 的返回值完全一致。
//
// 注意：这是一个 static inline 函数，会被直接内联到调用处，
// 因此定义在头文件中不需要额外的 .c 文件。
// =============================================================================
static inline const char *rejson_type_name(ReJSONType t) {
    switch (t) {
        case REJSON_NULL:   return "null";
        case REJSON_BOOL:   return "boolean";
        case REJSON_LONG:   return "integer";   // ← 整数和浮点数分开
        case REJSON_DOUBLE: return "number";
        case REJSON_STRING: return "string";
        case REJSON_ARRAY:  return "array";
        case REJSON_OBJECT: return "object";
    }
    return "unknown";  // 不应到达
}

// =============================================================================
// 模块类型标识
//
// RedisModule_CreateDataType 需要。类型名必须是 9 个字符
// （moduleTypeEncodeId 的内部限制）。模块名可在 MODULE LIST 中看到。
// =============================================================================
#define REJSON_C_TYPE_NAME    "ReJSON-CS"   // 9 字符（必须）
#define REJSON_C_TYPE_VERSION 1             // 版本号，影响 RDB encver

#endif /* REJSON_H */
