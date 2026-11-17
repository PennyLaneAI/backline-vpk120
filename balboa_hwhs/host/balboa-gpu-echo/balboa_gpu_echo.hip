/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * balboa-gpu-echo - GPU coprocessor test peer for backline-roce (AMD GPU, or NVIDIA through HIP's
 * CUDA backend; ConnectX NIC).
 *
 * The data path of catalyst's GPU coprocessor (rdma/gpu_verbs GpuCoprocessorSession and
 * GpuLaunchers' fused<echo_decode>), on BALBOA's RC WRITEs:
 *
 *   BALBOA --RC WRITE--> request ring in GPU HBM (fine-grained on AMD, dma-buf MR at IOVA 0)
 *   persistent kernel, one thread: polls the slot's seq in HBM, decodes (echo), and
 *       publishes {correction, seq} with one 16 B store into a host-mapped handoff ring
 *   CPU thread: polls the handoff seq and posts the reply as an inline RC WRITE of
 *       {value, decoder_id 0, seq} into the FPGA, where hwhs matches the echoed seq
 *
 * The kernel raises a ready flag and the controller is told to start only after it, so round
 * 0 cannot time out on a slow first launch. Like catalyst's kernel, this one follows its own
 * cursor; RC delivers every request in order, so it never needs to resync.
 *
 *   balboa-gpu-echo -d mlx5_0 [-i ib_port] [-g gid_index] [-p tcp_port] [-G gpu] [-c cpu] [-r]
 *
 *   -G  HIP device of the GPU (default 0)
 *   -c  core of the CPU thread that posts the replies
 *   -r  realtime, as catalyst's rt=1: SCHED_FIFO at the top priority and
 *       /dev/cpu_dma_latency = 0 (that one needs root)
 *
 * The VPK120 side (balboactl run -H <this host's management ip>) connects to tcp_port, see
 * common/include/balboa_handshake.h. One session per run; the program exits when the
 * controller closes the connection.
 */
#include <inttypes.h>

#include <hip/hip_runtime.h>

#include "balboa_handshake.h"
#include "rc_peer.h"

#define RING_STRIDE        64u    /* catalyst's slot stride */
#define RING_SLOTS         256u   /* catalyst's K_RING_SLOTS; the kernel masks with it */
#define SIGNAL_EVERY       32u
#define STOP_POLL_INTERVAL 1024u  /* the kernel looks at the stop flag this often (GpuLaunchers.hip) */

/* GPU -> CPU, one per ring slot: catalyst's HandoffSlot */
struct alignas(16) handoff_slot {
    int64_t  correction;
    uint32_t seq;
    uint32_t pad;
};

static void hip_check(hipError_t e, const char *what)
{
    if (e == hipSuccess) return;
    fprintf(stderr, "balboa-gpu-echo: %s: %s\n", what, hipGetErrorString(e));
    exit(1);
}

/* catalyst's fused<echo_decode>: one thread polls the request ring in HBM and hands each
 * correction to the CPU through host-mapped memory. */
__global__ void echo_kernel(volatile uint8_t *ring, volatile handoff_slot *handoff,
                            volatile uint32_t *stop, volatile uint32_t *ready, uint32_t seq_off)
{
    if (threadIdx.x != 0 || blockIdx.x != 0) return;
    *ready = 1;
    __threadfence_system();
    for (uint64_t cursor = 0;; ++cursor) {
        const uint32_t expect = (uint32_t)(cursor + 1);
        const size_t index = cursor & (RING_SLOTS - 1);
        volatile uint32_t *sptr = (volatile uint32_t *)(ring + index * RING_STRIDE + seq_off);
        /* the stop flag is a read over PCIe: only every STOP_POLL_INTERVAL spins */
        for (uint32_t spins = 0; *sptr != expect; ++spins)
            if ((spins & (STOP_POLL_INTERVAL - 1)) == 0u && *stop != 0) return;
        __threadfence_system();  /* seq seen: the value read below is not stale */
        const uint64_t syndrome = *(volatile uint64_t *)(ring + index * RING_STRIDE);
        handoff_slot reply = {(int64_t)syndrome, expect, 0};   /* echo_decode */
        /* one 16 B store publishes correction and seq together, then one system fence */
        *reinterpret_cast<int4 *>(const_cast<handoff_slot *>(&handoff[index])) =
            *reinterpret_cast<const int4 *>(&reply);
        __threadfence_system();
    }
}

int main(int argc, char **argv)
{
    struct rc_peer p;
    memset(&p, 0, sizeof(p));
    p.who = "balboa-gpu-echo";
    const char *devname = NULL;
    int ib_port = 1, gid_index = 3, tcp_port = BALBOA_HS_PORT, gpu = 0, cpu = -1, rt = 0, c;

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
            fprintf(stderr, "usage: balboa-gpu-echo -d <ibdev> [-i ib_port] [-g gid_index] [-p tcp_port] "
                            "[-G gpu] [-c cpu] [-r]\n");
            return 2;
        }
    }
    if (!devname) {
        fprintf(stderr, "balboa-gpu-echo: -d <ibdev> is required\n");
        return 2;
    }

    /* ---- GPU: request ring in HBM, host-mapped handoff ring and flags (GpuRuntime) ---- */
    hip_check(hipSetDevice(gpu), "hipSetDevice");
    hipStream_t stream;
    hip_check(hipStreamCreate(&stream), "hipStreamCreate");

    /* the dma-buf range is whole pages (64 KiB on Grace) */
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t ring_len = ((size_t)RING_SLOTS * RING_STRIDE + page - 1) & ~(page - 1);
    void *ring_alloc = NULL;
