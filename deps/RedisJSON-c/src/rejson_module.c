// =============================================================================
// rejson_module.c —— RedisJSON 的 C 语言实现 (ReJSON-c)
//
// 这是 Redis 模块的主文件。功能：
//   1. RedisModule_OnLoad — 模块注册入口，注册所有 21 个 JSON.* 命令
//   2. ReJSONDoc 数据类型 — Redis key 关联的 JSON 文档
//   3. RDB 持久化 — save/load 回调
//   4. 全部 21 个命令处理器
//
// 架构：每个 Redis key 关联一个 ReJSONDoc（内含 cJSON 树）。
// 命令处理器用 path_resolve() 定位路径节点，操作后通过 RedisModule_Reply* 返回。
//
// Rust 对照：commands.rs (命令) + lib.rs (注册) + key_value.rs (读写) + manager.rs (trait)
// =============================================================================

#include <redismodule.h>   // RedisModule API（Init/CreateCommand/Reply* 等）
#include "rejson.h"        // IValue / ReJSONType / PathCtx
#include <string.h>        // memset, strlen, strcmp, strncpy, strchr
#include <stdio.h>         // snprintf
#include <stdlib.h>        // atoi, atof
#include <math.h>          // pow

// 前向声明：定义在后面的命令函数（OnLoad 需要引用它们）
static int cmd_json_mget(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
static int cmd_json_resp(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
static int cmd_json_merge(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
static int cmd_json_mset(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
static int cmd_json_arrinsert(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
static int cmd_json_arrtrim(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
static int cmd_json_arrindex(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);

// =============================================================================
// ReJSONDoc —— 每个 Redis key 对应的 JSON 文档
// =============================================================================

static RedisModuleType *ReJSONC_Type;  // 模块类型引用

typedef struct { IValue *root; } ReJSONDoc;  // root=NULL 表示空 key

static ReJSONDoc *rejson_doc_new(IValue *root) {
    ReJSONDoc *d = RedisModule_Alloc(sizeof(ReJSONDoc));
    d->root = root;
    return d;
}

// 释放 ReJSONDoc — RedisModuleTypeMethods.free 回调
static void rejson_doc_free(void *val) {
    ReJSONDoc *d = (ReJSONDoc *)val;
    if (d->root) cJSON_Delete(d->root);
    RedisModule_Free(d);
}

// =============================================================================
// RDB 持久化回调 — 紧凑字符串格式：[signed_len][json_bytes]
// =============================================================================

// RDB save：JSON 树 → 紧凑字符串 → 写入
static void rejson_doc_rdb_save(RedisModuleIO *rdb, void *val) {
    ReJSONDoc *d = (ReJSONDoc *)val;
    if (!d->root) { RedisModule_SaveSigned(rdb, 0); return; }
    char *json = cJSON_PrintUnformatted(d->root);
    if (!json) { RedisModule_SaveSigned(rdb, 0); return; }
    size_t len = strlen(json);
    RedisModule_SaveSigned(rdb, (long long)len);
    RedisModule_SaveStringBuffer(rdb, json, len);
    cJSON_free(json);
}

// RDB load：读字符串 → 解析为 IValue 树
static void *rejson_doc_rdb_load(RedisModuleIO *rdb, int encver) {
    (void)encver;
    long long len = RedisModule_LoadSigned(rdb);
    if (len <= 0) return rejson_doc_new(NULL);
    size_t actual_len = 0;
    char *buf = RedisModule_LoadStringBuffer(rdb, &actual_len);
    if (!buf || actual_len == 0) { if (buf) RedisModule_Free(buf); return rejson_doc_new(NULL); }
    IValue *root = cJSON_Parse(buf);
    RedisModule_Free(buf);
    if (!root) return rejson_doc_new(NULL);
    return rejson_doc_new(root);
}

// 内存占用估算
static size_t rejson_doc_mem_usage(const void *val) {
    const ReJSONDoc *d = (const ReJSONDoc *)val;
    if (!d->root) return sizeof(ReJSONDoc);
    char *s = cJSON_PrintUnformatted(d->root);
    size_t sz = sizeof(ReJSONDoc) + strlen(s) + 1;
    cJSON_free(s);
    return sz;
}

// =============================================================================
// key 打开辅助函数 — 打开 Redis key 并验证类型是 ReJSONDoc
// =============================================================================

// 打开 key（读或写），验证类型，非 JSON 类型返回 WRONGTYPE
static RedisModuleKey *open_key_read(RedisModuleCtx *ctx, RedisModuleString *keyname, int write) {
    int mode = write ? REDISMODULE_WRITE : REDISMODULE_READ;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyname, mode);
    if (!key) return NULL;
    int type = RedisModule_KeyType(key);
    if (type == REDISMODULE_KEYTYPE_EMPTY) return key;
    RedisModuleType *kt = RedisModule_ModuleTypeGetType(key);
    if (kt != ReJSONC_Type) {
        RedisModule_CloseKey(key);
        RedisModule_ReplyWithError(ctx, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return NULL;
    }
    return key;
}

static RedisModuleKey *open_key_write(RedisModuleCtx *ctx, RedisModuleString *keyname) {
    return open_key_read(ctx, keyname, 1);
}

// =============================================================================
// resolve_to_json — 解析路径并返回 JSON 文本
//
// JSONPath 无匹配 → "[]"，legacy 无匹配 → NULL，匹配成功 → JSON 文本
// JSONPath 单匹配数组包裹（$..* 已包裹的不重复包）
// =============================================================================
static char *resolve_to_json(const IValue *doc, const char *path_str) {
    PathCtx pctx;
    path_resolve(&pctx, doc, path_str);
    if (pctx.result != PATH_OK) {
        path_ctx_cleanup(&pctx);
        if (path_str[0] == '$') {
            char *empty_arr = cJSON_malloc(3);
            if (empty_arr) { empty_arr[0]='['; empty_arr[1]=']'; empty_arr[2]='\0'; }
            return empty_arr;
        }
        return NULL;
    }
    int is_jp = (path_str[0] == '$');
    char *json = NULL;
    if (is_jp) {
        int is_recursive = (strstr(path_str, "..") != NULL);
        if (is_recursive && pctx.node && pctx.node->type == cJSON_Array) {
            json = cJSON_PrintUnformatted(pctx.node);
        } else {
            cJSON *arr = cJSON_CreateArray();
            cJSON *dup = cJSON_Duplicate(pctx.node, 1);
            cJSON_AddItemToArray(arr, dup);
            json = cJSON_PrintUnformatted(arr);
            cJSON_Delete(arr);
        }
    } else {
        json = ivalue_to_string(pctx.node, 0);
    }
    path_ctx_cleanup(&pctx);
    return json;
}

// =============================================================================
// walk_to_parent — 沿路径字符串定位叶子节点的父节点
//
// 用于 JSON.SET 非根路径设置。遍历前 n-1 段找到父节点，
// 最后一段通过输出参数返回。中间段不存在则报错（不自动创建）。
// =============================================================================
static int walk_to_parent(IValue *root, const char *path_str, IValue **parent,
                           char *last_key, int *last_idx, int *is_array,
                           char *err_msg, size_t err_sz) {
    *parent = root; *is_array = 0;
    IValue *cur = root;
    const char *p = path_str;
    if (*p == '$' || *p == '.') p++;
    if (*p == '\0') return 0;

    char path_copy[512]; strncpy(path_copy, p, sizeof(path_copy)-1); path_copy[sizeof(path_copy)-1]='\0';
    int seg_is_array[64]; int seg_indices[64]; char seg_keys[64][256]; int n_segs = 0;
    char *save = NULL;
    char *tok = strtok_r(path_copy, ".", &save);
    while (tok && n_segs < 64) {
        char *bracket = strchr(tok, '[');
        if (bracket) {
            size_t key_len = bracket - tok;
            memcpy(seg_keys[n_segs], tok, key_len); seg_keys[n_segs][key_len]='\0';
            seg_is_array[n_segs]=0; n_segs++;
            seg_indices[n_segs]=atoi(bracket+1); seg_is_array[n_segs]=1; seg_keys[n_segs][0]='\0'; n_segs++;
        } else {
            strncpy(seg_keys[n_segs], tok, 255); seg_keys[n_segs][255]='\0';
            seg_is_array[n_segs]=0; n_segs++;
        }
        tok = strtok_r(NULL, ".", &save);
    }
    if (n_segs == 0) return 0;
    for (int i = 0; i < n_segs - 1; i++) {
        if (seg_is_array[i]) {
            if (!cJSON_IsArray(cur)) { snprintf(err_msg, err_sz, "WRONGTYPE: not an array"); return -1; }
            int len = cJSON_GetArraySize(cur); int idx = seg_indices[i];
            if (idx < 0) idx = len + idx;
            if (idx < 0 || idx >= len) { snprintf(err_msg, err_sz, "ERR index out of range"); return -1; }
            cur = cJSON_GetArrayItem(cur, idx);
            if (!cur) { snprintf(err_msg, err_sz, "ERR index out of range"); return -1; }
        } else {
            if (!cJSON_IsObject(cur)) { snprintf(err_msg, err_sz, "WRONGTYPE: not an object"); return -1; }
            cur = cJSON_GetObjectItemCaseSensitive(cur, seg_keys[i]);
            if (!cur) { snprintf(err_msg, err_sz, "new objects must be created at the root"); return -1; }
        }
    }
    *parent = cur;
    int last = n_segs - 1;
    if (seg_is_array[last]) { *is_array=1; *last_idx=seg_indices[last]; }
    else { *is_array=0; strncpy(last_key, seg_keys[last],255); last_key[255]='\0'; }
    return 0;
}

// =============================================================================
// JSON.SET <key> <path> <json> [NX | XX]
// 设置 JSON 文档中指定路径的值。支持 NX/XX。
// =============================================================================
static int cmd_json_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }

    int nx = 0, xx = 0;
    for (int i = 4; i < argc; i++) {
        const char *s = RedisModule_StringPtrLen(argv[i], NULL);
        if (strcasecmp(s, "NX") == 0) nx = 1;
        else if (strcasecmp(s, "XX") == 0) xx = 1;
    }
    if (nx && xx) { RedisModule_ReplyWithError(ctx, "ERR syntax error: NX and XX are mutually exclusive"); return REDISMODULE_OK; }

    const char *json_str = RedisModule_StringPtrLen(argv[3], NULL);
    IValue *new_val = ivalue_parse(json_str);
    if (!new_val) { RedisModule_ReplyWithError(ctx, "ERR failed to parse JSON"); return REDISMODULE_OK; }

    RedisModuleKey *key = open_key_write(ctx, argv[1]);
    if (!key) { cJSON_Delete(new_val); return REDISMODULE_OK; }

    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    int is_root = (strcmp(path_str, ".") == 0 || strcmp(path_str, "$") == 0);

    ReJSONDoc *doc = NULL;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        if (xx) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); cJSON_Delete(new_val); return REDISMODULE_OK; }
        if (!is_root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR new objects must be created at the root"); cJSON_Delete(new_val); return REDISMODULE_OK; }
        doc = rejson_doc_new(NULL);
        RedisModule_ModuleTypeSetValue(key, ReJSONC_Type, doc);
    } else {
        doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
        if (!doc) { RedisModule_CloseKey(key); cJSON_Delete(new_val); return REDISMODULE_OK; }
    }

    // 根路径：替换整个文档
    if (is_root) {
        if (nx && doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); cJSON_Delete(new_val); return REDISMODULE_OK; }
        if (doc->root) cJSON_Delete(doc->root);
        doc->root = new_val;
        RedisModule_CloseKey(key);
        RedisModule_ReplyWithSimpleString(ctx, "OK");
        return REDISMODULE_OK;
    }

    // 非根路径：walk_to_parent 找到父节点，替换叶子
    if (!doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR new objects must be created at the root"); cJSON_Delete(new_val); return REDISMODULE_OK; }

    char last_key[256] = {0}; int last_idx = 0, is_array = 0; IValue *parent = NULL; char err_msg[256] = {0};
    if (walk_to_parent(doc->root, path_str, &parent, last_key, &last_idx, &is_array, err_msg, sizeof(err_msg)) != 0) {
        RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, err_msg); cJSON_Delete(new_val); return REDISMODULE_OK;
    }

    if (is_array) {
        int len = cJSON_GetArraySize(parent); int idx = last_idx;
        if (idx < 0) idx = len + idx;
        if (idx < 0 || idx >= len) { RedisModule_CloseKey(key); cJSON_Delete(new_val); RedisModule_ReplyWithError(ctx, "index out of range"); return REDISMODULE_OK; }
        cJSON_DeleteItemFromArray(parent, idx);
        cJSON *new_arr = cJSON_CreateArray();
        for (int j = 0; j < len; j++) {
            if (j == idx) { cJSON_AddItemToArray(new_arr, new_val); }
            else { cJSON *d = cJSON_Duplicate(cJSON_GetArrayItem(parent, j), 1); cJSON_AddItemToArray(new_arr, d); }
        }
        parent->child = new_arr->child; new_arr->child = NULL; cJSON_free(new_arr);
    } else {
        cJSON *existing = cJSON_GetObjectItemCaseSensitive(parent, last_key);
        if (nx && existing) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); cJSON_Delete(new_val); return REDISMODULE_OK; }
        if (xx && !existing) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); cJSON_Delete(new_val); return REDISMODULE_OK; }
        if (existing) cJSON_DeleteItemFromObject(parent, last_key);
        cJSON_AddItemToObject(parent, last_key, new_val);
    }

    RedisModule_CloseKey(key);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.GET <key> [path ...] [FORMAT ...]
