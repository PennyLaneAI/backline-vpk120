/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * rc_peer.h - what balboa-echo, balboa-gpu-echo and balboa-standin share: one RC QP on a
 * RoCE v2 IPv4 GID, connected with one PSN for both directions (balboa_handshake.h), the
 * TCP helpers of the handshake, and the real-time setup of the polling thread.
 * Header-only, C and HIP C++.
 */
#ifndef RC_PEER_H
#define RC_PEER_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define RC_MAX_SEND 64u

struct rc_peer {
    const char *who;              /* program name, for messages */
    struct ibv_device **list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;            /* send completions; nothing is ever received */
    struct ibv_qp *qp;
    int ib_port;
    int gid_index;
    enum ibv_mtu mtu;
    uint32_t ip;                  /* the GID's IPv4 address, host byte order */
    uint8_t mac[6];
    char netdev[64];
};

static inline void rc_die(const struct rc_peer *p, const char *what)
{
    fprintf(stderr, "%s: %s: %s\n", p->who, what, strerror(errno));
    exit(1);
}

static inline int rc_read_sysfs(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

/* Spin-wait hint (catalyst's cpu_relax) */
static inline void rc_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/* Pin to `cpu` (if >= 0) and lock memory; with rt, SCHED_FIFO at the top priority and
 * /dev/cpu_dma_latency = 0 (that one needs root), as catalyst's rt=1. */
static inline void rc_realtime(const struct rc_peer *p, int cpu, int rt)
{
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set)) rc_die(p, "sched_setaffinity");
    }
    mlockall(MCL_CURRENT | MCL_FUTURE);
    if (rt) {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
        if (sched_setscheduler(0, SCHED_FIFO, &sp)) rc_die(p, "SCHED_FIFO");
        /* the request holds while the file stays open, i.e. for the life of the process */
        int qos = open("/dev/cpu_dma_latency", O_RDWR);
        int32_t zero = 0;
        if (qos < 0 || write(qos, &zero, sizeof(zero)) != sizeof(zero)) rc_die(p, "/dev/cpu_dma_latency");
    }
}

/* Device, port, the RoCE v2 IPv4 GID and its netdev's MAC, PD, CQ, and an RC QP in INIT that
 * lets the other side WRITE into it. */
