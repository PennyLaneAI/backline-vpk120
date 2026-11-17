/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * balboa-echo - CPU coprocessor test peer for backline-roce
 *
 * BALBOA writes every request into a ring here with an RC RDMA WRITE (64 B, slot k of round
 * k). This program answers each one with an RC RDMA WRITE of the request's first 16 bytes
 * ({u64 value; u32 decoder_id; u32 seq}) into the FPGA, which BALBOA hands to hwhs, and hwhs
 * matches the echoed seq. RC delivers in order and retries on loss, so nothing is posted to
 * receive and nothing needs resyncing.
 *
 *   balboa-echo -d mlx5_0 [-i ib_port] [-g gid_index] [-p tcp_port] [-s slots] [-c cpu] [-r]
 *
 *   -s  ring slots (power of two, <= 1024), 64 B apart
 *   -r  realtime, as catalyst's rt=1: SCHED_FIFO at the top priority and
 *       /dev/cpu_dma_latency = 0 (that one needs root)
 *
 * The VPK120 side (balboactl run -H <this host's management ip>) connects to tcp_port, see
 * common/include/balboa_handshake.h. One session per run; the program exits when the
 * controller closes the connection.
 *
 * The RoCE interface must be in BALBOA's subnet: BALBOA is 192.168.0.2 after a reset.
 */
#define _GNU_SOURCE
#include "balboa_handshake.h"
#include "rc_peer.h"

#include <inttypes.h>

#define RING_STRIDE  64u
#define SIGNAL_EVERY 32u

int main(int argc, char **argv)
{
    struct rc_peer p = {.who = "balboa-echo"};
    const char *devname = NULL;
    int ib_port = 1, gid_index = 3, tcp_port = BALBOA_HS_PORT, cpu = -1, rt = 0, c;
    unsigned slots = 256;

    while ((c = getopt(argc, argv, "d:i:g:p:s:c:r")) != -1) {
        switch (c) {
        case 'd': devname = optarg; break;
        case 'i': ib_port = atoi(optarg); break;
        case 'g': gid_index = atoi(optarg); break;
        case 'p': tcp_port = atoi(optarg); break;
        case 's': slots = (unsigned)atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'r': rt = 1; break;
        default:
            fprintf(stderr, "usage: balboa-echo -d <ibdev> [-i ib_port] [-g gid_index] [-p tcp_port] "
                            "[-s slots] [-c cpu] [-r]\n");
            return 2;
        }
    }
    if (!devname || slots == 0 || slots > 1024 || (slots & (slots - 1))) {
        fprintf(stderr, "balboa-echo: -d <ibdev> is required; slots must be a power of two <= 1024\n");
        return 2;
    }
    rc_realtime(&p, cpu, rt);
    rc_open(&p, devname, ib_port, gid_index);

    /* the ring BALBOA writes, at IOVA 0; page-aligned, as IOVA 0 needs (64 KiB pages on Grace) */
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t ring_len = (size_t)slots * RING_STRIDE;
    uint8_t *ring = (uint8_t *)aligned_alloc(page, (ring_len + page - 1) & ~(page - 1));
    if (!ring) rc_die(&p, "ring");
    memset(ring, 0, ring_len);
    struct ibv_mr *mr = ibv_reg_mr_iova(p.pd, ring, ring_len, 0, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) rc_die(&p, "ibv_reg_mr_iova");

    /* ---- handshake ---------------------------------------------------------------------- */
    int ls = rc_listen(&p, tcp_port);
    uint32_t psn = rc_new_psn();
    printf("balboa-echo: %s port %d gid %d (%u.%u.%u.%u, %s), RC QP 0x%06x psn 0x%06x, ring %u x %u B "
           "rkey 0x%08x; waiting on tcp %d\n", devname, ib_port, gid_index, p.ip >> 24, (p.ip >> 16) & 255,
           (p.ip >> 8) & 255, p.ip & 255, p.netdev, p.qp->qp_num, psn, slots, RING_STRIDE, mr->rkey, tcp_port);
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
    hi.ring_slots = slots;
    hi.ring_iova = 0;
    hi.ring_stride = RING_STRIDE;
    hi.ip = p.ip;
    memcpy(hi.mac, p.mac, 6);
    struct balboa_hs_fpga fo;
    if (rc_write_full(fd, &hi, sizeof(hi)) || rc_read_full(fd, &fo, sizeof(fo)) ||
        fo.magic != BALBOA_HS_MAGIC || fo.version != BALBOA_HS_VERSION) {
        fprintf(stderr, "balboa-echo: bad handshake\n");
        return 1;
    }
    if (fo.reply_bytes == 0 || fo.reply_bytes > 64 || fo.req_bytes > RING_STRIDE ||
        fo.seq_off + 4 > fo.reply_bytes || fo.seq_off % 4) {
        fprintf(stderr, "balboa-echo: controller asks for %u-byte replies / %u-byte requests, seq at %u\n",
                fo.reply_bytes, fo.req_bytes, fo.seq_off);
        return 1;
    }

    rc_connect(&p, fo.qpn, fo.psn, fo.ip);
    struct balboa_hs_ready rd = {BALBOA_HS_MAGIC, IBV_QPS_RTS};
    if (rc_write_full(fd, &rd, sizeof(rd))) rc_die(&p, "handshake ready");
    printf("balboa-echo: session up, replies to %u.%u.%u.%u QP %u at 0x%" PRIx64 ", %u-byte requests, "
           "%u-byte replies\n", fo.ip >> 24, (fo.ip >> 16) & 255, (fo.ip >> 8) & 255, fo.ip & 255, fo.qpn,
           fo.reply_addr, fo.req_bytes, fo.reply_bytes);
    fflush(stdout);

    /* ---- echo loop ---------------------------------------------------------------------- */
    uint64_t served = 0, errors = 0;
    unsigned inflight = 0;            /* WRITEs posted and not yet known complete */
    uint32_t slot = 0, expect = 1;    /* hwhs stamps seq 1 in the first round after its reset */

    for (uint64_t spin = 0;; spin++) {
        const uint8_t *s = ring + (size_t)slot * RING_STRIDE;
        if (*(const volatile uint32_t *)(s + fo.seq_off) == expect) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            int sig = (served + 1) % SIGNAL_EVERY == 0;
            while (inflight >= RC_MAX_SEND) rc_reap(&p, &inflight, SIGNAL_EVERY, &errors);
            rc_write_inline(&p, s, fo.reply_bytes, fo.reply_addr, fo.reply_rkey, served, sig);
            inflight++;
            served++;
            slot = (slot + 1) & (slots - 1);
            expect++;
            continue;
        }

        /* Idle */
        rc_cpu_relax();
        if (inflight >= SIGNAL_EVERY) rc_reap(&p, &inflight, SIGNAL_EVERY, &errors);
        if ((spin & 0xFFFFF) == 0 && rc_hung_up(fd)) break;
    }

    printf("balboa-echo: session closed after %" PRIu64 " replies, %" PRIu64 " errors\n", served, errors);
    close(fd);
    close(ls);
    ibv_dereg_mr(mr);
    rc_close(&p);
    free(ring);
    return errors ? 1 : 0;
}
