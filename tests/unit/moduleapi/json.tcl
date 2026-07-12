# Tests for the RedisJSON C module (ReJSON-c)
# JSON objects need double braces in Tcl: {{"key":val}}
# Run: ./runtest --single unit/moduleapi/json

set testmodule [file normalize tests/modules/rejson-c.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    test {JSON.SET/GET number} {
        r JSON.SET k . 42
        assert_equal {42} [r JSON.GET k .]
    }

    test {JSON.SET/GET string} {
        r JSON.SET k . {"hello"}
        assert_equal {"hello"} [r JSON.GET k .]
    }

    test {JSON.SET/GET array} {
        r JSON.SET k . {[1,2,3]}
        assert_equal {[1,2,3]} [r JSON.GET k .]
    }

    test {JSON.TYPE integer} {
        r JSON.SET k . 1
        assert_equal {integer} [r JSON.TYPE k .]
    }

    test {JSON.TYPE number} {
        r JSON.SET k . 1.5
        assert_equal {number} [r JSON.TYPE k .]
    }

    test {JSON.TYPE string} {
        r JSON.SET k . {"s"}
        assert_equal {string} [r JSON.TYPE k .]
    }

    test {JSON.TYPE boolean} {
        r JSON.SET k . true
        assert_equal {boolean} [r JSON.TYPE k .]
    }

    test {JSON.TYPE null} {
        r JSON.SET k . null
        assert_equal {null} [r JSON.TYPE k .]
    }

    test {JSON.TYPE array} {
        r JSON.SET k . {[1]}
        assert_equal {array} [r JSON.TYPE k .]
    }

    test {JSON.DEL} {
        r JSON.SET k . 1
        assert_equal 1 [r JSON.DEL k]
        assert_equal 0 [r EXISTS k]
    }

    test {JSON.DEL non-existent key} {
        assert_equal 0 [r JSON.DEL nosuch]
    }

    test {JSON.NUMINCRBY} {
        r JSON.SET k . 10
        assert_equal 15 [r JSON.NUMINCRBY k . 5]
    }

    test {JSON.ARRLEN} {
        r JSON.SET k . {[1,2,3]}
        assert_equal 3 [r JSON.ARRLEN k .]
    }

    test {JSON.STRLEN} {
        r JSON.SET k . {"hello"}
        assert_equal 5 [r JSON.STRLEN k]
    }

    test {JSON.ARRAPPEND} {
        r JSON.SET k . {[1,2]}
        assert_equal 3 [r JSON.ARRAPPEND k . 3]
    }

    test {JSON.OBJLEN} {
        r JSON.SET k . {{"x":1,"y":2}}
        assert_equal 2 [r JSON.OBJLEN k]
    }

    test {JSON.DEBUG MEMORY} {
        r JSON.SET k . 1
        assert {[r JSON.DEBUG MEMORY k] > 0}
    }

    test {JSON.RESP} {
        r JSON.SET k . 42
        assert_equal {42} [r JSON.RESP k .]
    }

    test {JSON.GET on non-JSON key} {
        r SET s hello
        catch {r JSON.GET s} err
        assert {[string match "*WRONGTYPE*" $err]}
    }
}
