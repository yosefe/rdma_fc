/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2018.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#define _GNU_SOURCE
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>
#include <rdma/rdma_cma.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdlib.h>
#include <assert.h>
#include <getopt.h>
#include <string.h>
#include <malloc.h>
#include <netdb.h>

#include "list.h"


enum log_level {
    LOG_LEVEL_ERROR,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_TRACE
};

enum transport_type {
    XPORT_RC,
    XPORT_DC
};

enum packet_type {
    PACKET_SYN  = 0,
    PACKET_FIN  = 1,
    PACKET_LAST
};

#define LOG(_level, _fmt, ...) \
    { \
        if (LOG_LEVEL_##_level <= g_options.log_level) { \
            printf("%12s:%-4d %-5s " _fmt "\n", basename(__FILE__), \
                   __LINE__, #_level, ## __VA_ARGS__); \
        } \
    }
#define LOG_ERROR(_fmt, ...)   LOG(ERROR, _fmt, ## __VA_ARGS__)
#define LOG_INFO(_fmt, ...)    LOG(INFO,  _fmt, ## __VA_ARGS__)
#define LOG_DEBUG(_fmt, ...)   LOG(DEBUG, _fmt, ## __VA_ARGS__)
#define LOG_TRACE(_fmt, ...)   LOG(TRACE, _fmt, ## __VA_ARGS__)
#define BIT(_index)            (1ul << (_index))
#define DC_KEY                 0x1
#define MEM_ACCESS_FLAGS       (IBV_ACCESS_LOCAL_WRITE  | \
                                IBV_ACCESS_REMOTE_WRITE | \
                                IBV_ACCESS_REMOTE_READ)
#define IB_GRH_SIZE            40
#define NUM_DCI                8
#define MIN(a, b)              ({ \
                                  typeof(a) _a = (a);  \
                                  typeof(b) _b = (b);  \
                                  _a < _b ? _a : _b;   \
                               })
#define MAX(a, b)              ({ \
                                  typeof(a) _a = (a);  \
                                  typeof(b) _b = (b);  \
                                  _a > _b ? _a : _b;   \
                               })

/* Test options */
typedef struct {
    int                       log_level;
    const char                *dest_address;
    unsigned                  port_num;
    unsigned                  conn_backlog;
    unsigned                  num_connections;
    int                       conn_timeout_ms;
    enum transport_type       transport;
    enum ibv_wr_opcode        opcode;
    size_t                    message_size;
    size_t                    alignment;
    unsigned                  num_iterations;
    size_t                    max_outstanding;
    size_t                    max_seg_size;
    size_t                    tx_queue_len;
    size_t                    rx_queue_len;
    size_t                    max_send_sge;
    size_t                    max_recv_sge;
    unsigned                  max_rd_atomic;
    unsigned                  min_rnr_timer;
    unsigned                  xport_timeout;
    unsigned                  rnr_retry;
    unsigned                  xport_retry_cnt;
    uint8_t                   gid_index;
    uint8_t                   hop_limit;
    uint8_t                   traffic_class;
    uint8_t                   sl;
} options_t;

/* Connection to remote peer (on either client or server) */
typedef struct connection connection_t;
struct connection {
    struct rdma_cm_id         *rdma_id;
    int                       remote_conn_index;
    struct ibv_ah             *dc_ah;
    uint32_t                  remote_dctn;
    uint32_t                  rkey;
    uint64_t                  remote_addr;
    size_t                    message_offset;
    unsigned                  num_recvd[PACKET_LAST];
    list_link_t               list;
};

/* Registered memory */
typedef struct {
    void                      *ptr;
    struct ibv_mr             *mr;
} buffer_t;

/* Global test context */
typedef struct {
    struct rdma_event_channel *event_ch;
    struct rdma_cm_id         *listen_cm_id;
    struct ibv_cq             *cq;
    struct ibv_srq            *srq;
    buffer_t                  recv_buf;
    buffer_t                  rdma_buf;
    struct ibv_exp_dct        *dct;
    struct ibv_qp             *dcis[NUM_DCI];
    unsigned                  dci_outstanding[NUM_DCI];
    uint64_t                  free_dci_bitmap;
    connection_t              *conns;
    unsigned                  num_conns;
    unsigned                  num_established;
    unsigned                  num_disconnect;
    unsigned                  barrier_count;
    unsigned                  recv_available;
    unsigned long             num_outstanding;
} test_context_t;

/* Control packet */
typedef struct {
    uint8_t                   type;        /* packet_type */
    uint32_t                  conn_index;  /* index of connection to FIN */
    int                       id;
} packet_t;

/* Private data passed over rdma_cm protocol */
typedef struct {
    uint32_t                  conn_index;  /* Index of connection in the array */
    uint32_t                  dct_num;     /* DCT number (for DC only) */
    uint32_t                  rkey;        /* Remote key for RDMA */
    uint64_t                  virt_addr;   /* Remote buffer address for RDMA */
} conn_priv_t;

static const char* transport_names[] = {
    [XPORT_RC]          = "RC",
    [XPORT_DC]          = "DC"
};

static const char* opcode_names[] = {
    [IBV_WR_SEND]       = "SEND",
    [IBV_WR_RDMA_READ]  = "RDMA_READ",
    [IBV_WR_RDMA_WRITE] = "RDMA_WRITE",
};

static const char* wc_opcode_names[] = {
    [IBV_WC_SEND]       = "SEND",
    [IBV_WC_RDMA_READ]  = "RDMA_READ",
    [IBV_WC_RDMA_WRITE] = "RDMA_WRITE",
};

static const char* packet_names[] = {
    [PACKET_SYN] = "SYN",
    [PACKET_FIN] = "FIN"
};

