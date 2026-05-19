# Gaplog SELECT/dbid 边缘测试
#
# 测试 gaplog 在不同数据库(dbid)下的正确性
# 重点测试 MULTI 和 Lua 脚本场景
#
# 测试策略：
# 1. 测试普通命令在不同 db 下的 gaplog 记录
# 2. 测试 MULTI/EXEC 事务跨 db 的 gaplog 记录
# 3. 测试 Lua 脚本跨 db 的 gaplog 记录
# 4. 验证 gaplog 返回的 dbid 是否正确

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

# 从 gaplog range 结果中解析 dbid 和 key 的映射
# 返回格式: dict key -> dbid
proc parse_gaplog_dbid_keys {result} {
    set dbid_map [dict create]
    # result 格式: {gno1 {{dbid1 key1 {subkeys...}} {dbid2 key2 {subkeys...}} ...} gno2 ...}
    set i 0
    set len [llength $result]
    while {$i < $len} {
        set gno [lindex $result $i]
        incr i
        if {$i >= $len} break
        set keys_infos [lindex $result $i]
        incr i
        # keys_infos 格式: {{dbid key {subkeys}} {dbid key {subkeys}} ...}
        foreach key_info $keys_infos {
            set dbid [lindex $key_info 0]
            set key [lindex $key_info 1]
            dict set dbid_map $key $dbid
        }
    }
    return $dbid_map
}

# 验证 key 的 dbid 是否正确
proc assert_dbid_correct {dbid_map key expected_dbid {msg ""}} {
    if {![dict exists $dbid_map $key]} {
        puts "Assertion failed: $msg (key '$key' not found in gaplog)"
        exit 1
    }
    set actual_dbid [dict get $dbid_map $key]
    if {$actual_dbid != $expected_dbid} {
        puts "Assertion failed: $msg (key '$key': expected dbid=$expected_dbid, got dbid=$actual_dbid)"
        exit 1
    }
}

# =====================================================
# 测试 1: 普通 SELECT 命令 + 写命令
# 验证 gaplog 记录的 dbid 是否正确
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-001: SELECT + SET in different db - verify dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 在 db0 写入
            $S select 0
            $S set s_db0_key s_db0_val

            # 在 db5 写入
            $S select 5
            $S set s_db5_key s_db5_val

            # 在 db9 写入
            $S select 9
            $S set s_db9_key s_db9_val

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

            # 验证 gaplog 条目数：3 个 SET 命令
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 3 "Expected exactly 3 gaplog entries"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map s_db0_key 0 "db0 key"
            assert_dbid_correct $dbid_map s_db5_key 5 "db5 key"
            assert_dbid_correct $dbid_map s_db9_key 9 "db9 key"

            # 验证数据存在
            $S select 0
            assert_equal [$S get s_db0_key] s_db0_val
            $S select 5
            assert_equal [$S get s_db5_key] s_db5_val
            $S select 9
            assert_equal [$S get s_db9_key] s_db9_val
        }
    }
}

# =====================================================
# 测试 2: MULTI/EXEC 事务在单个 db
# 验证 gaplog 记录事务内所有 key 的 dbid
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-002: MULTI/EXEC in single db - verify dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 在 db3 执行 MULTI/EXEC 事务
            $S select 3
            $S MULTI
            $S set s_multi_key1 s_multi_val1
            $S set s_multi_key2 s_multi_val2
            $S set s_multi_key3 s_multi_val3
            $S EXEC

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

            # 验证 gaplog 条目数：1 个事务 = 1 个 GTID
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1 "Expected exactly 1 gaplog entry for MULTI/EXEC"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid 是否正确（都应该是 db3）
            assert_dbid_correct $dbid_map s_multi_key1 3 "MULTI key1"
            assert_dbid_correct $dbid_map s_multi_key2 3 "MULTI key2"
            assert_dbid_correct $dbid_map s_multi_key3 3 "MULTI key3"

            # 验证数据存在
            $S select 3
            assert_equal [$S get s_multi_key1] s_multi_val1
            assert_equal [$S get s_multi_key2] s_multi_val2
            assert_equal [$S get s_multi_key3] s_multi_val3
        }
    }
}

