# Gaplog MULTI/EXEC offset 定位测试
#
# 测试目的：验证 gtidSeqLookup 返回的 offset 指向的是 SELECT 还是 MULTI
#
# 测试策略：
# 1. 在不同 db 执行 MULTI/EXEC 事务
# 2. 通过日志观察 offset 定位到的第一个命令是什么
# 3. 验证 gaplog 能正确解析事务内的所有 key

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
proc parse_gaplog_dbid_keys {result} {
    set dbid_map [dict create]
    set i 0
    set len [llength $result]
    while {$i < $len} {
        set gno [lindex $result $i]
        incr i
        if {$i >= $len} break
        set keys_infos [lindex $result $i]
        incr i
        foreach key_info $keys_infos {
            set dbid [lindex $key_info 0]
            set key [lindex $key_info 1]
            dict set dbid_map $key $dbid
        }
    }
    return $dbid_map
}

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
# 测试 1: MULTI/EXEC 在 db0（不需要 SELECT）
# 验证 offset 指向 MULTI
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-OFFSET-001: MULTI/EXEC in db0 - offset should point to MULTI" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 确保在 db0
            $S select 0

            # 执行 MULTI/EXEC 事务
            $S MULTI
            $S set s_db0_key1 s_db0_val1
            $S set s_db0_key2 s_db0_val2
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

            # 验证每个 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map s_db0_key1 0 "db0 key1"
            assert_dbid_correct $dbid_map s_db0_key2 0 "db0 key2"

            # 验证数据存在
            assert_equal [$S get s_db0_key1] s_db0_val1
            assert_equal [$S get s_db0_key2] s_db0_val2

            puts "  [gaplog] Test passed: MULTI in db0, offset should point to MULTI directly"
        }
    }
}

# =====================================================
# 测试 2: MULTI/EXEC 在 db5（需要 SELECT）
# 验证 offset 是否指向 SELECT 还是 MULTI
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-OFFSET-002: MULTI/EXEC in db5 - verify offset and dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # 先在 db0 写入一个命令，确保 slaveseldb = 0
            $S select 0
            $S set s_pre_key s_pre_val

            # 然后在 db5 执行 MULTI/EXEC 事务
            # 这会导致 backlog 中写入 SELECT 5 -> MULTI -> ...
            $S select 5
            $S MULTI
            $S set s_db5_key1 s_db5_val1
            $S set s_db5_key2 s_db5_val2
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

            # 验证 gaplog 条目数：1(pre) + 1(MULTI) = 2
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 2 "Expected exactly 2 gaplog entries"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map s_pre_key 0 "pre key in db0"
            assert_dbid_correct $dbid_map s_db5_key1 5 "db5 key1"
            assert_dbid_correct $dbid_map s_db5_key2 5 "db5 key2"

            # 验证数据存在
            $S select 0
            assert_equal [$S get s_pre_key] s_pre_val
            $S select 5
            assert_equal [$S get s_db5_key1] s_db5_val1
            assert_equal [$S get s_db5_key2] s_db5_val2

            puts "  [gaplog] Test passed: MULTI in db5 after db0, offset may point to SELECT 5"
        }
    }
}

# =====================================================
# 测试 3: 连续在不同 db 执行 MULTI/EXEC
# 验证每次切换 db 时 offset 的行为
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-OFFSET-003: Multiple MULTI/EXEC switching dbs - verify dbid" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # db0: 第一个事务（不需要 SELECT）
            $S select 0
            $S MULTI
            $S set s_tx0_key s_tx0_val
            $S EXEC

            # db3: 第二个事务（需要 SELECT 3）
            $S select 3
            $S MULTI
            $S set s_tx3_key s_tx3_val
            $S EXEC

            # db0: 第三个事务（需要 SELECT 0）
            $S select 0
            $S MULTI
            $S set s_tx0_key2 s_tx0_val2
            $S EXEC

            # db7: 第四个事务（需要 SELECT 7）
            $S select 7
            $S MULTI
            $S set s_tx7_key s_tx7_val
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

            # 验证 gaplog 条目数：4 个事务
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 4 "Expected exactly 4 gaplog entries"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 $gaplog_len]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid 是否正确
            assert_dbid_correct $dbid_map s_tx0_key 0 "tx0 key"
            assert_dbid_correct $dbid_map s_tx3_key 3 "tx3 key"
            assert_dbid_correct $dbid_map s_tx0_key2 0 "tx0 key2"
            assert_dbid_correct $dbid_map s_tx7_key 7 "tx7 key"

            # 验证数据存在
            $S select 0
            assert_equal [$S get s_tx0_key] s_tx0_val
            assert_equal [$S get s_tx0_key2] s_tx0_val2
            $S select 3
            assert_equal [$S get s_tx3_key] s_tx3_val
            $S select 7
            assert_equal [$S get s_tx7_key] s_tx7_val

            puts "  [gaplog] Test passed: Multiple MULTI/EXEC switching dbs, all dbid correct"
        }
    }
}

# =====================================================
# 测试 4: MULTI/EXEC 内部包含 SELECT
# 验证事务内部的 SELECT 能正确更新 dbid
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    start_server {overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
        set M [srv -1 client]
        set M_host [srv -1 host]
        set M_port [srv -1 port]
        set S [srv 0 client]

        test "GAPLOG-OFFSET-004: MULTI/EXEC with SELECT inside - verify dbid per key" {
            # 同步
            $S replicaof $M_host $M_port
            wait_for_sync $S
            $M set m_key m_val
            wait_for_sync $S

            # 断开并独立写入
            $S replicaof no one
            after 100

            # MULTI/EXEC 内部包含 SELECT
            $S select 2
            $S MULTI
            $S set s_inner_key1 s_inner_val1   # db2
            $S select 4
            $S set s_inner_key2 s_inner_val2   # db4
            $S select 6
            $S set s_inner_key3 s_inner_val3   # db6
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

            # 验证 gaplog 条目数：1 个事务
            set gaplog_len [get_gaplog_entries $S]
            assert_equal $gaplog_len 1 "Expected exactly 1 gaplog entry"

            # 获取 gaplog range 结果并解析 dbid
            set result [$S GTIDX GAPLOG RANGE $slave_uuid 1 1]
            set dbid_map [parse_gaplog_dbid_keys $result]

            # 验证每个 key 的 dbid 是否正确
            # key1 在 db2, key2 在 db4, key3 在 db6
            assert_dbid_correct $dbid_map s_inner_key1 2 "inner key1 (db2)"
            assert_dbid_correct $dbid_map s_inner_key2 4 "inner key2 (db4)"
            assert_dbid_correct $dbid_map s_inner_key3 6 "inner key3 (db6)"

            # 验证数据存在
            $S select 2
            assert_equal [$S get s_inner_key1] s_inner_val1
            $S select 4
            assert_equal [$S get s_inner_key2] s_inner_val2
            $S select 6
            assert_equal [$S get s_inner_key3] s_inner_val3

            puts "  [gaplog] Test passed: MULTI with SELECT inside, each key has correct dbid"
        }
    }
}

# =====================================================
# 测试总结
# =====================================================
puts "=========================================="
puts "Gaplog offset tests completed"
puts "=========================================="
puts "Test coverage:"
puts "  - MULTI in db0 (no SELECT): 1 test"
puts "  - MULTI in db5 after db0: 1 test"
puts "  - Multiple MULTI switching dbs: 1 test"
puts "  - MULTI with SELECT inside: 1 test"
puts "Total: 4 tests"
puts "=========================================="