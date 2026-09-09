// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC hardware handshake driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 * Copyright 2026 Xanadu Quantum Technologies Inc.
 *
 * Author : Anjaneyulu Reddy Mule <anjaneyu@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation and may be copied,
 * distributed, and modified under those terms.
 */
#ifndef _HW_HAND_SHAKE_H
#define _HW_HAND_SHAKE_H

#include <linux/mutex.h> // struct mutex
#include <linux/types.h> // u8/u16/u32/u64, dev_t, dma_addr_t, bool

#define HH_REG_CTRL 0x00 // W1P[0]=start  RW[1]=abort  RW[2]=soft_reset
#define HH_REG_STATUS                                                                              \
    0x04                      // RO  [3:0]state [4]busy [5]done [6]reply_seen
                              //     [7]timeout [8]axi_err
#define HH_REG_QP_NUM 0x08    // ERNIC QP number
#define HH_REG_PAY_LEN 0x0C   // payload bytes (<=4096 (4KB), multiple of 64)
#define HH_REG_PATTERN 0x10   // 32-bit fixed pattern (replicated across 64B) Only for debugging
#define HH_REG_DBUF_LO 0x14   // local send-buffer DDR address [31:0]
#define HH_REG_DBUF_HI 0x18   //                               [63:32]
#define HH_REG_SQBASE_LO 0x1C // SQ ring base DDR address [31:0]
#define HH_REG_SQBASE_HI 0x20 //                          [63:32]
#define HH_REG_SQ_DEPTH 0x24  // SQ ring depth (entries)
#define HH_REG_FLAGS                                                                               \
    0x28 // [0]=repost_wqe(=1 default; =0 skip reposting the WQE)
         // [1]=write_payload (=0 default: engine doesn't write the fixed pattern)
         // [2]=complete_on_send (1: round done on send-CQ completion; 0: on inbound RQ reply)
         // [3]=reply_poll (=0 default: no reply polling; 1: polling reply buffer)
#define HH_REG_REPLY_TO 0x30       // reply timeout in core cycles (0: wait forever)
#define HH_REG_ROUND_CNT 0x34      // completed rounds (for now, it is always 1)
#define HH_REG_RTT_LO 0x38         // round cycle count, measured from START to reply [31:0]
#define HH_REG_RTT_HI 0x3C         //                                                 [63:32]
#define HH_REG_WQE_OPCODE 0x40     // [1:0] opcode (00 write, 01 read, 10 send)
#define HH_REG_WQE_XFER_LEN 0x44   // RDMA transfer length in bytes (e.g. 64)
#define HH_REG_WQE_RKEY 0x48       // remote key
#define HH_REG_WQE_VA_LO 0x4C      // remote VA [31:0]
#define HH_REG_WQE_VA_HI 0x50      // remote VA [63:32]
#define HH_REG_WQE_OFFS_LO 0x54    // local offset (DDR buf addr) [31:0]
#define HH_REG_WQE_OFFS_HI 0x58    //                             [63:32]
#define HH_REG_WQE_WRID 0x5C       // [15:0] WQE id (TODO: not used in this version, always 0)
#define HH_REG_WQE_COMMIT 0x60     // assemble current fields and store at WQE store[idx]
#define HH_REG_RUN_WQE_IDX 0x64    // fetches and sends
#define HH_REG_TIMER_QP_SEL 0x68   // selects the QP whose reply latches RTT
#define HH_REG_REPLY_BUF_LO 0x6C   // reply buffer head DDR address [31:0]
#define HH_REG_REPLY_BUF_HI 0x70   //                               [63:32]
#define HH_REG_REPLY_SEQ_OFF 0x74  // byte offset of seq_num inside a reply slot (reply_poll)
#define HH_REG_LAST_RD 0x78        // DEBUG: last value the reply-poll READ got
#define HH_REG_QPCTX_COMMIT 0x7C   // [2:0]ctx commit staging into QP-context[ctx]; [31]rebind
#define HH_REG_WQE_QP_REF 0x84     // [ctx] qp_ref: which QP-context the WQE being built uses
#define HH_REG_WQE_RING_CFG 0x88   // [0]ring_en (rings LADDR+ROFFSET+reply) [13:8]sll [21:16]srl
#define HH_REG_WQE_RING_LEN 0x8C   // [15:0] ring length (buffers in the pool)
#define HH_REG_RING_RESET 0x90     // [idx] zero that WQE's ring_idx (also auto on WQE_COMMIT)
#define HH_REG_REPLY_RING 0x94     // [5:0]=reply_stride_log2 (reply slot = ring_idx)