// 读取 JSON 值。单路径返回裸值/数组包裹，多路径返回 {path: val} 对象。
// =============================================================================
static int is_format_arg(const char *s) {
    return !strcasecmp(s, "FORMAT") || !strcasecmp(s, "INDENT") || !strcasecmp(s, "NEWLINE") || !strcasecmp(s, "SPACE");
}

static int cmd_json_get(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }

    RedisModuleKey *key = open_key_read(ctx, argv[1], 0);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }

    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }

    const char *paths[64]; int n_paths = 0;
    for (int i = 2; i < argc; i++) {
        const char *s = RedisModule_StringPtrLen(argv[i], NULL);
        if (!strcasecmp(s, "FORMAT") || !strcasecmp(s, "INDENT") || !strcasecmp(s, "NEWLINE") || !strcasecmp(s, "SPACE")) { i++; continue; }
        if (n_paths < 64) paths[n_paths++] = s;
    }
    if (n_paths == 0) { paths[0] = "."; n_paths = 1; }

    // 单路径 → 直接返回 JSON 文本
    if (n_paths == 1) {
        char *json = resolve_to_json(doc->root, paths[0]);
        if (!json) {
            RedisModule_ReplyWithNull(ctx);
        } else {
            RedisModule_ReplyWithStringBuffer(ctx, json, strlen(json));
            cJSON_free(json);
        }
        RedisModule_CloseKey(key);
        return REDISMODULE_OK;
    }

    // 多路径 → 返回 {path: val, ...} 对象
    cJSON *result_obj = cJSON_CreateObject();
    for (int i = 0; i < n_paths; i++) {
        char *json = resolve_to_json(doc->root, paths[i]);
        if (json) {
            cJSON *val = cJSON_Parse(json); cJSON_free(json);
            if (val) cJSON_AddItemToObject(result_obj, paths[i], val);
        } else {
            cJSON_AddNullToObject(result_obj, paths[i]);
        }
    }
    char *out = cJSON_PrintUnformatted(result_obj);
    cJSON_Delete(result_obj);
    if (out) { RedisModule_ReplyWithStringBuffer(ctx, out, strlen(out)); cJSON_free(out); }
    else { RedisModule_ReplyWithNull(ctx); }

    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.TYPE <key> [path]
