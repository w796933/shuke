//
// Created by Yu Yang <yyangplus@NOSPAM.gmail.com> on 2017-05-05
//

#ifndef _SHUKE_H_
#define _SHUKE_H_ 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include "defines.h"
#include "version.h"
#include "ae.h"
#include "anet.h"
#include "zmalloc.h"
#include "endianconv.h"
#include "list.h"
#include "log.h"
#include "ds.h"
#include "dpdk_module.h"

#include "himongo/async.h"

#define CONFIG_BINDADDR_MAX 16
#define TIME_INTERVAL 1000

#define TASK_RELOAD_ZONE     1
#define TASK_RELOAD_ALL      2

#define CONN_READ_N     0     /**< reading in a fixed number of bytes */
#define CONN_READ_LEN   1     /**< reading length bytes */
#define CONN_SWALLOW    2     /**< swallowing unnecessary bytes w/o storing */
#define CONN_WRITE_N    3     /**< writing out fixed number of bytes */
#define CONN_WRITE_LEN  4     /**< writing out the length bytes */
#define CONN_CLOSING    5     /**< closing this connection */
#define CONN_CLOSED     6     /**< connection is closed */
#define CONN_MAX_STATE  7     /**< Max state value (used for assertion) */

#define MAX_NUMA_NODES  32

enum taskStates {
    TASK_PENDING = 0,
    TASK_RUNNING = 1,
    TASK_ERROR = 2,
};

typedef struct numaNode_s {
    int numa_id;
    int main_lcore_id;
    int nr_lcore_ids;           // enabled lcores belong this numa node;
    zoneDict *zd;
} numaNode_t;

typedef struct _tcpServer {
    int ipfd[CONFIG_BINDADDR_MAX];  // only for tcp server(listening fd)
    int ipfd_count;

    aeEventLoop *el;
    pthread_t tid;
    struct list_head tcp_head;     // tcp connection list.

    char errstr[ERR_STR_LEN];
} tcpServer;

typedef struct _tcpConn {
    int fd;
    aeEventLoop *el;
    char buf[MAX_UDP_SIZE];
    size_t nRead;
    struct tcpContext *whead;
    struct tcpContext *wtail;
    struct _tcpServer *srv;

    char cip[IP_STR_LEN];
    int cport;

    char len[2];
    // when the query packet size is smaller than MAX_UDP_SIZE, data points to buf
    // otherwise it will point to dynamic allocated memory, in this case,
    // memory free is needed when finished.
    char *data;
    int state;

    long lastActiveTs;

    size_t dnsPacketSize;    // size of current dns query packet
    struct list_head node;     // for connection list
} tcpConn;

struct tcpContext {
    tcpConn *sock;

    struct tcpContext *next;

    size_t wcur;       // tcp write cursor.
    size_t wsize;      // total size of data size.
    char reply[];
};

typedef struct {
    int type;

    int status;

    char *dotOrigin;  // origin in <label dot> format
    uint32_t sn;      // serial number in current cache.
    // last reload timestamp, if it is -1 then dotOrigin is a new zone.
    long ts;

    RRParser *psr;
    size_t nr_names;  // number of pending names.
    // never free or decrement reference count of old_zn, it will handle by zoneDict automatically.
    zone *old_zn;
    zone *new_zn;
}zoneReloadContext;

struct shuke {
    char errstr[ERR_STR_LEN];

    // config
    char *configfile;

    char *coremask;
    int master_lcore_id;
    char *kni_tx_config;
    char *kni_kernel_config;
    char *mem_channels;
    int portmask;
    bool promiscuous_on;
    bool numa_on;
    bool jumbo_on;
    int max_pkt_len;
    char *rx_queue_config;

    char *bindaddr[CONFIG_BINDADDR_MAX];
    int bindaddr_count;
    int port;
    char *pidfile;
    bool daemonize;
    char *query_log_file;
    char *logLevelStr;
    char *logfile;
    bool logVerbose;

    int tcp_backlog;
    int tcp_keepalive;
    int tcp_idle_timeout;
    int max_tcp_connections;

    char *zone_files_root;
    dict *zone_files_dict;

    char *redis_host;
    int redis_port;
    char *redis_zone_prefix;
    char *redis_soa_prefix;
    char *redis_origins_key;

    char *mongo_host;
    int mongo_port;
    char *mongo_dbname;

    long retry_interval;

    char *admin_host;
    int admin_port;

    char *data_store;
    int all_reload_interval;
    bool minimize_resp;
    // end config