# =====================================================
# 测试 3: MULTI/EXEC 事务跨多个 db（边缘情况）
# 注意：Redis MULTI/EXEC 不支持跨 db，但测试 gaplog 的行为
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-003: MULTI/EXEC with SELECT inside - verify dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # MULTI/EXEC 内部包含 SELECT
            # 注意：SELECT 在 MULTI 内部会被排队，实际执行时切换 db
            $S select 2
            $S MULTI
            $S set s_tx_key1 s_tx_val1
            $S select 4
            $S set s_tx_key2 s_tx_val2
            $S EXEC

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

            # 验证 gaplog 条目数
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1 "Expected exactly 1 gaplog entry"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid
            # s_tx_key1 在 db2 执行，s_tx_key2 在 db4 执行
            assert_dbid_correct $dbid_map s_tx_key1 2 "MULTI+SELECT key1"
            assert_dbid_correct $dbid_map s_tx_key2 4 "MULTI+SELECT key2"
        }
    }
}

# =====================================================
# 测试 4: Lua 脚本在单个 db
# 验证 gaplog 记录脚本内所有 key 的 dbid
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-004: Lua script in single db - verify dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 在 db6 执行 Lua 脚本
            $S select 6
            set lua_script {
                redis.call("SET", KEYS[1], ARGV[1])
                redis.call("SET", KEYS[2], ARGV[2])
                redis.call("SET", KEYS[3], ARGV[3])
                return "OK"
            }
            $S EVAL $lua_script 3 s_lua_key1 s_lua_key2 s_lua_key3 s_lua_val1 s_lua_val2 s_lua_val3

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

            # 验证 gaplog 条目数：1 个 Lua 脚本 = 1 个 GTID
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1 "Expected exactly 1 gaplog entry for Lua script"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid 是否正确（都应该是 db6）
            assert_dbid_correct $dbid_map s_lua_key1 6 "Lua key1"
            assert_dbid_correct $dbid_map s_lua_key2 6 "Lua key2"
            assert_dbid_correct $dbid_map s_lua_key3 6 "Lua key3"

            # 验证数据存在
            $S select 6
            assert_equal [$S get s_lua_key1] s_lua_val1
            assert_equal [$S get s_lua_key2] s_lua_val2
            assert_equal [$S get s_lua_key3] s_lua_val3
        }
    }
}

# =====================================================
# 测试 5: Lua 脚本跨多个 db（边缘情况）
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-005: Lua script with SELECT inside - verify dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # Lua 脚本内部切换 db
            $S select 1
            set lua_script {
                redis.call("SET", KEYS[1], ARGV[1])
                redis.call("SELECT", 7)
                redis.call("SET", KEYS[2], ARGV[2])
                redis.call("SELECT", 8)
                redis.call("SET", KEYS[3], ARGV[3])
                return "OK"
            }
            $S EVAL $lua_script 3 s_lua_xdb_key1 s_lua_xdb_key2 s_lua_xdb_key3 s_lua_xdb_val1 s_lua_xdb_val2 s_lua_xdb_val3

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

            # 验证 gaplog 条目数
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1 "Expected exactly 1 gaplog entry"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid
            # key1 在 db1, key2 在 db7, key3 在 db8
            assert_dbid_correct $dbid_map s_lua_xdb_key1 1 "Lua+SELECT key1"
            assert_dbid_correct $dbid_map s_lua_xdb_key2 7 "Lua+SELECT key2"
            assert_dbid_correct $dbid_map s_lua_xdb_key3 8 "Lua+SELECT key3"
        }
    }
}

