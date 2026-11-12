/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * hwhs.h - userspace driver for hwhs-balboa and the BALBOA RoCE v2 stack behind it (VPK120 A72).
 *
 * The windows are mapped through generic-uio (matched by physical address) or through /dev/mem.
 */
#ifndef HWHS_H
#define HWHS_H

#include <stddef.h>
#include <stdint.h>

#include "hwhs_regs.h"

struct hwhs {
    volatile uint32_t *regs;     /* hwhs registers */
    volatile uint8_t *slot;      /* slot RAM: request payloads */
    volatile uint8_t *reply;     /* reply RAM */
    volatile uint32_t *trace;    /* RTT trace, two 16-bit entries per word */
    volatile uint8_t *demo;      /* transport_demo table */
    volatile uint32_t *gpio_id;  /* ch1 BUILD_ID, ch2 net clock Hz */
    volatile uint32_t *gpio_ctl; /* ch1 [0] BALBOA reset, ch2 status */
    uint32_t clk_hz;             /* net clock */
};

/* BALBOA's address on the 100G port: what lynx_pkg.sv gives it after a reset, the same as
 * HSB host port 0's in the HSB build */
#define HWHS_FPGA_IP  0xC0A80002u                          /* 192.168.0.2 */
#define HWHS_FPGA_MAC {0x02, 0x93, 0xc6, 0x48, 0x4d, 0x8e}

/*
 * The QP the requests go out on. Coyote's rdma_flow (BALBOA's flow control and the slot of
 * each WRITE in the retransmission buffer) looks up its per-QP pointers one cycle late and
 * always reads the entry of QP 0, so only QP 0 (or a QP with the same entry, vfid even and
 * pid 0) is tracked correctly. QP 0 is also the first QP Coyote's own software opens.
 */
#define HWHS_FPGA_QPN 0u

/* Map a physical window: the UIO device whose map0 starts at `phys`, else /dev/mem. */
void *hwhs_map(uint64_t phys, size_t len);

int hwhs_open(struct hwhs *h);
void hwhs_close(struct hwhs *h);

static inline uint32_t hwhs_rd(const struct hwhs *h, uint32_t off) { return h->regs[off / 4]; }
static inline void hwhs_wr(const struct hwhs *h, uint32_t off, uint32_t v) { h->regs[off / 4] = v; }

/* Hold BALBOA in reset for a moment (QP state, PSNs, ARP table), then wait for it to come back.
 * The engine is reset with it; hwhs's registers keep their values. */
int hwhs_stack_reset(struct hwhs *h, int timeout_ms);

/* Give BALBOA its IPv4 (host byte order) and MAC. */
int hwhs_set_addr(struct hwhs *h, uint32_t ip, const uint8_t mac[6]);

/* Ask BALBOA's ARP server to resolve `ip`; 0 once an ARP reply has come in, -1 on timeout. */
int hwhs_arp(struct hwhs *h, uint32_t ip, int timeout_ms);

/* One RC connection between BALBOA and a coprocessor */
struct hwhs_qp {
    uint32_t local_qpn;   /* BALBOA's QP: HWHS_FPGA_QPN */
    uint32_t remote_qpn;  /* the coprocessor's QP */
    uint32_t psn;         /* the PSN of both directions (balboa_handshake.h) */
    uint32_t rkey;        /* rkey of the coprocessor's ring */
    uint64_t ring_iova;   /* its slot 0 */
    uint32_t remote_ip;   /* its RoCE IPv4, host byte order */
    uint16_t udp_sport;   /* UDP source port of BALBOA's packets */
};

/*
 * The contexts BALBOA takes, as the six 32-bit words of hwhs_regs QP_CTX / CONN_CTX
 * (word i = bits [32i+31:32i]):
 *
 *   rdma_qp_ctx_t   [31:0] new_state (0) [55:32] qp_num [79:56] remote_psn [103:80] local_psn
 *                   [135:104] r_key [183:136] vaddr
 *   rdma_qp_conn_t  [15:0] local_qpn [39:16] remote_qpn [167:40] remote_ip_address
 *                   [183:168] remote_udp_port
 *
 * The remote address is the IPv4 address four times, first octet in the low byte of each
 * 32-bit word; remote_udp_port is the source port of BALBOA's packets (the destination is
 * always 4791).
 */
void hwhs_qp_ctx_words(const struct hwhs_qp *qp, uint32_t w[6]);
void hwhs_conn_ctx_words(const struct hwhs_qp *qp, uint32_t w[6]);

/* Write both contexts and push them to BALBOA. */
int hwhs_qp_connect(struct hwhs *h, const struct hwhs_qp *qp, int timeout_ms);

/* Engine setup for a session: fresh counters, seq from 1, the ring on both sides. */
struct hwhs_ring {
    uint64_t rmt_base;      /* coprocessor ring, slot 0 */
    uint32_t stride;        /* bytes between its slots, a power of two >= 64 */
    uint32_t slots;         /* RING_LEN, <= HWHS_SLOTS */
    uint32_t req_bytes;     /* REQ_LEN, 1..64 */
    uint32_t qpn;           /* LOCAL_QPN */
    uint32_t reply_to;      /* REPLY_TO, net clock cycles (0 = forever) */
};
int hwhs_engine_setup(struct hwhs *h, const struct hwhs_ring *r);

/* BALBOA's counters, HWHS_STAT_* order */
void hwhs_net_stats(const struct hwhs *h, uint32_t out[HWHS_NET_STATS_N]);

/*
 * One sw-loop round: `req` (<= 64 B) goes into slot RAM[RING_IDX], CTRL.start, wait.
 * Returns 0 with the reply (64 B) and the RTT in net clock cycles, 1 on reply timeout,
 * -1 if the engine did not finish at all.
 */
int hwhs_round(struct hwhs *h, const void *req, size_t len, void *reply, uint64_t *rtt_cycles);

/* hw-loop: transport_demo sends `n` syndromes paced `period` net clock cycles apart. */
struct hwhs_demo_result {
    uint32_t rounds; /* replies matched (ROUND_CNT delta) */
    uint32_t timeouts;
    uint32_t errors;    /* transport_demo compare errors */
    uint32_t trace_cnt; /* RTT trace entries */
};
int hwhs_demo_run(struct hwhs *h, const void *syndromes, const void *expected, uint32_t table,
                  uint32_t n, uint64_t period, uint32_t span_mask, int timeout_ms,
                  struct hwhs_demo_result *r);

/* Copy `n` RTT trace entries (net clock cycles, 0xFFFF = saturated) out of the trace RAM. */
void hwhs_trace_read(const struct hwhs *h, uint16_t *out, uint32_t n);

static inline double hwhs_ns(const struct hwhs *h, uint64_t cycles)
{
    return (double)cycles * 1e9 / (double)h->clk_hz;
}

#endif /* HWHS_H */
