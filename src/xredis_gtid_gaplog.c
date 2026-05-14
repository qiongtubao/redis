/* Copyright (c) 2026, ctrip.com
 * All rights reserved.
 *
 * 高性能GTID Gaplog实现 - 环形缓冲区 + 对象池版本
 * 优化点：
 * 1. 使用环形缓冲区，插入和删除都是O(1)，避免数组移动
 * 2. 缓存上次使用的uuid_entry，避免重复dictFind
 * 3. gtidGnoEntry对象池，避免频繁zmalloc/zfree
 * 4. 固定大小keys/subkeys数组，避免每次分配
 */

#include "server.h"
#include "./xredis_gtid_gaplog.h"

/* 前向声明 */
static void gtidGnoEntryDestroy(gtidGnoEntry *entry);
static void gtidGnoEntryRecycle(gtidGaplog *gaplog, gtidGnoEntry *entry);

/* ============================================================================
 * Dict类型定义 - 使用sds作为key
 * ============================================================================ */

static uint64_t sdsDictHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((sds)key));
}

static int sdsDictKeyCompare(void *privdata, const void *key1, const void *key2) {
    DICT_NOTUSED(privdata);
    return sdscmp((sds)key1, (sds)key2) == 0;
}

static void sdsDictKeyDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    sdsfree((sds)val);
}

static void uuidEntryDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    gtidUuidEntry *entry = (gtidUuidEntry *)val;
    if (entry == NULL) return;

    /* 销毁所有gno条目 */
    for (size_t i = 0; i < entry->count; i++) {
        size_t idx = (entry->head + i) % entry->capacity;
        gtidGnoEntryDestroy(entry->entries[idx]);
    }
    zfree(entry->entries);
    zfree(entry);
}

static dictType uuidDictType = {
    sdsDictHash,           /* hash function */
    NULL,                  /* key dup */
    NULL,                  /* val dup */
    sdsDictKeyCompare,     /* key compare */
    sdsDictKeyDestructor,  /* key destructor */
    uuidEntryDestructor    /* val destructor */
};

/* ============================================================================
 * gtidGnoEntry 对象池
 * ============================================================================ */

#define GAPLOG_ENTRY_POOL_SIZE 1024  /* 对象池大小 */
#define GAPLOG_MAX_KEYS_PER_ENTRY 16 /* 每个entry最多保存的key数 */

/**
 * gtidGnoEntry 扩展结构 - 包含固定大小的keys/subkeys数组
 * 避免每次动态分配数组
 */
typedef struct gtidGnoEntryExt {
    gtidGnoEntry base;                    /* 基础结构 */
    robj *keys_buf[GAPLOG_MAX_KEYS_PER_ENTRY];   /* 固定大小keys数组 */
    robj *subkeys_buf[GAPLOG_MAX_KEYS_PER_ENTRY]; /* 固定大小subkeys数组 */
    struct gtidGnoEntryExt *next;         /* 对象池链表指针 */
} gtidGnoEntryExt;

/**
 * 从对象池获取一个entry，如果没有则创建
 */
static gtidGnoEntry *gtidGnoEntryPoolGet(gtidGaplog *gaplog) {
    gtidGnoEntryExt *ext = NULL;

    if (gaplog->entry_pool != NULL) {
        /* 从池中获取 */
        ext = gaplog->entry_pool;
        gaplog->entry_pool = ext->next;
        gaplog->entry_pool_count--;
    } else {
        /* 创建新的 */
        ext = zmalloc(sizeof(gtidGnoEntryExt));
        if (ext == NULL) return NULL;
    }

    /* 初始化 */
    memset(ext, 0, sizeof(gtidGnoEntryExt));
    ext->base.keys = ext->keys_buf;
    ext->base.subkeys = ext->subkeys_buf;

    return &ext->base;
}

/**
 * 将entry放回对象池
 */
static void gtidGnoEntryRecycle(gtidGaplog *gaplog, gtidGnoEntry *entry) {
    if (entry == NULL) return;

    /* 先减少引用计数 */
    if (entry->keys) {
        for (size_t i = 0; i < entry->key_count; i++) {
            if (entry->keys[i]) decrRefCount(entry->keys[i]);
        }
    }
    if (entry->subkeys) {
        for (size_t i = 0; i < entry->key_count; i++) {
            if (entry->subkeys[i]) decrRefCount(entry->subkeys[i]);
        }
    }

    /* 获取扩展结构 */
    gtidGnoEntryExt *ext = (gtidGnoEntryExt *)entry;

    /* 如果池未满，放回池中 */
    if (gaplog->entry_pool_count < GAPLOG_ENTRY_POOL_SIZE) {
        ext->next = gaplog->entry_pool;
        gaplog->entry_pool = ext;
        gaplog->entry_pool_count++;
    } else {
        /* 池已满，直接释放 */
        zfree(ext);
    }
}

