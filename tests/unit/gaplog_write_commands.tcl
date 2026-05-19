# Gaplog 写命令测试
#
# 测试所有写命令是否能正确记录 key 和 subkey 到 gaplog
#
# 测试策略：
# 1. Slave 同步 Master
# 2. Slave 断开并独立执行写命令
# 3. Slave 重新连接
# 4. 验证 gaplog 记录的 key 是否正确

proc get_info_property {r section line property} {
    set str [$r info $section]
    if {[regexp ".*${line}:\[^\r\n\]*${property}=(\[^,\r\n\]*).*" $str match submatch]} {
        set submatch
    }
}

proc get_gaplog_entries {client} {
    set info [$client INFO gtid]
    foreach line [split $info "\r\n"] {
        if {[string match "gtid_gaplog_entries:*" $line]} {
            return [string range $line 20 end]
        }
    }
    return 0
}

# 从 GTID seq 中提取 slave 独立写入的 uuid
proc get_slave_gtid_uuid {client} {
    set seq [$client GTIDX seq gtid.set]
    set parts [split $seq ","]
    if {[llength $parts] >= 2} {
        set uuid_gno [lindex $parts 1]
        set uuid [lindex [split $uuid_gno ":"] 0]
        return $uuid
    } elseif {[llength $parts] == 1} {
        set uuid_gno [lindex $parts 0]
        set uuid [lindex [split $uuid_gno ":"] 0]
        return $uuid
    }
    return ""
}

# 断言 a >= b
proc assert_morethan {a b {msg ""}} {
    if {$a < $b} {
        puts "Assertion failed: $msg (expected >= $b, got $a)"
        exit 1
    }
}

# =====================================================
# String 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-001: String commands - SET/GETEX/GETDEL/APPEND/SETRANGE/INCR/DECR" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 以下每个命令产生 1 个 GTID，共 15 个
            # SET (1)
            $S set s_str_key1 s_val1
            # SETEX (1)
            $S setex s_str_key2 3600 s_val2
            # PSETEX (1)
            $S psetex s_str_key3 3600000 s_val3
            # SETNX (1) - 成功时产生 GTID
            $S setnx s_str_key4 s_val4
            # SET (1) + APPEND (1)
            $S set s_str_key5 "hello"
            $S append s_str_key5 " world"
            # SET (1) + SETRANGE (1)
            $S set s_str_key6 "hello world"
            $S setrange s_str_key6 6 "Redis"
            # SET (1) + INCR (1) + DECR (1) + INCRBY (1) + DECRBY (1) + INCRBYFLOAT (1)
            $S set s_counter 10
            $S incr s_counter
            $S decr s_counter
            $S incrby s_counter 5
            $S decrby s_counter 3
            $S incrbyfloat s_counter 2.5
            # GETSET (1)
            $S getset s_getset_key new_val

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：15 个写命令，每个产生 1 个 GTID
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 15 "Expected exactly 15 gaplog entries"

            # 验证 gaplog 记录的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_str_key1*" $result
            assert_match "*s_str_key2*" $result
            assert_match "*s_str_key3*" $result
            assert_match "*s_str_key4*" $result
            assert_match "*s_str_key5*" $result
            assert_match "*s_str_key6*" $result
            assert_match "*s_counter*" $result
            assert_match "*s_getset_key*" $result
        }
    }
}

# =====================================================
# List 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-002: List commands - LPUSH/RPUSH/LPOP/RPOP/LSET/LTRIM/LREM" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 以下每个命令产生 1 个 GTID，共 12 个
            # LPUSH (1) + RPUSH (1)
            $S lpush s_list_key1 a b c
            $S rpush s_list_key1 d e f
            # LPUSHX (1) + RPUSHX (1)
            $S lpushx s_list_key1 x
            $S rpushx s_list_key1 y
            # LPOP (1) + RPOP (1)
            $S lpop s_list_key1
            $S rpop s_list_key1
            # LSET (1)
            $S lset s_list_key1 0 new_val
            # LTRIM (1)
            $S ltrim s_list_key1 0 2
            # LREM (1)
            $S lrem s_list_key1 1 a
            # LINSERT (1)
            $S linsert s_list_key1 before b inserted
            # RPOPLPUSH (1)
            $S rpoplpush s_list_key1 s_list_key2
            # LMOVE (1)
            $S lmove s_list_key1 s_list_key2 LEFT RIGHT

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：12 个写命令
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 12 "Expected exactly 12 gaplog entries"

            # 验证 gaplog 记录的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            # s_list_key1 是主要操作的 key
            assert_match "*s_list_key1*" $result
        }
    }
}

