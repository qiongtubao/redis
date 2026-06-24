start_server {tags {"activedefrag swap"}} {
    proc populate_defrag_hash {key fields} {
        r del $key
        for {set i 0} {$i < $fields} {incr i} {
            r hset $key field:$i [string repeat x 64]
        }
    }

    proc populate_defrag_allocator_noise {prefix keys} {
        for {set i 0} {$i < $keys} {incr i} {
            r set $prefix:$i [string repeat y 256]
        }
        for {set i 0} {$i < $keys} {incr i 2} {
            r del $prefix:$i
        }
    }

    proc defrag_hash_fields {start end} {
        set fields {}
        for {set i $start} {$i <= $end} {incr i} {
            lappend fields field:$i
        }
        return $fields
    }

    proc assert_defrag_hash_fields {key fields value} {
        set vals [r hmget $key {*}$fields]
        assert_equal [llength $vals] [llength $fields]
        foreach v $vals {
            assert_equal $v $value
        }
    }

    r config set swap-debug-evict-keys 0

    test {active defrag does not touch hash while swap lock is held} {
        r config set hz 100
        r config set active-defrag-threshold-lower 0
        r config set active-defrag-threshold-upper 1
        r config set active-defrag-cycle-min 99
        r config set active-defrag-cycle-max 99
        r config set active-defrag-ignore-bytes 1
        catch {r config set activedefrag no}

        catch {r config set activedefrag yes} e
        if {[r config get activedefrag] eq "activedefrag yes"} {
            r config set activedefrag no

            populate_defrag_hash defrag-hash 512
            r swap.evict defrag-hash
            wait_key_cold r defrag-hash
            assert_equal [object_meta_len r defrag-hash] 512

            # Make the hash warm so the swap-in path merges fields into the
            # in-memory object from the swap thread.
            assert_equal [r hget defrag-hash field:0] [string repeat x 64]
            assert_equal [object_is_warm r defrag-hash] 1

            populate_defrag_allocator_noise defrag-noise 3000
            r config resetstat

            set fields_to_load {field:1 field:2 field:3 field:4 field:5 field:6 field:7 field:8}
            r debug set-swap-debug-rio-delay-micro 4000000
            set rd [redis_deferring_client]
            $rd hmget defrag-hash {*}$fields_to_load

            # Give the swap request time to acquire its key lock, then let
            # active defrag run while the swap thread is still delayed in RIO.
            after 100
            r config set activedefrag yes
            wait_for_condition 30 100 {
                [s active_defrag_key_misses] > 0 || [s active_defrag_key_hits] > 0
            } else {
                fail "active defrag did not scan while swap request was pending"
            }

            set vals [$rd read]
            $rd close
            r debug set-swap-debug-rio-delay-micro 0
            r config set activedefrag no

            assert_equal [llength $vals] 8
            foreach v $vals {
                assert_equal $v [string repeat x 64]
            }

            # If defrag moved the main dict key without updating swap metadata,
            # this lookup can fail or crash on the buggy implementation. The
            # meta length is the remaining cold fields: field:0 plus the HMGET
            # fields have been swapped into memory.
            assert_equal [object_meta_len r defrag-hash] [expr {512 - 1 - [llength $fields_to_load]}]
            assert_equal [r hlen defrag-hash] 512
            assert_equal [r hget defrag-hash field:0] [string repeat x 64]
        }
    }

    test {active defrag and swap-in concurrent warm hash merge stress} {
        r config set hz 100
        r config set active-defrag-threshold-lower 0
        r config set active-defrag-threshold-upper 1
        r config set active-defrag-cycle-min 99
        r config set active-defrag-cycle-max 99
        r config set active-defrag-ignore-bytes 1
        catch {r config set activedefrag no}

        catch {r config set activedefrag yes} e
        if {[r config get activedefrag] eq "activedefrag yes"} {
            r config set activedefrag no

            set total_fields 4096
            set load_fields [defrag_hash_fields 1 2048]

            for {set round 0} {$round < 5} {incr round} {
                r flushdb
                r debug set-swap-debug-rio-delay-micro 0
                r config set activedefrag no

                populate_defrag_hash race-hash $total_fields
                r swap.evict race-hash
                wait_key_cold r race-hash
                assert_equal [object_meta_len r race-hash] $total_fields

                # Turn the key warm. The following HMGET will merge many cold
                # fields into this in-memory hash from the swap thread.
                assert_equal [r hget race-hash field:0] [string repeat x 64]
                assert_equal [object_is_warm r race-hash] 1

                populate_defrag_allocator_noise race-noise 6000
                r config resetstat

                # Delay RIO long enough for active defrag to scan the locked key,
                # then let a large merge happen while defrag is still enabled.
                r debug set-swap-debug-rio-delay-micro 200000
                set rd [redis_deferring_client]
                $rd hmget race-hash {*}$load_fields

                after 10
                r config set activedefrag yes
                wait_for_condition 30 100 {
                    [s active_defrag_key_misses] > 0 || [s active_defrag_key_hits] > 0
                } else {
                    fail "active defrag did not scan during swap-in stress round"
                }

                set vals [$rd read]
                $rd close
                r debug set-swap-debug-rio-delay-micro 0
                r config set activedefrag no

                assert_equal [llength $vals] [llength $load_fields]
                foreach v $vals {
                    assert_equal $v [string repeat x 64]
                }

                assert_equal [r hlen race-hash] $total_fields
                assert_equal [object_meta_len r race-hash] [expr {$total_fields - 1 - [llength $load_fields]}]
                assert_defrag_hash_fields race-hash {field:0 field:1 field:1024 field:2048 field:4095} [string repeat x 64]
            }
        }
    }
}
