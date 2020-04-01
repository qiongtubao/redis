#include <time.h>
#define CONFIG_RUN_ID_SIZE 40
typedef struct replidInfo {
    /* Replication (master) */
    char replid[CONFIG_RUN_ID_SIZE+1];  /* My current replication ID. */
    char replid2[CONFIG_RUN_ID_SIZE+1]; /* replid inherited from master*/
    long long master_repl_offset;   /* My current replication offset */
    long long master_repl_meaningful_offset; /* Offset minus latest PINGs. */
    long long second_replid_offset; /* Accept offsets up to this for replid2. */

    int repl_ping_slave_period;     /* Master pings the slave every N seconds */
    char *repl_backlog;             /* Replication backlog for partial syncs */
    long long repl_backlog_size;    /* Backlog circular buffer size */
    long long repl_backlog_histlen; /* Backlog actual data length */
    long long repl_backlog_idx;     /* Backlog circular buffer current offset,
                                       that is the next byte will'll write to.*/
    long long repl_backlog_off;     /* Replication "master offset" of first
                                       byte in the replication backlog buffer.*/
    time_t repl_backlog_time_limit; /* Time without slaves after the backlog
                                       gets released. */
    time_t repl_no_slaves_since;    /* We have no slaves since that time.
                                       Only valid if server.slaves len is 0. */
    int repl_min_slaves_to_write;   /* Min number of slaves to write. */
    int repl_min_slaves_max_lag;    /* Max lag of <count> slaves to write. */
    int repl_good_slaves_count;     /* Number of slaves with lag <= max_lag. */
    int repl_diskless_sync;         /* Master send RDB to slaves sockets directly. */
    int repl_diskless_load;         /* Slave parse RDB directly from the socket.
                                     * see REPL_DISKLESS_LOAD_* enum */
    int repl_diskless_sync_delay;   /* Delay to start a diskless repl BGSAVE. */
} replidInfo;