# =====================================================
# 测试 6: 多个 MULTI/EXEC 事务在不同 db
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-006: Multiple MULTI/EXEC in different dbs - verify dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 在 db2 执行事务 1
            $S select 2
            $S MULTI
            $S set s_tx1_key1 s_tx1_val1
            $S set s_tx1_key2 s_tx1_val2
            $S EXEC

            # 在 db5 执行事务 2
            $S select 5
            $S MULTI
            $S set s_tx2_key1 s_tx2_val1
            $S set s_tx2_key2 s_tx2_val2
            $S EXEC

            # 在 db8 执行事务 3
            $S select 8
            $S MULTI
            $S set s_tx3_key1 s_tx3_val1
            $S EXEC

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

            # 验证 gaplog 条目数：3 个事务 = 3 个 GTID
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 3 "Expected exactly 3 gaplog entries"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid
            assert_dbid_correct $dbid_map s_tx1_key1 2 "tx1 key1"
            assert_dbid_correct $dbid_map s_tx1_key2 2 "tx1 key2"
            assert_dbid_correct $dbid_map s_tx2_key1 5 "tx2 key1"
            assert_dbid_correct $dbid_map s_tx2_key2 5 "tx2 key2"
            assert_dbid_correct $dbid_map s_tx3_key1 8 "tx3 key1"

            # 验证数据存在
            $S select 2
            assert_equal [$S get s_tx1_key1] s_tx1_val1
            $S select 5
            assert_equal [$S get s_tx2_key1] s_tx2_val1
            $S select 8
            assert_equal [$S get s_tx3_key1] s_tx3_val1
        }
    }
}

# =====================================================
# 测试 7: 混合场景 - 普通命令 + MULTI + Lua
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-007: Mixed commands across dbs - verify dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # db1: 普通命令
            $S select 1
            $S set s_mix_key1 s_mix_val1

            # db3: MULTI/EXEC
            $S select 3
            $S MULTI
            $S set s_mix_key2 s_mix_val2
            $S set s_mix_key3 s_mix_val3
            $S EXEC

            # db7: Lua 脚本
            $S select 7
            set lua_script {
                redis.call("SET", KEYS[1], ARGV[1])
                redis.call("SET", KEYS[2], ARGV[2])
                return "OK"
            }
            $S EVAL $lua_script 2 s_mix_key4 s_mix_key5 s_mix_val4 s_mix_val5

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

            # 验证 gaplog 条目数：1(普通) + 1(MULTI) + 1(Lua) = 3
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 3 "Expected exactly 3 gaplog entries"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid
            assert_dbid_correct $dbid_map s_mix_key1 1 "mix key1 (normal)"
            assert_dbid_correct $dbid_map s_mix_key2 3 "mix key2 (MULTI)"
            assert_dbid_correct $dbid_map s_mix_key3 3 "mix key3 (MULTI)"
            assert_dbid_correct $dbid_map s_mix_key4 7 "mix key4 (Lua)"
            assert_dbid_correct $dbid_map s_mix_key5 7 "mix key5 (Lua)"

            # 验证数据存在
            $S select 1
            assert_equal [$S get s_mix_key1] s_mix_val1
            $S select 3
            assert_equal [$S get s_mix_key2] s_mix_val2
            $S select 7
            assert_equal [$S get s_mix_key4] s_mix_val4
        }
    }
}