static inline void rc_open(struct rc_peer *p, const char *devname, int ib_port, int gid_index)
{
    static const uint8_t v4prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    int ndev = 0;

    p->ib_port = ib_port;
    p->gid_index = gid_index;
    p->list = ibv_get_device_list(&ndev);
    for (int i = 0; p->list && i < ndev; i++)
        if (!strcmp(ibv_get_device_name(p->list[i]), devname)) p->ctx = ibv_open_device(p->list[i]);
    if (!p->ctx) {
        fprintf(stderr, "%s: no RDMA device %s\n", p->who, devname);
        exit(1);
    }

    struct ibv_port_attr pattr;
    if (ibv_query_port(p->ctx, (uint8_t)ib_port, &pattr)) rc_die(p, "ibv_query_port");
    p->mtu = pattr.active_mtu;
    union ibv_gid gid;
    if (ibv_query_gid(p->ctx, (uint8_t)ib_port, gid_index, &gid)) rc_die(p, "ibv_query_gid");
    if (memcmp(gid.raw, v4prefix, 12)) {
        fprintf(stderr, "%s: GID %d of %s is not an IPv4 GID; pick the RoCE v2 IPv4 one (-g)\n", p->who,
                gid_index, devname);
        exit(1);
    }
    p->ip = ((uint32_t)gid.raw[12] << 24) | ((uint32_t)gid.raw[13] << 16) | ((uint32_t)gid.raw[14] << 8) |
            gid.raw[15];

    char path[256], buf[64];
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/gid_attrs/types/%d", devname, ib_port,
             gid_index);
    if (rc_read_sysfs(path, buf, sizeof(buf)) == 0 && !strstr(buf, "v2"))
        fprintf(stderr, "%s: warning: GID %d is \"%s\", BALBOA speaks RoCE v2\n", p->who, gid_index, buf);
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/gid_attrs/ndevs/%d", devname, ib_port,
             gid_index);
    if (rc_read_sysfs(path, p->netdev, sizeof(p->netdev))) rc_die(p, "netdev of the GID");
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", p->netdev);
    unsigned m[6];
    if (rc_read_sysfs(path, buf, sizeof(buf)) ||
        sscanf(buf, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        rc_die(p, "netdev MAC");
    for (int i = 0; i < 6; i++) p->mac[i] = (uint8_t)m[i];

    p->pd = ibv_alloc_pd(p->ctx);
    p->cq = ibv_create_cq(p->ctx, RC_MAX_SEND * 2, NULL, NULL, 0);
    if (!p->pd || !p->cq) rc_die(p, "pd/cq");

    struct ibv_qp_init_attr qia;
    memset(&qia, 0, sizeof(qia));
    qia.send_cq = p->cq;
    qia.recv_cq = p->cq;
    qia.qp_type = IBV_QPT_RC;
    qia.sq_sig_all = 0;
    qia.cap.max_send_wr = RC_MAX_SEND;
    qia.cap.max_recv_wr = 1;
    qia.cap.max_send_sge = 1;
    qia.cap.max_recv_sge = 1;
    qia.cap.max_inline_data = 64;
    p->qp = ibv_create_qp(p->pd, &qia);
    if (!p->qp) rc_die(p, "ibv_create_qp (RC)");

    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_INIT;
    a.pkey_index = 0;
    a.port_num = (uint8_t)ib_port;
    a.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(p->qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        rc_die(p, "QP -> INIT");
}

/* RTR and RTS toward dest_qpn at dest_ip, one PSN for both directions */
static inline void rc_connect(struct rc_peer *p, uint32_t dest_qpn, uint32_t psn, uint32_t dest_ip)
{
    static const uint8_t v4prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    struct ibv_qp_attr a;

    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = p->mtu;
    a.dest_qp_num = dest_qpn;
    a.rq_psn = psn;
    a.max_dest_rd_atomic = 1;
    a.min_rnr_timer = 12;
    a.ah_attr.is_global = 1;
    a.ah_attr.port_num = (uint8_t)p->ib_port;
    a.ah_attr.grh.sgid_index = (uint8_t)p->gid_index;
    a.ah_attr.grh.hop_limit = 64;
    memcpy(a.ah_attr.grh.dgid.raw, v4prefix, 12);
    a.ah_attr.grh.dgid.raw[12] = (uint8_t)(dest_ip >> 24);
    a.ah_attr.grh.dgid.raw[13] = (uint8_t)(dest_ip >> 16);
    a.ah_attr.grh.dgid.raw[14] = (uint8_t)(dest_ip >> 8);
    a.ah_attr.grh.dgid.raw[15] = (uint8_t)dest_ip;
    if (ibv_modify_qp(p->qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                                     IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
        rc_die(p, "QP -> RTR");

    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.sq_psn = psn;
    a.timeout = 14;       /* 4.096 us * 2^14 = 67 ms */
    a.retry_cnt = 7;
    a.rnr_retry = 7;
    a.max_rd_atomic = 1;
    if (ibv_modify_qp(p->qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                                     IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC))
        rc_die(p, "QP -> RTS");
}

static inline void rc_close(struct rc_peer *p)
{
    if (p->qp) ibv_destroy_qp(p->qp);
    if (p->cq) ibv_destroy_cq(p->cq);
    if (p->pd) ibv_dealloc_pd(p->pd);
    if (p->ctx) ibv_close_device(p->ctx);
    if (p->list) ibv_free_device_list(p->list);
}

/* Retire one signaled WRITE completion, i.e. `signal_every` posts (every signal_every-th post
 * is signaled). Returns 0 if none is ready. */
static inline int rc_reap(struct rc_peer *p, unsigned *inflight, unsigned signal_every, uint64_t *errors)
{
    struct ibv_wc wc;
    int k = ibv_poll_cq(p->cq, 1, &wc);
    if (k < 0) rc_die(p, "ibv_poll_cq");
    if (k == 0) return 0;
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "%s: WRITE completion %s\n", p->who, ibv_wc_status_str(wc.status));
        (*errors)++;
    }
    *inflight -= signal_every;
    return 1;
}

/* An RDMA WRITE of `len` bytes from `buf`, inline */
static inline void rc_write_inline(struct rc_peer *p, const void *buf, uint32_t len, uint64_t raddr, uint32_t rkey,
                            uint64_t id, int signaled)
{
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad;
    sge.addr = (uintptr_t)buf;
    sge.length = len;
    sge.lkey = 0;                 /* inline: the NIC copies the bytes at post time */
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_INLINE | (signaled ? IBV_SEND_SIGNALED : 0);
    wr.wr.rdma.remote_addr = raddr;
    wr.wr.rdma.rkey = rkey;
    if (ibv_post_send(p->qp, &wr, &bad)) rc_die(p, "ibv_post_send");
}

/* ---- the handshake's TCP side ---------------------------------------------------------- */

static inline int rc_read_full(int fd, void *buf, size_t n)
{
    uint8_t *b = (uint8_t *)buf;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static inline int rc_write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *b = (const uint8_t *)buf;
    while (n) {
        ssize_t r = write(fd, b, n);
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static inline int rc_listen(const struct rc_peer *p, int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in sa;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) || listen(ls, 1)) rc_die(p, "listen");
    return ls;
}

static inline int rc_dial(const struct rc_peer *p, const char *ip, int port)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        fprintf(stderr, "%s: bad address %s\n", p->who, ip);
        exit(2);
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) rc_die(p, "connect");
    return fd;
}

/* The controller has hung up: the session is over. */
static inline int rc_hung_up(int fd)
{
    char b;
    ssize_t r = recv(fd, &b, 1, MSG_DONTWAIT);
    return r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

/* 24-bit PSN to start the session with */
static inline uint32_t rc_new_psn(void)
{
    uint32_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &v, sizeof(v)) != sizeof(v)) v = 0;
        close(fd);
    }
    return (v ^ (uint32_t)getpid()) & 0xFFFFFF;
}

#endif /* RC_PEER_H */
