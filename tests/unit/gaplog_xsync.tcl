# Gaplog 正确触发场景测试
#
# Gaplog 只在 xsync continue 时填充（不是 xfullresync）
#
# 触发 xcontinue 的条件：
#   1. Slave 和 Master 有共同的 GTID 历史（先同步过）
#   2. Slave 有一些 Master 不承认的 GTID（差距不超过 maxgap）
#
# 正确场景：
#   1. Slave 同步 Master（建立 GTID 关联）
#   2. Master 写入数据（GTID 被记录在 gtid_seq）
#   3. Slave 断开连接
#   4. Slave 独立写入数据（产生自己的 GTID）
#   5. Slave 重新连接同一个 Master
#   6. xsync continue 时，gtid_mlost = slave_gtid - master_cont
#   7. saveGapLogFromGtidSet 从 backlog 解析 slave 独立写入的命令

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

proc get_uuid {client} {
    set info [$client INFO server]
    foreach line [split $info "\r\n"] {
        if {[string match "gtid_uuid:*" $line]} {
            return [string range $line 11 end]
        }
    }
    return ""
}

# 从 GTID seq 中提取 slave 独立写入的 uuid
# 返回格式：uuid
proc get_slave_gtid_uuid {client} {
    set seq [$client GTIDX seq gtid.set]
    # seq 格式为 "uuid1:gno1,uuid2:gno2"
    # 找到 slave 自己的 uuid（通常是第二个 uuid）
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

# 从 gaplog range 结果中解析 key 列表
# 返回格式：list of keys
proc parse_gaplog_keys {gaplog_result} {
    set keys {}
    set lines [split $gaplog_result "\r\n"]
    foreach line $lines {
        # 跳过数组长度行和空行
        if {[string match "*:*" $line] || $line eq ""} {
            continue
        }
        # keyinfo 格式为 "key1\0key2\0key3" 或单个 key
        # 这里简化处理，直接提取 key
        if {[string match "s_*" $line] || [string match "m_*" $line]} {
            lappend keys $line
        }
    }
    return $keys
}

# 验证 gaplog 中是否包含指定的 key
proc gaplog_contains_key {client uuid gno_start gno_end expected_keys} {
    # 查询 gaplog range
    catch {
        set result [$client GTIDX GAPLOG RANGE $uuid $gno_start $gno_end]
    }

    # 解析结果中的 key
    set found_keys {}
    # gaplog range 返回格式：数组，每个元素是 (gno, keyinfo)
    # 简化验证：检查返回结果是否包含预期的 key

    foreach key $expected_keys {
        if {[string match "*$key*" $result]} {
            lappend found_keys $key
        }
    }

    return [list [llength $found_keys] $found_keys $result]
}

# =====================================================
# GAPLOG-XSYNC-001: 正确的 xcontinue 触发场景
# Slave 断开后重新连接同一个 Master
# 验证 gaplog 记录的 key 是否正确
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-XSYNC-001: slave reconnects same master triggers xcontinue" {
            # 1. Slave 同步 Master（建立 GTID 关联）
            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 2. Master 写入数据
            $M set m_key1 m_val1
            $M set m_key2 m_val2
            wait_for_sync $S

            # 3. Slave 断开连接
            $S replicaof no one
            after 100

            # 4. Slave 独立写入数据（产生自己的 GTID）
            $S set s_key1 s_val1
            $S set s_key2 s_val2

            # 获取 Slave 独立写入的 GTID uuid（从 gtid.set 中获取）
            set slave_uuid [get_slave_gtid_uuid $S]

            # 5. Slave 重新连接同一个 Master
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]

            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 等待 xsync continue
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }

            # 6. 验证 gaplog 条目数
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 2

            # 7. 验证 gaplog 记录的 key 是否正确
            # 查询 gaplog range，验证是否包含 s_key1 和 s_key2
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 2]
            # 结果应该包含 s_key1 和 s_key2
            assert_match "*s_key1*" $result
            assert_match "*s_key2*" $result

            # 验证 Master 的数据存在
            assert_equal [$S get m_key1] m_val1
            assert_equal [$S get m_key2] m_val2

            # 验证 Slave 独立写入的数据仍然存在
            assert_equal [$S get s_key1] s_val1
            assert_equal [$S get s_key2] s_val2
        }
    }
}