// 返回节点类型名（integer/number/string/boolean/null/array/object）
// =============================================================================
static int cmd_json_type(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_read(ctx, argv[1], 0);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK) { RedisModule_ReplyWithNull(ctx); }
    else { ReJSONType t = ivalue_type(pctx.node); RedisModule_ReplyWithSimpleString(ctx, rejson_type_name(t)); }
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.DEL <key> [path ...]
// 删除路径节点。无路径删整个 key，多路径返回计数，空文档自动删 key。
// =============================================================================
static int cmd_json_del(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithLongLong(ctx, 0); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithLongLong(ctx, 0); return REDISMODULE_OK; }

    const char *paths[64]; int n_paths = 0;
    for (int i = 2; i < argc; i++) { if (n_paths < 64) paths[n_paths++] = RedisModule_StringPtrLen(argv[i], NULL); }

    if (n_paths == 0) {
        cJSON_Delete(doc->root); doc->root = NULL;
        RedisModule_DeleteKey(key); RedisModule_CloseKey(key);
        RedisModule_ReplyWithLongLong(ctx, 1); return REDISMODULE_OK;
    }

    int deleted = 0;
    for (int i = 0; i < n_paths; i++) {
        PathCtx pctx; path_resolve(&pctx, doc->root, paths[i]);
        if (pctx.result == PATH_OK && pctx.node != NULL) {
            const char *p = paths[i]; if (*p == '$' || *p == '.') p++;
            const char *dot = strrchr(p, '.'); const char *bracket = strrchr(p, '[');
            const char *split = (dot > bracket) ? dot : bracket;
            if (!split) {
                if (*p == '\0') { cJSON_Delete(doc->root); doc->root = NULL; deleted++; }
                else if (*p == '[' && p[strlen(p)-1]==']') {
                    int idx = atoi(p+1); int len = cJSON_GetArraySize(doc->root);
                    if (idx < 0) idx = len + idx;
                    if (idx>=0 && idx<len) { cJSON *item=cJSON_DetachItemFromArray(doc->root,idx); if(item){cJSON_Delete(item);deleted++;} }
                } else {
                    cJSON *item = cJSON_DetachItemFromObject(doc->root, p);
                    if (item) { cJSON_Delete(item); deleted++; }
                }
            } else if (*split == '.') {
                char parent_path[256]; int plen = (int)(split - p);
                int prefix_len = (paths[i][0]=='$'||paths[i][0]=='.')?1:0;
                snprintf(parent_path,sizeof(parent_path),"%.*s",plen+prefix_len,paths[i]);
                const char *key = split+1;
                PathCtx pc; path_resolve(&pc, doc->root, parent_path);
                if (pc.result==PATH_OK && pc.node && pc.node->type==cJSON_Object) {
                    cJSON *item = cJSON_DetachItemFromObject(pc.node, key);
                    if (item) { cJSON_Delete(item); deleted++; }
                }
                path_ctx_cleanup(&pc);
            } else if (*split == '[') {
                char parent_path[256]; int plen = (int)(split - p);
                int prefix_len = (paths[i][0]=='$'||paths[i][0]=='.')?1:0;
                snprintf(parent_path,sizeof(parent_path),"%.*s",plen+prefix_len,paths[i]);
                int idx = atoi(split+1);
                PathCtx pc; path_resolve(&pc, doc->root, parent_path);
                if (pc.result==PATH_OK && pc.node && pc.node->type==cJSON_Array) {
                    int len = cJSON_GetArraySize(pc.node);
                    if (idx < 0) idx = len + idx;
                    if (idx>=0 && idx<len) { cJSON *item=cJSON_DetachItemFromArray(pc.node,idx); if(item){cJSON_Delete(item);deleted++;} }
                }
                path_ctx_cleanup(&pc);
            }
        }
        path_ctx_cleanup(&pctx);
    }
    if (doc->root && ivalue_is_empty(doc->root)) { cJSON_Delete(doc->root); doc->root = NULL; RedisModule_DeleteKey(key); }
    RedisModule_CloseKey(key); RedisModule_ReplyWithLongLong(ctx, deleted);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.OBJLEN / ARRLEN / STRLEN / OBJKEYS
