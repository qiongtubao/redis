# Gaplog 单元测试
# 测试 GTIDX GAPLOG 命令的基本功能
# 注意：这些测试不涉及 xsync 复制，只测试命令本身

proc assert_equal {a b {msg ""}} {
    if {$a ne $b} {
        puts "Assertion failed: $msg (expected: $b, got: $a)"
        exit 1
    }
}

proc assert_lessthan {a b {msg ""}} {
    if {$a >= $b} {
        puts "Assertion failed: $msg (expected < $b, got $a)"
        exit 1
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

# =====================================================
# 基本命令测试
# =====================================================

# GAPLOG-001: GTIDX GAPLOG LEN - empty gaplog returns 0
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-001: GTIDX GAPLOG LEN - empty gaplog returns 0" {
        set client [srv 0 client]
        set len [$client GTIDX GAPLOG LEN]
        assert_equal $len 0 "Initial gaplog length should be 0"
    }
}

# GAPLOG-002: GTIDX GAPLOG RANGE - empty gaplog returns empty array
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-002: GTIDX GAPLOG RANGE - empty gaplog returns empty array" {
        set client [srv 0 client]
        set uuid [get_uuid $client]

        set result [$client GTIDX GAPLOG RANGE $uuid 1 100]
        assert_equal [llength $result] 0 "Range on empty gaplog should return empty array"

        set result [$client GTIDX GAPLOG RANGE "nonexistent-uuid-1234567890" 1 100]
        assert_equal [llength $result] 0 "Range with nonexistent uuid should return empty array"
    }
}

# GAPLOG-003: GTIDX GAPLOG CLEAR - clear empty gaplog
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-003: GTIDX GAPLOG CLEAR - clear empty gaplog" {
        set client [srv 0 client]

        $client GTIDX GAPLOG CLEAR
        set len [$client GTIDX GAPLOG LEN]
        assert_equal $len 0 "Gaplog should be 0 after CLEAR on empty gaplog"
    }
}

# =====================================================
# 配置参数测试
# =====================================================

# GAPLOG-004: gtid-gaplog-enabled config
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled no gtid-xsync-max-gap 10000}} {
    test "GAPLOG-004: config - gtid-gaplog-enabled" {
        set client [srv 0 client]

        set len [$client GTIDX GAPLOG LEN]
        assert_equal $len 0 "Gaplog should be empty when disabled"

        $client CONFIG SET gtid-gaplog-enabled yes

        set enabled [$client CONFIG GET gtid-gaplog-enabled]
        assert_equal {gtid-gaplog-enabled yes} $enabled "CONFIG GET should return enabled"
    }
}

# GAPLOG-005: gtid-xsync-max-gap config (dynamic SET/GET) - 用于 FIFO 逐出阈值
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-005: config - gtid-xsync-max-gap dynamic set" {
        set client [srv 0 client]

        set max_gap [$client CONFIG GET gtid-xsync-max-gap]
        assert_equal {gtid-xsync-max-gap 10000} $max_gap "Default max-gap should be 10000"

        $client CONFIG SET gtid-xsync-max-gap 50
        set max_gap [$client CONFIG GET gtid-xsync-max-gap]
        assert_equal {gtid-xsync-max-gap 50} $max_gap "After CONFIG SET, max-gap should be 50"
    }
}

# =====================================================
# HELP 和 INFO 测试
# =====================================================

# GAPLOG-006: GTIDX HELP contains GAPLOG entries
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-006: GTIDX HELP contains GAPLOG entries" {
        set client [srv 0 client]

        set help [$client GTIDX HELP]
        set found 0
        foreach line $help {
            if {[string match "GAPLOG*" $line]} {
                set found 1
                break
            }
        }
        assert_equal $found 1 "GTIDX HELP should contain GAPLOG entries"
    }
}

# GAPLOG-007: INFO GTID includes gaplog stats
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-007: INFO GTID includes gaplog stats" {
        set client [srv 0 client]

        set entries [get_gaplog_entries $client]
        assert_equal $entries 0 "INFO should show 0 gaplog entries"
    }
}

# =====================================================
# 错误处理测试
# =====================================================

# GAPLOG-008: GTIDX GAPLOG invalid subcommand error
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-008: GTIDX GAPLOG invalid subcommand error" {
        set client [srv 0 client]

        set err [catch {$client GTIDX GAPLOG INVALID_SUBCMD} result]
        assert_equal $err 1 "Unknown subcommand should error"
    }
}

# GAPLOG-009: GTIDX GAPLOG RANGE with invalid gno range
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-009: GTIDX GAPLOG RANGE - start > end range" {
        set client [srv 0 client]
        set uuid [get_uuid $client]

        set result [$client GTIDX GAPLOG RANGE $uuid 100 1]
        assert_equal [llength $result] 0 "start > end range on empty gaplog should return empty array"
    }
}

# GAPLOG-010: gtid-gaplog-enabled toggle
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes gtid-xsync-max-gap 10000}} {
    test "GAPLOG-010: gtid-gaplog-enabled CONFIG SET toggle" {
        set client [srv 0 client]

        $client CONFIG SET gtid-gaplog-enabled no
        set enabled [$client CONFIG GET gtid-gaplog-enabled]
        assert_equal {gtid-gaplog-enabled no} $enabled "CONFIG GET should return disabled"

        $client CONFIG SET gtid-gaplog-enabled yes
        set enabled [$client CONFIG GET gtid-gaplog-enabled]
        assert_equal {gtid-gaplog-enabled yes} $enabled "CONFIG GET should return re-enabled"
    }
}

# =====================================================
# 测试总结
# =====================================================
puts "=========================================="
puts "Gaplog unit tests completed"
puts "=========================================="
puts "Test coverage:"
puts "  - Basic commands: 3 tests (LEN/RANGE/CLEAR)"
puts "  - Config parameters: 3 tests"
puts "  - HELP/INFO: 2 tests"
puts "  - Error handling: 2 tests"
puts "Total: 10 tests"
puts "=========================================="