# =====================================================
# GAPLOG-XSYNC-002: Master 写入后 slave 断开重连
# 测试 master 的 GTID 能否被 gaplog 记录
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
            set MA [srv -2 client]
            set MA_host [srv -2 host]
            set MA_port [srv -2 port]
            set MB [srv -1 client]
            set MB_host [srv -1 host]
            set MB_port [srv -1 port]
            set S [srv 0 client]

            test "GAPLOG-XSYNC-002: master's GTID in gaplog after slave switch" {
                # 1. Slave 同步 MasterA
                $S replicaof $MA_host $MA_port
                wait_for_sync $S

                # 2. MasterA 写入数据（这些 GTID 会被记录在 slave 的 gtid_seq）
                $MA set ma_key1 ma_val1
                $MA set ma_key2 ma_val2
                $MA set ma_key3 ma_val3
                wait_for_sync $S

                # 3. Slave 断开连接
                $S replicaof no one
                after 100

                # 4. MasterA 继续写入数据
                $MA set ma_key4 ma_val4
                $MA set ma_key5 ma_val5

                # 5. Slave 重新连接 MasterA
                set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]

                $S replicaof $MA_host $MA_port
                wait_for_sync $S

                wait_for_condition 50 100 {
                    [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
                } else {
                    fail "xsync xcontinue not detected"
                }

                # 6. 验证 gaplog 为空
                set gaplog_len [get_gaplog_entries $S]
                assert_equal $gaplog_len 0

                # 验证数据一致性
                assert_equal [$S get ma_key1] ma_val1
                assert_equal [$S get ma_key4] ma_val4
                assert_equal [$S get ma_key5] ma_val5
            }
        }
    }
}

# =====================================================
# GAPLOG-XSYNC-003: Slave 独立写入 MULTI/EXEC 事务测试
# MULTI/EXEC 事务生成一个 GTID，包含事务内所有 key
# 验证 gaplog 记录的 key 是否包含所有事务内的 key
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-XSYNC-003: slave independent MULTI/EXEC transaction" {
            # 1. Slave 同步 Master
            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 2. Master 写入数据
            $M set m_key m_val
            wait_for_sync $S

            # 3. Slave 断开并独立写入 MULTI/EXEC
            $S replicaof no one
            after 100

            # Slave 独立写入 MULTI/EXEC 事务
            $S MULTI
            $S set s_multi_key1 s_multi_val1
            $S set s_multi_key2 s_multi_val2
            $S set s_multi_key3 s_multi_val3
            $S EXEC

            # 获取 Slave 独立写入的 GTID uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 4. Slave 重新连接
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]

            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 等待 xsync continue
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }

            after 100

            # 5. 验证 gaplog 条目数：MULTI/EXEC 事务生成 1 个 GTID
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1

            # 6. 验证 gaplog 记录的 key 是否包含所有事务内的 key
            # 查询 gaplog range，验证是否包含所有 3 个 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            puts $result
            # 结果应该包含 s_multi_key1, s_multi_key2, s_multi_key3
            assert_match "*s_multi_key1*" $result
            assert_match "*s_multi_key2*" $result
            assert_match "*s_multi_key3*" $result

            # 验证数据
            assert_equal [$S get m_key] m_val
            assert_equal [$S get s_multi_key1] s_multi_val1
            assert_equal [$S get s_multi_key2] s_multi_val2
            assert_equal [$S get s_multi_key3] s_multi_val3
        }
    }
}