# =====================================================
# Set 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-003: Set commands - SADD/SREM/SMOVE/SPOP/SINTERSTORE/SUNIONSTORE/SDIFFSTORE" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 以下每个命令产生 1 个 GTID，共 8 个
            # SADD (1)
            $S sadd s_set_key1 a b c d e
            # SREM (1)
            $S srem s_set_key1 a b
            # SMOVE (1)
            $S smove s_set_key1 s_set_key2 c
            # SPOP (1)
            $S spop s_set_key1
            # SADD (1)
            $S sadd s_set_key3 x y z
            # SINTERSTORE (1)
            $S sinterstore s_set_result s_set_key1 s_set_key3
            # SUNIONSTORE (1)
            $S sunionstore s_set_union s_set_key1 s_set_key3
            # SDIFFSTORE (1)
            $S sdiffstore s_set_diff s_set_key1 s_set_key3

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：7 个写命令
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 7 "Expected exactly 7 gaplog entries"

            # 验证 gaplog 记录的 key 和 subkey (member)
            # Set 命令会记录 key\0member 格式
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_set_key1*" $result
            # 验证 member 被记录
            assert_match "*a*" $result
            assert_match "*b*" $result
        }
    }
}

# =====================================================
# Sorted Set 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-004: Sorted Set commands - ZADD/ZREM/ZINCRBY/ZPOPMIN/ZPOPMAX/ZREMRANGEBY*" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 以下每个命令产生 1 个 GTID，共 15 个
            # ZADD (1)
            $S zadd s_zset_key1 1 a 2 b 3 c 4 d 5 e
            # ZINCRBY (1)
            $S zincrby s_zset_key1 2.5 a
            # ZREM (1)
            $S zrem s_zset_key1 a
            # ZREMRANGEBYSCORE (1)
            $S zremrangebyscore s_zset_key1 2 3
            # ZREMRANGEBYRANK (1)
            $S zremrangebyrank s_zset_key1 0 0
            # ZADD (1) + ZREMRANGEBYLEX (1)
            $S zadd s_zset_key2 0 a 0 b 0 c 0 d
            $S zremrangebylex s_zset_key2 (b (c
            # ZPOPMIN (1) + ZPOPMAX (1)
            $S zpopmin s_zset_key1
            $S zpopmax s_zset_key1
            # ZADD (1) + ZUNIONSTORE (1) + ZINTERSTORE (1) + ZDIFFSTORE (1)
            $S zadd s_zset_key3 1 x 2 y 3 z
            $S zunionstore s_zset_union 2 s_zset_key1 s_zset_key3
            $S zinterstore s_zset_inter 2 s_zset_key1 s_zset_key3
            $S zdiffstore s_zset_diff 2 s_zset_key1 s_zset_key3
            # ZADD (1) + ZRANGESTORE (1)
            $S zadd s_zset_key4 1 a 2 b 3 c
            $S zrangestore s_zset_range s_zset_key4 0 -1

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：11 个写命令
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 11 "Expected exactly 11 gaplog entries"

            # 验证 gaplog 记录的 key 和 subkey (member)
            # Sorted Set 命令会记录 key\0member 格式
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_zset_key1*" $result
            assert_match "*s_zset_key2*" $result
            assert_match "*s_zset_union*" $result
            # 验证 member 被记录
            assert_match "*a*" $result
            assert_match "*b*" $result
        }
    }
}

# =====================================================
# Hash 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-005: Hash commands - HSET/HSETNX/HDEL/HINCRBY/HINCRBYFLOAT" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 以下每个命令产生 1 个 GTID，共 5 个
            # HSET (1)
            $S hset s_hash_key1 field1 val1 field2 val2 field3 val3
            # HSETNX (1)
            $S hsetnx s_hash_key1 field4 val4
            # HINCRBY (1)
            $S hincrby s_hash_key1 counter 1
            # HINCRBYFLOAT (1)
            $S hincrbyfloat s_hash_key1 counter 2.5
            # HDEL (1)
            $S hdel s_hash_key1 field1

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：5 个写命令
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 5 "Expected exactly 5 gaplog entries"

            # 验证 gaplog 记录的 key 和 subkey (field)
            # Hash 命令会记录 key\0field 格式
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_hash_key1*" $result
            # 验证 field 被记录（field1, field2, field3, field4, counter）
            assert_match "*field1*" $result
            assert_match "*field2*" $result
            assert_match "*counter*" $result
        }
    }
}