/**
 * 初始化entry（设置值）
 */
static void gtidGnoEntryInit(gtidGnoEntry *entry, gno_t gno,
                             robj **keys, robj **subkeys, size_t key_count) {
    entry->gno = gno;
    entry->key_count = key_count > GAPLOG_MAX_KEYS_PER_ENTRY ? GAPLOG_MAX_KEYS_PER_ENTRY : key_count;
    entry->timestamp = time(NULL);

    /* 复制robj指针并增加引用计数 */
    for (size_t i = 0; i < entry->key_count; i++) {
        entry->keys[i] = keys[i];
        if (keys[i]) incrRefCount(keys[i]);
    }
    if (subkeys) {
        for (size_t i = 0; i < entry->key_count; i++) {
            entry->subkeys[i] = subkeys[i];
            if (subkeys[i]) incrRefCount(subkeys[i]);
        }
    } else {
        for (size_t i = 0; i < entry->key_count; i++) {
            entry->subkeys[i] = NULL;
        }
    }
}

static void gtidGnoEntryDestroy(gtidGnoEntry *entry) {
    if (entry == NULL) return;
    /* 减少引用计数 */
    if (entry->keys) {
        for (size_t i = 0; i < entry->key_count; i++) {
            if (entry->keys[i]) decrRefCount(entry->keys[i]);
        }
    }
    if (entry->subkeys) {
        for (size_t i = 0; i < entry->key_count; i++) {
            if (entry->subkeys[i]) decrRefCount(entry->subkeys[i]);
        }
    }
    /* 释放扩展结构（不使用对象池时） */
    gtidGnoEntryExt *ext = (gtidGnoEntryExt *)entry;
    zfree(ext);
}

/* ============================================================================
 * gtidUuidEntry Functions - 环形缓冲区版本
 * ============================================================================ */

static gtidUuidEntry *gtidUuidEntryCreate(size_t capacity) {
    gtidUuidEntry *entry = zmalloc(sizeof(gtidUuidEntry));
    if (entry == NULL) return NULL;

    entry->capacity = capacity;
    entry->entries = zmalloc(sizeof(gtidGnoEntry*) * capacity);
    if (entry->entries == NULL) {
        zfree(entry);
        return NULL;
    }
    memset(entry->entries, 0, sizeof(gtidGnoEntry*) * capacity);

    entry->head = 0;
    entry->tail = 0;
    entry->count = 0;
    entry->min_gno = 0;
    entry->max_gno = 0;

    return entry;
}

/**
 * 环形缓冲区插入 - O(1)
 * @return GAPLOG_OK成功，GAPLOG_ERR表示删除了旧条目
 */
static int gtidUuidEntryPush(gtidGaplog *gaplog, gtidUuidEntry *uuid_entry, gtidGnoEntry *gno_entry) {
    int deleted_old = 0;

    /* 如果缓冲区已满，删除最老条目（放回对象池） */
    if (uuid_entry->count >= uuid_entry->capacity) {
        gtidGnoEntryRecycle(gaplog, uuid_entry->entries[uuid_entry->head]);
        uuid_entry->head = (uuid_entry->head + 1) % uuid_entry->capacity;
        uuid_entry->count--;
        deleted_old = 1;
    }

    /* 在tail位置插入新条目 */
    uuid_entry->entries[uuid_entry->tail] = gno_entry;
    uuid_entry->tail = (uuid_entry->tail + 1) % uuid_entry->capacity;
    uuid_entry->count++;

    /* 更新min/max */
    if (uuid_entry->count == 1) {
        uuid_entry->min_gno = gno_entry->gno;
        uuid_entry->max_gno = gno_entry->gno;
    } else {
        if (gno_entry->gno < uuid_entry->min_gno) uuid_entry->min_gno = gno_entry->gno;
        if (gno_entry->gno > uuid_entry->max_gno) uuid_entry->max_gno = gno_entry->gno;
    }

    return deleted_old ? GAPLOG_ERR : GAPLOG_OK;
}

/**
 * 环形缓冲区查找
 */