#ifdef __HIP_PLATFORM_NVIDIA__
    hip_check(hipMalloc(&ring_alloc, ring_len + page), "hipMalloc (ring)");
#else
    hip_check(hipExtMallocWithFlags(&ring_alloc, ring_len + page, hipDeviceMallocFinegrained),
              "hipExtMallocWithFlags (fine-grained ring)");
#endif
    void *ring = (void *)(((uintptr_t)ring_alloc + page - 1) & ~(uintptr_t)(page - 1));
    hip_check(hipMemset(ring, 0, ring_len), "hipMemset (ring)");
    int dmabuf_fd = -1;
#ifdef __HIP_PLATFORM_NVIDIA__
    /* HIP's CUDA backend maps hipMemGetHandleForAddressRange only for CUDA 12.8 and later */
    if (cuMemGetHandleForAddressRange(&dmabuf_fd, (CUdeviceptr)ring, ring_len,
                                      CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0) != CUDA_SUCCESS) {
        fprintf(stderr, "balboa-gpu-echo: cuMemGetHandleForAddressRange (dma-buf) failed\n");
        return 1;
    }
#else
    hip_check(hipMemGetHandleForAddressRange(&dmabuf_fd, (hipDeviceptr_t)ring, ring_len,
                                             hipMemRangeHandleTypeDmaBufFd, 0),
              "hipMemGetHandleForAddressRange (dma-buf)");