// 容器/叶子统计查询。类型不匹配返回错误。
// =============================================================================
static int cmd_json_objlen(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_read(ctx, argv[1], 0);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Object) { RedisModule_ReplyWithNull(ctx); }
    else { RedisModule_ReplyWithLongLong(ctx, (long long)cJSON_GetArraySize(pctx.node)); }
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

static int cmd_json_arrlen(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_read(ctx, argv[1], 0);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Array) { RedisModule_ReplyWithNull(ctx); }
    else { RedisModule_ReplyWithLongLong(ctx, (long long)cJSON_GetArraySize(pctx.node)); }
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

static int cmd_json_strlen(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_read(ctx, argv[1], 0);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_String) { RedisModule_ReplyWithNull(ctx); }
    else { RedisModule_ReplyWithLongLong(ctx, (long long)strlen(pctx.node->valuestring)); }
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

static int cmd_json_objkeys(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_read(ctx, argv[1], 0);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Object) { RedisModule_ReplyWithNull(ctx); }
    else {
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
        cJSON *child; int count = 0;
        cJSON_ArrayForEach(child, pctx.node) { RedisModule_ReplyWithStringBuffer(ctx, child->string, strlen(child->string)); count++; }
        RedisModule_ReplySetArrayLength(ctx, count);
    }
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.NUMINCRBY / NUMMULTBY / NUMPOWBY
// 数值运算：原地修改 JSON 文档中的数值节点。
// =============================================================================
static int cmd_json_numincrby(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    double incr = atof(RedisModule_StringPtrLen(argv[3], NULL));
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Number) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR not a number"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    double new_val = pctx.node->valuedouble + incr;
    pctx.node->valuedouble = new_val; pctx.node->valueint = (int64_t)new_val;
    char buf[64];
    if (new_val == (double)(int64_t)new_val) { snprintf(buf, sizeof(buf), "%lld", (long long)new_val); }
    else { snprintf(buf, sizeof(buf), "%.15g", new_val); }
    RedisModule_ReplyWithStringBuffer(ctx, buf, strlen(buf));
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

static int cmd_json_nummultby(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    double factor = atof(RedisModule_StringPtrLen(argv[3], NULL));
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Number) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR not a number"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    double new_val = pctx.node->valuedouble * factor;
    pctx.node->valuedouble = new_val; pctx.node->valueint = (int64_t)new_val;
    char buf[64];
    if (new_val == (double)(int64_t)new_val) { snprintf(buf, sizeof(buf), "%lld", (long long)new_val); }
    else { snprintf(buf, sizeof(buf), "%.15g", new_val); }
    RedisModule_ReplyWithStringBuffer(ctx, buf, strlen(buf));
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

static int cmd_json_numpowby(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    double power = atof(RedisModule_StringPtrLen(argv[3], NULL));
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Number) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR not a number"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    double new_val = pow(pctx.node->valuedouble, power);
    pctx.node->valuedouble = new_val; pctx.node->valueint = (int64_t)new_val;
    char buf[64];
    if (new_val == (double)(int64_t)new_val) { snprintf(buf, sizeof(buf), "%lld", (long long)new_val); }
    else { snprintf(buf, sizeof(buf), "%.15g", new_val); }
    RedisModule_ReplyWithStringBuffer(ctx, buf, strlen(buf));
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.ARR* — 数组操作全家桶
// =============================================================================