options_t g_options = {
    .log_level             = LOG_LEVEL_INFO,
    .dest_address          = "",
    .port_num              = 20000,
    .conn_backlog          = 1000,
    .num_connections       = 1,
    .conn_timeout_ms       = 2000,
    .transport             = XPORT_RC,
    .opcode                = IBV_WR_RDMA_READ,
    .message_size          = 1024 * 1024,
    .alignment             = 64,
    .num_iterations        = 1000,
    .max_outstanding       = SIZE_MAX,
    .max_seg_size          = SIZE_MAX,
    .tx_queue_len          = 128,
    .rx_queue_len          = 128,
    .max_send_sge          = 1,
    .max_recv_sge          = 1,
    .max_rd_atomic         = 8,
    .min_rnr_timer         = 17,
    .xport_timeout         = 17,
    .rnr_retry             = 7,
    .xport_retry_cnt       = 7,
    .gid_index             = 3,
    .hop_limit             = 64,
    .traffic_class         = 0,
    .sl                    = 0
};

test_context_t g_test = {
    .event_ch        = NULL,
    .listen_cm_id    = NULL,
    .cq              = NULL,
    .srq             = NULL,
    .recv_buf = {
         .ptr        = NULL,
         .mr         = NULL
     },
    .rdma_buf = {
        .ptr         = NULL,
        .mr          = NULL
    },
    .dct             = NULL,
    .conns           = NULL,
    .num_conns       = 0,
    .num_established = 0,
    .num_disconnect  = 0,
    .barrier_count   = 0,
    .recv_available  = 0
};

static int poll_cq();

static void usage(const options_t *defaults) {
    printf("Usage: many2one [ options ] [ server-address]\n");
    printf("Common options:\n");
    printf("   -p <num>       Server port number to use (%d)\n", defaults->port_num);
    printf("   -x <transport> Which RDMA transport to use (%s)\n", transport_names[defaults->transport]);
    printf("                  possible values are:\n");
    printf("                    'rc' : Reliable Connection (RC) transport\n");
    printf("                    'dc' : Dynamic Connection (DC) transport\n");
    printf("   -w <opcode>    WR opcode to use [RDMA_READ|RDMA_WRITE|SEND] (%s)\n", opcode_names[defaults->opcode]);
    printf("   -s <size>      Message size (%zu)\n", defaults->message_size);
    printf("   -a <boundary>  Message buffer alignment(%zu)\n", defaults->alignment);
    printf("   -v             Increase logging level\n");
    printf("   -G <index>     (DC only) GID index to use (%d)\n", defaults->gid_index);
    printf("   -T <class>     (DC only) Traffic class / DSCP to use (%d)\n", defaults->traffic_class);
    printf("   -H <limit>     (DC only) Hop limit / TTL (%d)\n", defaults->hop_limit);
    printf("   -S <sl>        (DC only) SL / Ethernet priority (%d)\n", defaults->sl);
    printf("   -t <ms>        Connection timeout, milliseconds (%d)\n", defaults->conn_timeout_ms);
    printf("   -n <num>       Number of connections to expect (%d)\n", defaults->num_connections);
    printf("   -i <num>       Number of iterations to run (%d)\n", defaults->num_iterations);
    printf("   -r <size>      Maximal size of a single segment (%zu)\n", defaults->max_seg_size);
    printf("   -o <num>       Maximal number of outstanding segments (%zu)\n", defaults->max_outstanding);
}

static int value_by_str(const char *str, const char **names, int max)
{
    int i;

    for (i = 0; i < max; ++i) {
        if (names[i] && !strcasecmp(str, names[i])) {
            return i;
        }
    }

    return -1;
}

static int parse_opts(int argc, char **argv) {
    options_t defaults = g_options;
    int ret, c;

    while ( (c = getopt(argc, argv, "hp:n:vt:s:a:x:w:i:r:o:G:T:H:S:")) != -1 ) {
        switch (c) {
        case 'p':
            g_options.port_num = atoi(optarg);
            break;
        case 'n':
            g_options.num_connections = atoi(optarg);
            break;
        case 'v':
            ++g_options.log_level;
            break;
        case 't':
            g_options.conn_timeout_ms = atoi(optarg);
            break;
        case 's':
            g_options.message_size = strtol(optarg, NULL, 0);
            break;
        case 'a':
            g_options.alignment = atol(optarg);
            break;
        case 'x':
            ret = value_by_str(optarg, transport_names,
                               sizeof(transport_names) / sizeof(transport_names[0]));
            if (ret < 0) {
                LOG_ERROR("Invalid transport name '%s'", optarg);
                usage(&defaults);
                return ret;
            }

            g_options.transport = ret;
            break;
        case 'w':
            ret = value_by_str(optarg, opcode_names,
                               sizeof(opcode_names) / sizeof(opcode_names[0]));
            if (ret < 0) {
                LOG_ERROR("Invalid opcode name '%s'", optarg);
                usage(&defaults);
                return ret;
            }

            g_options.opcode = ret;
            break;
        case 'i':
            g_options.num_iterations = atoi(optarg);
            break;
        case 'r':
            g_options.max_seg_size = atol(optarg);
            break;
        case 'o':
            g_options.max_outstanding = atol(optarg);
            break;
        case 'G':
            g_options.gid_index = atoi(optarg);
            break;
        case 'T':
            g_options.traffic_class = atoi(optarg);
            break;
        case 'H':
            g_options.hop_limit = atoi(optarg);
            break;
        case 'S':
            g_options.sl = atoi(optarg);
            break;
        case 'h':
            usage(&defaults);
            exit(0);
        default:
            LOG_ERROR("Invalid option '%c'", c);
            usage(&defaults);
            return -1;
        }
    }

    if (optind < argc) {
        g_options.dest_address    = argv[optind++];
    }

    return 0;
}

