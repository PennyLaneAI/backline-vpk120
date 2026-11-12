/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * balboa_handshake.h - session setup between the controller (balboactl on the A72, or
 * balboa-standin in its place) and a coprocessor (balboa-echo, balboa-gpu-echo), over TCP on
 * the management network.
 *
 *   A72: balboactl (controller)                coprocessor: balboa-echo
 *                                              0. create the RC QP (INIT), register the ring,
 *                                                 listen on BALBOA_HS_PORT
 *   1. connect to BALBOA_HS_PORT ----------->
 *                    <---- balboa_hs_host ---  its QP, PSN and ring
 *   2. reset BALBOA, ARP for the coprocessor,
 *      write the QP and connection contexts
 *                    ---- balboa_hs_fpga --->  BALBOA's QP and address, where replies go
 *                                              3. QP to RTR/RTS from balboa_hs_fpga
 *                    <---- balboa_hs_ready --  the QP is ready
 *   4. rounds: BALBOA --RC WRITE--> ring slot; coprocessor --RC WRITE--> BALBOA
 *   5. either side closes the socket to end the session
 *
 * One PSN for both directions: BALBOA's state table takes the QP context's local_psn as the
 * PSN it expects and remote_psn as the PSN it sends with, the opposite of the verbs naming;
 * with one value for both, as BALBOA's own tests against commercial NICs do, the two cannot
 * be mixed up.
 */
#ifndef BALBOA_HANDSHAKE_H
#define BALBOA_HANDSHAKE_H

#include <assert.h>
#include <stdint.h>

#define BALBOA_HS_MAGIC    0x31424C42u  /* "BLB1" */
#define BALBOA_HS_VERSION  1u
#define BALBOA_HS_PORT     7760

/* coprocessor -> controller */
struct balboa_hs_host {
    uint32_t magic;          /* BALBOA_HS_MAGIC */
    uint32_t version;        /* BALBOA_HS_VERSION */
    uint32_t qpn;            /* RC QP that takes BALBOA's WRITEs        -> conn remote_qpn */
    uint32_t psn;            /* the PSN of both directions (24 bit)      -> both PSNs */
    uint32_t rkey;           /* rkey of the ring MR                      -> qp r_key */
    uint32_t ring_slots;     /* slots in the ring, <= 1024               -> RING_LEN */
    uint64_t ring_iova;      /* IOVA of slot 0 (0 with an IOVA-0 MR)     -> RMT_BASE */
    uint32_t ring_stride;    /* bytes between slots, a power of two >= 64 -> RMT_STRIDE */
    uint32_t ip;             /* RoCE IPv4, host byte order               -> conn remote IP */
    uint8_t  mac[6];         /* RoCE MAC: informational, BALBOA resolves it with ARP */
    uint16_t pad;
};
static_assert(sizeof(struct balboa_hs_host) == 48, "balboa_hs_host layout");

/* controller -> coprocessor */
struct balboa_hs_fpga {
    uint32_t magic;
    uint32_t version;
    uint32_t qpn;            /* BALBOA's QP (dest_qp_num of the coprocessor's QP) */
    uint32_t psn;            /* the same PSN back (rq_psn = sq_psn = psn) */
    uint32_t ip;             /* BALBOA's IPv4, host byte order (the coprocessor's DGID) */
    uint32_t reply_rkey;     /* rkey of the reply WRITE (BALBOA does not check it) */
    uint64_t reply_addr;     /* remote address of the reply WRITE (hwhs takes every WRITE) */
    uint8_t  mac[6];         /* BALBOA's MAC */
    uint16_t pad0;
    uint32_t req_bytes;      /* bytes of each request WRITE */
    uint32_t reply_bytes;    /* bytes the reply WRITE carries: {u64 value; u32 decoder_id; u32 seq} */
    uint32_t seq_off;        /* byte offset of the u32 seq in request and reply */
    uint32_t pad1;
};
static_assert(sizeof(struct balboa_hs_fpga) == 56, "balboa_hs_fpga layout");

/* coprocessor -> controller: the QP is at RTS, BALBOA may start writing */
struct balboa_hs_ready {
    uint32_t magic;
    uint32_t qp_state;       /* IBV_QPS_RTS */
};
static_assert(sizeof(struct balboa_hs_ready) == 8, "balboa_hs_ready layout");

#endif /* BALBOA_HANDSHAKE_H */