#endif

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

    /* ---- RC QP; the HBM ring registered through dma-buf at IOVA 0 -------------------- */
    rc_open(&p, devname, ib_port, gid_index);
    struct ibv_mr *mr = ibv_reg_dmabuf_mr(p.pd, 0, ring_len, 0, dmabuf_fd,
                                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) rc_die(&p, "ibv_reg_dmabuf_mr");
    close(dmabuf_fd);   /* the MR holds its own reference */

    /* ---- handshake ------------------------------------------------------------------ */
    int ls = rc_listen(&p, tcp_port);
    uint32_t psn = rc_new_psn();
    printf("balboa-gpu-echo: %s port %d gid %d (%u.%u.%u.%u, %s), RC QP 0x%06x psn 0x%06x, HBM ring %u x %u B "
           "on GPU %d rkey 0x%08x; waiting on tcp %d\n", devname, ib_port, gid_index, p.ip >> 24,
           (p.ip >> 16) & 255, (p.ip >> 8) & 255, p.ip & 255, p.netdev, p.qp->qp_num, psn, RING_SLOTS,
           RING_STRIDE, gpu, mr->rkey, tcp_port);
    fflush(stdout);
    int fd = accept(ls, NULL, NULL);
    if (fd < 0) rc_die(&p, "accept");

    struct balboa_hs_host hi;
    memset(&hi, 0, sizeof(hi));
    hi.magic = BALBOA_HS_MAGIC;
    hi.version = BALBOA_HS_VERSION;
    hi.qpn = p.qp->qp_num;
    hi.psn = psn;
    hi.rkey = mr->rkey;
    hi.ring_slots = RING_SLOTS;
    hi.ring_iova = 0;
    hi.ring_stride = RING_STRIDE;
    hi.ip = p.ip;
    memcpy(hi.mac, p.mac, 6);
    struct balboa_hs_fpga fo;
    if (rc_write_full(fd, &hi, sizeof(hi)) || rc_read_full(fd, &fo, sizeof(fo)) ||
        fo.magic != BALBOA_HS_MAGIC || fo.version != BALBOA_HS_VERSION) {
        fprintf(stderr, "balboa-gpu-echo: bad handshake\n");
        return 1;
    }
    if (fo.reply_bytes < 16 || fo.reply_bytes > 64 || fo.req_bytes > RING_STRIDE ||
        fo.seq_off < 8 || fo.seq_off + 4 > fo.reply_bytes || fo.seq_off % 4) {
        fprintf(stderr, "balboa-gpu-echo: controller asks for %u-byte replies / %u-byte requests, seq at %u\n",
                fo.reply_bytes, fo.req_bytes, fo.seq_off);
        return 1;
    }
    rc_connect(&p, fo.qpn, fo.psn, fo.ip);

    /* ---- the persistent kernel; the controller starts only once it is spinning ------- */
    hipLaunchKernelGGL(echo_kernel, dim3(1), dim3(1), 0, stream, (volatile uint8_t *)ring,
                       (volatile handoff_slot *)handoff_dev, (volatile uint32_t *)&flags_dev[0],
                       (volatile uint32_t *)&flags_dev[1], fo.seq_off);
    hip_check(hipGetLastError(), "kernel launch");
    while (!*ready) {
        hipError_t q = hipStreamQuery(stream);
        if (q != hipErrorNotReady) hip_check(q == hipSuccess ? hipErrorLaunchFailure : q, "kernel ended before it was ready");
        rc_cpu_relax();
    }
    /* pinned only now, so the GPU runtime's own threads stay off the reply core */
    rc_realtime(&p, cpu, rt);

    struct balboa_hs_ready rd = {BALBOA_HS_MAGIC, IBV_QPS_RTS};
    if (rc_write_full(fd, &rd, sizeof(rd))) rc_die(&p, "handshake ready");
    printf("balboa-gpu-echo: session up, replies to %u.%u.%u.%u QP %u at 0x%" PRIx64 ", %u-byte requests, "
           "%u-byte replies\n", fo.ip >> 24, (fo.ip >> 16) & 255, (fo.ip >> 8) & 255, fo.ip & 255, fo.qpn,
           fo.reply_addr, fo.req_bytes, fo.reply_bytes);
    fflush(stdout);

    /* ---- reply loop (catalyst's run_coprocessor) --------------------------------------- */
    /* Round n's correction is in handoff[n % RING_SLOTS] once its seq reads n + 1. The send
     * completions are reaped while idle, never between a handoff and its reply. */
    uint64_t served = 0, errors = 0;
    unsigned inflight = 0;          /* WRITEs posted and not yet known complete */
    uint8_t reply[64];
    memset(reply, 0, sizeof(reply));

    for (uint64_t spin = 0;; spin++) {
        const uint32_t expect = (uint32_t)(served + 1);
        volatile handoff_slot *h = &handoff[served & (RING_SLOTS - 1)];
        if (h->seq == expect) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            const int64_t correction = h->correction;
            memcpy(reply, &correction, sizeof(correction));     /* value; decoder_id stays 0 */
            memcpy(reply + fo.seq_off, &expect, sizeof(expect));
            int sig = (served + 1) % SIGNAL_EVERY == 0;
            while (inflight >= RC_MAX_SEND) rc_reap(&p, &inflight, SIGNAL_EVERY, &errors);
            rc_write_inline(&p, reply, fo.reply_bytes, fo.reply_addr, fo.reply_rkey, served, sig);
            inflight++;
            served++;
            continue;
        }

        /* Idle */
        rc_cpu_relax();
        if (inflight >= SIGNAL_EVERY) rc_reap(&p, &inflight, SIGNAL_EVERY, &errors);
        if ((spin & 0xFFFFF) == 0 && rc_hung_up(fd)) break;
    }

    *stop = 1;                            /* the kernel sees it within STOP_POLL_INTERVAL spins */
    hip_check(hipStreamSynchronize(stream), "hipStreamSynchronize");
    printf("balboa-gpu-echo: session closed after %" PRIu64 " replies, %" PRIu64 " errors\n", served, errors);
    close(fd);
    close(ls);
    ibv_dereg_mr(mr);
    rc_close(&p);
    (void)hipHostFree(flags);
    (void)hipHostFree(handoff);
    (void)hipFree(ring_alloc);
    (void)hipStreamDestroy(stream);
    return errors ? 1 : 0;
}
