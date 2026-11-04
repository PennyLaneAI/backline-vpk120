/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * hsb-gpu-echo - GPU coprocessor test peer for backline-hsb (x86 host, AMD GPU, ConnectX NIC).
 *
 *   HSB --UC WRITE_WITH_IMM--> request ring in GPU HBM (dma-buf MR at IOVA 0)
 *   persistent kernel: polls each slot's seq, echoes, and publishes {correction, seq} into a
 *       host-mapped handoff ring
 *   CPU thread: posts each reply as an inline UC SEND to QP 2 (HSB sensor-TX 0)
 *
 * A lost frame stalls the session (hsb-echo resyncs instead).
 *
 *   hsb-gpu-echo -d mlx5_1 [-i ib_port] [-g gid_index] [-p tcp_port] [-G gpu] [-c cpu] [-r]
 *
 *   -G  HIP device (default 0)
 *   -c  core of the CPU thread that posts the replies
 *   -r  realtime: SCHED_FIFO and /dev/cpu_dma_latency = 0 (needs root)
 *
 * Serves one session (common/include/hsb_handshake.h), then exits.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <hip/hip_runtime.h>

#include "hsb_handshake.h"

#define RING_PITCH         128u   /* HSB's page: the smallest slot pitch it can express */
#define RING_SLOTS         256u   /* power of two: the kernel masks with it */
#define MAX_RECV           512u
#define MAX_SEND           64u
#define SIGNAL_EVERY       32u
#define STOP_POLL_INTERVAL 1024u  /* spins between stop-flag reads, power of two */

/* GPU -> CPU, one per ring slot */
struct alignas(16) handoff_slot {
    int64_t  correction;
    uint32_t seq;
    uint32_t pad;
};

