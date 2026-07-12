// =============================================================================
// rejson_path.c —— 最小化 JSONPath / legacy 路径解析器
//
// 同时支持两种路径语法：
//   - legacy:   .a.b[0]       （以点开头，返回裸值，单匹配）
//   - JSONPath: $.a.b[0]      （以 $ 开头，数组包裹，多匹配）
//               $..*          （递归下降所有后代）
//               $..[?(@.n)]   （过滤器表达式，当前占位返回空）
//
// 架构：
//   1. 词法分析：parse_path() 将路径字符串拆分为 Token 列表
//   2. 匹配执行：match_tokens() 递归遍历 JSON 树收集匹配
//   3. 公共入口：path_resolve() 对外暴露统一接口
//
// Rust 对照：json_path crate 的 compile() + calc_once() 体系
// =============================================================================

#include <stdlib.h>    // NULL, malloc, free, realloc
#include <string.h>    // memset, strdup, strchr, strncpy
#include <stdio.h>     // snprintf
#include <ctype.h>     // isalpha, isdigit
#include "rejson.h"    // IValue, PathCtx, PathResult 等

// =============================================================================
// 词法单元类型 (TokenType)
//
// 路径表达式中的每种语法单位对应一种 Token 类型。
// 解析器逐字符扫描路径字符串，将 ".", "..", "[*]", "$" 等结构识别为 Token。
// =============================================================================
typedef enum {
    TOK_ROOT,         // $ —— 根节点引用
    TOK_KEY,          // .foo 或 ['foo'] —— 对象键名
    TOK_RECURSE,      // ..foo —— 递归下降匹配指定名称的键
    TOK_RECURSE_ALL,  // ..* 或 $..* —— 递归下降所有后代节点
    TOK_INDEX,        // [N] —— 按索引取数组元素（支持负索引）
    TOK_WILDCARD,     // [*] 或 .* —— 匹配对象/数组的所有子节点
    TOK_FILTER,       // [?(@.n>1)] —— 过滤器（占位，不做条件解析）
    TOK_DONE,         // 哨兵，标记解析结束（当前未使用）
} TokenType;

// 单个 Token 的数据载荷
//    key      —— TOK_KEY/TOK_RECURSE 的键名字符串（动态分配）
//    index    —— TOK_INDEX 的数组索引（负索引已在此阶段转换？不，在匹配阶段）
//    indices  —— 多索引表达式 [0,1,2]（预留，当前未实现）
//    n_indices —— 多索引的数量
typedef struct {
    TokenType type;     // Token 类型
    char *key;          // 键名（对象步进或递归匹配用）
    int index;          // 数组索引（TOK_INDEX 用）
    int *indices;       // 多索引列表（预留）
    int n_indices;      // 多索引个数（预留）
} Token;

// 最大 Token 数量（路径深度超过此限制返回错误）
#define MAX_TOKENS 64

// 已解析的完整路径 —— Token 序列表示的路径
//    tokens    —— 按顺序排列的 Token 数组
//    n_tokens  —— 实际有效的 Token 数量
//    is_legacy —— 1 = legacy 语法（点开头），0 = JSONPath（$ 开头）
//    err       —— 解析失败时的错误描述文本
typedef struct {
    Token tokens[MAX_TOKENS];  // 固定大小数组（避免堆分配）
    int n_tokens;              // 当前 Token 数量
    int is_legacy;             // 路径风格标志
    char err[256];             // 错误消息缓冲区
} ParsedPath;

// =============================================================================
// 内部辅助函数
// =============================================================================

// 释放 ParsedPath 中所有 Token 的动态分配内存
// 注意：只释放 key 和 indices 字段，Token 结构体本身是 inline 数组
static void parsed_path_cleanup(ParsedPath *pp) {
    for (int i = 0; i < pp->n_tokens; i++) {
        // key 是 strndup 分配的，必须释放
        if (pp->tokens[i].key) free(pp->tokens[i].key);
        // indices 是 malloc 分配的（预留），必须释放
        if (pp->tokens[i].indices) free(pp->tokens[i].indices);
    }
    // 清理后置为 0，避免重复释放
    pp->n_tokens = 0;
}