# =====================================================
# GAPLOG-XSYNC-004: Slave 独立写入 Lua 脚本测试
# Lua 脚本生成一个 GTID，包含脚本内所有 key
# 验证 gaplog 记录的 key 是否包含所有脚本内的 key
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-XSYNC-004: slave independent Lua script multi-key" {
            # 1. Slave 同步 Master
            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 2. Master 写入数据
            $M set m_key m_val
            wait_for_sync $S

            # 3. Slave 断开并独立写入 Lua 脚本
            $S replicaof no one
            after 100

            # Slave 独立写入 Lua 脚本
            set lua_script {
                redis.call("SET", KEYS[1], ARGV[1])
                redis.call("SET", KEYS[2], ARGV[2])
                redis.call("SET", KEYS[3], ARGV[3])
                return "OK"
            }
            $S EVAL $lua_script 3 s_lua_key1 s_lua_key2 s_lua_key3 s_lua_val1 s_lua_val2 s_lua_val3

            # 获取 Slave 独立写入的 GTID uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 4. Slave 重新连接
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]

            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 等待 xsync continue
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }

            after 100

            # 5. 验证 gaplog 条目数：Lua 脚本生成 1 个 GTID
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1

            # 6. 验证 gaplog 记录的 key 是否包含所有脚本内的 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            # 结果应该包含 s_lua_key1, s_lua_key2, s_lua_key3
            assert_match "*s_lua_key1*" $result
            assert_match "*s_lua_key2*" $result
            assert_match "*s_lua_key3*" $result

            # 验证数据
            assert_equal [$S get m_key] m_val
            assert_equal [$S get s_lua_key1] s_lua_val1
            assert_equal [$S get s_lua_key2] s_lua_val2
            assert_equal [$S get s_lua_key3] s_lua_val3
        }
    }
}

# =====================================================
# GAPLOG-XSYNC-005: 配置和基本命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-XSYNC-005: config and basic commands" {
        set client [srv 0 client]

        # 测试配置
        set enabled [$client CONFIG GET gtid-gaplog-enabled]
        assert_equal {gtid-gaplog-enabled yes} $enabled

        $client CONFIG SET gtid-gaplog-enabled no
        set enabled [$client CONFIG GET gtid-gaplog-enabled]
        assert_equal {gtid-gaplog-enabled no} $enabled

        $client CONFIG SET gtid-gaplog-enabled yes

        # 测试 gtid-xsync-max-gap (用于 FIFO 逐出阈值)
        set max_gap [$client CONFIG GET gtid-xsync-max-gap]
        assert_equal {gtid-xsync-max-gap 10000} $max_gap

        $client CONFIG SET gtid-xsync-max-gap 100
        set max_gap [$client CONFIG GET gtid-xsync-max-gap]
        assert_equal {gtid-xsync-max-gap 100} $max_gap

        $client CONFIG SET gtid-xsync-max-gap 10000

        # 测试 GAPLOG LEN
        set len [$client GTIDX GAPLOG LEN]
        assert_equal $len 0

        # 测试 GAPLOG CLEAR
        $client GTIDX GAPLOG CLEAR
        set len [$client GTIDX GAPLOG LEN]
        assert_equal $len 0
    }
}

# =====================================================
# GAPLOG-XSYNC-006: 边缘测试 - 空数据同步
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-XSYNC-006: edge case - empty data sync" {
            # 1. Slave 同步 Master（无数据）
            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 2. Slave 断开连接（无独立写入）
            $S replicaof no one
            after 100

            # 3. Slave 重新连接
            $S replicaof $M_host $M_port
            wait_for_sync $S

            after 100

            # 4. 验证 gaplog 为空
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 0
        }
    }
}

# =====================================================
# GAPLOG-XSYNC-007: 边缘测试 - 大量独立写入
# 验证 gaplog 记录的 key 数量是否正确
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-XSYNC-007: edge case - many independent writes" {
            # 1. Slave 同步 Master
            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 2. Master 写入数据
            $M set m_key m_val
            wait_for_sync $S

            # 3. Slave 断开并独立写入大量数据
            $S replicaof no one
            after 100

            set num_writes 10
            for {set i 1} {$i <= $num_writes} {incr i} {
                $S set s_key_$i s_val_$i
            }

            # 获取 Slave 独立写入的 GTID uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 4. Slave 重新连接
            $S replicaof $M_host $M_port
            wait_for_sync $S

            after 100

            # 5. 验证 gaplog 条目数
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len $num_writes

            # 6. 验证 gaplog 记录的 key 是否正确
            # 查询 gaplog range，验证是否包含所有 key
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $num_writes]
            # 验证所有 key 都在结果中
            for {set i 1} {$i <= $num_writes} {incr i} {
                assert_match "*s_key_$i*" $result
            }

            # 验证数据
            assert_equal [$S get m_key] m_val
            for {set i 1} {$i <= $num_writes} {incr i} {
                assert_equal [$S get s_key_$i] s_val_$i
            }
        }
    }
}

