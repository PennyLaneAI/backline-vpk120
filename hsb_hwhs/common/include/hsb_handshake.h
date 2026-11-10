/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * hsb_handshake.h - session setup between hsbctl and the coprocessor (hsb-echo), over TCP.
 *
 *   A72: hsbctl (controller)                 coprocessor: hsb-echo
 *                                            0. create the UC QP (INIT)
 *                                               register the ring
 *                                               post RECVs
 *                                               listen on HSB_HS_PORT
 *   1. connect to HSB_HS_PORT ------------>
 *                    <---- hsb_hs_host ----  where HSB must write
 *   2.               ----- hsb_hs_fpga --->  where the reply SEND goes
 *                                            3. QP to RTR/RTS from hsb_hs_fpga
 *                    <---- hsb_hs_ready ---  the QP is ready
 *   4. reset HSB, program it over ECB from
 *      hsb_hs_host (hwhs_hsb_configure), run rounds
 *      rounds: HSB --UC WRITE (imm=0) or WRITE_WITH_IMM (imm=1)--> ring slot;
 *      coprocessor --UC SEND--> HSB
 *   5. either side closes the socket to end the session
 *
 * HSB has no verbs, so the controller exchanges addresses for it and programs HSB over ECB.
 */
#ifndef HSB_HANDSHAKE_H
#define HSB_HANDSHAKE_H

#include <stdint.h>

#define HSB_HS_MAGIC    0x31425348u  /* "HSB1" */
#define HSB_HS_VERSION  1u
#define HSB_HS_PORT     7760

/* coprocessor -> controller */
struct hsb_hs_host {
    uint32_t magic;          /* HSB_HS_MAGIC */
    uint32_t version;        /* HSB_HS_VERSION */
    uint32_t qpn;            /* UC QP that receives HSB's WRITEs        -> HSB DP_QP */
    uint32_t rkey;           /* rkey of the ring MR                     -> DP_RKEY */
    uint64_t ring_iova;      /* IOVA of slot 0 (0 with ibv_reg_mr_iova) -> DP_PAGE_LSB/MSB */
    uint32_t ring_pitch;     /* bytes between slots, multiple of 128    -> DP_PAGE_INC */
    uint32_t ring_slots;     /* slots in the ring, <= 4096              -> DP_MAX_BUFF + 1 */
    uint32_t ip;             /* host RoCE IPv4, host byte order         -> DP_HOST_IP */
    uint8_t  mac[6];         /* host RoCE MAC                           -> DP_HOST_MAC_* */
    uint16_t imm;            /* 1: WRITE_WITH_IMM, 0: plain WRITE */
    uint32_t reserved;
};
_Static_assert(sizeof(struct hsb_hs_host) == 48, "hsb_hs_host layout");

/* controller -> coprocessor */
struct hsb_hs_fpga {
    uint32_t magic;
    uint32_t version;
    uint32_t ip;             /* HSB host port 0 IPv4, host byte order (reply GID) */
    uint8_t  mac[6];         /* HSB host port 0 MAC */
    uint16_t pad0;
    uint32_t qpn;            /* destination QP of the reply SEND: 2 = sensor-TX port 0 */
    uint32_t frame_bytes;    /* request bytes HSB writes per message (DP_BUFFER_LENGTH) */
    uint32_t reply_bytes;    /* reply SEND payload the controller expects */
    uint32_t seq_off;        /* byte offset of the u32 seq in request and reply */
};
_Static_assert(sizeof(struct hsb_hs_fpga) == 36, "hsb_hs_fpga layout");

/* coprocessor -> controller: the QP is at RTS, HSB may start writing */
struct hsb_hs_ready {
    uint32_t magic;
    uint32_t qp_state;       /* IBV_QPS_RTS */
};
_Static_assert(sizeof(struct hsb_hs_ready) == 8, "hsb_hs_ready layout");

#endif /* HSB_HANDSHAKE_H */
