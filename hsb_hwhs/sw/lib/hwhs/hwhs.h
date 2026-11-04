/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * hwhs.h - userspace driver for hwhs-hsb and the HSB behind it (VPK120 A72).
 *
 * The windows are mapped through generic-uio (matched by physical address) or through /dev/mem.
 */
#ifndef HWHS_H
#define HWHS_H

#include <stddef.h>
#include <stdint.h>

#include "hsb_regs.h"
#include "hwhs_regs.h"

struct hwhs {
    volatile uint32_t *regs;     /* hwhs registers + ECB mailbox */
    volatile uint8_t *slot;      /* slot RAM: request payloads */
    volatile uint8_t *reply;     /* reply RAM */
    volatile uint32_t *trace;    /* RTT trace, two 16-bit entries per word */
    volatile uint8_t *demo;      /* transport_demo table */
    volatile uint32_t *gpio_id;  /* ch1 BUILD_ID, ch2 hif Hz */
    volatile uint32_t *gpio_ctl; /* ch1 [0] HSB reset, ch2 status */
    uint32_t clk_hz;             /* hif clock */

    /* ECB: the A72 is a "host" on HSB host port 1 (the mailbox) */
    uint8_t ps_mac[6];
    uint32_t ps_ip;
    uint16_t ps_port;
    uint8_t hsb_mac[6]; /* HSB host port 1 */
    uint32_t hsb_ip;
    uint16_t ecb_seq;
    int ecb_timeout_ms;
};

/* HSB host port addresses, as wired in hw/rtl/top/backline_hsb_top.sv */
#define HWHS_HSB_MAC0 {0x02, 0x93, 0xc6, 0x48, 0x4d, 0x8e} /* port 0: 100G to the coprocessor */
#define HWHS_HSB_MAC1 {0x02, 0x93, 0xc6, 0x48, 0x4d, 0x8f} /* port 1: ECB mailbox */
#define HWHS_PS_MAC {0x02, 0x93, 0xc6, 0x48, 0x4d, 0x90}   /* the A72's source MAC on port 1 */
#define HWHS_PS_IP 0xC0A800FEu                             /* 192.168.0.254 */
#define HWHS_PS_PORT 12345u

/* Map a physical window: the UIO device whose map0 starts at `phys`, else /dev/mem. */
void *hwhs_map(uint64_t phys, size_t len);

int hwhs_open(struct hwhs *h);
void hwhs_close(struct hwhs *h);

static inline uint32_t hwhs_rd(const struct hwhs *h, uint32_t off) { return h->regs[off / 4]; }
static inline void hwhs_wr(const struct hwhs *h, uint32_t off, uint32_t v) { h->regs[off / 4] = v; }

/* HSB registers over ECB. 0 on success, -1 on timeout or an ECB error code. */
int hwhs_ecb_read(struct hwhs *h, uint32_t addr, uint32_t *val);
int hwhs_ecb_write(struct hwhs *h, uint32_t addr, uint32_t val);

/* Pulse HSB's reset (clears its slot pointer and PSN) and wait for init_done. */
int hwhs_hsb_reset(struct hwhs *h, int timeout_ms);

/* Where HSB writes requests: the coprocessor's ring (see hsb_handshake.h). */
struct hwhs_dp {
    uint32_t qpn;
    uint32_t rkey;
    uint64_t ring_iova;
    uint32_t ring_pitch; /* multiple of 128 */
    uint32_t ring_slots;
    uint32_t host_ip;
    uint8_t host_mac[6];
    uint32_t frame_bytes; /* bytes per request message (64) */
};

/* Program HSB data plane 0, sensor 0 and sensor-TX 0. */
int hwhs_hsb_configure(struct hwhs *h, const struct hwhs_dp *dp);
/* Stop HSB sending (VP mask off). */
int hwhs_hsb_stop(struct hwhs *h);

/*
 * One sw-loop round: `req` (<= 64 B) goes into slot RAM[RING_IDX], CTRL.start, wait.
 * Returns 0 with the reply (64 B) and the RTT in hif cycles, 1 on reply timeout,
 * -1 if the engine did not finish at all.
 */
int hwhs_round(struct hwhs *h, const void *req, size_t len, void *reply, uint64_t *rtt_cycles);

/* hw-loop: transport_demo sends `n` syndromes paced `period` hif cycles apart. */
struct hwhs_demo_result {
    uint32_t rounds; /* replies matched (ROUND_CNT delta) */
    uint32_t timeouts;
    uint32_t errors;    /* transport_demo compare errors */
    uint32_t trace_cnt; /* RTT trace entries */
};
int hwhs_demo_run(struct hwhs *h, const void *syndromes, const void *expected, uint32_t table,
                  uint32_t n, uint64_t period, uint32_t span_mask, int timeout_ms,
                  struct hwhs_demo_result *r);

/* Copy `n` RTT trace entries (hif cycles, 0xFFFF = saturated) out of the trace RAM. */
void hwhs_trace_read(const struct hwhs *h, uint16_t *out, uint32_t n);

static inline double hwhs_ns(const struct hwhs *h, uint64_t cycles)
{
    return (double)cycles * 1e9 / (double)h->clk_hz;
}

#endif /* HWHS_H */