// JSON.ARRAPPEND <key> <path> <json> [json ...] — 追加到尾部
static int cmd_json_arrappend(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Array) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR not an array"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    for (int i = 3; i < argc; i++) {
        const char *val_str = RedisModule_StringPtrLen(argv[i], NULL);
        IValue *v = cJSON_Parse(val_str);
        if (!v) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR failed to parse JSON"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
        cJSON_AddItemToArray(pctx.node, v);
    }
    RedisModule_ReplyWithLongLong(ctx, (long long)cJSON_GetArraySize(pctx.node));
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// JSON.ARRINSERT <key> <path> <index> <json> [json ...] — 在指定位置插入
static int cmd_json_arrinsert(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 5) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    int ins_idx = atoi(RedisModule_StringPtrLen(argv[3], NULL));
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Array) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR not an array"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    int len = cJSON_GetArraySize(pctx.node);
    if (ins_idx < 0) ins_idx = len + ins_idx;
    if (ins_idx < 0 || ins_idx > len) ins_idx = len;
    cJSON *new_vals[64]; int n_new = 0;
    for (int i = 4; i < argc; i++) {
        cJSON *v = cJSON_Parse(RedisModule_StringPtrLen(argv[i], NULL));
        if (!v) { for (int j = 0; j < n_new; j++) cJSON_Delete(new_vals[j]); RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR failed to parse JSON"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
        new_vals[n_new++] = v;
    }
    cJSON *new_arr = cJSON_CreateArray();
    for (int i = 0; i < len; i++) {
        if (i == ins_idx) for (int j = 0; j < n_new; j++) cJSON_AddItemToArray(new_arr, new_vals[j]);
        cJSON *item = cJSON_GetArrayItem(pctx.node, i);
        if (item) { cJSON *dup = cJSON_Duplicate(item, 1); cJSON_AddItemToArray(new_arr, dup); }
    }
    if (ins_idx >= len) for (int j = 0; j < n_new; j++) cJSON_AddItemToArray(new_arr, new_vals[j]);
    pctx.node->child = new_arr->child; new_arr->child = NULL; cJSON_free(new_arr);
    RedisModule_ReplyWithLongLong(ctx, (long long)cJSON_GetArraySize(pctx.node));
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// JSON.ARRPOP <key> [path [index]] — 弹出元素
static int cmd_json_arrpop(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]);
    if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    int idx = -1; if (argc > 3) idx = atoi(RedisModule_StringPtrLen(argv[3], NULL));
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Array) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    int len = cJSON_GetArraySize(pctx.node);
    if (idx < 0) idx = len + idx;
    if (idx < 0 || idx >= len) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    cJSON *popped = cJSON_DetachItemFromArray(pctx.node, idx);
    if (popped) { char *s = cJSON_PrintUnformatted(popped); RedisModule_ReplyWithStringBuffer(ctx, s, strlen(s)); cJSON_free(s); cJSON_Delete(popped); }
    else { RedisModule_ReplyWithNull(ctx); }
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// JSON.ARRTRIM <key> <path> <start> <stop> — 保留指定范围
static int cmd_json_arrtrim(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 5) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]); if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    int start_idx = atoi(RedisModule_StringPtrLen(argv[3], NULL));
    int stop_idx = atoi(RedisModule_StringPtrLen(argv[4], NULL));
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Array) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR not an array"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    int len = cJSON_GetArraySize(pctx.node);
    if (start_idx < 0) start_idx = len + start_idx;
    if (stop_idx < 0) stop_idx = len + stop_idx;
    if (start_idx < 0) start_idx = 0;
    if (stop_idx >= len) stop_idx = len - 1;
    if (start_idx > stop_idx || start_idx >= len) {
        cJSON_Delete(pctx.node->child); pctx.node->child = NULL;
        RedisModule_ReplyWithLongLong(ctx, 0); path_ctx_cleanup(&pctx); RedisModule_CloseKey(key); return REDISMODULE_OK;
    }
    cJSON *new_arr = cJSON_CreateArray();
    for (int i = start_idx; i <= stop_idx && i < len; i++) {
        cJSON *item = cJSON_GetArrayItem(pctx.node, i);
        if (item) { cJSON *dup = cJSON_Duplicate(item, 1); cJSON_AddItemToArray(new_arr, dup); }
    }
    pctx.node->child = new_arr->child; new_arr->child = NULL; cJSON_free(new_arr);
    RedisModule_ReplyWithLongLong(ctx, (long long)cJSON_GetArraySize(pctx.node));
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// JSON.ARRINDEX <key> <path> <json-value> [start [stop]] — 查找首次出现位置
static int cmd_json_arrindex(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_read(ctx, argv[1], 0); if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    const char *val_str = RedisModule_StringPtrLen(argv[3], NULL);
    int start = 0; int stop = -1;
    if (argc > 4) start = atoi(RedisModule_StringPtrLen(argv[4], NULL));
    if (argc > 5) stop = atoi(RedisModule_StringPtrLen(argv[5], NULL));
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_Array) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    cJSON *search_val = cJSON_Parse(val_str);
    if (!search_val) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR failed to parse JSON"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    int len = cJSON_GetArraySize(pctx.node);
    if (start < 0) start = len + start;
    if (stop < 0) stop = len + stop + 1;
    if (start < 0) start = 0;
    if (stop > len) stop = len;
    int found = -1;
    for (int i = start; i < stop && i < len; i++) {
        cJSON *item = cJSON_GetArrayItem(pctx.node, i);
        if (item && cJSON_Compare(item, search_val, 1)) { found = i; break; }
    }
    cJSON_Delete(search_val);
    RedisModule_ReplyWithLongLong(ctx, (long long)found);
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.CLEAR / TOGGLE / STRAPPEND — 简单操作
// =============================================================================