static int init_buffer(struct ibv_pd *pd, size_t size, buffer_t *buf)
{
    if (buf->ptr) {
        return 0; /* already initialized */
    }

    buf->ptr = memalign(g_options.alignment, size);
    if (!buf->ptr) {
        LOG_ERROR("Failed to allocate buffer");
        return -1;
    }

    buf->mr = ibv_reg_mr(pd, buf->ptr, size, MEM_ACCESS_FLAGS);
    if (!buf->mr) {
        LOG_ERROR("ibv_reg_mr() failed: %m");
        return -1;
    }

    LOG_DEBUG("Registered buffer %p length %zu lkey 0x%x rkey 0x%x",
              buf->ptr, size, buf->mr->lkey, buf->mr->rkey);
    return 0;
}

static void cleanup_buffer(buffer_t *buf)
{
    if (buf->ptr) {
        free(buf->ptr);
    }
    if (buf->mr) {
        ibv_dereg_mr(buf->mr);
    }
}

static int init_test()
{
    /* Create rdma_cm event channel */
    g_test.event_ch = rdma_create_event_channel();
    if (!g_test.event_ch) {
        LOG_ERROR("rdma_create_event_channel() failed: %m");
        return -1;
    }

    /* Allocate array of connections */
    g_test.conns = calloc(g_options.num_connections, sizeof(*g_test.conns));
    if (!g_test.conns) {
        LOG_ERROR("failed to allocate connections array");
        return -1;
    }

    return 0;
}

static void cleanup_test()
{
    unsigned i;

    for (i = 0; i < g_test.num_conns; ++i) {
        if (g_test.conns[i].dc_ah) {
            ibv_destroy_ah(g_test.conns[i].dc_ah);
        }
        if (g_test.conns[i].rdma_id != NULL) {
            rdma_destroy_id(g_test.conns[i].rdma_id);
        }
    }
    for (i = 0; i < NUM_DCI; ++i) {
        if (g_test.dcis[i]) {
            ibv_destroy_qp(g_test.dcis[i]);
        }
    }
    if (g_test.srq) {
        ibv_destroy_srq(g_test.srq);
    }
    if (g_test.cq) {
        ibv_destroy_cq(g_test.cq);
    }
    cleanup_buffer(&g_test.rdma_buf);
    cleanup_buffer(&g_test.recv_buf);
    if (g_test.listen_cm_id) {
        rdma_destroy_id(g_test.listen_cm_id);
    }
    free(g_test.conns);
    rdma_destroy_event_channel(g_test.event_ch);
}

static int get_address(struct sockaddr_in *in_addr)
{
    struct hostent *he = gethostbyname(g_options.dest_address);
    if (!he || !he->h_addr_list) {
        LOG_ERROR("host %s not found: %s", g_options.dest_address,
                  hstrerror(h_errno));
        return -1;
    }

    if (he->h_addrtype != AF_INET) {
        LOG_ERROR("Only IPv4 addresses are supported");
        return -1;
    }

    if (he->h_length != sizeof(struct in_addr)) {
        LOG_ERROR("Mismatching address length");
        return -1;
    }

    memset(in_addr, 0, sizeof(*in_addr));
    in_addr->sin_family = AF_INET;
    in_addr->sin_port   = htons(g_options.port_num);
    in_addr->sin_addr   = *(struct in_addr*)he->h_addr_list[0];
    return 0;
}