static gtidGnoEntry *gtidUuidEntryFind(gtidUuidEntry *uuid_entry, gno_t gno) {
    if (uuid_entry->count == 0) return NULL;

    /* 快速范围检查 */
    if (gno < uuid_entry->min_gno || gno > uuid_entry->max_gno) return NULL;

    /* 从最新条目向前遍历（gno通常递增） */
    for (size_t i = 0; i < uuid_entry->count; i++) {
        size_t idx = (uuid_entry->tail + uuid_entry->capacity - 1 - i) % uuid_entry->capacity;
        if (uuid_entry->entries[idx]->gno == gno) {
            return uuid_entry->entries[idx];
        }
    }
    return NULL;
}

/**
 * 环形缓冲区清理
 */
static size_t gtidUuidEntryTrimByWatermark(gtidGaplog *gaplog, gtidUuidEntry *uuid_entry, gno_t watermark) {
    size_t trimmed = 0;

    while (uuid_entry->count > 0) {
        gtidGnoEntry *oldest = uuid_entry->entries[uuid_entry->head];
        if (oldest->gno >= watermark) break;

        gtidGnoEntryRecycle(gaplog, oldest);
        uuid_entry->entries[uuid_entry->head] = NULL;
        uuid_entry->head = (uuid_entry->head + 1) % uuid_entry->capacity;
        uuid_entry->count--;
        trimmed++;
    }

    if (uuid_entry->count > 0) {
        uuid_entry->min_gno = uuid_entry->entries[uuid_entry->head]->gno;
    } else {
        uuid_entry->min_gno = 0;
        uuid_entry->max_gno = 0;
    }

    return trimmed;
}

/* ============================================================================
 * Core API Implementation
 * ============================================================================ */

gtidGaplog *gtidGaplogCreate(size_t max_gap) {
    gtidGaplog *gaplog = zmalloc(sizeof(gtidGaplog));
    if (gaplog == NULL) return NULL;

    gaplog->uuid_index = dictCreate(&uuidDictType, NULL);
    if (gaplog->uuid_index == NULL) {
        zfree(gaplog);
        return NULL;
    }

    gaplog->max_gap = max_gap > 0 ? max_gap : GAPLOG_DEFAULT_MAX_GAP;
    gaplog->used_memory = sizeof(gtidGaplog);
    gaplog->hit_count = 0;
    gaplog->miss_count = 0;
    gaplog->total_entries = 0;

    /* 对象池初始化 */
    gaplog->entry_pool = NULL;
    gaplog->entry_pool_count = 0;

    /* 缓存初始化 */
    gaplog->cached_uuid = NULL;
    gaplog->cached_uuid_len = 0;
    gaplog->cached_entry = NULL;

    return gaplog;
}

void gtidGaplogDestroy(gtidGaplog *gaplog) {
    if (gaplog == NULL) return;

    /* 释放对象池 */
    while (gaplog->entry_pool != NULL) {
        gtidGnoEntryExt *ext = gaplog->entry_pool;
        gaplog->entry_pool = ext->next;
        zfree(ext);
    }

    dictRelease(gaplog->uuid_index);
    zfree(gaplog);
}

int gtidGaplogAppend(gtidGaplog *gaplog, sds uuid, gno_t gno,
                     robj **keys, robj **subkeys, size_t key_count) {
    if (gaplog == NULL || uuid == NULL || (keys == NULL && key_count > 0)) {
        return GAPLOG_ERR_INVALID_PARAM;
    }

    gtidUuidEntry *uuid_entry = NULL;

    /* 优化1：检查缓存是否命中（大多数情况下uuid相同） */
    size_t uuid_len = sdslen(uuid);
    if (gaplog->cached_entry != NULL &&
        gaplog->cached_uuid_len == uuid_len &&
        memcmp(gaplog->cached_uuid, uuid, uuid_len) == 0) {
        /* 缓存命中 */
        uuid_entry = gaplog->cached_entry;
    } else {
        /* 缓存未命中，需要查找dict */
        dictEntry *de = dictFind(gaplog->uuid_index, uuid);
        if (de == NULL) {
            /* 创建新的UUID条目 */
            uuid_entry = gtidUuidEntryCreate(gaplog->max_gap);
            if (uuid_entry == NULL) return GAPLOG_ERR_MEMORY;

            sds uuid_key = sdsdup(uuid);
            if (uuid_key == NULL) {
                zfree(uuid_entry->entries);
                zfree(uuid_entry);
                return GAPLOG_ERR_MEMORY;
            }

            if (dictAdd(gaplog->uuid_index, uuid_key, uuid_entry) != DICT_OK) {
                sdsfree(uuid_key);
                zfree(uuid_entry->entries);
                zfree(uuid_entry);
                return GAPLOG_ERR_MEMORY;
            }
        } else {
            uuid_entry = dictGetVal(de);
        }

        /* 更新缓存 */
        gaplog->cached_uuid = uuid;
        gaplog->cached_uuid_len = uuid_len;
        gaplog->cached_entry = uuid_entry;
    }

    /* 优化2：从对象池获取entry，避免频繁zmalloc */
    gtidGnoEntry *gno_entry = gtidGnoEntryPoolGet(gaplog);
    if (gno_entry == NULL) return GAPLOG_ERR_MEMORY;

    /* 初始化entry */
    gtidGnoEntryInit(gno_entry, gno, keys, subkeys, key_count);

    /* 插入环形缓冲区 - O(1) */
    int result = gtidUuidEntryPush(gaplog, uuid_entry, gno_entry);
    if (result == GAPLOG_ERR_MEMORY) {
        gtidGnoEntryRecycle(gaplog, gno_entry);
        return GAPLOG_ERR_MEMORY;
    }

    /* 更新计数 */
    if (result == GAPLOG_OK) {
        gaplog->total_entries++;
    }

    return GAPLOG_OK;
}

