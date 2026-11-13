/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * balboa-standin - the controller's side of a session in software, to test balboa-echo and
 * balboa-gpu-echo without the board.
 *
 * It runs the same handshake as balboactl (common/include/balboa_handshake.h) and the same
 * rounds as the sw-loop: each round RDMA-WRITEs a 64-byte request with the next seq into the
 * coprocessor's ring and waits for the coprocessor's WRITE of the reply into a 64-byte buffer
 * here, the way BALBOA hands it to hwhs. It stands in for BALBOA with an ordinary RC QP: one
 * PSN for both directions, the reply buffer at IOVA 0.
 *
 *   balboa-standin -d mlx5_0 -H <coprocessor management ip> [-i ib_port] [-g gid_index]
 *                  [-p tcp_port] [-n rounds] [-c cpu] [-r] [-o rtt.csv]
 *
 * Both ends can be on one host and one port (the NIC loops RC traffic between its own QPs);
 * the RTTs then measure the host and the NIC, not a wire.
 */
#define _GNU_SOURCE
#include "balboa_handshake.h"
#include "rc_peer.h"

#include <inttypes.h>
#include <time.h>

#define REQ_BYTES    64u
#define REPLY_BYTES  16u
#define SEQ_OFF      12u
#define SIGNAL_EVERY 32u

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    struct rc_peer p = {.who = "balboa-standin"};
    const char *devname = NULL, *host = NULL, *csv = NULL;
    int ib_port = 1, gid_index = 3, tcp_port = BALBOA_HS_PORT, cpu = -1, rt = 0, c;
    uint32_t rounds = 10000;

    while ((c = getopt(argc, argv, "d:H:i:g:p:n:c:ro:")) != -1) {
        switch (c) {
        case 'd': devname = optarg; break;
        case 'H': host = optarg; break;
        case 'i': ib_port = atoi(optarg); break;
        case 'g': gid_index = atoi(optarg); break;
        case 'p': tcp_port = atoi(optarg); break;
        case 'n': rounds = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'c': cpu = atoi(optarg); break;
        case 'r': rt = 1; break;
        case 'o': csv = optarg; break;
        default:
            fprintf(stderr, "usage: balboa-standin -d <ibdev> -H <coprocessor ip> [-i ib_port] [-g gid_index] "
                            "[-p tcp_port] [-n rounds] [-c cpu] [-r] [-o rtt.csv]\n");
            return 2;
        }
    }
    if (!devname || !host || rounds == 0) {
        fprintf(stderr, "balboa-standin: -d <ibdev> and -H <coprocessor ip> are required\n");
        return 2;
    }
    rc_realtime(&p, cpu, rt);
    rc_open(&p, devname, ib_port, gid_index);

    /* where the replies land, at IOVA 0 (balboactl tells the coprocessor address 0 too);
     * one page, as IOVA 0 needs (64 KiB pages on Grace) */
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *reply = (uint8_t *)aligned_alloc(page, page);
    if (!reply) rc_die(&p, "reply buffer");
    memset(reply, 0, page);
    struct ibv_mr *mr = ibv_reg_mr_iova(p.pd, reply, page, 0, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) rc_die(&p, "ibv_reg_mr_iova");

    /* ---- handshake, as balboactl run ----------------------------------------------------- */
    int fd = rc_dial(&p, host, tcp_port);
    struct balboa_hs_host hi;
    if (rc_read_full(fd, &hi, sizeof(hi)) || hi.magic != BALBOA_HS_MAGIC || hi.version != BALBOA_HS_VERSION) {
        fprintf(stderr, "balboa-standin: bad handshake from %s\n", host);
        return 1;
    }
    if (hi.ring_slots == 0 || hi.ring_slots > 1024 || (hi.ring_slots & (hi.ring_slots - 1)) ||
        hi.ring_stride < REQ_BYTES || (hi.ring_stride & (hi.ring_stride - 1))) {
        fprintf(stderr, "balboa-standin: ring of %u slots, %u B apart\n", hi.ring_slots, hi.ring_stride);
        return 1;
    }
    printf("coprocessor: qpn 0x%06x psn 0x%06x rkey 0x%08x ring %u x %u B @ 0x%" PRIx64 ", %u.%u.%u.%u\n",
           hi.qpn, hi.psn, hi.rkey, hi.ring_slots, hi.ring_stride, hi.ring_iova, hi.ip >> 24,
           (hi.ip >> 16) & 255, (hi.ip >> 8) & 255, hi.ip & 255);
    rc_connect(&p, hi.qpn, hi.psn & 0xFFFFFF, hi.ip);

    struct balboa_hs_fpga fo;
    memset(&fo, 0, sizeof(fo));
    fo.magic = BALBOA_HS_MAGIC;
    fo.version = BALBOA_HS_VERSION;
    fo.qpn = p.qp->qp_num;
    fo.psn = hi.psn & 0xFFFFFF;
    fo.ip = p.ip;
    fo.reply_rkey = mr->rkey;
    fo.reply_addr = 0;
    memcpy(fo.mac, p.mac, 6);
    fo.req_bytes = REQ_BYTES;
    fo.reply_bytes = REPLY_BYTES;
    fo.seq_off = SEQ_OFF;
    struct balboa_hs_ready rd;
    if (rc_write_full(fd, &fo, sizeof(fo)) || rc_read_full(fd, &rd, sizeof(rd)) || rd.magic != BALBOA_HS_MAGIC) {
        fprintf(stderr, "balboa-standin: coprocessor did not get ready\n");
        return 1;
    }

    /* ---- rounds: request into slot k, wait for its seq in the reply buffer ------------- */
    uint64_t *rtt = (uint64_t *)calloc(rounds, sizeof(uint64_t));
    FILE *out = csv ? fopen(csv, "w") : NULL;
    if (!rtt || (csv && !out)) rc_die(&p, "rtt buffer");
    if (out) fprintf(out, "round,rtt_ns\n");
    uint64_t errors = 0, bad = 0;
    unsigned inflight = 0;
    uint32_t done = 0, timeouts = 0;

    for (uint32_t i = 0; i < rounds; i++) {
        uint8_t req[REQ_BYTES];
        uint64_t value = 0x1000000000ull + i;
        uint32_t seq = i + 1, zero = 0;
        memset(req, 0, sizeof(req));
        memcpy(req, &value, 8);
        memcpy(req + 8, &zero, 4);
        memcpy(req + SEQ_OFF, &seq, 4);
        memcpy(req + 16, &i, 4);
        uint64_t raddr = hi.ring_iova + (uint64_t)(i & (hi.ring_slots - 1)) * hi.ring_stride;

        int sig = (i + 1) % SIGNAL_EVERY == 0;
        while (inflight >= RC_MAX_SEND) rc_reap(&p, &inflight, SIGNAL_EVERY, &errors);
        uint64_t t0 = now_ns();
        rc_write_inline(&p, req, REQ_BYTES, raddr, hi.rkey, i, sig);
        inflight++;
        while (*(volatile uint32_t *)(reply + SEQ_OFF) != seq) {
            if (now_ns() - t0 > 1000000000ull) break;
            rc_cpu_relax();
        }
        uint64_t t1 = now_ns();
        if (*(volatile uint32_t *)(reply + SEQ_OFF) != seq) {
            timeouts++;
            fprintf(stderr, "balboa-standin: round %u: no reply in 1 s\n", i);
            break;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint64_t got;
        memcpy(&got, reply, 8);
        if (got != value) bad++;
        rtt[done++] = t1 - t0;
        if (out) fprintf(out, "%u,%" PRIu64 "\n", i, t1 - t0);
        if (inflight >= SIGNAL_EVERY) rc_reap(&p, &inflight, SIGNAL_EVERY, &errors);
    }

    uint32_t n = done;
    printf("standin: %u replies, %u timeouts, %" PRIu64 " value mismatches, %" PRIu64 " WRITE errors\n", n,
           timeouts, bad, errors);
    if (n) {
        qsort(rtt, n, sizeof(*rtt), cmp_u64);
        printf("RTT (ns) over %u rounds: min %" PRIu64 "  p50 %" PRIu64 "  p99 %" PRIu64 "  p99.9 %" PRIu64
               "  max %" PRIu64 "\n", n, rtt[0], rtt[n / 2], rtt[(uint64_t)n * 99 / 100],
               rtt[(uint64_t)n * 999 / 1000], rtt[n - 1]);
    }
    if (out) fclose(out);
    close(fd);
    ibv_dereg_mr(mr);
    rc_close(&p);
    free(reply);
    free(rtt);
    return (timeouts || bad || errors) ? 1 : 0;
}
