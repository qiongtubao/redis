# Gaplog 命令单测
#
# 测试所有 GTIDX GAPLOG 子命令的输出格式
#
# 命令列表:
#   GTIDX GAPLOG LEN          -> integer (条目数量)
#   GTIDX GAPLOG RANGE <uuid> <start> <end> -> array [gno, keys_infos, gno, keys_infos, ...]
#   GTIDX GAPLOG CLEAR        -> OK
#
# 注意: GAPLOG ADD 命令已删除，gaplog 通过 saveGapLogFromGtidSet 自动填充

# =====================================================
# GAPLOG LEN 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes}} {
    test "GAPLOG-CMD-001: GAPLOG LEN - empty gaplog returns 0" {
        # 空的 gaplog 应该返回 0
        set len [r GTIDX GAPLOG LEN]
        assert {$len == 0}
    }

    test "GAPLOG-CMD-002: GAPLOG CLEAR - clears all entries" {
        # CLEAR 命令应该返回 OK
        set result [r GTIDX GAPLOG CLEAR]
        assert_equal $result "OK"

        set len [r GTIDX GAPLOG LEN]
        assert {$len == 0}
    }
}

# =====================================================
# GAPLOG RANGE 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes}} {
    test "GAPLOG-CMD-003: GAPLOG RANGE - empty gaplog returns empty array" {
        r GTIDX GAPLOG CLEAR
        set result [r GTIDX GAPLOG RANGE "uuid-001" 1 10]
        # 空数组
        assert_equal $result {}
    }

    test "GAPLOG-CMD-004: GAPLOG RANGE - non-existent uuid returns empty array" {
        r GTIDX GAPLOG CLEAR
        set result [r GTIDX GAPLOG RANGE "non-existent-uuid" 1 10]
        assert_equal $result {}
    }

    test "GAPLOG-CMD-005: GAPLOG RANGE - start > end returns empty" {
        r GTIDX GAPLOG CLEAR
        set result [r GTIDX GAPLOG RANGE "uuid-001" 5 1]
        assert_equal $result {}
    }

    test "GAPLOG-CMD-006: GAPLOG RANGE - start beyond max gno returns empty" {
        r GTIDX GAPLOG CLEAR
        set result [r GTIDX GAPLOG RANGE "uuid-001" 10 20]
        assert_equal $result {}
    }
}

# =====================================================
# GAPLOG CLEAR 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes}} {
    test "GAPLOG-CMD-007: GAPLOG CLEAR - empty gaplog returns OK" {
        r GTIDX GAPLOG CLEAR
        set result [r GTIDX GAPLOG CLEAR]
        assert_equal $result "OK"
        set len [r GTIDX GAPLOG LEN]
        assert {$len == 0}
    }

    test "GAPLOG-CMD-008: GAPLOG CLEAR - RANGE returns empty after clear" {
        r GTIDX GAPLOG CLEAR
        set result [r GTIDX GAPLOG CLEAR]
        assert_equal $result "OK"

        set result [r GTIDX GAPLOG RANGE "uuid-001" 1 10]
        assert_equal $result {}
    }
}

# =====================================================
# 错误处理测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes}} {
    test "GAPLOG-CMD-009: GAPLOG LEN - wrong args count error" {
        catch {r GTIDX GAPLOG LEN "extra-arg"} err
        assert_match "*wrong*" $err
    }

    test "GAPLOG-CMD-010: GAPLOG RANGE - wrong args count error" {
        catch {r GTIDX GAPLOG RANGE "uuid"} err
        assert_match "*wrong*" $err
    }

    test "GAPLOG-CMD-011: GAPLOG RANGE - invalid gno format error" {
        catch {r GTIDX GAPLOG RANGE "uuid-001" "abc" "def"} err
        assert_match "*integer*" $err
    }

    test "GAPLOG-CMD-012: GAPLOG invalid subcommand error" {
        catch {r GTIDX GAPLOG INVALID} err
        assert_match "*subcommand*" $err
    }
}

# =====================================================
# HELP 命令测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes}} {
    test "GAPLOG-CMD-013: GTIDX HELP contains GAPLOG commands" {
        set help [r GTIDX HELP]
        assert_match "*GAPLOG LEN*" $help
        assert_match "*GAPLOG RANGE*" $help
        assert_match "*GAPLOG CLEAR*" $help
    }
}

# =====================================================
# INFO 统计测试
# =====================================================
start_server {tags {"gaplog"} overrides {gtid-enabled yes gtid-gaplog-enabled yes}} {
    test "GAPLOG-CMD-014: INFO GTID contains gaplog stats" {
        r GTIDX GAPLOG CLEAR
        set info [r INFO gtid]
        # 验证 gaplog 统计信息
        assert_match "*gtid_gaplog_entries:0*" $info
    }
}