static int init_dc_qps(struct rdma_cm_id *rdma_cm_id)
{
    struct ibv_exp_qp_init_attr qp_init_attr;
    struct ibv_exp_dct_init_attr dct_attr;
    struct ibv_port_attr port_attr;
    struct ibv_exp_qp_attr qp_attr;
    int i, ret;

    if (g_test.dct) {
        return 0; /* Already initialized */
    }

    ret = ibv_query_port(rdma_cm_id->verbs, rdma_cm_id->port_num, &port_attr);
    if (ret) {
        LOG_ERROR("ibv_query_port() failed: %m");
        return ret;
    }

    if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
        LOG_ERROR("This test can run only on Ethernet (RoCE) port");
        return -1;
    }

   /* Create DCT
     * Note: For RoCE, must specify gid_index, hop_limit, traffic_class
     *       on command line.
     * */
    memset(&dct_attr, 0, sizeof(dct_attr));
    dct_attr.pd            = rdma_cm_id->pd;
    dct_attr.cq            = g_test.cq;
    dct_attr.srq           = g_test.srq;
    dct_attr.dc_key        = DC_KEY;
    dct_attr.port          = rdma_cm_id->port_num;
    dct_attr.mtu           = port_attr.active_mtu;
    dct_attr.access_flags  = MEM_ACCESS_FLAGS;
    dct_attr.min_rnr_timer = g_options.min_rnr_timer;
    dct_attr.tclass        = g_options.traffic_class;
    dct_attr.hop_limit     = g_options.hop_limit;
    dct_attr.gid_index     = g_options.gid_index;

    g_test.dct = ibv_exp_create_dct(rdma_cm_id->verbs, &dct_attr);
    if (!g_test.dct) {
        LOG_ERROR("ibv_exp_create_dct() failed: %m");
        return -1;
    }

    LOG_DEBUG("Created DCT 0x%x tclass %d hlimit %d gid_index %d",
              g_test.dct->dct_num, dct_attr.tclass, dct_attr.hop_limit,
              dct_attr.gid_index);

    /* Create and initialize DC initiators */
    for (i = 0; i < NUM_DCI; ++i) {
        qp_init_attr.qp_type             = IBV_EXP_QPT_DC_INI;
        qp_init_attr.send_cq             = g_test.cq;
        qp_init_attr.recv_cq             = g_test.cq;
        qp_init_attr.srq                 = g_test.srq;
        qp_init_attr.cap.max_send_wr     = g_options.tx_queue_len;
        qp_init_attr.cap.max_recv_wr     = g_options.rx_queue_len;
        qp_init_attr.cap.max_send_sge    = g_options.max_send_sge;
        qp_init_attr.cap.max_recv_sge    = g_options.max_recv_sge;
        qp_init_attr.cap.max_inline_data = sizeof(packet_t);
        qp_init_attr.sq_sig_all          = 0;
        qp_init_attr.comp_mask           = IBV_EXP_QP_INIT_ATTR_PD;
        qp_init_attr.pd                  = rdma_cm_id->pd;

        g_test.dcis[i] = ibv_exp_create_qp(rdma_cm_id->verbs, &qp_init_attr);
        if (!g_test.dcis[i]) {
            LOG_ERROR("ibv_exp_create_qp() failed: %m");
            return -1;
        }

        memset(&qp_attr, 0, sizeof(qp_attr));
        qp_attr.path_mtu           = port_attr.active_mtu;
        qp_attr.max_dest_rd_atomic = g_options.max_rd_atomic;
        qp_attr.min_rnr_timer      = g_options.min_rnr_timer;
        qp_attr.timeout            = g_options.xport_timeout;
        qp_attr.rnr_retry          = g_options.rnr_retry;
        qp_attr.retry_cnt          = g_options.xport_retry_cnt;
        qp_attr.max_rd_atomic      = g_options.max_rd_atomic;
        qp_attr.port_num           = rdma_cm_id->port_num;
        qp_attr.qp_access_flags    = MEM_ACCESS_FLAGS;
        qp_attr.ah_attr.is_global  = 1;
        qp_attr.ah_attr.port_num   = rdma_cm_id->port_num;
        qp_attr.ah_attr.sl         = g_options.sl;
        qp_attr.rq_psn             = 0;
        qp_attr.sq_psn             = 0;
        qp_attr.dct_key            = DC_KEY;

        qp_attr.qp_state = IBV_QPS_INIT;
        int ret = ibv_exp_modify_qp(g_test.dcis[i], &qp_attr,
                                    IBV_EXP_QP_STATE      |
                                    IBV_EXP_QP_PKEY_INDEX |
                                    IBV_EXP_QP_PORT       |
                                    IBV_EXP_QP_DC_KEY);
        if (ret) {
            LOG_ERROR("ibv_exp_modify_qp(INIT) failed: %m");
            return -1;
        }

        qp_attr.qp_state = IBV_QPS_RTR;
        ret = ibv_exp_modify_qp(g_test.dcis[i], &qp_attr,
                                IBV_EXP_QP_STATE    |
                                IBV_EXP_QP_PATH_MTU |
                                IBV_EXP_QP_AV);
        if (ret) {
            LOG_ERROR("ibv_exp_modify_qp(RTR) failed: %m");
            return -1;
        }

        qp_attr.qp_state = IBV_QPS_RTS;
        ret = ibv_exp_modify_qp(g_test.dcis[i], &qp_attr,
                                IBV_EXP_QP_STATE      |
                                IBV_EXP_QP_TIMEOUT    |
                                IBV_EXP_QP_RETRY_CNT  |
                                IBV_EXP_QP_RNR_RETRY  |
                                IBV_EXP_QP_MAX_QP_RD_ATOMIC);
        if (ret) {
            LOG_ERROR("ibv_exp_modify_qp(RTS) failed: %m");
            return -1;
        }

        g_test.dci_outstanding[i] = 0;
        LOG_DEBUG("Created DCI[%d]=0x%x", i, g_test.dcis[i]->qp_num);
    }

    g_test.free_dci_bitmap = -1;

    return 0;
}

static int post_receives()
{
    struct ibv_recv_wr wr, *bad_wr;
    struct ibv_sge sge;
    unsigned i;
    void *ptr;
    int ret;

    /* We post receives only once, should have enough to handle a control
     * message for every connection (FIN)
     */
    for (i = 0; i < g_test.recv_available; ++i) {
        /* sge.addr points to grh, wr_id points after grh */
        ptr        = g_test.recv_buf.ptr + (i * sizeof(packet_t));
        wr.next    = NULL;
        wr.num_sge = 1;
        wr.sg_list = &sge;
        sge.addr   = (uintptr_t)ptr;
        sge.length = sizeof(packet_t);
        sge.lkey   = g_test.recv_buf.mr->lkey;
        wr.wr_id   = (uintptr_t)ptr;

        ret = ibv_post_srq_recv(g_test.srq, &wr, &bad_wr);
        if (ret) {
            LOG_ERROR("ibv_post_srq_recv() failed: %m");
            return ret;
        }
    }

    if (g_test.recv_available) {
        LOG_DEBUG("Posted %d receives", g_test.recv_available);
        g_test.recv_available = 0;
    }

    return 0;
}

static int init_transport(struct rdma_cm_event *event)
{
    struct ibv_srq_init_attr srq_init_attr;
    struct ibv_qp_init_attr qp_init_attr;
    int ret;

    if (!g_test.cq) {
        /* Create single completion queue for both sends and receives */
        g_test.cq = ibv_create_cq(event->id->verbs,
                                  g_options.tx_queue_len + g_options.rx_queue_len, /* cqe */
                                  NULL, /* cq_context */
                                  NULL, /* comp_channel */
                                  0  /* comp_vector */ );
        if (!g_test.cq) {
            LOG_ERROR("ibv_create_cq() failed: %m");
            return -1;
        }

        LOG_DEBUG("Created CQ @%p", g_test.cq)
    }

    if (!g_test.srq) {
        /* Create a shared receive queue for control messages */
        srq_init_attr.srq_context    = NULL;
        srq_init_attr.attr.max_wr    = g_options.rx_queue_len;
        srq_init_attr.attr.max_sge   = g_options.max_recv_sge;
        srq_init_attr.attr.srq_limit = 0;
        g_test.srq = ibv_create_srq(event->id->pd, &srq_init_attr);
        if (!g_test.srq) {
            LOG_ERROR("ibv_create_srq() failed: %m");
            return -1;
        }

        g_test.recv_available = g_options.rx_queue_len;

        LOG_DEBUG("Created SRQ @%p", g_test.srq);
    }

    /* Create buffer for receives */
    ret = init_buffer(event->id->pd, g_options.rx_queue_len * sizeof(packet_t),
                      &g_test.recv_buf);
    if (ret) {
        return ret;
    }

    /* Create buffer for RDMA (one buffer which is split between connections) */
    ret = init_buffer(event->id->pd,
                      g_options.num_connections * g_options.message_size,
                      &g_test.rdma_buf);
    if (ret) {
        return ret;
    }

    ret = post_receives();
    if (ret) {
        return ret;
    }

    if (g_options.transport == XPORT_RC) {
        /* Create RC QP for the connection */
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        qp_init_attr.qp_type             = IBV_QPT_RC;
        qp_init_attr.send_cq             = g_test.cq;
        qp_init_attr.recv_cq             = g_test.cq;
        qp_init_attr.srq                 = g_test.srq;
        qp_init_attr.cap.max_send_wr     = g_options.tx_queue_len;
        qp_init_attr.cap.max_recv_wr     = g_options.rx_queue_len;
        qp_init_attr.cap.max_send_sge    = g_options.max_send_sge;
        qp_init_attr.cap.max_recv_sge    = g_options.max_recv_sge;
        qp_init_attr.cap.max_inline_data = 0;
        qp_init_attr.sq_sig_all          = 0;

        ret = rdma_create_qp(event->id, event->id->pd, &qp_init_attr);
        if (ret) {
            LOG_ERROR("rdma_create_qp() failed: %m");
            return ret;
        }

        LOG_DEBUG("Created RC QP 0x%x", event->id->qp->qp_num);
    } else if (g_options.transport == XPORT_DC) {
        /* Initialize DC objects (done only once) */
        ret = init_dc_qps(event->id);
        if (ret) {
            return ret;
        }
    }

    return 0;
}