// JSON.CLEAR <key> [path] — 清空容器/置零数字
static int cmd_json_clear(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]); if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithLongLong(ctx, 0); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithLongLong(ctx, 0); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK) { RedisModule_CloseKey(key); RedisModule_ReplyWithLongLong(ctx, 0); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    int cleared = 0;
    if (pctx.node->type == cJSON_Object || pctx.node->type == cJSON_Array) {
        cJSON *child = pctx.node->child;
        while (child) { cJSON *next = child->next; cJSON_DetachItemViaPointer(pctx.node, child); cJSON_Delete(child); cleared++; child = next; }
    } else if (pctx.node->type == cJSON_Number) { pctx.node->valuedouble = 0; pctx.node->valueint = 0; cleared = 1; }
    RedisModule_ReplyWithLongLong(ctx, (long long)cleared);
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// JSON.TOGGLE <key> <path> — 翻转布尔值
static int cmd_json_toggle(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]); if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || (pctx.node->type != cJSON_True && pctx.node->type != cJSON_False)) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR not a boolean"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    pctx.node->type = (pctx.node->type == cJSON_True) ? cJSON_False : cJSON_True;
    RedisModule_ReplyWithLongLong(ctx, (pctx.node->type == cJSON_True) ? 1 : 0);
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// JSON.STRAPPEND <key> [path] <json-string> — 追加字符串
static int cmd_json_strappend(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]); if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR key does not exist"); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    const char *append_str = RedisModule_StringPtrLen(argv[3], NULL);
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK || pctx.node->type != cJSON_String) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR not a string"); path_ctx_cleanup(&pctx); return REDISMODULE_OK; }
    size_t old_len = strlen(pctx.node->valuestring);
    size_t app_len = strlen(append_str);
    const char *actual_append = append_str; size_t actual_app_len = app_len;
    if (*append_str == '"') { actual_append = append_str + 1; actual_app_len = (app_len >= 2) ? app_len - 2 : 0; }
    char *new_str = RedisModule_Alloc(old_len + actual_app_len + 1);
    memcpy(new_str, pctx.node->valuestring, old_len);
    memcpy(new_str + old_len, actual_append, actual_app_len);
    new_str[old_len + actual_app_len] = '\0';
    cJSON_free(pctx.node->valuestring); pctx.node->valuestring = new_str;
    RedisModule_ReplyWithLongLong(ctx, (long long)(old_len + actual_app_len));
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.DEBUG <subcommand> [args] — MEMORY/HELP
// =============================================================================
static int cmd_json_debug(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    const char *subcmd = RedisModule_StringPtrLen(argv[1], NULL);
    if (strcasecmp(subcmd, "HELP") == 0) {
        const char *help[] = {"DEBUG <subcommand> [args]","MEMORY <key> - report memory usage of a JSON key","HELP - this message",NULL};
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
        int count = 0;
        for (int i = 0; help[i]; i++) { RedisModule_ReplyWithStringBuffer(ctx, help[i], strlen(help[i])); count++; }
        RedisModule_ReplySetArrayLength(ctx, count);
    } else if (strcasecmp(subcmd, "MEMORY") == 0) {
        if (argc < 3) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
        RedisModuleKey *key = open_key_read(ctx, argv[2], 0);
        if (!key) return REDISMODULE_OK;
        if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithLongLong(ctx, 0); return REDISMODULE_OK; }
        ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
        if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithLongLong(ctx, 0); return REDISMODULE_OK; }
        RedisModule_ReplyWithLongLong(ctx, (long long)rejson_doc_mem_usage(doc));
        RedisModule_CloseKey(key);
    } else { RedisModule_ReplyWithError(ctx, "ERR unknown subcommand"); }
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.MGET <key> [key ...] <path> — 多 key 读取
// =============================================================================
static int cmd_json_mget(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    const char *path_str = RedisModule_StringPtrLen(argv[argc - 1], NULL);
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    int count = 0;
    for (int i = 1; i < argc - 1; i++) {
        RedisModuleKey *key = open_key_read(ctx, argv[i], 0);
        if (!key) { RedisModule_ReplyWithNull(ctx); count++; continue; }
        if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_ReplyWithNull(ctx); count++; RedisModule_CloseKey(key); continue; }
        ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
        if (!doc || !doc->root) { RedisModule_ReplyWithNull(ctx); count++; }
        else { char *json = resolve_to_json(doc->root, path_str); if(json){RedisModule_ReplyWithStringBuffer(ctx,json,strlen(json));cJSON_free(json);}else{RedisModule_ReplyWithNull(ctx);} count++; }
        RedisModule_CloseKey(key);
    }
    RedisModule_ReplySetArrayLength(ctx, count);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.MSET <key> <path> <json> [[<key> <path> <json>] ...] — 多 key 写入