# =====================================================
# GAPLOG-XSYNC-008: 边缘测试 - 不同数据类型
# 验证 gaplog 记录不同数据类型的 key 是否正确
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-XSYNC-008: edge case - different data types" {
            # 1. Slave 同步 Master
            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 2. Master 写入数据
            $M set m_key m_val
            wait_for_sync $S

            # 3. Slave 断开并独立写入不同数据类型
            $S replicaof no one
            after 100

            # String
            $S set s_str_key s_str_val
            # Hash
            $S hset s_hash_key field1 val1 field2 val2
            # List
            $S lpush s_list_key elem1 elem2 elem3
            # Set
            $S sadd s_set_key member1 member2
            # Sorted Set
            $S zadd s_zset_key 1 member1 2 member2

            # 获取 Slave 独立写入的 GTID uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 4. Slave 重新连接
            $S replicaof $M_host $M_port
            wait_for_sync $S

            after 100

            # 5. 验证 gaplog 条目数（5 种数据类型）
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 5

            # 6. 验证 gaplog 记录的 key 是否正确
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 5]
            # 验证所有 key 都在结果中
            assert_match "*s_str_key*" $result
            assert_match "*s_hash_key*" $result
            assert_match "*s_list_key*" $result
            assert_match "*s_set_key*" $result
            assert_match "*s_zset_key*" $result

            # 验证数据
            assert_equal [$S get m_key] m_val
            assert_equal [$S get s_str_key] s_str_val
            assert_equal [$S hget s_hash_key field1] val1
            assert_equal [$S lrange s_list_key 0 -1] {elem3 elem2 elem1}
            assert_equal [lsort [$S smembers s_set_key]] {member1 member2}
            assert_equal [$S zrange s_zset_key 0 -1] {member1 member2}
        }
    }
}

# =====================================================
# GAPLOG-XSYNC-009: 边缘测试 - SELECT 命令
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-XSYNC-009: edge case - SELECT command" {
            # 1. Slave 同步 Master
            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 2. Master 写入数据到 db0
            $M set m_key m_val
            wait_for_sync $S

            # 验证 Slave 在 db0 有数据
            assert_equal [$S get m_key] m_val

            # 3. Slave 断开并独立写入到 db0（不切换 db）
            $S replicaof no one
            after 100

            # 在 db0 写入数据
            $S set s_db0_key s_db0_val

            # 获取 Slave 独立写入的 GTID uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 4. Slave 重新连接
            set orig_xcontinue [get_info_property $S gtid gtid_sync_stat xsync_xcontinue]

            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 等待 xsync continue
            wait_for_condition 50 100 {
                [get_info_property $S gtid gtid_sync_stat xsync_xcontinue] > $orig_xcontinue
            } else {
                fail "xsync xcontinue not detected"
            }

            after 100

            # 5. 验证 gaplog 条目数（1 条写入命令）
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1

            # 6. 验证 gaplog 记录的 key 是否正确
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            assert_match "*s_db0_key*" $result

            # 验证数据（Slave 当前在 db0）
            assert_equal [$S get m_key] m_val
            assert_equal [$S get s_db0_key] s_db0_val
        }
    }
}

# =====================================================
# GAPLOG-XSYNC-010: 边缘测试 - DEL 命令
# 验证 gaplog 记录 DEL 命令的 key 是否正确
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-XSYNC-010: edge case - DEL command" {
            # 1. Slave 同步 Master
            $S replicaof $M_host $M_port
            wait_for_sync $S

            # 2. Master 写入数据
            $M set m_key1 m_val1
            $M set m_key2 m_val2
            wait_for_sync $S

            # 3. Slave 断开并独立写入 DEL 命令
            $S replicaof no one
            after 100

            # Slave 独立写入
            $S set s_key s_val
            # Slave 删除 Master 的 key
            $S del m_key1

            # 获取 Slave 独立写入的 GTID uuid
            set slave_uuid [get_slave_gtid_uuid $S]

            # 4. Slave 重新连接
            $S replicaof $M_host $M_port
            wait_for_sync $S

            after 100

            # 5. 验证 gaplog 条目数（2 条命令：SET 和 DEL）
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 2

            # 6. 验证 gaplog 记录的 key 是否正确
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 2]
            # 验证 s_key 和 m_key1 都在结果中
            assert_match "*s_key*" $result
            assert_match "*m_key1*" $result

            # 验证数据
            assert_equal [$S get m_key2] m_val2
            assert_equal [$S get s_key] s_val
        }
    }
}