static int get_conn_param(struct rdma_cm_id *rdma_id,
                          struct rdma_conn_param *conn_param,
                          conn_priv_t *priv, unsigned conn_index)
{
    memset(conn_param, 0, sizeof(*conn_param));

    priv->conn_index          = conn_index;
    priv->rkey                = g_test.rdma_buf.mr->rkey;
    priv->virt_addr           = (uint64_t)g_test.rdma_buf.ptr +
                                (conn_index * g_options.message_size);
    if (g_options.transport == XPORT_DC) {
        priv->dct_num         = g_test.dct->dct_num;
    }

    conn_param->private_data        = priv;
    conn_param->private_data_len    = sizeof(*priv);
    conn_param->responder_resources = g_options.max_rd_atomic;
    conn_param->initiator_depth     = g_options.max_rd_atomic;
    conn_param->retry_count         = g_options.xport_retry_cnt;
    conn_param->rnr_retry_count     = g_options.rnr_retry;

    return 0;
}

/* get this from rdmacm */
int rdma_init_qp_attr(struct rdma_cm_id *id, struct ibv_qp_attr *qp_attr,
                      int *qp_attr_mask);

static int set_conn_param(connection_t *conn, struct rdma_cm_event *event)
{
    const conn_priv_t *remote_priv = event->param.conn.private_data;
    struct ibv_qp_attr qp_attr = {0};
    int qp_attr_mask = 0;
    int ret;

    conn->remote_addr       = remote_priv->virt_addr;
    conn->rkey              = remote_priv->rkey;
    conn->remote_conn_index = remote_priv->conn_index;

    if (g_options.transport == XPORT_DC) {
        qp_attr.qp_state = IBV_QPS_RTR;
        ret = rdma_init_qp_attr(event->id, &qp_attr, &qp_attr_mask);
        if (ret) {
            LOG_ERROR("rdma_init_qp_attr() failed: %m");
            return ret;
        }

        conn->dc_ah = ibv_create_ah(event->id->pd, &qp_attr.ah_attr);
        if (!conn->dc_ah) {
            LOG_ERROR("ibv_create_ah(port_num=%d) failed: %m",
                      qp_attr.ah_attr.port_num);
            return -1;
        }

        conn->remote_dctn = remote_priv->dct_num;
        LOG_DEBUG("DC ah @%p remote_dctn 0x%x", conn->dc_ah, conn->remote_dctn);
    }

    return 0;
}

static connection_t* add_connection(struct rdma_cm_event *event)
{
    connection_t *conn;
    int ret;

    conn = &g_test.conns[g_test.num_conns];
    conn->rdma_id = event->id;

    ret = set_conn_param(conn, event);
    if (ret) {
        return NULL;
    }

    ++g_test.num_conns;
    return conn;
}

static int handle_connect_request(struct rdma_cm_event *event)
{
    struct rdma_conn_param conn_param;
    conn_priv_t priv;
    connection_t *conn;
    int ret;

    ret = init_transport(event);
    if (ret) {
        return ret;
    }

    /* add a new connection and set its parameters according to private_data
     * received from the client
     */
    conn = add_connection(event);
    if (!conn) {
        return -1;
    }

    LOG_DEBUG("calling rdma_accept()");

    ret = get_conn_param(event->id, &conn_param, &priv, conn - g_test.conns);
    if (ret) {
        return ret;
    }

    /* send accept message to the client with our own parameters in private_data */
    ret = rdma_accept(event->id, &conn_param);
    if (ret) {
        LOG_ERROR("rdma_accept() failed: %m");
        return -1;
    }

    return 0;
}

static int is_client()
{
    return strlen(g_options.dest_address);
}

/* post post_operation, set *posted to 1 if the operation was posted, 0 if no
 * available QP is found
 */