static void die(const char *what)
{
    fprintf(stderr, "hsb-gpu-echo: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void hip_check(hipError_t e, const char *what)
{
    if (e == hipSuccess) return;
    fprintf(stderr, "hsb-gpu-echo: %s: %s\n", what, hipGetErrorString(e));
    exit(1);
}

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = static_cast<uint8_t *>(buf);
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int read_sysfs(const char *path, char *buf, size_t n)
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

/* spin-wait hint */
static inline void cpu_relax(void)
{
    __builtin_ia32_pause();
}

/* Retire one signaled send completion, i.e. SIGNAL_EVERY sends. Returns 0 if none is ready. */
static int reap_send(struct ibv_cq *scq, unsigned *inflight, uint64_t *errors)
{
    struct ibv_wc swc;
    int k = ibv_poll_cq(scq, 1, &swc);
    if (k < 0) die("ibv_poll_cq (send)");
    if (k == 0) return 0;
    if (swc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "hsb-gpu-echo: send completion %s\n", ibv_wc_status_str(swc.status));
        (*errors)++;
    }
    *inflight -= SIGNAL_EVERY;
    return 1;
}

static void post_recvs(struct ibv_qp *qp, unsigned n)
{
    /* WRITE_WITH_IMM consumes a receive but scatters nothing: zero-SGE receives */
    for (unsigned i = 0; i < n; i++) {
        struct ibv_recv_wr wr = {.wr_id = i, .next = NULL, .sg_list = NULL, .num_sge = 0}, *bad;
        if (ibv_post_recv(qp, &wr, &bad)) die("ibv_post_recv");
    }
}

/* One thread polls the request ring in HBM and hands each correction to the CPU through
 * host-mapped memory. */
__global__ void echo_kernel(volatile uint8_t *ring, volatile handoff_slot *handoff,
                            volatile uint32_t *stop, volatile uint32_t *ready, uint32_t seq_off)
{
    if (threadIdx.x != 0 || blockIdx.x != 0) return;
    *ready = 1;
    __threadfence_system();
    for (uint64_t cursor = 0;; ++cursor) {
        const uint32_t expect = (uint32_t)(cursor + 1);
        const size_t index = cursor & (RING_SLOTS - 1);
        volatile uint32_t *sptr = (volatile uint32_t *)(ring + index * RING_PITCH + seq_off);
        /* the stop flag is a read over PCIe: only every STOP_POLL_INTERVAL spins */
        for (uint32_t spins = 0; *sptr != expect; ++spins)
            if ((spins & (STOP_POLL_INTERVAL - 1)) == 0u && *stop != 0) return;
        __threadfence_system();  /* seq seen: the value read below is not stale */
        const uint64_t syndrome = *(volatile uint64_t *)(ring + index * RING_PITCH);
        handoff_slot reply = {(int64_t)syndrome, expect, 0};   /* echo */
        /* one 16 B store publishes correction and seq together, then one system fence */
        *reinterpret_cast<int4 *>(const_cast<handoff_slot *>(&handoff[index])) =
            *reinterpret_cast<const int4 *>(&reply);
        __threadfence_system();
    }
}

int main(int argc, char **argv)
{
    const char *devname = NULL;
    int ib_port = 1, gid_index = 3, tcp_port = HSB_HS_PORT, gpu = 0, cpu = -1, rt = 0, c;

    while ((c = getopt(argc, argv, "d:i:g:p:G:c:r")) != -1) {
        switch (c) {
        case 'd': devname = optarg; break;
        case 'i': ib_port = atoi(optarg); break;
        case 'g': gid_index = atoi(optarg); break;
        case 'p': tcp_port = atoi(optarg); break;
        case 'G': gpu = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'r': rt = 1; break;
        default:
            fprintf(stderr, "usage: hsb-gpu-echo -d <ibdev> [-i ib_port] [-g gid_index] [-p tcp_port] "
                            "[-G gpu] [-c cpu] [-r]\n");
            return 2;
        }
    }
    if (!devname) {
        fprintf(stderr, "hsb-gpu-echo: -d <ibdev> is required\n");
        return 2;
    }
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set)) die("sched_setaffinity");
    }
    mlockall(MCL_CURRENT | MCL_FUTURE);
    if (rt) {
        struct sched_param sp = {.sched_priority = sched_get_priority_max(SCHED_FIFO)};
        if (sched_setscheduler(0, SCHED_FIFO, &sp)) die("SCHED_FIFO");
        /* the request holds while the file stays open, i.e. for the life of the process */
        int qos = open("/dev/cpu_dma_latency", O_RDWR);
        int32_t zero = 0;
        if (qos < 0 || write(qos, &zero, sizeof(zero)) != sizeof(zero)) die("/dev/cpu_dma_latency");
    }

    /* ---- GPU: request ring in HBM, host-mapped handoff ring and flags ----------------- */
    hip_check(hipSetDevice(gpu), "hipSetDevice");
    hipStream_t stream;
    hip_check(hipStreamCreate(&stream), "hipStreamCreate");

    const size_t ring_len = (size_t)RING_SLOTS * RING_PITCH;
    void *ring = NULL;
    hip_check(hipExtMallocWithFlags(&ring, ring_len, hipDeviceMallocFinegrained),
              "hipExtMallocWithFlags (fine-grained ring)");
    hip_check(hipMemset(ring, 0, ring_len), "hipMemset (ring)");
    int dmabuf_fd = -1;
    hip_check(hipMemGetHandleForAddressRange(&dmabuf_fd, (hipDeviceptr_t)ring, ring_len,
                                             hipMemRangeHandleTypeDmaBufFd, 0),
              "hipMemGetHandleForAddressRange (dma-buf)");

    handoff_slot *handoff = NULL, *handoff_dev = NULL;
    hip_check(hipHostMalloc((void **)&handoff, RING_SLOTS * sizeof(handoff_slot),
                            hipHostMallocMapped | hipHostMallocCoherent), "hipHostMalloc (handoff)");
    memset(handoff, 0, RING_SLOTS * sizeof(handoff_slot));   /* no stale seq can look published */
    hip_check(hipHostGetDevicePointer((void **)&handoff_dev, handoff, 0), "hipHostGetDevicePointer (handoff)");

    uint32_t *flags = NULL, *flags_dev = NULL;               /* [0] stop, [1] ready */
    hip_check(hipHostMalloc((void **)&flags, 2 * sizeof(uint32_t),
                            hipHostMallocMapped | hipHostMallocCoherent), "hipHostMalloc (flags)");
    flags[0] = flags[1] = 0;
    hip_check(hipHostGetDevicePointer((void **)&flags_dev, flags, 0), "hipHostGetDevicePointer (flags)");
    volatile uint32_t *stop = &flags[0], *ready = &flags[1];

    /* ---- device, GID (RoCE v2 over IPv4), MAC ------------------------------------- */
    int ndev = 0;
    struct ibv_device **list = ibv_get_device_list(&ndev);
    struct ibv_context *ctx = NULL;
    for (int i = 0; list && i < ndev; i++)
        if (!strcmp(ibv_get_device_name(list[i]), devname)) ctx = ibv_open_device(list[i]);
    if (!ctx) {
        fprintf(stderr, "hsb-gpu-echo: no RDMA device %s\n", devname);
        return 1;
    }
    struct ibv_port_attr pattr;
    if (ibv_query_port(ctx, (uint8_t)ib_port, &pattr)) die("ibv_query_port");
    union ibv_gid gid;
    if (ibv_query_gid(ctx, (uint8_t)ib_port, gid_index, &gid)) die("ibv_query_gid");
    static const uint8_t v4prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(gid.raw, v4prefix, 12)) {
        fprintf(stderr, "hsb-gpu-echo: GID %d of %s is not an IPv4 GID; pick the RoCE v2 IPv4 one (-g)\n",
                gid_index, devname);
        return 1;
    }
    uint32_t host_ip = ((uint32_t)gid.raw[12] << 24) | ((uint32_t)gid.raw[13] << 16) |
                       ((uint32_t)gid.raw[14] << 8) | gid.raw[15];

    char path[256], buf[64], netdev[64];
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/gid_attrs/types/%d", devname, ib_port, gid_index);
    if (read_sysfs(path, buf, sizeof(buf)) == 0 && !strstr(buf, "v2"))
        fprintf(stderr, "hsb-gpu-echo: warning: GID %d is \"%s\", HSB speaks RoCE v2\n", gid_index, buf);
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/gid_attrs/ndevs/%d", devname, ib_port, gid_index);
    if (read_sysfs(path, netdev, sizeof(netdev))) die("netdev of the GID");
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", netdev);
    unsigned m[6];
    if (read_sysfs(path, buf, sizeof(buf)) ||
        sscanf(buf, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        die("netdev MAC");

    /* ---- PD, CQs, UC QP, the HBM ring registered through dma-buf at IOVA 0 ---------- */
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_cq *rcq = ibv_create_cq(ctx, MAX_RECV * 2, NULL, NULL, 0);
    struct ibv_cq *scq = ibv_create_cq(ctx, MAX_SEND * 2, NULL, NULL, 0);
    if (!pd || !rcq || !scq) die("pd/cq");

    struct ibv_qp_init_attr qia;
    memset(&qia, 0, sizeof(qia));
    qia.send_cq = scq;
    qia.recv_cq = rcq;
    qia.cap.max_send_wr = MAX_SEND;
    qia.cap.max_recv_wr = MAX_RECV;
    qia.cap.max_send_sge = 1;
    qia.cap.max_recv_sge = 1;
    qia.cap.max_inline_data = 64;
    qia.qp_type = IBV_QPT_UC;
    qia.sq_sig_all = 0;
    struct ibv_qp *qp = ibv_create_qp(pd, &qia);
    if (!qp) die("ibv_create_qp (UC)");

    struct ibv_mr *mr = ibv_reg_dmabuf_mr(pd, 0, ring_len, 0, dmabuf_fd,
                                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) die("ibv_reg_dmabuf_mr");
    close(dmabuf_fd);   /* the MR holds its own reference */

    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_INIT;
    a.pkey_index = 0;
    a.port_num = (uint8_t)ib_port;
    a.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        die("QP -> INIT");
    post_recvs(qp, MAX_RECV);

    /* ---- handshake ------------------------------------------------------------------ */
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)tcp_port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) || listen(ls, 1)) die("listen");
    printf("hsb-gpu-echo: %s port %d gid %d (%u.%u.%u.%u, %s), UC QP 0x%06x, HBM ring %u x %u B on GPU %d "
           "rkey 0x%08x; waiting on tcp %d\n", devname, ib_port, gid_index, host_ip >> 24, (host_ip >> 16) & 255,
           (host_ip >> 8) & 255, host_ip & 255, netdev, qp->qp_num, RING_SLOTS, RING_PITCH, gpu, mr->rkey,
           tcp_port);
    fflush(stdout);
    int fd = accept(ls, NULL, NULL);
    if (fd < 0) die("accept");

    struct hsb_hs_host hi;
    memset(&hi, 0, sizeof(hi));
    hi.magic = HSB_HS_MAGIC;
    hi.version = HSB_HS_VERSION;
    hi.qpn = qp->qp_num;
    hi.rkey = mr->rkey;
    hi.ring_iova = 0;
    hi.ring_pitch = RING_PITCH;
    hi.ring_slots = RING_SLOTS;
    hi.ip = host_ip;
    for (int i = 0; i < 6; i++) hi.mac[i] = (uint8_t)m[i];
    hi.imm = 1;
    struct hsb_hs_fpga fo;
    if (write_full(fd, &hi, sizeof(hi)) || read_full(fd, &fo, sizeof(fo)) ||
        fo.magic != HSB_HS_MAGIC || fo.version != HSB_HS_VERSION) {
        fprintf(stderr, "hsb-gpu-echo: bad handshake\n");
        return 1;
    }
    if (fo.reply_bytes < 16 || fo.reply_bytes > 64 || fo.frame_bytes > RING_PITCH ||
        fo.seq_off < 8 || fo.seq_off + 4 > fo.reply_bytes || fo.seq_off % 4) {
        fprintf(stderr, "hsb-gpu-echo: controller asks for %u-byte replies / %u-byte frames, seq at %u\n",
                fo.reply_bytes, fo.frame_bytes, fo.seq_off);
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
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN))
        die("QP -> RTR");
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.sq_psn = 0;
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN)) die("QP -> RTS");

    /* ---- the persistent kernel; the controller starts only once it is spinning ------- */
    hipLaunchKernelGGL(echo_kernel, dim3(1), dim3(1), 0, stream, (volatile uint8_t *)ring,
                       (volatile handoff_slot *)handoff_dev, (volatile uint32_t *)&flags_dev[0],
                       (volatile uint32_t *)&flags_dev[1], fo.seq_off);
    hip_check(hipGetLastError(), "kernel launch");
    while (!*ready) {
        hipError_t q = hipStreamQuery(stream);
        if (q != hipErrorNotReady) hip_check(q == hipSuccess ? hipErrorLaunchFailure : q, "kernel ended before it was ready");
        cpu_relax();
    }

    struct hsb_hs_ready rd = {.magic = HSB_HS_MAGIC, .qp_state = IBV_QPS_RTS};
    if (write_full(fd, &rd, sizeof(rd))) die("handshake ready");
    printf("hsb-gpu-echo: session up, replies to %u.%u.%u.%u QP %u, %u-byte frames, %u-byte replies\n",
           fo.ip >> 24, (fo.ip >> 16) & 255, (fo.ip >> 8) & 255, fo.ip & 255, fo.qpn, fo.frame_bytes,
           fo.reply_bytes);
    fflush(stdout);

    /* ---- reply loop -------------------------------------------------------------------- */
    /* Round n's correction is in handoff[n % RING_SLOTS] once its seq reads n + 1. The
     * receive and send completions are reaped while idle, never between a handoff and
     * its reply. */
    uint64_t served = 0, recvd = 0, errors = 0;
    unsigned inflight = 0;          /* sends posted and not yet known complete */
    uint8_t reply[64];
    memset(reply, 0, sizeof(reply));
    struct ibv_wc wc[16];

    for (uint64_t spin = 0;; spin++) {
        const uint32_t expect = (uint32_t)(served + 1);
        volatile handoff_slot *h = &handoff[served & (RING_SLOTS - 1)];
        if (h->seq == expect) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            const int64_t correction = h->correction;
            memcpy(reply, &correction, sizeof(correction));     /* value; decoder_id stays 0 */
            memcpy(reply + fo.seq_off, &expect, sizeof(expect));

            /* every SIGNAL_EVERY-th send is signaled; its completion retires that many */
            int sig = (served + 1) % SIGNAL_EVERY == 0;
            struct ibv_sge sge = {.addr = (uintptr_t)reply, .length = fo.reply_bytes, .lkey = mr->lkey};
            struct ibv_send_wr wr;
            memset(&wr, 0, sizeof(wr));
            wr.wr_id = served;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_SEND;
            wr.send_flags = IBV_SEND_INLINE | (sig ? IBV_SEND_SIGNALED : 0);
            struct ibv_send_wr *bad;
            while (inflight >= MAX_SEND) reap_send(scq, &inflight, &errors);   /* only if idle reaping fell behind */
            if (ibv_post_send(qp, &wr, &bad)) die("ibv_post_send");
            inflight++;
            served++;
            continue;
        }

        /* Idle */
        cpu_relax();
        if (recvd < served) {   /* each frame's receive completion, due by now */
            int n = ibv_poll_cq(rcq, 16, wc);
            if (n < 0) die("ibv_poll_cq");
            for (int i = 0; i < n; i++) {
                if (wc[i].status != IBV_WC_SUCCESS || wc[i].opcode != IBV_WC_RECV_RDMA_WITH_IMM) {
                    fprintf(stderr, "hsb-gpu-echo: receive completion status %s opcode %d\n",
                            ibv_wc_status_str(wc[i].status), wc[i].opcode);
                    errors++;
                } else {
                    recvd++;
                }
                post_recvs(qp, 1);
            }
        }
        if (inflight >= SIGNAL_EVERY) reap_send(scq, &inflight, &errors);

        if ((spin & 0xFFFFF) == 0) {      /* session over when the controller hangs up */
            char b;
            ssize_t r = recv(fd, &b, 1, MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) break;
        }
    }

    *stop = 1;                            /* the kernel sees it within STOP_POLL_INTERVAL spins */
    hip_check(hipStreamSynchronize(stream), "hipStreamSynchronize");
    printf("hsb-gpu-echo: session closed after %" PRIu64 " replies, %" PRIu64 " errors\n", served, errors);
    close(fd);
    close(ls);
    ibv_destroy_qp(qp);
    ibv_dereg_mr(mr);
    ibv_destroy_cq(scq);
    ibv_destroy_cq(rcq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(list);
    (void)hipHostFree(flags);
    (void)hipHostFree(handoff);
    (void)hipFree(ring);
    (void)hipStreamDestroy(stream);
    return errors ? 1 : 0;
}