#define HH_REG_DEMO_CTRL 0xA8  // W1P [0]=start_demo             RW [1]=reset_demo
#define HH_REG_FREQ_LO 0xAC    // RW pacer period in core cycles    [31:0]
#define HH_REG_FREQ_HI 0xB0    //                                   [63:32]
#define HH_REG_DEMO_STS 0xB4   // RO [0]=demo_done
#define HH_REG_CMD_CNT 0xB8    // RW how many syndromes one run sends (syn_sum)
#define HH_REG_DEMO_ERR 0xBC   // RO accumulated compare errors
#define HH_REG_DEMO_DEPTH 0xC0 // RW syndrome table size in BYTES
#define HH_REG_FREQ_SPAN 0xC4  // RW [31:0] pacer jitter mask (0 = fixed interval)
#define HH_REG_LFSR_SEED 0xC8  // RW [31:0] pacer jitter seed (0 = default)

// RTT TRACE:
#define HH_REG_TRACE_CTRL                                                                          \
    0xCC                      // RW [0]=trace_clear. Hold it high to keep the RTT trace empty.
                              // drop it to start recording. Have to raise it again before a run.
#define HH_REG_TRACE_CNT 0xD0 // RO [31:0] rtt trace entries recorded so far.
#define HH_REG_TRACE_STS                                                                           \
    0xD4 // RO [0]=full
         //    [1]=saturated
         //       (some round exceeded 16 bits and was clamped to 0xFFFF)
#define HH_TRACE_CLEAR (1u << 0)
#define HH_TRACE_STS_FULL (1u << 0)
#define HH_TRACE_STS_SAT (1u << 1)

#define HH_REG_WIN_SIZE 0x1000 // map one 4 KB AXI4-Lite window

#define HH_NUM_CTX 8 // QP-context table depth in the controller (C_NUM_CTX)

// CTRL (0x00)
#define HH_CTRL_START (1u << 0) // W1P to start a round
#define HH_CTRL_ABORT (1u << 1)
#define HH_CTRL_SOFT_RESET (1u << 2)

// DEMO_CTRL (0xA8)
#define HH_DEMO_START (1u << 0) // W1P
#define HH_DEMO_RESET (1u << 1) // level: clears all of them

// DEMO_STS (0xB4)
#define HH_DEMO_STS_DONE (1u << 0)

// STATUS (0x04)
#define HH_STATUS_STATE_MASK 0xFu
#define HH_STATUS_BUSY (1u << 4)
#define HH_STATUS_DONE (1u << 5)
#define HH_STATUS_REPLY_SEEN (1u << 6)
#define HH_STATUS_TIMEOUT (1u << 7)
#define HH_STATUS_AXI_ERR (1u << 8)

// FLAGS (0x28)
#define HH_FLAG_REPOST_WQE (1u << 0)       // re-post the WQE each round (default on)
#define HH_FLAG_WRITE_PAYLOAD (1u << 1)    // UNUSED: the engine no longer writes the data buf
#define HH_FLAG_COMPLETE_ON_SEND (1u << 2) // round done on send-CQ (read/no-reply)
#define HH_FLAG_REPLY_POLL (1u << 3)       // detect the reply by POLLING reply_buf in DDR

// WQE opcode encoding (matches the controller)
#define HH_OP_WRITE 0
#define HH_OP_READ 1
#define HH_OP_SEND 2

// QPCTX_COMMIT (0x7C) bits
#define HH_QPCTX_REBIND (1u << 31)

// WQE_RING_CFG (0x88) bits
#define HH_RING_EN (1u << 0) // rings LADDR + ROFFSET + reply together

#define ADDRW_32BIT 32
#define ADDRW_64BIT 64

enum { HH_DIS_STATE, HH_EN_STATE };

// Round configuration.
struct hh_round_cfg {
    u32 qp_num;            // controller QP_NUM (physical ERNIC QP for this round) -> ctx0
    u32 qp_cnt;            // #QPs to place in HW-HS mode (>=1)
    u32 payload_len;       // PAY_LEN, bytes
    u32 pattern;           // PATTERN
    u64 data_buf;          // DBUF  (local send-buffer DDR address)
    u64 sq_base;           // SQBASE (SQ ring DDR address)
    u32 sq_depth;          // SQ_DEPTH (entries)
    u32 flags;             // HH_FLAG_*
    u32 reply_to;          // REPLY_TO, core cycles (0= wait forever)
    u32 timer_qp_sel;      // TIMER_QP_SEL (which QP's reply latches rtt)
    u32 run_wqe_idx;       // RUN_WQE_IDX (which stored WQE a round sends)
    u64 reply_buf;         // REPLY_BUF reply buffer head DDR addr (reply_poll mode)
    u32 reply_seq_off;     // byte offset of seq_num inside a reply slot
    u32 reply_stride_log2; // reply ring: poll reply_buf + (ring_idx << reply_stride_log2);
};

// WQE configuration
struct hh_wqe_cfg {
    u32 idx;      // COMMIT slot index
    u32 opcode;   // HH_OP_*
    u32 xfer_len; // RDMA transfer length, BYTES
    u32 rkey;
    u32 va_lsb;
    u32 va_msb;
    u64 local_offset;       // WQE local offset (DDR buffer address)
    u32 wrid;               // [15:0] used
    u32 qp_ref;             // which QP-context this WQE uses (0 = ctx0)
    u32 ring_en;            // 1 = ring LADDR + ROFFSET + reply together
    u32 ring_len;           // number of buffers in the pool
    u32 stride_local_log2;  // LADDR   += ring_idx << stride_local_log2
    u32 stride_remote_log2; // ROFFSET += ring_idx << stride_remote_log2
};