// 原子性：任一失败则回滚已创建的 key
// =============================================================================
static int cmd_json_mset(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    if ((argc - 1) % 3 != 0) { RedisModule_ReplyWithError(ctx, "ERR wrong number of arguments"); return REDISMODULE_OK; }
    RedisModuleString *created_keys[64]; int n_created = 0;
    for (int i = 1; i < argc; i += 3) {
        RedisModuleString *key_name = argv[i];
        const char *path_str = RedisModule_StringPtrLen(argv[i+1], NULL);
        IValue *new_val = cJSON_Parse(RedisModule_StringPtrLen(argv[i+2], NULL));
        if (!new_val) {
            for (int j = 0; j < n_created; j++) { RedisModuleKey *rk = RedisModule_OpenKey(ctx, created_keys[j], REDISMODULE_WRITE); if(rk){RedisModule_DeleteKey(rk);RedisModule_CloseKey(rk);} }
            RedisModule_ReplyWithError(ctx, "ERR failed to parse JSON"); return REDISMODULE_OK;
        }
        RedisModuleKey *key = RedisModule_OpenKey(ctx, key_name, REDISMODULE_WRITE);
        if (!key) { cJSON_Delete(new_val); continue; }
        int is_new = (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY);
        ReJSONDoc *doc = NULL;
        if (is_new) { doc = rejson_doc_new(NULL); RedisModule_ModuleTypeSetValue(key, ReJSONC_Type, doc); created_keys[n_created++] = key_name; }
        else { doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key); }
        int is_root = (strcmp(path_str, ".") == 0 || strcmp(path_str, "$") == 0);
        if (is_root) { if(doc->root)cJSON_Delete(doc->root); doc->root=new_val; RedisModule_CloseKey(key); continue; }
        if (!doc->root) {
            for (int j = 0; j < n_created; j++) { RedisModuleKey *rk = RedisModule_OpenKey(ctx, created_keys[j], REDISMODULE_WRITE); if(rk){RedisModule_DeleteKey(rk);RedisModule_CloseKey(rk);} }
            cJSON_Delete(new_val); RedisModule_CloseKey(key);
            RedisModule_ReplyWithError(ctx, "ERR new objects must be created at the root"); return REDISMODULE_OK;
        }
        char last_key[256]={0}; int last_idx=0,is_array=0; IValue *parent=NULL; char err_msg[256]={0};
        if (walk_to_parent(doc->root, path_str, &parent, last_key, &last_idx, &is_array, err_msg, sizeof(err_msg)) != 0) {
            for (int j = 0; j < n_created; j++) { RedisModuleKey *rk = RedisModule_OpenKey(ctx, created_keys[j], REDISMODULE_WRITE); if(rk){RedisModule_DeleteKey(rk);RedisModule_CloseKey(rk);} }
            cJSON_Delete(new_val); RedisModule_CloseKey(key);
            RedisModule_ReplyWithError(ctx, err_msg); return REDISMODULE_OK;
        }
        if (is_array) {
            if (!cJSON_IsArray(parent)) { /* rollback and error */ for(int j=0;j<n_created;j++){RedisModuleKey*rk=RedisModule_OpenKey(ctx,created_keys[j],REDISMODULE_WRITE);if(rk){RedisModule_DeleteKey(rk);RedisModule_CloseKey(rk);}} cJSON_Delete(new_val);RedisModule_CloseKey(key);RedisModule_ReplyWithError(ctx,"WRONGTYPE: not an array");return REDISMODULE_OK; }
            int len = cJSON_GetArraySize(parent); int idx = last_idx;
            if (idx < 0) idx = len + idx;
            if (idx < 0 || idx >= len) { /* rollback */ for(int j=0;j<n_created;j++){RedisModuleKey*rk=RedisModule_OpenKey(ctx,created_keys[j],REDISMODULE_WRITE);if(rk){RedisModule_DeleteKey(rk);RedisModule_CloseKey(rk);}} cJSON_Delete(new_val);RedisModule_CloseKey(key);RedisModule_ReplyWithError(ctx,"index out of range");return REDISMODULE_OK; }
            cJSON_DeleteItemFromArray(parent, idx);
            cJSON *new_arr = cJSON_CreateArray();
            for (int j = 0; j < len; j++) { if(j==idx){cJSON_AddItemToArray(new_arr,new_val);}else{cJSON*d=cJSON_Duplicate(cJSON_GetArrayItem(parent,j),1);cJSON_AddItemToArray(new_arr,d);} }
            parent->child = new_arr->child; new_arr->child = NULL; cJSON_free(new_arr);
        } else {
            cJSON *existing = cJSON_GetObjectItemCaseSensitive(parent, last_key);
            if (existing) cJSON_DeleteItemFromObject(parent, last_key);
            cJSON_AddItemToObject(parent, last_key, new_val);
        }
        RedisModule_CloseKey(key);
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.RESP <key> [path] — RESP 格式输出（递归转 RESP 类型）
// =============================================================================
static void resp_reply_value(RedisModuleCtx *ctx, const IValue *v) {
    if (!v) { RedisModule_ReplyWithNull(ctx); return; }
    switch ((int)v->type) {
        case cJSON_NULL:    RedisModule_ReplyWithNull(ctx); break;
        case cJSON_True:    RedisModule_ReplyWithStringBuffer(ctx, "true", 4); break;
        case cJSON_False:   RedisModule_ReplyWithStringBuffer(ctx, "false", 5); break;
        case cJSON_Number: {
            if (v->valuedouble == (double)v->valueint) RedisModule_ReplyWithLongLong(ctx, (long long)v->valueint);
            else { char buf[64]; snprintf(buf,sizeof(buf),"%.15g",v->valuedouble); RedisModule_ReplyWithStringBuffer(ctx, buf, strlen(buf)); }
            break;
        }
        case cJSON_String:  RedisModule_ReplyWithStringBuffer(ctx, v->valuestring, strlen(v->valuestring)); break;
        case cJSON_Array: {
            RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN); int n=0;
            cJSON *child; cJSON_ArrayForEach(child, v) { resp_reply_value(ctx, child); n++; }
            RedisModule_ReplySetArrayLength(ctx, n); break;
        }
        case cJSON_Object: {
            RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN); int n=0;
            cJSON *child; cJSON_ArrayForEach(child, v) {
                RedisModule_ReplyWithStringBuffer(ctx, child->string, strlen(child->string)); n++;
                resp_reply_value(ctx, child); n++;
            }
            RedisModule_ReplySetArrayLength(ctx, n); break;
        }
    }
}

static int cmd_json_resp(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_read(ctx, argv[1], 0); if (!key) return REDISMODULE_OK;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    ReJSONDoc *doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key);
    if (!doc || !doc->root) { RedisModule_CloseKey(key); RedisModule_ReplyWithNull(ctx); return REDISMODULE_OK; }
    const char *path_str = (argc > 2) ? RedisModule_StringPtrLen(argv[2], NULL) : ".";
    PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
    if (pctx.result != PATH_OK) { RedisModule_ReplyWithNull(ctx); }
    else { resp_reply_value(ctx, pctx.node); }
    path_ctx_cleanup(&pctx); RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

// =============================================================================
// JSON.MERGE <key> <path> <json> — 递归对象合并
// =============================================================================
static void merge_into(IValue *target, IValue *patch) {
    if (!target || !patch) return;
    if (!cJSON_IsObject(target) || !cJSON_IsObject(patch)) {
        if (target->child) { cJSON *child = target->child; while (child) { cJSON *next = child->next; cJSON_DetachItemViaPointer(target, child); cJSON_Delete(child); child = next; } }
        if (patch->child) { cJSON *child = patch->child; while (child) { cJSON *next = child->next; cJSON_DetachItemViaPointer(patch, child); cJSON_AddItemToObject(target, child->string ? child->string : "", child); child = next; } }
        return;
    }
    cJSON *patch_child = patch->child;
    while (patch_child) {
        cJSON *next = patch_child->next;
        if (cJSON_IsNull(patch_child)) { if (patch_child->string) cJSON_DeleteItemFromObject(target, patch_child->string); }
        else {
            cJSON *existing = patch_child->string ? cJSON_GetObjectItemCaseSensitive(target, patch_child->string) : NULL;
            if (existing && cJSON_IsObject(existing) && cJSON_IsObject(patch_child)) { merge_into(existing, patch_child); }
            else { if (existing && patch_child->string) cJSON_DeleteItemFromObject(target, patch_child->string); if (patch_child->string) { cJSON *dup = cJSON_Duplicate(patch_child, 1); cJSON_AddItemToObject(target, patch_child->string, dup); } }
        }
        patch_child = next;
    }
}