static int post_send(connection_t *conn, struct ibv_sge *sge,
                     enum ibv_wr_opcode opcode, int send_flags,
                     uintptr_t remote_addr, int *posted)
{
    char buf[100];
    packet_t *hdr;
    int dci;

    switch (opcode) {
    case IBV_WR_SEND:
        hdr = (packet_t*)sge->addr;
        snprintf(buf, sizeof(buf), "%s id %d", packet_names[hdr->type],\
                 hdr->id);
        break;
    case IBV_WR_RDMA_READ:
         snprintf(buf, sizeof(buf), "remote_address 0x%lx into 0x%lx",
                  remote_addr, sge->addr);
         break;
    case IBV_WR_RDMA_WRITE:
         snprintf(buf, sizeof(buf), "remote_address 0x%lx from 0x%lx",
                  remote_addr, sge->addr);
         break;
    default:
        snprintf(buf, sizeof(buf), "???");
        break;
   }

    if (g_options.transport == XPORT_RC) {
        struct ibv_send_wr wr, *bad_wr;

        memset(&wr, 0, sizeof(wr));
        wr.sg_list             = sge;
        wr.num_sge             = 1;
        wr.opcode              = opcode;
        wr.send_flags          = IBV_SEND_SIGNALED | send_flags;
        wr.wr.rdma.remote_addr = remote_addr;
        wr.wr.rdma.rkey        = conn->rkey;

        int ret = ibv_post_send(conn->rdma_id->qp, &wr, &bad_wr);
        if (ret) {
            LOG_DEBUG("ibv_post_send() failed: %m");
            return ret;
        }

        *posted = 1;

        LOG_TRACE("%s on QP 0x%x %s length %u",
                  opcode_names[opcode], conn->rdma_id->qp->qp_num, buf,
                  sge->length);
    } else if (g_options.transport == XPORT_DC) {

        struct ibv_exp_send_wr wr, *bad_wr;

        if (!g_test.free_dci_bitmap) {
            *posted = 0;
            goto out;
        }

        /* find an available DCI */
        dci = __builtin_ffsl(g_test.free_dci_bitmap) - 1;
        assert(g_test.dci_outstanding[dci] == 0);

        memset(&wr, 0, sizeof(wr));
        wr.wr_id               = dci;
        wr.sg_list             = sge;
        wr.num_sge             = 1;
        wr.exp_opcode          = opcode;
        wr.exp_send_flags      = IBV_EXP_SEND_SIGNALED | send_flags;
        wr.wr.rdma.remote_addr = remote_addr;
        wr.wr.rdma.rkey        = conn->rkey;
        wr.dc.ah               = conn->dc_ah;
        wr.dc.dct_number       = conn->remote_dctn;
        wr.dc.dct_access_key   = DC_KEY;

        int ret = ibv_exp_post_send(g_test.dcis[dci], &wr, &bad_wr);
        if (ret) {
            LOG_DEBUG("ibv_post_send() failed: %m");
            return ret;
        }

        ++g_test.dci_outstanding[dci];
        g_test.free_dci_bitmap &= ~BIT(dci);
        *posted = 1;

        LOG_TRACE("%s on DCI[%d] 0x%x %s length %u",
                  opcode_names[opcode], dci, g_test.dcis[dci]->qp_num, buf,
                  sge->length);
    } else {
        *posted = 0;
    }

    /* Update counters */
    if (*posted) {
        ++g_test.num_outstanding;
    }

out:
    return 0;
}