# =====================================================
# Bitmap 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-006: Bitmap commands - SETBIT/BITFIELD/BITOP" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # SETBIT
            $S setbit s_bitmap_key 0 1
            $S setbit s_bitmap_key 1 1
            # BITFIELD
            $S bitfield s_bitmap_key2 set u8 0 100 set u8 8 200
            # BITOP
            $S set s_bit1 "\xff"
            $S set s_bit2 "\x0f"
            $S bitop and s_bit_result s_bit1 s_bit2

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：6 个写命令
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 6 "Expected exactly 6 gaplog entries"

            # 验证 gaplog 记录的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_bitmap_key*" $result
        }
    }
}

# =====================================================
# Keyspace 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-007: Keyspace commands - DEL/UNLINK/RENAME/RENAMENX/COPY/MOVE/EXPIRE/PEXPIRE" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # DEL
            $S set s_del_key val
            $S del s_del_key
            # UNLINK
            $S set s_unlink_key val
            $S unlink s_unlink_key
            # RENAME
            $S set s_rename_src val
            $S rename s_rename_src s_rename_dst
            # RENAMENX
            $S set s_renamenx_src val
            $S set s_renamenx_dst existing
            $S renamenx s_renamenx_src s_renamenx_dst
            # COPY
            $S set s_copy_src val
            $S copy s_copy_src s_copy_dst
            # EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT
            $S set s_expire_key val
            $S expire s_expire_key 3600
            $S pexpire s_expire_key 3600000
            $S expireat s_expire_key [expr [clock seconds] + 3600]
            $S pexpireat s_expire_key [expr [clock milliseconds] + 3600000]
            # PERSIST
            $S persist s_expire_key

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：16 个写命令
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 16 "Expected exactly 16 gaplog entries"

            # 验证 gaplog 记录的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_del_key*" $result
            assert_match "*s_rename_src*" $result
            assert_match "*s_copy_src*" $result
            assert_match "*s_expire_key*" $result
        }
    }
}

# =====================================================
# HyperLogLog 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-008: HyperLogLog commands - PFADD/PFMERGE" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # PFADD
            $S pfadd s_hll_key1 a b c d e
            # PFMERGE
            $S pfadd s_hll_key2 x y z
            $S pfmerge s_hll_result s_hll_key1 s_hll_key2

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：3 个写命令 (PFADD + PFADD + PFMERGE)
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 3 "Expected exactly 3 gaplog entries"

            # 验证 gaplog 记录的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_hll_key1*" $result
            assert_match "*s_hll_result*" $result
        }
    }
}

# =====================================================
# Geo 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-009: Geo commands - GEOADD/GEOSEARCHSTORE" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # GEOADD
            $S geoadd s_geo_key 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"
            # GEOSEARCHSTORE - STORE 选项不能与 WITHCOORD/WITHDIST 一起使用
            $S geosearchstore s_geo_result s_geo_key frommember Palermo byradius 200 km

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：2 个写命令 (GEOADD + GEOSEARCHSTORE)
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 2 "Expected exactly 2 gaplog entries"

            # 验证 gaplog 记录的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_geo_key*" $result
            assert_match "*s_geo_result*" $result
        }
    }
}

# =====================================================
# Stream 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-010: Stream commands - XADD/XTRIM/XDEL" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # XADD
            set id1 [$S xadd s_stream_key * field1 val1]
            set id2 [$S xadd s_stream_key * field2 val2]
            set id3 [$S xadd s_stream_key * field3 val3]
            # XTRIM
            $S xtrim s_stream_key maxlen 2
            # XDEL
            $S xdel s_stream_key $id3

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：5 个写命令 (XADD + XADD + XADD + XTRIM + XDEL)
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 5 "Expected exactly 5 gaplog entries"

            # 验证 gaplog 记录的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            assert_match "*s_stream_key*" $result
        }
    }
}

# =====================================================
# MSET/MSETNX 命令测试（多 key）
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-WRITE-011: Multi-key commands - MSET/MSETNX" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # MSET
            $S mset s_mkey1 val1 s_mkey2 val2 s_mkey3 val3
            # MSETNX
            $S msetnx s_mkey4 val4 s_mkey5 val5

            # 获取 uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 重连
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]
            $S replicaof $M_host $M_port
            wait_for_sync $S
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }
            after 100

            # 验证 gaplog 条目数：2 个写命令 (MSET + MSETNX)
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 2 "Expected exactly 2 gaplog entries"

            # 验证 gaplog 记录的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            # MSET 应该记录所有 key
            assert_match "*s_mkey1*" $result
            assert_match "*s_mkey2*" $result
            assert_match "*s_mkey3*" $result
        }
    }
}