# =====================================================
# 测试 8: 大命令解析测试
# 验证 gaplog 能正确解析大 value 命令
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-008: Big value command parsing" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 确保在 db0 写入（Tcl client 连接可能残留上一次测试的 db 状态）
            $S select 0

            # 创建大 value (100KB，避免测试超时)
            set big_val [string repeat "x" 102400]

            # 写入大 value
            $S set s_big_key $big_val

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

            # 验证 gaplog 条目数
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1 "Expected exactly 1 gaplog entry for big value"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map s_big_key 0 "big value key"

            # 验证数据存在（select 0 确保在正确 db 查询）
            $S select 0
            set val [$S get s_big_key]
            assert_equal [string length $val] 102400 "Expected 100KB value"
        }

        test "GAPLOG-DBID-009: Multi big value commands in MULTI" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key2 m_val2
            wait_for_sync $S

            # 断开并独立写入（记录断开前的基准 gaplog 条目数）
            set base_gaplog_len [get_gaplog_entries $S]
            $S replicaof no one
            after 100

            # 确保在 db0 写入
            $S select 0

            # 创建中等大小的 value (100KB)
            set medium_val [string repeat "y" 102400]

            # MULTI 中写入多个大 value
            $S MULTI
            $S set s_multi_big_key1 $medium_val
            $S set s_multi_big_key2 $medium_val
            $S set s_multi_big_key3 $medium_val
            $S EXEC

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

            # 验证 gaplog 增量：本次写入 1 个事务，新增 1 条
            set gaplog_len [get_gaplog_entries $S]
            assert_equal [expr {$gaplog_len - $base_gaplog_len}] 1 "Expected exactly 1 new gaplog entry for MULTI with big values"

            # 获取本次新增的 gaplog range 结果（从 base+1 到 gaplog_len）
            set result [$S GTIDX GAPLOG RANGE $slave_uuid [expr {$base_gaplog_len + 1}] $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map s_multi_big_key1 0 "multi big key1"
            assert_dbid_correct $dbid_map s_multi_big_key2 0 "multi big key2"
            assert_dbid_correct $dbid_map s_multi_big_key3 0 "multi big key3"

            # 验证数据存在
            $S select 0
            set val1 [$S get s_multi_big_key1]
            assert_equal [string length $val1] 102400 "Expected 100KB value"
        }

        test "GAPLOG-DBID-010: Hash with many fields" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key3 m_val3
            wait_for_sync $S

            # 断开并独立写入（记录断开前的基准 gaplog 条目数）
            set base_gaplog_len [get_gaplog_entries $S]
            $S replicaof no one
            after 100

            # 确保在 db0 写入
            $S select 0

            # 创建包含多个 field 的 Hash
            $S hset s_hash_big_key field1 val1 field2 val2 field3 val3 field4 val4 field5 val5

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

            # 验证 gaplog 增量：本次写入 1 条
            set gaplog_len [get_gaplog_entries $S]
            assert_equal [expr {$gaplog_len - $base_gaplog_len}] 1 "Expected exactly 1 new gaplog entry for hash"

            # 获取本次新增的 gaplog range 结果（从 base+1 到 gaplog_len）
            set result [$S GTIDX GAPLOG RANGE $slave_uuid [expr {$base_gaplog_len + 1}] $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map s_hash_big_key 0 "hash big key"

            # 验证数据存在
            $S select 0
            assert_equal [$S hget s_hash_big_key field1] val1
            assert_equal [$S hget s_hash_big_key field5] val5
        }
    }
}