gtidGnoEntry *gtidGaplogGet(gtidGaplog *gaplog, sds uuid, gno_t gno) {
    if (gaplog == NULL || uuid == NULL) return NULL;

    gtidUuidEntry *uuid_entry = NULL;

    /* 检查缓存 */
    size_t uuid_len = sdslen(uuid);
    if (gaplog->cached_entry != NULL &&
        gaplog->cached_uuid_len == uuid_len &&
        memcmp(gaplog->cached_uuid, uuid, uuid_len) == 0) {
        uuid_entry = gaplog->cached_entry;
    } else {
        dictEntry *de = dictFind(gaplog->uuid_index, uuid);
        if (de == NULL) {
            gaplog->miss_count++;
            return NULL;
        }
        uuid_entry = dictGetVal(de);
    }

    gtidGnoEntry *entry = gtidUuidEntryFind(uuid_entry, gno);
    if (entry == NULL) {
        gaplog->miss_count++;
        return NULL;
    }

    gaplog->hit_count++;
    return entry;
}

size_t gtidGaplogTrim(gtidGaplog *gaplog, sds uuid, gno_t watermark) {
    if (gaplog == NULL) return 0;

    size_t total_trimmed = 0;

    if (uuid != NULL) {
        gtidUuidEntry *uuid_entry = NULL;

        /* 检查缓存 */
        size_t uuid_len = sdslen(uuid);
        if (gaplog->cached_entry != NULL &&
            gaplog->cached_uuid_len == uuid_len &&
            memcmp(gaplog->cached_uuid, uuid, uuid_len) == 0) {
            uuid_entry = gaplog->cached_entry;
        } else {
            dictEntry *de = dictFind(gaplog->uuid_index, uuid);
            if (de == NULL) return 0;
            uuid_entry = dictGetVal(de);
        }

        total_trimmed = gtidUuidEntryTrimByWatermark(gaplog, uuid_entry, watermark);
    } else {
        dictIterator *di = dictGetIterator(gaplog->uuid_index);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            gtidUuidEntry *uuid_entry = dictGetVal(de);
            total_trimmed += gtidUuidEntryTrimByWatermark(gaplog, uuid_entry, watermark);
        }
        dictReleaseIterator(di);
    }

    gaplog->total_entries -= total_trimmed;
    return total_trimmed;
}

void gtidGaplogClear(gtidGaplog *gaplog) {
    if (gaplog == NULL) return;
    dictEmpty(gaplog->uuid_index, NULL);
    gaplog->total_entries = 0;
    gaplog->used_memory = sizeof(gtidGaplog);

    /* 清空缓存 */
    gaplog->cached_uuid = NULL;
    gaplog->cached_uuid_len = 0;
    gaplog->cached_entry = NULL;
}

void gtidGaplogGetStat(gtidGaplog *gaplog, gtidGaplogStat *stat) {
    if (gaplog == NULL || stat == NULL) return;

    stat->total_entries = gaplog->total_entries;
    stat->uuid_count = dictSize(gaplog->uuid_index);
    stat->hit_count = gaplog->hit_count;
    stat->miss_count = gaplog->miss_count;
    stat->total_memory = gaplog->used_memory;
}

