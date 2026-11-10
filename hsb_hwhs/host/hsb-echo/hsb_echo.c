/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * hsb-echo - CPU coprocessor test peer for backline-hsb.
 *
 * HSB writes each request into a ring here as a UC RDMA WRITE (the bitstream needs
 * patches/0001). hsb-echo answers with a UC SEND of the request's first 16 bytes to QP 2 (HSB
 * sensor-TX 0).
 *
 *   hsb-echo -d mlx5_1 [-i ib_port] [-g gid_index] [-p tcp_port] [-s slots] [-c cpu] [-r] [-I]
 *
 *   -r  realtime: SCHED_FIFO and /dev/cpu_dma_latency = 0 (needs root)
 *   -I  ask for WRITE_WITH_IMM instead (stock HSB, imm = {psn[19:0], slot[11:0]}), which takes a
 *       receive per request
 *
 * Serves one session (common/include/hsb_handshake.h), then exits. The RoCE interface must
 * be in HSB's subnet (HSB host port 0: 192.168.0.2).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "hsb_handshake.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

#define RING_PITCH 128u /* HSB's page: the smallest slot pitch it can express */
#define MAX_RECV 512u
#define MAX_SEND 64u
#define SIGNAL_EVERY 32u

static int die(const char *what)
{
    fprintf(stderr, "hsb-echo: %s: %s\n", what, strerror(errno));
    exit(1);
}

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) {
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) {
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int read_sysfs(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

/* spin-wait hint */
static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/* The seq dword of a ring slot, as the NIC last wrote it */
static uint32_t seq_at(const uint8_t *ring, uint32_t slot, uint32_t seq_off)
{
    return *(const volatile uint32_t *)(ring + (size_t)slot * RING_PITCH + seq_off);
}

/* Retire one signaled send completion, i.e. SIGNAL_EVERY sends. Returns 0 if none is ready. */
static int reap_send(struct ibv_cq *scq, unsigned *inflight, uint64_t *errors)
{
    struct ibv_wc swc;
    int k = ibv_poll_cq(scq, 1, &swc);
    if (k < 0) {
        die("ibv_poll_cq (send)");
    }
    if (k == 0) {
        return 0;
    }
    if (swc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "hsb-echo: send completion %s\n", ibv_wc_status_str(swc.status));
        (*errors)++;
    }
    *inflight -= SIGNAL_EVERY;
    return 1;
}

static void post_recvs(struct ibv_qp *qp, unsigned n)
{
    /* WRITE_WITH_IMM consumes a receive but scatters nothing: zero-SGE receives */
    for (unsigned i = 0; i < n; i++) {
        struct ibv_recv_wr wr = {.wr_id = i, .sg_list = NULL, .num_sge = 0}, *bad;
        if (ibv_post_recv(qp, &wr, &bad)) {
            die("ibv_post_recv");
        }
    }
}

int main(int argc, char **argv)
{
    const char *devname = NULL;
    int ib_port = 1, gid_index = 3, tcp_port = HSB_HS_PORT, cpu = -1, rt = 0, imm = 0, c;
    unsigned slots = 256;

    while ((c = getopt(argc, argv, "d:i:g:p:s:c:rI")) != -1) {
        switch (c) {
        case 'd':
            devname = optarg;
            break;
        case 'i':
            ib_port = atoi(optarg);
            break;
        case 'g':
            gid_index = atoi(optarg);
            break;
        case 'p':
            tcp_port = atoi(optarg);
            break;
        case 's':
            slots = (unsigned)atoi(optarg);
            break;
        case 'c':
            cpu = atoi(optarg);
            break;
        case 'r':
            rt = 1;
            break;
        case 'I':
            imm = 1;
            break;
        default:
            fprintf(stderr, "usage: hsb-echo -d <ibdev> [-i ib_port] [-g gid_index] [-p tcp_port] "
                            "[-s slots] [-c cpu] [-r] [-I]\n");
            return 2;
        }
    }
    if (!devname || slots == 0 || slots > 4096) {
        fprintf(stderr, "hsb-echo: -d <ibdev> is required; slots must be 1..4096\n");
        return 2;
    }
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set)) {
            die("sched_setaffinity");
        }
    }
    mlockall(MCL_CURRENT | MCL_FUTURE);
    if (rt) {
        struct sched_param sp = {.sched_priority = sched_get_priority_max(SCHED_FIFO)};
        if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
            die("SCHED_FIFO");
        }
        /* the request holds while the file stays open, i.e. for the life of the process */
        int qos = open("/dev/cpu_dma_latency", O_RDWR);
        int32_t zero = 0;
        if (qos < 0 || write(qos, &zero, sizeof(zero)) != sizeof(zero)) {
            die("/dev/cpu_dma_latency");
        }
    }

    /* ---- device, GID (RoCE v2 over IPv4), MAC ------------------------------------- */
    int ndev = 0;
    struct ibv_device **list = ibv_get_device_list(&ndev);
    struct ibv_context *ctx = NULL;
    for (int i = 0; list && i < ndev; i++) {
        if (!strcmp(ibv_get_device_name(list[i]), devname)) {
            ctx = ibv_open_device(list[i]);
        }
    }
    if (!ctx) {
        fprintf(stderr, "hsb-echo: no RDMA device %s\n", devname);
        return 1;
    }
    struct ibv_port_attr pattr;
    if (ibv_query_port(ctx, (uint8_t)ib_port, &pattr)) {
        die("ibv_query_port");
    }
    union ibv_gid gid;
    if (ibv_query_gid(ctx, (uint8_t)ib_port, gid_index, &gid)) {
        die("ibv_query_gid");
    }
    static const uint8_t v4prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(gid.raw, v4prefix, 12)) {
        fprintf(stderr,
                "hsb-echo: GID %d of %s is not an IPv4 GID; pick the RoCE v2 IPv4 one (-g)\n",
                gid_index, devname);
        return 1;
    }
    uint32_t host_ip = ((uint32_t)gid.raw[12] << 24) | ((uint32_t)gid.raw[13] << 16) |
                       ((uint32_t)gid.raw[14] << 8) | gid.raw[15];

    char path[256], buf[64], netdev[64];
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/gid_attrs/types/%d", devname,
             ib_port, gid_index);
    if (read_sysfs(path, buf, sizeof(buf)) == 0 && !strstr(buf, "v2")) {
        fprintf(stderr, "hsb-echo: warning: GID %d is \"%s\", HSB speaks RoCE v2\n", gid_index,
                buf);
    }
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/gid_attrs/ndevs/%d", devname,
             ib_port, gid_index);
    if (read_sysfs(path, netdev, sizeof(netdev))) {
        die("netdev of the GID");
    }
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", netdev);
    unsigned m[6];
    if (read_sysfs(path, buf, sizeof(buf)) ||
        sscanf(buf, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        die("netdev MAC");
    }

    /* ---- PD, CQs, UC QP, ring at IOVA 0 --------------------------------------------- */
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    const unsigned recvs = imm ? MAX_RECV : 1; /* only a WRITE_WITH_IMM takes a receive */
    struct ibv_cq *rcq = ibv_create_cq(ctx, (int)recvs * 2, NULL, NULL, 0);
    struct ibv_cq *scq = ibv_create_cq(ctx, MAX_SEND * 2, NULL, NULL, 0);
    if (!pd || !rcq || !scq) {
        die("pd/cq");
    }

    struct ibv_qp_init_attr qia = {
        .send_cq = scq,
        .recv_cq = rcq,
        .qp_type = IBV_QPT_UC,
        .sq_sig_all = 0,
        .cap = {.max_send_wr = MAX_SEND,
                .max_recv_wr = recvs,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                .max_inline_data = 64},
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qia);
    if (!qp) {
        die("ibv_create_qp (UC)");
    }

    size_t ring_len = (size_t)slots * RING_PITCH;
    uint8_t *ring = aligned_alloc(4096, (ring_len + 4095) & ~(size_t)4095);
    if (!ring) {
        die("ring");
    }
    memset(ring, 0, ring_len);
    struct ibv_mr *mr =
        ibv_reg_mr_iova(pd, ring, ring_len, 0, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        die("ibv_reg_mr_iova");
    }

    struct ibv_qp_attr a = {.qp_state = IBV_QPS_INIT,
                            .pkey_index = 0,
                            .port_num = (uint8_t)ib_port,
                            .qp_access_flags = IBV_ACCESS_REMOTE_WRITE};
    if (ibv_modify_qp(qp, &a,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        die("QP -> INIT");
    }
    if (imm) {
        post_recvs(qp, MAX_RECV);
    }

    /* ---- handshake ------------------------------------------------------------------ */
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = {.sin_family = AF_INET,
                             .sin_port = htons((uint16_t)tcp_port),
                             .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) || listen(ls, 1)) {
        die("listen");
    }
    printf(
        "hsb-echo: %s port %d gid %d (%u.%u.%u.%u, %s), UC QP 0x%06x, ring %u x %u B rkey 0x%08x; "
        "waiting on tcp %d\n",
        devname, ib_port, gid_index, host_ip >> 24, (host_ip >> 16) & 255, (host_ip >> 8) & 255,
        host_ip & 255, netdev, qp->qp_num, slots, RING_PITCH, mr->rkey, tcp_port);
    fflush(stdout);
    int fd = accept(ls, NULL, NULL);
    if (fd < 0) {
        die("accept");
    }

    struct hsb_hs_host hi = {
        .magic = HSB_HS_MAGIC,
        .version = HSB_HS_VERSION,
        .qpn = qp->qp_num,
        .rkey = mr->rkey,
        .ring_iova = 0,
        .ring_pitch = RING_PITCH,
        .ring_slots = slots,
        .ip = host_ip,
        .imm = (uint16_t)imm,
    };
    for (int i = 0; i < 6; i++) {
        hi.mac[i] = (uint8_t)m[i];
    }
    struct hsb_hs_fpga fo;
    if (write_full(fd, &hi, sizeof(hi)) || read_full(fd, &fo, sizeof(fo)) ||
        fo.magic != HSB_HS_MAGIC || fo.version != HSB_HS_VERSION) {
        fprintf(stderr, "hsb-echo: bad handshake\n");
        return 1;
    }
    if (fo.reply_bytes == 0 || fo.reply_bytes > 64 || fo.frame_bytes > RING_PITCH) {
        fprintf(stderr, "hsb-echo: controller asks for %u-byte replies / %u-byte frames\n",
                fo.reply_bytes, fo.frame_bytes);
        return 1;
    }

    /* ---- UC: RTR (to HSB sensor-TX QP) and RTS; no RC-only attributes ---------------- */
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = pattr.active_mtu;
    a.dest_qp_num = fo.qpn;
    a.rq_psn = 0;
    a.ah_attr.is_global = 1;
    a.ah_attr.port_num = (uint8_t)ib_port;
    a.ah_attr.grh.sgid_index = (uint8_t)gid_index;
    a.ah_attr.grh.hop_limit = 64;
    memcpy(a.ah_attr.grh.dgid.raw, v4prefix, 12);
    a.ah_attr.grh.dgid.raw[12] = (uint8_t)(fo.ip >> 24);
    a.ah_attr.grh.dgid.raw[13] = (uint8_t)(fo.ip >> 16);
    a.ah_attr.grh.dgid.raw[14] = (uint8_t)(fo.ip >> 8);
    a.ah_attr.grh.dgid.raw[15] = (uint8_t)fo.ip;
    if (ibv_modify_qp(
            qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN)) {
        die("QP -> RTR");
    }
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.sq_psn = 0;
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        die("QP -> RTS");
    }

    struct hsb_hs_ready rd = {.magic = HSB_HS_MAGIC, .qp_state = IBV_QPS_RTS};
    if (write_full(fd, &rd, sizeof(rd))) {
        die("handshake ready");
    }
    printf("hsb-echo: session up, replies to %u.%u.%u.%u QP %u, %u-byte frames, %u-byte replies\n",
           fo.ip >> 24, (fo.ip >> 16) & 255, (fo.ip >> 8) & 255, fo.ip & 255, fo.qpn,
           fo.frame_bytes, fo.reply_bytes);
    fflush(stdout);

    /* ---- echo loop ------------------------------------------------------------------- */
    uint64_t served = 0, recvd = 0, errors = 0;
    unsigned inflight = 0;         /* sends posted and not yet known complete */
    uint32_t slot = 0, expect = 1; /* hwhs stamps seq 1 in the first round after its reset */
    struct ibv_wc wc[16];

    for (uint64_t spin = 0;; spin++) {
        if (seq_at(ring, slot, fo.seq_off) == expect) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);

            /* reply: the slot's first reply_bytes, inline; every SIGNAL_EVERY-th send signaled */
            int sig = (served + 1) % SIGNAL_EVERY == 0;
            struct ibv_sge sge = {.addr = (uintptr_t)(ring + (size_t)slot * RING_PITCH),
                                  .length = fo.reply_bytes,
                                  .lkey = mr->lkey};
            struct ibv_send_wr wr = {.wr_id = served,
                                     .sg_list = &sge,
                                     .num_sge = 1,
                                     .opcode = IBV_WR_SEND,
                                     .send_flags = IBV_SEND_INLINE | (sig ? IBV_SEND_SIGNALED : 0)};
            struct ibv_send_wr *bad;
            while (inflight >= MAX_SEND) {
                reap_send(scq, &inflight, &errors); /* only if idle reaping fell behind */
            }
            if (ibv_post_send(qp, &wr, &bad)) {
                die("ibv_post_send");
            }
            inflight++;
            served++;
            slot = (slot + 1) % slots;
            expect++;
            continue;
        }

        /* Idle */
        cpu_relax();

        /* Receive completions, one per WRITE_WITH_IMM: due after each reply */
        if (imm && (recvd < served || (spin & 0x1FFF) == 0)) {
            int n = ibv_poll_cq(rcq, 16, wc);
            if (n < 0) {
                die("ibv_poll_cq");
            }
            for (int i = 0; i < n; i++) {
                if (wc[i].status != IBV_WC_SUCCESS || wc[i].opcode != IBV_WC_RECV_RDMA_WITH_IMM) {
                    fprintf(stderr, "hsb-echo: receive completion status %s opcode %d\n",
                            ibv_wc_status_str(wc[i].status), wc[i].opcode);
                    errors++;
                }
                else if (++recvd > served && seq_at(ring, slot, fo.seq_off) != expect) {
                    slot = (ntohl(wc[i].imm_data) & 0xFFFu) % slots;
                    expect = seq_at(ring, slot, fo.seq_off);
                }
                post_recvs(qp, 1);
            }
        }
        if (inflight >= SIGNAL_EVERY) {
            reap_send(scq, &inflight, &errors);
        }

        if ((spin & 0xFFFFF) == 0) { /* session over when the controller hangs up */
            char b;
            ssize_t r = recv(fd, &b, 1, MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                break;
            }
        }
    }

    printf("hsb-echo: session closed after %" PRIu64 " replies, %" PRIu64 " errors\n", served,
           errors);
    close(fd);
    close(ls);
    ibv_destroy_qp(qp);
    ibv_dereg_mr(mr);
    ibv_destroy_cq(scq);
    ibv_destroy_cq(rcq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(list);
    free(ring);
    return errors ? 1 : 0;
}