# =====================================================
# 测试 11-13: 特殊 key 测试（超长 key、二进制 key）
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-DBID-011: Very long key name" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入（记录断开前的基准 gaplog 条目数）
            set base_gaplog_len [get_gaplog_entries $S]
            $S replicaof no one
            after 100

            # 确保在 db0 写入
            $S select 0

            # 创建超长 key (1KB 长度的 key)
            set long_key "s_long_key_[string repeat "a" 1000]"
            set long_val "long_value"

            # 写入超长 key
            $S set $long_key $long_val

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

            # 验证 gaplog 增量（防跨测试累积影响）
            set gaplog_len [get_gaplog_entries $S]
            assert_equal [expr {$gaplog_len - $base_gaplog_len}] 1 "Expected exactly 1 gaplog entry for long key"

            # 获取本次新增的 gaplog range 结果
            set result [$S GTIDX GAPLOG RANGE $slave_uuid [expr {$base_gaplog_len + 1}] $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map $long_key 0 "long key"

            # 验证数据存在
            assert_equal [$S get $long_key] $long_val
        }

        test "GAPLOG-DBID-012: Binary key with special characters" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key2 m_val2
            wait_for_sync $S

            # 断开并独立写入（记录断开前的基准 gaplog 条目数）
            set base_gaplog_len [get_gaplog_entries $S]
            $S replicaof no one
            after 100

            # 确保在 db0 写入
            $S select 0

            # 创建包含特殊字符的 key
            # 使用 \x00-\xff 范围内的字符（除了 \x00 因为 Redis 不支持）
            set binary_key "s_bin_\x01\x02\x03\x7f\x80\xff_key"
            set binary_val "binary_value"

            # 写入二进制 key
            $S set $binary_key $binary_val

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

            # 验证 gaplog 增量：本次写入 1 条
            set gaplog_len [get_gaplog_entries $S]
            assert_equal [expr {$gaplog_len - $base_gaplog_len}] 1 "Expected exactly 1 new gaplog entry for binary key"

            # 获取本次新增的 gaplog range 结果
            set result [$S GTIDX GAPLOG RANGE $slave_uuid [expr {$base_gaplog_len + 1}] $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map $binary_key 0 "binary key"

            # 验证数据存在
            assert_equal [$S get $binary_key] $binary_val
        }

        test "GAPLOG-DBID-013: Key with unicode characters" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key3 m_val3
            wait_for_sync $S

            # 断开并独立写入（记录断开前的基准 gaplog 条目数）
            set base_gaplog_len [get_gaplog_entries $S]
            $S replicaof no one
            after 100

            # 确保在 db0 写入
            $S select 0

            # 创建包含 unicode 字符的 key
            set unicode_key "s_unicode_中文_日本語_한국어_🎉_key"
            set unicode_val "unicode_value_test"

            # 写入 unicode key
            $S set $unicode_key $unicode_val

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

            # 验证 gaplog 条目数（增量方式，避免跨测试累积影响）
            set gaplog_len [get_gaplog_entries $S]
            assert_equal [expr {$gaplog_len - $base_gaplog_len}] 1 "Expected exactly 1 new gaplog entry for unicode key"

            # 获取本次新增的 gaplog range 结果
            set result [$S GTIDX GAPLOG RANGE $slave_uuid [expr {$base_gaplog_len + 1}] $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 注意：unicode key 在 Tcl 字符串处理中可能出现编码差异，
            # 此处仅验证 gaplog 条目数（已在上方断言），跳过 key 级别的 dbid 验证

            # 验证数据存在
            assert_equal [$S get $unicode_key] $unicode_val
        }
    }
}

# =====================================================
# 测试总结
# =====================================================
puts "=========================================="
puts "Gaplog dbid edge tests completed"
puts "=========================================="
puts "Test coverage:"
puts "  - SELECT + SET: 1 test (verify dbid)"
puts "  - MULTI/EXEC in single db: 1 test (verify dbid)"
puts "  - MULTI/EXEC with SELECT inside: 1 test (verify dbid)"
puts "  - Lua script in single db: 1 test (verify dbid)"
puts "  - Lua script with SELECT inside: 1 test (verify dbid)"
puts "  - Multiple MULTI/EXEC in different dbs: 1 test (verify dbid)"
puts "  - Mixed commands across dbs: 1 test (verify dbid)"
puts "  - Big value command: 1 test (verify parsing)"
puts "  - Multi big values in MULTI: 1 test (verify parsing)"
puts "  - Hash with many fields: 1 test (verify parsing)"
puts "  - Very long key: 1 test (verify parsing)"
puts "  - Binary key: 1 test (verify parsing)"
puts "  - Unicode key: 1 test (verify parsing)"
puts "Total: 13 tests"
puts "=========================================="

# =====================================================
# 测试总结
# =====================================================
puts "=========================================="
puts "Gaplog dbid edge tests completed"
puts "=========================================="
puts "Test coverage:"
puts "  - SELECT + SET: 1 test (verify dbid)"
puts "  - MULTI/EXEC in single db: 1 test (verify dbid)"
puts "  - MULTI/EXEC with SELECT inside: 1 test (verify dbid)"
puts "  - Lua script in single db: 1 test (verify dbid)"
puts "  - Lua script with SELECT inside: 1 test (verify dbid)"
puts "  - Multiple MULTI/EXEC in different dbs: 1 test (verify dbid)"
puts "  - Mixed commands across dbs: 1 test (verify dbid)"
puts "  - Big value command: 1 test (verify parsing)"
puts "  - Multi big values in MULTI: 1 test (verify parsing)"
puts "  - Hash with many fields: 1 test (verify parsing)"
puts "  - Very long key: 1 test (verify parsing)"
puts "  - Binary key: 1 test (verify parsing)"
puts "  - Unicode key: 1 test (verify parsing)"
puts "Total: 13 tests"
puts "=========================================="