/* send control message */
static int send_control(connection_t *conn, enum packet_type type, int id)
{
    packet_t packet = { .type       = type,
                        .conn_index = conn->remote_conn_index,
                        .id         = id };
    struct ibv_sge sge;
    int posted;
    int ret;

    sge.addr   = (uintptr_t)&packet;
    sge.length = sizeof(packet);
    sge.lkey   = 0;

    assert((int)IBV_SEND_INLINE == (int)IBV_EXP_SEND_INLINE);

    for (;;) {
        ret = post_send(conn, &sge, IBV_WR_SEND, IBV_SEND_INLINE, 0, &posted);
        if (ret < 0) {
            return ret;
        } else if (posted) {
            break;
        }

        ret = poll_cq();
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int post_send_message(connection_t *conn, size_t offset, size_t length,
                             int *posted)
{
    uintptr_t remote_addr;
    struct ibv_sge sge;
    int ret;

    sge.addr    = (uintptr_t)g_test.rdma_buf.ptr + offset;
    sge.length  = length;
    sge.lkey    = g_test.rdma_buf.mr->lkey;

    if (g_options.opcode == IBV_WR_RDMA_READ || g_options.opcode == IBV_WR_RDMA_WRITE) {
        remote_addr = conn->remote_addr + offset;
    } else {
        remote_addr = 0;
    }

    ret = post_send(conn, &sge, g_options.opcode, 0, remote_addr, posted);
    if (ret < 0) {
        return ret;
    }

    if (*posted) {
        conn->message_offset += length;
    }
    return ret;
}

static int handle_established(struct rdma_cm_event *event)
{
    connection_t *conn;

    if (is_client()) {
        /* Add new (and only) connection on client side */
        conn = add_connection(event);
        if (!conn) {
            return -1;
        }
    }

    ++g_test.num_established;
    return 0;
}

/* wait and process single event */
static int wait_and_process_one_event(uint64_t event_mask) {
    struct rdma_conn_param conn_param;
    struct rdma_cm_event *event;
    conn_priv_t priv;
    int ret;

    ret = rdma_get_cm_event(g_test.event_ch, &event);
    if (ret) {
        LOG_ERROR("rdma_get_cm_event() failed: %m");
        return -1;
    }

    if (!(BIT(event->event) & event_mask)) {
        LOG_ERROR("Unexpected event %s", rdma_event_str(event->event));
        ret = -1;
        goto out_ack_event;
    }

    LOG_DEBUG("Got rdma_cm event %s", rdma_event_str(event->event));
    switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        ret = rdma_resolve_route(event->id, g_options.conn_timeout_ms);
        if (ret) {
            LOG_ERROR("rdma_resolve_route() failed: %m");
            goto out_ack_event;
        }
        break;
    case RDMA_CM_EVENT_ROUTE_RESOLVED:
        ret = init_transport(event);
        if (ret) {
            goto out_ack_event;
        }

        ret = get_conn_param(event->id, &conn_param, &priv, 0);
        if (ret) {
            return ret;
        }

        ret = rdma_connect(event->id, &conn_param);
        if (ret) {
            LOG_ERROR("rdma_connect() failed: %m");
            return -1;
        }

        break;
    case RDMA_CM_EVENT_CONNECT_REQUEST:
        ret = handle_connect_request(event);
        if (ret) {
            goto out_ack_event;
        }
        break;
    case RDMA_CM_EVENT_ESTABLISHED:
        ret = handle_established(event);
        if (ret) {
            goto out_ack_event;
        }
        break;
    case RDMA_CM_EVENT_DISCONNECTED:
        ++g_test.num_disconnect;
        break;
    default:
        break;
    }

    ret = rdma_ack_cm_event(event);
    if (ret) {
        LOG_ERROR("rdma_ack_cm_event() failed: %m");
        return ret;
    }

    return 0;

out_ack_event:
    (void)rdma_ack_cm_event(event);
    return ret;
}

static enum rdma_port_space get_port_space()
{
    return RDMA_PS_TCP;
}

static void dc_send_completion(const struct ibv_wc* wc)
{
    int dci = wc->wr_id;

    /* update outstanding sends counter and free dci bitmap */
    assert(wc->qp_num == g_test.dcis[dci]->qp_num);
    if (--g_test.dci_outstanding[dci] == 0) {
        g_test.free_dci_bitmap |= BIT(dci);
    }
}

static int poll_cq()
{
    connection_t *conn;
    packet_t *packet;
    struct ibv_wc wc;
    int ret;

    ret = ibv_poll_cq(g_test.cq, 1, &wc);
    if (ret < 0) {
        return ret;
    } else if (ret > 0) {
        if (wc.status != IBV_WC_SUCCESS) {
            LOG_ERROR("Completion with error: %s, vendor_err 0x%x",
                      ibv_wc_status_str(wc.status), wc.vendor_err);
            LOG_ERROR("Press any key to exit...");
            getchar();
            return -1;
        }

        switch (wc.opcode) {
        case IBV_WC_RECV:
            /* Packet was received */
            packet = (packet_t*)wc.wr_id;
            conn = &g_test.conns[packet->conn_index];
            ++conn->num_recvd[packet->type];
            LOG_TRACE("Received packet %s id %d conn_index %d at %p, total: %d",
                      packet_names[packet->type], packet->id, packet->conn_index,
                      packet, conn->num_recvd[packet->type]);
            break;
        case IBV_WC_SEND:
        case IBV_WC_RDMA_READ:
        case IBV_WC_RDMA_WRITE:
            LOG_TRACE("%s completion wr_id %ld", wc_opcode_names[wc.opcode],
                      wc.wr_id);
            --g_test.num_outstanding;
            if (g_options.transport == XPORT_DC) {
                dc_send_completion(&wc);
            }
            break;
        default:
            LOG_ERROR("Unexpected completion opcode %d", wc.opcode);
            return -1;
        }
    }
    return 0;
}

static int do_barrier();

static int disconnect()
{
    unsigned i;
    int ret;

    LOG_INFO("Disconnecting %d connections", g_test.num_conns);

    ret = do_barrier();
    if (ret < 0) {
        return ret;
    }

    /* Send rdma_cm disconnects on all connections */
    for (i = 0; i < g_test.num_conns; ++i) {
        ret = rdma_disconnect(g_test.conns[i].rdma_id);
        if (ret) {
            LOG_ERROR("rdma_disconnect() failed: %m");
            return ret;
        }
    }

    /* Wait for rdma_cm disconnects on all connections */
    while (g_test.num_disconnect < g_test.num_conns) {
        ret = wait_and_process_one_event(BIT(RDMA_CM_EVENT_DISCONNECTED));
        if (ret) {
            return ret;
        }
    }

    return 0;
}

static int connect_client()
{
    struct sockaddr_in dest_addr;
    struct rdma_cm_id *rdma_id;
    int i, ret;

    ret = get_address(&dest_addr);
    if (ret) {
        return ret;
    }

    for (i = 0; i < g_options.num_connections; ++i) {

        LOG_INFO("Connection[%d] to %s...", i, g_options.dest_address);

        ret = rdma_create_id(g_test.event_ch, &rdma_id, NULL, get_port_space());
        if (ret) {
            LOG_ERROR("rdma_create_id() failed: %m");
            return ret;
        }

        ret = rdma_resolve_addr(rdma_id, NULL, /* src_addr */
                                (struct sockaddr *)&dest_addr,
                                g_options.conn_timeout_ms);
        if (ret) {
            LOG_ERROR("rdma_resolve_addr() failed: %m");
            return -1;
        }
    }

    while (g_test.num_established < g_options.num_connections) {
        ret = wait_and_process_one_event(BIT(RDMA_CM_EVENT_ADDR_RESOLVED) |
                                         BIT(RDMA_CM_EVENT_ROUTE_RESOLVED) |
                                         BIT(RDMA_CM_EVENT_ESTABLISHED));
        if (ret) {
            return ret;
        }
    }

    return 0;
}

static int barrier_send()
{
    int i, ret;

    for (i = 0; i < g_test.num_conns; ++i) {
        ret = send_control(&g_test.conns[i], PACKET_SYN, g_test.barrier_count);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int barrier_recv()
{
    int done, i, ret;

    do {
        ret = poll_cq();
        if (ret < 0) {
            return ret;
        }

        /* expect each connection to receive the barrier */
        done = 1;
        for (i = 0; i < g_test.num_conns; ++i) {
            if (g_test.conns[i].num_recvd[PACKET_SYN] < g_test.barrier_count) {
                done = 0;
                break;
            }
        }
    } while (!done);

    return 0;
}

static int do_barrier()
{
    int ret;

    ++g_test.barrier_count;

    LOG_DEBUG("performing barrier # %d with %d connections",
              g_test.barrier_count, g_test.num_conns);

    if (is_client()) {
        ret = barrier_send();
        ret = (ret < 0) ? ret : barrier_recv();
    } else {
        ret = barrier_recv();
        ret = (ret < 0) ? ret : barrier_send();
    }

    LOG_DEBUG("done barrier # %d with %d connections",
              g_test.barrier_count, g_test.num_conns);

    return ret;
}

static int run_traffic()
{
    struct timeval tv_start, tv_end;
    connection_t *conn;
    double sec_elapsed;
    unsigned i, iter;
    size_t size, bytes_transferred;
    int do_post_send;
    int posted;
    int ret;
    LIST_HEAD(sched);

    /*
     * Which side should post sends?
     * client: for SEND and RDMA_WRITE
     * server: for RDMA_READ
     */
    do_post_send = (is_client() && ((g_options.opcode == IBV_WR_RDMA_WRITE ||
                    g_options.opcode == IBV_WR_SEND))) ||
                                    (!is_client() && (g_options.opcode == IBV_WR_RDMA_READ));

    g_test.num_outstanding = 0;

    /* measure the time over several iterations */
    gettimeofday(&tv_start, NULL);

    for (iter = 0; iter < g_options.num_iterations; ++iter) {

        /* insert all connections to the schedule queue */
        if (do_post_send) {
            for (i = 0; i < g_test.num_conns; ++i) {
                g_test.conns[i].message_offset = 0;
                list_add_tail(&sched, &g_test.conns[i].list);
            }
        }

        /* As long as not all read operations are completed, and the list is not
         * empty, continue working
         */
        while (g_test.num_outstanding || !list_is_empty(&sched)) {

            /* If there are more connections with pending data, and we have not
             * exceeded the total number of outstanding reads, post the next
             * read operation
             */
            if (!list_is_empty(&sched) &&
                (g_test.num_outstanding < g_options.max_outstanding))
            {
                /* take the first connection from the schedule queue */
                conn = list_extract_head(&sched, connection_t, list);

                /* see how many bytes are left to read */
                size = MIN(g_options.max_seg_size,
                           g_options.message_size - conn->message_offset);
                assert(size > 0);

                /* send next message */
                posted = 0;
                ret = post_send_message(conn, conn->message_offset, size, &posted);
                if (ret < 0) {
                    return ret;
                }

                if (posted) {
                    /* if this connection is not done, reinsert it to the tail of
                     * the schedule queue
                     */
                    if (conn->message_offset < g_options.message_size) {
                        list_add_tail(&sched, &conn->list);
                    }
                } else {
                    /* If we could not sent, reinsert to the head of the queue */
                    list_add_head(&sched, &conn->list);
                }
            }

            ret = poll_cq();
            if (ret) {
                return ret;
            }
        }
    }

    assert(g_test.num_outstanding == 0);

    gettimeofday(&tv_end, NULL);

    /* make sure everything was read */
    if (do_post_send) {
        for (i = 0; i < g_test.num_conns; ++i) {
            assert(g_test.conns[i].message_offset == g_options.message_size);
        }

        /* Calculate and report total read bandwidth */
        sec_elapsed = (tv_end.tv_sec - tv_start.tv_sec) +
                      (tv_end.tv_usec - tv_start.tv_usec) * 1e-6;
        bytes_transferred = g_test.num_conns *
                            g_options.num_iterations * g_options.message_size;
        LOG_INFO("Total bandwidth: %.2f MB/s",
                 bytes_transferred / sec_elapsed / BIT(20));
    }
    return 0;
}

static int run_traffic_multi()
{
    int i, ret;

    for (i = 0; i < 10; ++i) {
        ret = run_traffic();
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int connect_server()
{
    struct sockaddr_in in_addr;
    int ret;

    /* Make TX/RX queue lengths are large enough to send/recv control messages
     * on all connections without extra CQ polling (this is done for simplicity)
     */
    g_options.rx_queue_len = MAX(g_options.rx_queue_len,
                                 g_options.num_connections * 2);
    g_options.tx_queue_len = MAX(g_options.tx_queue_len,
                                 g_options.num_connections);

    ret = rdma_create_id(g_test.event_ch, &g_test.listen_cm_id, NULL,
                         get_port_space());
    if (ret) {
        LOG_ERROR("rdma_create_id() failed: %m");
        return ret;
    }

    /* Listen on INADDR_ANY */
    memset(&in_addr, 0, sizeof(in_addr));
    in_addr.sin_family      = AF_INET;
    in_addr.sin_addr.s_addr = INADDR_ANY;
    in_addr.sin_port        = htons(g_options.port_num);
    ret = rdma_bind_addr(g_test.listen_cm_id, (struct sockaddr*)&in_addr);
    if (ret) {
        LOG_ERROR("rdma_bind_addr() failed: %m");
        return ret;
    }

    ret = rdma_listen(g_test.listen_cm_id, g_options.conn_backlog);
    if (ret) {
        LOG_ERROR("rdma_listen() failed: %m");
        return ret;
    }

    LOG_INFO("Waiting for %d connections...", g_options.num_connections);
    while (g_test.num_established < g_options.num_connections) {
        /* For RC, wait until all connections got ESTABLISHED event */
        ret = wait_and_process_one_event(BIT(RDMA_CM_EVENT_CONNECT_REQUEST) |
                                         BIT(RDMA_CM_EVENT_ESTABLISHED) |
                                         BIT(RDMA_CM_EVENT_DISCONNECTED));
        if (ret) {
            return ret;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    int ret;

    ret = parse_opts(argc, argv);
    if (ret) {
        return ret;
    }

    ret = init_test();
    if (ret) {
        return ret;
    }

    if (is_client()) {
        ret = connect_client();
    } else {
        ret = connect_server();
    }
    if (ret) {
        return ret;
    }

    ret = do_barrier();
    if (ret < 0) {
        return ret;
    }

    ret = run_traffic_multi();
    if (ret) {
        return ret;
    }

    ret = do_barrier();
    if (ret < 0) {
        return ret;
    }

    ret = disconnect();
    if (ret) {
        return ret;
    }

    cleanup_test();

    return ret;
}