// QP-context configuration
struct hh_qpctx_cfg {
    u32 ctx_idx;           // QP-context slot (0..HH_NUM_CTX-1)
    u32 qp_num;            // physical ERNIC QP for this context
    u64 sq_base;           // SQ ring base address (DDR/BRAM)
    u32 sq_depth;          // SQ ring depth (entries)
    u64 reply_buf;         // per-QP reply buffer base address
    u32 flags;             // HH_FLAG_*
    u32 reply_to;          // per-QP reply timeout, core cycles (0 = wait forever)
    u32 reply_seq_off;     // per-QP: seq_num's byte offset in a reply slot
    u32 reply_stride_log2; // per-QP reply slot stride (log2 bytes)
    u32 rebind;            // 1 = (re)create QP: zero sq_pi + ERNIC SQPI; 0 = config-only update
};

struct hh_mr_rebase {
    u32 rkey; // key from ibv_reg_mr_ex on the DDR placeholder
    u32 _pad;
    u64 bram_pa; // BRAM physical base the MR should point at
};

// Status snapshot
struct hh_status_rd {
    u32 status;    // raw STATUS register (HH_STATUS_*)
    u32 round_cnt; // ROUND_CNT
    u64 rtt;       // {RTT_HI, RTT_LO} cycles
    u32 last_rd;   // DEBUG: last value the reply-poll READ returned
};

struct hh_demo_cfg {
    __u64 freq_num;  // cycles between sends
    __u32 cmd_cnt;   // syndromes per run
    __u32 syn_depth; // syndrome table size in bytes (multiple of 64)
    __u32 freq_span; // jitter mask
    __u32 lfsr_seed; // 0 = default
};

// The RTT trace's state.
struct hh_trace_status {
    __u32 cnt;  // num of completed rounds
    __u32 full; // flag: the buffer filled and stopped: rounds after cnt were not recorded
    __u32 sat;  // flag: at least one entry was clamped to 0xFFFF
    __u32 _pad;
};

struct hh_demo_status {
    __u32 done;      // DEMO_STS[0]
    __u32 err_cnt;   // accumulated compare errors
    __u32 cmd_cnt;   // read back from CMD_CNT
    __u32 syn_depth; // read back from DEMO_DEPTH
    __u64 freq_num;  // read back from FREQ_{LO,HI}
};

struct hh_cfg_info {
    u64 base;
    u64 size;
};

struct hw_hs_dev {
    struct hh_cfg_info cfg;
    struct hh_round_cfg rcfg; // last config (start/reset reuse qp_cnt/depth)
    u8 __iomem *reg_base;     // single controller AXI4-Lite window
    struct class *cl;
    struct cdev *cdevice;
    struct mutex mutx;
    struct device *dev;
    dev_t dev_num;
    u32 addr_width;
    u32 num_qp;

    // committed QP contexts
    struct {
        u32 qp_num;
        u32 sq_depth;
        bool valid;
    } ctx[HH_NUM_CTX];
};

enum hw_hsk_mod_fn {
    HH_FN_CONFIG = 0,   // write the round config (struct hh_round_cfg)
    HH_FN_WQE,          // assemble + store one WQE   (struct hh_wqe_cfg)
    HH_FN_START,        // CTRL.start (after ERNIC HW-HS enable)
    HH_FN_ABORT,        // CTRL.abort
    HH_FN_RESET,        // CTRL.soft_reset + ERNIC HW-HS disable
    HH_FN_READ_STATUS,  // read STATUS/ROUND_CNT/RTT  (struct hh_status_rd)
    HH_FN_SET_PATTERN,  // write PATTERN only (u32)
    HH_FN_SET_WQE_IDX,  // write RUN_WQE_IDX only (u32)
    HH_FN_QPCTX_COMMIT, // bind SQ<->QP into a context  (struct hh_qpctx_cfg)
    HH_FN_MR_REBASE,    // re-point an MR's buffer base at a BRAM PA (struct hh_mr_rebase)

    // transport demo
    HH_FN_DEMO_CFG,     // program the pacer                (struct hh_demo_cfg)
    HH_FN_DEMO_START,   // DEMO_CTRL[0] W1P, arm one round
    HH_FN_DEMO_STOP,    // clear the demo counters
    HH_FN_DEMO_STATUS,  // read the pacer result            (struct hh_demo_status)
    HH_FN_TRACE_ARM,    // clear the RTT trace and start recording
    HH_FN_TRACE_STATUS, // entries recorded + full/saturated (struct hh_trace_status)

    MAX_HW_HSK_FNS,
};
#endif