gtidKeyMapping **gtidGaplogList(gtidGaplog *gaplog, sds uuid,
                                 size_t count, size_t *actual_count) {
    if (gaplog == NULL || actual_count == NULL) return NULL;

    if (gaplog->total_entries == 0) {
        *actual_count = 0;
        return NULL;
    }

    size_t max_count = count > 0 ? count : gaplog->total_entries;
    gtidKeyMapping **result = zmalloc(sizeof(gtidKeyMapping*) * max_count);
    if (result == NULL) return NULL;

    size_t result_count = 0;

    if (uuid != NULL) {
        gtidUuidEntry *uuid_entry = NULL;

        /* 检查缓存 */
        size_t uuid_len = sdslen(uuid);
        if (gaplog->cached_entry != NULL &&
            gaplog->cached_uuid_len == uuid_len &&
            memcmp(gaplog->cached_uuid, uuid, uuid_len) == 0) {
            uuid_entry = gaplog->cached_entry;
        } else {
            dictEntry *de = dictFind(gaplog->uuid_index, uuid);
            if (de == NULL) {
                zfree(result);
                *actual_count = 0;
                return NULL;
            }
            uuid_entry = dictGetVal(de);
        }

        sds uuid_sds = uuid;

        for (size_t i = 0; i < uuid_entry->count && result_count < max_count; i++) {
            size_t idx = (uuid_entry->head + i) % uuid_entry->capacity;
            gtidGnoEntry *gno_entry = uuid_entry->entries[idx];
            gtidKeyMapping *mapping = zmalloc(sizeof(gtidKeyMapping));
            if (mapping == NULL) break;
            mapping->uuid = uuid_sds;
            mapping->gno = gno_entry->gno;
            if (gno_entry->keys) {
                mapping->keys = zmalloc(sizeof(sds) * gno_entry->key_count);
                for (size_t j = 0; j < gno_entry->key_count; j++) {
                    mapping->keys[j] = gno_entry->keys[j] ? gno_entry->keys[j]->ptr : NULL;
                }
            } else {
                mapping->keys = NULL;
            }
            if (gno_entry->subkeys) {
                mapping->subkeys = zmalloc(sizeof(sds) * gno_entry->key_count);
                for (size_t j = 0; j < gno_entry->key_count; j++) {
                    mapping->subkeys[j] = gno_entry->subkeys[j] ? gno_entry->subkeys[j]->ptr : NULL;
                }
            } else {
                mapping->subkeys = NULL;
            }
            mapping->key_count = gno_entry->key_count;
            mapping->timestamp = gno_entry->timestamp;

            result[result_count++] = mapping;
        }
    } else {
        dictIterator *di = dictGetIterator(gaplog->uuid_index);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL && result_count < max_count) {
            gtidUuidEntry *uuid_entry = dictGetVal(de);
            sds uuid_sds = dictGetKey(de);

            for (size_t i = 0; i < uuid_entry->count && result_count < max_count; i++) {
                size_t idx = (uuid_entry->head + i) % uuid_entry->capacity;
                gtidGnoEntry *gno_entry = uuid_entry->entries[idx];
                gtidKeyMapping *mapping = zmalloc(sizeof(gtidKeyMapping));
                if (mapping == NULL) break;
                mapping->uuid = uuid_sds;
                mapping->gno = gno_entry->gno;
                if (gno_entry->keys) {
                    mapping->keys = zmalloc(sizeof(sds) * gno_entry->key_count);
                    for (size_t j = 0; j < gno_entry->key_count; j++) {
                        mapping->keys[j] = gno_entry->keys[j] ? gno_entry->keys[j]->ptr : NULL;
                    }
                } else {
                    mapping->keys = NULL;
                }
                if (gno_entry->subkeys) {
                    mapping->subkeys = zmalloc(sizeof(sds) * gno_entry->key_count);
                    for (size_t j = 0; j < gno_entry->key_count; j++) {
                        mapping->subkeys[j] = gno_entry->subkeys[j] ? gno_entry->subkeys[j]->ptr : NULL;
                    }
                } else {
                    mapping->subkeys = NULL;
                }
                mapping->key_count = gno_entry->key_count;
                mapping->timestamp = gno_entry->timestamp;

                result[result_count++] = mapping;
            }
        }
        dictReleaseIterator(di);
    }

    *actual_count = result_count;
    return result;
}

size_t gtidGaplogGetCount(gtidGaplog *gaplog) {
    if (gaplog == NULL) return 0;
    return gaplog->total_entries;
}

void gtidGaplogSetMaxGap(gtidGaplog *gaplog, size_t max_gap) {
    if (gaplog == NULL) return;
    gaplog->max_gap = max_gap > 0 ? max_gap : GAPLOG_DEFAULT_MAX_GAP;
}