// 跳过空白字符（JSONPath 在方括号内允许空格）
static inline const char *skip_spaces(const char *p) {
    // 跳过空格和制表符
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// =============================================================================
// parse_path —— 路径表达式词法分析器
//
// 将 ".a.b[0]" 或 "$..*" 等路径字符串解析为 Token 列表。
//
// 语法处理规则：
//   .key      → TOK_KEY     （对象键名）
//   .*        → TOK_WILDCARD（对象通配）
//   ..key     → TOK_RECURSE （递归键）
//   ..*       → TOK_RECURSE_ALL（递归全部）
//   [N]       → TOK_INDEX   （数组索引，支持负）
//   [*]       → TOK_WILDCARD（数组通配）
//   [?().]    → TOK_FILTER  （过滤器占位）
//   $         → TOK_ROOT    （根）
//
// 返回值：0=成功，-1=失败（err 字段写明原因）
// Rust 对照：json_path/src/parser.rs 的 Parser::parse()
// =============================================================================
static int parse_path(const char *path_str, ParsedPath *pp) {
    // 初始化 ParsedPath（全部字段置零）
    memset(pp, 0, sizeof(*pp));

    // 空路径直接报错
    const char *p = path_str;
    if (!p || *p == '\0') {
        snprintf(pp->err, sizeof(pp->err), "empty path");
        return -1;
    }

    // 判断路径风格：$ 开头 → JSONPath，. 或其他 → legacy
    if (*p == '$') {
        pp->is_legacy = 0;  // JSONPath 风格
        p++;                 // 消费 $
    } else if (*p == '.') {
        pp->is_legacy = 1;  // legacy 风格
    } else {
        // 没有 $ 也没有 . → 按 legacy 处理（实际会解析失败）
        pp->is_legacy = 1;
    }

    // ---------------------------------------------------------------
    // 主解析循环：逐个字符处理，构建 Token 列表
    // 每个循环迭代消费一个语法单元
    // ---------------------------------------------------------------
    while (*p) {
        // 检查 Token 数量限制
        if (pp->n_tokens >= MAX_TOKENS) {
            snprintf(pp->err, sizeof(pp->err), "path too deep");
            return -1;
        }

        // 获取下一个 Token 指针，先清零
        Token *tok = &pp->tokens[pp->n_tokens];
        memset(tok, 0, sizeof(Token));

        // ---------------------------------------------------------------
        // 处理点语法：.key / .* / ..key / ..* / ..
        // ---------------------------------------------------------------
        if (*p == '.') {
            // 单独的 "." 表示根路径（legacy 写法），不产生额外 Token
            // 例如 "JSON.GET k ." 应与 "JSON.GET k" 或 "JSON.GET k $" 等价
            if (p[1] == '\0') {
                pp->n_tokens++;
                break;  // 退出 while 循环，路径解析完成
            }
            p++;  // 消费第一个点

            // .. → 递归下降
            if (*p == '.') {
                p++;  // 消费第二个点
                if (*p == '*') {
                    // ..* → 递归下降全部后代
                    tok->type = TOK_RECURSE_ALL;
                    p++;
                } else if (isalpha(*p) || *p == '_' || *p == '"') {
                    // ..key → 递归匹配指定键
                    tok->type = TOK_RECURSE;
                    const char *start = p;
                    // 支持引号括起来的键名
                    if (*p == '"') {
                        p++; start = p;
                        while (*p && *p != '"') p++;
                        tok->key = strndup(start, p - start);
                        if (*p == '"') p++;
                    } else {
                        // 未括的键名：遇到分隔符或结束符停止
                        while (*p && *p != '.' && *p != '[' && *p != '(' && *p != ')' && *p != ' ') p++;
                        tok->key = strndup(start, p - start);
                    }
                } else if (*p == '[') {
                    // ..[?(@.n)] — 简化为 FILTER 占位
                    tok->type = TOK_FILTER;
                } else {
                    // .. 后跟非法字符
                    snprintf(pp->err, sizeof(pp->err), "expected key after '..'");
                    return -1;
                }
            }
            // .* → 通配所有子节点
            else if (*p == '*') {
                tok->type = TOK_WILDCARD;
                p++;
            }
            // .key → 对象键名
            else if (isalpha(*p) || *p == '_' || *p == '"') {
                tok->type = TOK_KEY;
                const char *start;
                // 支持引号括起来的键名
                if (*p == '"') {
                    p++;  // 消费开引号
                    start = p;
                    while (*p && *p != '"') p++;
                    tok->key = strndup(start, p - start);
                    if (*p == '"') p++;  // 消费闭引号
                } else {
                    // 未括键名：遇到分隔符或结束符停止
                    start = p;
                    while (*p && *p != '.' && *p != '[' && *p != '(' && *p != ')' && *p != ' ') p++;
                    tok->key = strndup(start, p - start);
                }
            } else {
                // . 后跟非法字符
                snprintf(pp->err, sizeof(pp->err), "unexpected char after '.'");
                return -1;
            }
        }
        // ---------------------------------------------------------------
        // 处理括号语法：[N] / [*] / [?(@.)]
        // ---------------------------------------------------------------
        else if (*p == '[') {
            p++;                    // 消费开括号
            p = skip_spaces(p);     // 跳过可能的空格

            if (*p == '*') {
                // [*] → 数组通配
                tok->type = TOK_WILDCARD;
                p++;
            } else if (*p == '?' && p[1] == '(') {
                // [?(@.n>1)] → 过滤器占位
                tok->type = TOK_FILTER;
                // 跳过到闭括号（不解析过滤条件）
                while (*p && *p != ']') p++;
            } else if (*p == '-' || isdigit(*p)) {
                // [N] → 数组索引（支持负索引）
                tok->type = TOK_INDEX;
                tok->index = 0;
                int neg = 0;
                if (*p == '-') { neg = 1; p++; }
                // 解析数字部分
                while (isdigit(*p)) {
                    tok->index = tok->index * 10 + (*p - '0');
                    p++;
                }
                if (neg) tok->index = -tok->index;  // 应用负号
                p = skip_spaces(p);                  // 跳过尾部空格
            } else {
                // [ 后跟非法字符
                snprintf(pp->err, sizeof(pp->err), "unexpected char in brackets");
                return -1;
            }

            // 验证闭括号
            p = skip_spaces(p);
            if (*p != ']') {
                snprintf(pp->err, sizeof(pp->err), "missing ']'");
                return -1;
            }
            p++;  // 消费闭括号
        } else {
            // 路径以非法字符开头
            snprintf(pp->err, sizeof(pp->err), "unexpected char '%c'", *p);
            return -1;
        }

        // Token 构建完成，计数加一
        pp->n_tokens++;
    }

    // 解析成功
    return 0;
}

// =============================================================================
// 路径匹配引擎
//
// 根据 Token 列表在 JSON 树上执行深度优先遍历，收集所有匹配节点。
// 核心函数 match_tokens() 递归调用自身，遍历树的分支。
//
// Rust 对照：json_path/src/selector.rs 的 select() 方法
// =============================================================================

// 前向声明：match_recursive 用于 TOK_RECURSE
static void match_recursive(cJSON *doc, cJSON ***results, int *n_results, int *cap, const char *key);

// ---------------------------------------------------------------
// ensure_capacity —— 动态扩容结果数组
//
// 当结果数组已满时，将容量翻倍。
// 初始容量 16，后续按 2 倍增长。
// ---------------------------------------------------------------
static void ensure_capacity(cJSON ***results, int *n, int *cap) {
    if (*n >= *cap) {
        // 首次分配 16，后续翻倍
        *cap = *cap ? *cap * 2 : 16;
        *results = realloc(*results, sizeof(cJSON*) * (*cap));
    }
}

// ---------------------------------------------------------------
// match_tokens —— 递归路径匹配核心
//
// 根据传入的 Token 列表（从 tok_idx 开始），在当前 JSON 子树 doc 上
// 执行匹配，将结果追加到 results 数组。
//
// 参数：
//   pp        —— 已编译的路径 Token 列表
//   doc       —— 当前遍历的 JSON 子树根节点
//   tok_idx   —— 当前要处理的 Token 索引（递归中递增）
//   results   —— 结果数组（** 表示可能 realloc）
//   n_results —— 已收集的结果数
//   cap       —— 结果数组容量
//
// 返回：已匹配的路径数（递归累加）
// ---------------------------------------------------------------
static int match_tokens(const ParsedPath *pp, cJSON *doc, int tok_idx,
                         cJSON ***results, int *n_results, int *cap) {
    // 所有 Token 已消耗完毕 → 当前节点是一个完整匹配
    if (tok_idx >= pp->n_tokens) {
        ensure_capacity(results, n_results, cap);
        (*results)[(*n_results)++] = doc;  // 保存节点指针
        return 1;                          // 计数 1
    }

    // 空节点不匹配
    if (!doc) return 0;

    const Token *tok = &pp->tokens[tok_idx];
    int matched = 0;  // 本层匹配计数

    // 根据 Token 类型执行不同的匹配策略
    switch (tok->type) {

    // ---------------------------------------------------------------
    // TOK_ROOT ($) —— 当前节点就是根，直接处理下一 Token
    // ---------------------------------------------------------------
    case TOK_ROOT:
        return match_tokens(pp, doc, tok_idx + 1, results, n_results, cap);

    // ---------------------------------------------------------------
    // TOK_KEY (.key) —— 当前节点必须是对象，取指定键
    // ---------------------------------------------------------------
    case TOK_KEY: {
        // 非对象 → 无法继续（legacy 模式会报 WRONGTYPE）
        if (doc->type != cJSON_Object) break;
        // 在对象中查找键名
        cJSON *child = cJSON_GetObjectItemCaseSensitive(doc, tok->key);
        // 键存在 → 递归匹配剩余 Token
        if (child)
            matched += match_tokens(pp, child, tok_idx + 1, results, n_results, cap);
        break;
    }

    // ---------------------------------------------------------------
    // TOK_INDEX ([N]) —— 当前节点必须是数组，取指定索引
    // 支持负索引（-1 表示最后一个）
    // ---------------------------------------------------------------
    case TOK_INDEX: {
        // 非数组 → 无法继续
        if (doc->type != cJSON_Array) break;
        int len = cJSON_GetArraySize(doc);
        int idx = tok->index;
        // Python 风格负索引
        if (idx < 0) idx = len + idx;
        // 越界检查
        if (idx < 0 || idx >= len) break;
        cJSON *child = cJSON_GetArrayItem(doc, idx);
        if (child)
            matched += match_tokens(pp, child, tok_idx + 1, results, n_results, cap);
        break;
    }

    // ---------------------------------------------------------------
    // TOK_WILDCARD ([*] / .*) —— 匹配所有子节点
    // ---------------------------------------------------------------
    case TOK_WILDCARD: {
        cJSON *child;
        // 遍历 cJSON 链表的所有子节点
        cJSON_ArrayForEach(child, doc) {
            matched += match_tokens(pp, child, tok_idx + 1, results, n_results, cap);
        }
        break;
    }

    // ---------------------------------------------------------------
    // TOK_RECURSE (..key) —— 递归下降查找指定键名的节点
    // 在 doc 的整个子树中深度优先查找，收集所有匹配该键的节点
    // ---------------------------------------------------------------
    case TOK_RECURSE: {
        // match_recursive 遍历 doc 的子树查找所有匹配 key 的节点
        match_recursive(doc, results, n_results, cap, tok->key);
        // 还有剩余 Token → 在直接子节点上继续匹配
        if (tok_idx + 1 < pp->n_tokens) {
            cJSON *child;
            cJSON_ArrayForEach(child, doc) {
                if (cJSON_IsObject(child)) {
                    cJSON *target = cJSON_GetObjectItemCaseSensitive(child, tok->key);
                    if (target)
                        matched += match_tokens(pp, target, tok_idx + 1, results, n_results, cap);
                }
            }
        }
        break;
    }

    // ---------------------------------------------------------------
    // TOK_RECURSE_ALL ($..*) —— 收集当前节点及其所有后代
    //
    // 注意：原始根节点在 path_resolve() 的多匹配处理中被排除，
    // 以保证与 Rust 版的输出一致（Rust 版不包含根自身）。
    // ---------------------------------------------------------------
    case TOK_RECURSE_ALL: {
        // 将当前节点加入结果
        ensure_capacity(results, n_results, cap);
        (*results)[(*n_results)++] = doc;
        // 递归处理所有子节点
        cJSON *child;
        cJSON_ArrayForEach(child, doc) {
            cJSON **sub_results = NULL;
            int n_sub = 0, cap_sub = 0;
            // 注意：传递 tok_idx（而非 tok_idx+1），不消耗 Token，
            // 所以每个子节点也会应用 TOK_RECURSE_ALL，实现递归收集
            match_tokens(pp, child, tok_idx, &sub_results, &n_sub, &cap_sub);
            // 将子节点的结果合并到父节点的结果中
            for (int i = 0; i < n_sub; i++) {
                ensure_capacity(results, n_results, cap);
                (*results)[(*n_results)++] = sub_results[i];
            }
            free(sub_results);  // 只释放数组，不释放 cJSON 节点本身
        }
        break;
    }

    // ---------------------------------------------------------------
    // TOK_FILTER ([?(@.)]) —— 占位
    // 当前版本不解析过滤条件，直接返回无匹配
    // ---------------------------------------------------------------
    case TOK_FILTER:
        break;  // 不做条件匹配，安全返回空结果

    default:
        break;
    }

    return matched;
}

// ---------------------------------------------------------------
// match_recursive —— 递归下降查找指定键
//
// 在 doc 的整个子树（深度优先）中，查找所有键名等于 key 的节点。
// 不包含 doc 自身（即使 doc 的键名匹配），只查找后代。
// 用于 TOK_RECURSE (..key) 的递归匹配。
// ---------------------------------------------------------------
static void match_recursive(cJSON *doc, cJSON ***results, int *n_results, int *cap, const char *key) {
    if (!doc) return;

    if (cJSON_IsObject(doc)) {
        // 当前节点是对象 → 检查是否有匹配的键
        cJSON *child = cJSON_GetObjectItemCaseSensitive(doc, key);
        if (child) {
            ensure_capacity(results, n_results, cap);
            (*results)[*n_results] = child;  // 直接存储指针（doc 生命周期内有效）
            (*n_results)++;
        }
        // 继续递归到所有子节点
        cJSON *item;
        cJSON_ArrayForEach(item, doc) {
            match_recursive(item, results, n_results, cap, key);
        }
    } else if (cJSON_IsArray(doc)) {
        // 当前节点是数组 → 递归到每个元素
        cJSON *item;
        cJSON_ArrayForEach(item, doc) {
            match_recursive(item, results, n_results, cap, key);
        }
    }
    // 其他类型（数字/字符串/布尔/null）→ 没有后代，停止递归
}

// =============================================================================
// 公共 API
//
// path_resolve() 是对外的唯一入口。内部依次执行：
//   1. parse_path() —— 词法分析
//   2. 空路径/根路径 → 直接返回 doc
//   3. legacy 路径 → 单路径遍历（返回第一个匹配）
//   4. JSONPath 路径 → 多路径遍历（收集所有匹配）
// =============================================================================

// ---------------------------------------------------------------
// path_resolve —— 编译并执行路径表达式
//
// 参数：
//   ctx      输出：result / node / err_msg
//   doc      输入：JSON 文档根节点
//   path_str 输入：路径表达式（如 ".a.b[0]" 或 "$.a.b"）
//
// 匹配结果：
//   - 空路径或 "$"/"." → 直接返回 doc
//   - legacy 单匹配   → ctx->node 指向文档树内节点（不分配新内存）
//   - JSONPath 多匹配 → ctx->node 指向新分配的 cJSON 数组（调用者需清理）
// ---------------------------------------------------------------
void path_resolve(PathCtx *ctx, const IValue *doc, const char *path_str) {
    // 清零输出上下文
    memset(ctx, 0, sizeof(*ctx));
    ctx->result = PATH_ERR_NO_MATCH;  // 默认：无匹配

    // 空文档 → 错误
    if (!doc) {
        snprintf(ctx->err_msg, sizeof(ctx->err_msg), "nil document");
        return;
    }

    // ---------------------------------------------------------------
    // 第 1 步：词法分析
    // ---------------------------------------------------------------
    ParsedPath pp;
    if (parse_path(path_str, &pp) != 0) {
        // 解析失败 → 语法错误
        snprintf(ctx->err_msg, sizeof(ctx->err_msg), "%s", pp.err);
        ctx->result = PATH_ERR_SYNTAX;
        return;
    }

    // ---------------------------------------------------------------
    // 第 2 步：空路径或根路径 → 返回文档根节点
    // ---------------------------------------------------------------
    if (pp.n_tokens == 0) {
        ctx->node = (IValue *)doc;
        ctx->result = PATH_OK;
        parsed_path_cleanup(&pp);
        return;
    }

    // 路径仅为 "$" 或 "." → 返回文档根节点
    if (pp.n_tokens == 1 && pp.tokens[0].type == TOK_ROOT) {
        ctx->node = (IValue *)doc;
        ctx->result = PATH_OK;
        parsed_path_cleanup(&pp);
        return;
    }

    // ---------------------------------------------------------------
    // 第 3 步：确定起始 Token 索引
    // ---------------------------------------------------------------
    int start_tok = 0;
    if (pp.n_tokens > 0 && pp.tokens[0].type == TOK_ROOT)
        start_tok = 1;  // 跳过开头的 $

    // ---------------------------------------------------------------
    // 第 4 步：执行匹配
    // ---------------------------------------------------------------
    cJSON **results = NULL;
    int n_results = 0, cap = 0;

    if (pp.is_legacy && pp.n_tokens > 0) {
        // ===============================================================
        // legacy 模式：单路径遍历
        // 按 Token 列表顺序逐段深入，返回第一个完整的匹配。
        // 注意：不会像 JSONPath 那样收集多个匹配。
        // ===============================================================
        cJSON *cur = (cJSON *)doc;
        int ok = 1;  // 匹配状态标志

        for (int i = start_tok; i < pp.n_tokens && ok; i++) {
            Token *t = &pp.tokens[i];

            // .key → 对象键查找
            if (t->type == TOK_KEY) {
                if (!cJSON_IsObject(cur)) {
                    // 当前节点不是对象 → 无法取键
                    snprintf(ctx->err_msg, sizeof(ctx->err_msg), "WRONGTYPE: not an object");
                    ctx->result = PATH_ERR_TYPE;
                    ok = 0;
                    break;
                }
                cur = cJSON_GetObjectItemCaseSensitive(cur, t->key);
                if (!cur) {
                    // 键不存在 → 无匹配
                    ctx->result = PATH_ERR_NO_MATCH;
                    ok = 0;
                }
            }
            // [N] → 数组索引
            else if (t->type == TOK_INDEX) {
                if (!cJSON_IsArray(cur)) {
                    snprintf(ctx->err_msg, sizeof(ctx->err_msg), "WRONGTYPE: not an array");
                    ctx->result = PATH_ERR_TYPE;
                    ok = 0;
                    break;
                }
                int len = cJSON_GetArraySize(cur);
                int idx = t->index;
                if (idx < 0) idx = len + idx;  // 负索引调整
                if (idx < 0 || idx >= len) {
                    snprintf(ctx->err_msg, sizeof(ctx->err_msg), "index out of range");
                    ctx->result = PATH_ERR_OOB;
                    ok = 0;
                    break;
                }
                cur = cJSON_GetArrayItem(cur, idx);
            }
            // [*] / .* → 通配：将所有子节点包装为 JSON 数组
            else if (t->type == TOK_WILDCARD) {
                cJSON *arr = cJSON_CreateArray();
                cJSON *child;
                cJSON_ArrayForEach(child, cur) {
                    cJSON *dup = cJSON_Duplicate(child, 1);
                    cJSON_AddItemToArray(arr, dup);
                }
                cur = arr;  // 注意：此内存不会自动释放
            }
            // 其他 Token 类型在 legacy 模式下不支持
            else {
                ok = 0;
                ctx->result = PATH_ERR_NO_MATCH;
            }
        }

        // legacy 匹配成功 → 输出结果
        if (ok) {
            ctx->node = cur;
            ctx->result = PATH_OK;
        }
    } else {
        // ===============================================================
        // JSONPath 模式：多路径遍历
        // 收集所有匹配的节点，根据数量决定返回方式：
        //   - 0 匹配 → PATH_ERR_NO_MATCH
        //   - 1 匹配 → 返回裸节点
        //   - 多匹配 → 包装为 JSON 数组（$..* 跳过根）
        // ===============================================================
        match_tokens(&pp, (cJSON *)doc, start_tok, &results, &n_results, &cap);

        if (n_results == 0) {
            // 无匹配
            ctx->result = PATH_ERR_NO_MATCH;
        } else if (n_results == 1) {
            // 单匹配 → 返回节点本身
            ctx->node = results[0];
            ctx->result = PATH_OK;
        } else {
            // 多匹配 → 包装为 JSON 数组
            // 对 $..* 需要跳过原始根节点（与 Rust 版一致）
            int skip_root = 0;
            if (pp.n_tokens == 1 && pp.tokens[0].type == TOK_RECURSE_ALL) {
                skip_root = 1;  // 路径只有 ..* → 跳过根
            } else if (pp.n_tokens >= 2 && pp.tokens[1].type == TOK_RECURSE_ALL) {
                skip_root = 1;  // 路径是 $..* → 跳过根
            }
            // 构建结果数组，深拷贝每个匹配节点
            cJSON *arr = cJSON_CreateArray();
            for (int i = (skip_root ? 1 : 0); i < n_results; i++) {
                cJSON *dup = cJSON_Duplicate(results[i], 1);
                cJSON_AddItemToArray(arr, dup);
            }
            ctx->node = arr;   // arr 由调用者通过 path_ctx_cleanup 释放
            ctx->result = PATH_OK;
        }
    }

    // ---------------------------------------------------------------
    // 清理临时资源
    // ---------------------------------------------------------------
    if (results) free(results);  // 仅释放结果指针数组，不释放 cJSON 节点
    parsed_path_cleanup(&pp);    // 释放 Token 中的动态分配
}

// ---------------------------------------------------------------
// path_ctx_cleanup —— 释放路径解析上下文的额外内存
//
// 对于 JSONPath 多匹配结果，ctx->node 指向新分配的 cJSON 数组，
// 需要在此释放。对于单匹配或 legacy 匹配，ctx->node 指向文档树
// 内的节点，不需要释放。
// ---------------------------------------------------------------
void path_ctx_cleanup(PathCtx *ctx) {
    // 当前实现中，多匹配结果的生命周期由调用命令处理器管理。
    // ctx->node 如果是 JSONPath 多匹配产生的数组，调用者会在
    // 使用完毕后通过 cJSON_Delete 释放。
    // 此函数暂时为空，为未来扩展预留接口。
    (void)ctx;
}