    /*
     * these fields will allocate using malloc
     */
    // MAP: lcore_id => lcore_conf_t
    lcore_conf_t lcore_conf[RTE_MAX_LCORE];
    // MAP: portid => port_kni_conf_t*
    port_kni_conf_t *kni_conf[RTE_MAX_ETHPORTS];

    //MAP: socketid => numaNode_t*
    numaNode_t *nodes[MAX_NUMA_NODES];
    int numa_ids[MAX_NUMA_NODES];
    int nr_numa_id;

    int master_numa_id;

    int *lcore_ids;
    int nr_lcore_ids;
    int *kni_tx_lcore_ids;
    int nr_kni_tx_lcore_id;
    int *port_ids;
    int nr_ports;
    // char *total_coremask;
    char *total_lcore_list;
    // end

    // pointer to master numa node's zoneDict instance
    zoneDict *zd;
    struct rb_root rbroot;

    volatile bool force_quit;
    FILE *query_log_fp;

    int (*syncGetAllZone)(void);
    int (*initAsyncContext)(void);
    int (*checkAsyncContext)(void);
    /*
     * these two functions should be called only in mainThreadCron.
     *
     * if zone reloading is needed, just enqueue an zoneReloadContext to task queue.
     * if all zone reloading is needed, just set `last_all_reload_ts` to `now-all_reload_interval`
     * then mainThreadCron will do reloading automatically.
     */
    int (*asyncReloadAllZone)(void);
    int (*asyncReloadZone)(zoneReloadContext *t);

    // redis context
    // it will be NULL when shuke is disconnected with redis,
    // redisAsyncContext *redis_ctx;

    // mongo context
    // it will be NULL when shuke is disconnected with mongodb
    mongoAsyncContext *mongo_ctx;
    long last_retry_ts;

    // admin server
    int fd;
    dict *commands;
    struct list_head head;

    // dns tcp server
    tcpServer *tcp_srv;

    int arch_bits;
    long last_all_reload_ts; // timestamp of last all reload


    aeEventLoop *el;      // event loop for main thread.

    long unixtime;
    long long mstime;

    time_t    starttime;     // server start time
    long long zone_load_time;


    uint64_t hz;		/**< Number of events per seconds */

    // statistics
    rte_atomic64_t nr_req;                   // number of processed requests
    rte_atomic64_t nr_dropped;

    uint64_t num_tcp_conn;
    uint64_t total_tcp_conn;
    uint64_t rejected_tcp_conn;
};

/*----------------------------------------------
 *     Extern declarations
 *---------------------------------------------*/
extern struct shuke sk;
extern dictType commandTableDictType;
extern dictType zoneFileDictType;

int snpack(char *buf, int offset, size_t size, char const *fmt, ...);
/*----------------------------------------------
 *     zoneReloadContext
 *---------------------------------------------*/
zoneReloadContext *zoneReloadContextCreate(char *dotOrigin, zone *old_zn);
void zoneReloadContextReset(zoneReloadContext *t);
void zoneReloadContextDestroy(zoneReloadContext *t);

int asyncReloadZoneRaw(char *dotOrigin, zone *old_zn);
int asyncRereloadZone(zoneReloadContext *t);

/*----------------------------------------------
 *     admin server
 *---------------------------------------------*/
int initAdminServer(void);
void releaseAdminServer(void);

/*----------------------------------------------
 *     tcp server
 *---------------------------------------------*/
tcpServer *tcpServerCreate();
int tcpServerCron(struct aeEventLoop *el, long long id, void *clientData);
void tcpConnAppendDnsResponse(tcpConn *conn, char *resp, size_t respLen);

/*----------------------------------------------
 *     mongo
 *---------------------------------------------*/
int initMongo(void);
int checkMongo(void);
int mongoGetAllZone(void);
int mongoAsyncReloadZone(zoneReloadContext *t);
int mongoAsyncReloadAllZone(void);

int processUDPDnsQuery(char *buf, size_t sz, char *resp, size_t respLen, char *src_addr, uint16_t src_port, bool is_ipv4,
                       numaNode_t *node);

int processTCPDnsQuery(tcpConn *conn, char *buf, size_t sz);

void deleteZoneOtherNuma(char *origin);
void reloadZoneOtherNuma(zone *z);

int masterZoneDictReplace(zone *z);
int masterZoneDictDelete(char *origin);
void refreshZone(zone* z);

#ifdef SK_TEST
int mongoTest(int argc, char *argv[]);
#endif

#endif /* _SHUKE_H_ */