static int cmd_json_merge(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) { RedisModule_WrongArity(ctx); return REDISMODULE_OK; }
    RedisModuleKey *key = open_key_write(ctx, argv[1]); if (!key) return REDISMODULE_OK;
    IValue *patch = cJSON_Parse(RedisModule_StringPtrLen(argv[3], NULL));
    if (!patch) { RedisModule_CloseKey(key); RedisModule_ReplyWithError(ctx, "ERR failed to parse JSON"); return REDISMODULE_OK; }
    ReJSONDoc *doc = NULL;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) { doc = rejson_doc_new(NULL); RedisModule_ModuleTypeSetValue(key, ReJSONC_Type, doc); }
    else { doc = (ReJSONDoc *)RedisModule_ModuleTypeGetValue(key); if (!doc) { RedisModule_CloseKey(key); cJSON_Delete(patch); return REDISMODULE_OK; } }
    const char *path_str = RedisModule_StringPtrLen(argv[2], NULL);
    if (strcmp(path_str, ".") == 0 || strcmp(path_str, "$") == 0) {
        if (!doc->root) { doc->root = patch; } else { merge_into(doc->root, patch); cJSON_Delete(patch); }
    } else {
        PathCtx pctx; path_resolve(&pctx, doc->root, path_str);
        if (pctx.result == PATH_OK && pctx.node) {
            if (cJSON_IsObject(pctx.node) && cJSON_IsObject(patch)) { merge_into(pctx.node, patch); cJSON_Delete(patch); }
            else { cJSON *dup = cJSON_Duplicate(patch, 1); if (doc->root) cJSON_Delete(doc->root); doc->root = dup; cJSON_Delete(patch); }
        } else { cJSON_Delete(patch); }
        path_ctx_cleanup(&pctx);
    }
    RedisModule_CloseKey(key); RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

// =============================================================================
// RedisModule_OnLoad — 模块注册入口
// 注册 ReJSON-c 模块类型 + 全部 21 个 JSON.* 命令
// =============================================================================
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "ReJSON-c", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) return REDISMODULE_ERR;

    cJSON_Hooks hooks; hooks.malloc_fn = RedisModule_Alloc; hooks.free_fn = RedisModule_Free;
    cJSON_InitHooks(&hooks);

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = rejson_doc_rdb_load, .rdb_save = rejson_doc_rdb_save,
        .free = rejson_doc_free, .mem_usage = rejson_doc_mem_usage,
    };
    ReJSONC_Type = RedisModule_CreateDataType(ctx, REJSON_C_TYPE_NAME, REJSON_C_TYPE_VERSION, &tm);
    if (ReJSONC_Type == NULL) { RedisModule_Log(ctx, "warning", "FAIL: CreateDataType"); return REDISMODULE_ERR; }

    #define REG_CMD(name, func, flags, first, last, step) do { \
        if (RedisModule_CreateCommand(ctx, name, func, flags, first, last, step) == REDISMODULE_ERR) { \
            RedisModule_Log(ctx, "warning", "FAIL: RedisModule_CreateCommand %s", name); return REDISMODULE_ERR; } \
    } while(0)

    REG_CMD("JSON.SET",      cmd_json_set,     "write deny-oom", 1, 1, 1);
    REG_CMD("JSON.GET",      cmd_json_get,     "readonly",       1, 1, 1);
    REG_CMD("JSON.TYPE",     cmd_json_type,    "readonly",       1, 1, 1);
    REG_CMD("JSON.DEL",      cmd_json_del,     "write",          1, 1, 1);
    REG_CMD("JSON.FORGET",   cmd_json_del,     "write",          1, 1, 1);
    REG_CMD("JSON.OBJLEN",   cmd_json_objlen,  "readonly",       1, 1, 1);
    REG_CMD("JSON.ARRLEN",   cmd_json_arrlen,  "readonly",       1, 1, 1);
    REG_CMD("JSON.STRLEN",   cmd_json_strlen,  "readonly",       1, 1, 1);
    REG_CMD("JSON.OBJKEYS",  cmd_json_objkeys, "readonly",       1, 1, 1);
    REG_CMD("JSON.NUMINCRBY",cmd_json_numincrby,"write",         1, 1, 1);
    REG_CMD("JSON.NUMMULTBY",cmd_json_nummultby,"write",         1, 1, 1);
    REG_CMD("JSON.NUMPOWBY", cmd_json_numpowby, "write",         1, 1, 1);
    REG_CMD("JSON.ARRAPPEND",cmd_json_arrappend,"write deny-oom",1, 1, 1);
    REG_CMD("JSON.ARRINSERT",cmd_json_arrinsert,"write deny-oom",1, 1, 1);
    REG_CMD("JSON.ARRPOP",   cmd_json_arrpop,  "write",          1, 1, 1);
    REG_CMD("JSON.ARRTRIM",  cmd_json_arrtrim,  "write",         1, 1, 1);
    REG_CMD("JSON.ARRINDEX", cmd_json_arrindex, "readonly",      1, 1, 1);
    REG_CMD("JSON.CLEAR",    cmd_json_clear,   "write",          1, 1, 1);
    REG_CMD("JSON.TOGGLE",   cmd_json_toggle,  "write",          1, 1, 1);
    REG_CMD("JSON.STRAPPEND",cmd_json_strappend,"write deny-oom",1, 1, 1);
    REG_CMD("JSON.DEBUG",    cmd_json_debug,   "readonly",       1, 1, 1);
    REG_CMD("JSON.MGET",     cmd_json_mget,    "readonly",       1, -1, 1);
    REG_CMD("JSON.MSET",     cmd_json_mset,    "write deny-oom", 1, -1, 1);
    REG_CMD("JSON.RESP",     cmd_json_resp,    "readonly",       1, 1, 1);
    REG_CMD("JSON.MERGE",    cmd_json_merge,   "write deny-oom", 1, 1, 1);

    RedisModule_Log(ctx, "notice", "All commands registered OK");
    return REDISMODULE_OK;
}
