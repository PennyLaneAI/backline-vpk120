/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * hwhs.c - userspace driver for hwhs-balboa and the BALBOA RoCE v2 stack behind it (VPK120 A72).
 */
#define _GNU_SOURCE
#include "hwhs.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Barrier to ensure all writes are visible to the device */
static inline void io_barrier(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------------------ */
/* Mapping                                                                              */
/* ------------------------------------------------------------------------------------ */

static int read_sysfs_u64(const char *path, uint64_t *v)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fscanf(f, "%" SCNx64, v) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

void *hwhs_map(uint64_t phys, size_t len)
{
    char path[128];
    uint64_t addr, size;

    for (int i = 0; i < 64; i++) {
        snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps/map0/addr", i);
        if (read_sysfs_u64(path, &addr) || addr != phys) continue;
        snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps/map0/size", i);
        if (read_sysfs_u64(path, &size) || size < len) continue;
        snprintf(path, sizeof(path), "/dev/uio%d", i);
        int fd = open(path, O_RDWR | O_SYNC);
        if (fd < 0) {   /* /dev/uio* are root-only */
            fprintf(stderr, "hwhs: %s (0x%" PRIx64 "): %s\n", path, phys, strerror(errno));
            return NULL;
        }
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        return p == MAP_FAILED ? NULL : p;
    }

    /* no UIO device for this window (device tree not applied?): /dev/mem */
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "hwhs: no UIO device at 0x%" PRIx64 " and /dev/mem: %s\n", phys, strerror(errno));
        return NULL;
    }
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)phys);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "hwhs: mmap 0x%" PRIx64 ": %s\n", phys, strerror(errno));
        return NULL;
    }
    return p;
}

int hwhs_open(struct hwhs *h)
{
    memset(h, 0, sizeof(*h));
    h->regs     = hwhs_map(HWHS_PHYS_REGS, HWHS_SIZE_REGS);
    h->slot     = hwhs_map(HWHS_PHYS_SLOT, HWHS_SIZE_SLOT);
    h->reply    = hwhs_map(HWHS_PHYS_REPLY, HWHS_SIZE_REPLY);
    h->trace    = hwhs_map(HWHS_PHYS_TRACE, HWHS_SIZE_TRACE);
    h->demo     = hwhs_map(HWHS_PHYS_DEMO, HWHS_SIZE_DEMO);
    h->gpio_id  = hwhs_map(HWHS_PHYS_GPIO_ID, HWHS_SIZE_GPIO);
    h->gpio_ctl = hwhs_map(HWHS_PHYS_GPIO_CTL, HWHS_SIZE_GPIO);
    if (!h->regs || !h->slot || !h->reply || !h->trace || !h->demo || !h->gpio_id || !h->gpio_ctl) {
        hwhs_close(h);
        return -1;
    }

    /* The GPIO is in the always-on pl0 domain; hwhs needs the net clock. Check before
     * touching hwhs, so a dead clock reports instead of hanging the bus. */
    if (!(h->gpio_ctl[GPIO2_DATA / 4] & CTL_IN_CLK_LOCKED)) {
        fprintf(stderr, "hwhs: platform clock wizard not locked (axi_gpio_ctl ch2 = 0x%08x)\n",
                h->gpio_ctl[GPIO2_DATA / 4]);
        hwhs_close(h);
        return -1;
    }
    h->clk_hz = hwhs_rd(h, HWHS_CLK_HZ);
    if (h->clk_hz == 0 || h->clk_hz != h->gpio_id[GPIO2_DATA / 4]) {
        fprintf(stderr, "hwhs: CLK_HZ %u does not match the platform (%u): wrong bitstream?\n",
                h->clk_hz, h->gpio_id[GPIO2_DATA / 4]);
        hwhs_close(h);
        return -1;
    }
    return 0;
}

void hwhs_close(struct hwhs *h)
{
    if (h->regs)     munmap((void *)h->regs, HWHS_SIZE_REGS);
    if (h->slot)     munmap((void *)h->slot, HWHS_SIZE_SLOT);
    if (h->reply)    munmap((void *)h->reply, HWHS_SIZE_REPLY);
    if (h->trace)    munmap((void *)h->trace, HWHS_SIZE_TRACE);
    if (h->demo)     munmap((void *)h->demo, HWHS_SIZE_DEMO);
    if (h->gpio_id)  munmap((void *)h->gpio_id, HWHS_SIZE_GPIO);
    if (h->gpio_ctl) munmap((void *)h->gpio_ctl, HWHS_SIZE_GPIO);
    memset(h, 0, sizeof(*h));
}

/* ------------------------------------------------------------------------------------ */
/* BALBOA                                                                               */
/* ------------------------------------------------------------------------------------ */

int hwhs_stack_reset(struct hwhs *h, int timeout_ms)
{
    h->gpio_ctl[GPIO_DATA / 4] = CTL_OUT_STACK_RESET;
    usleep(1000);
    h->gpio_ctl[GPIO_DATA / 4] = 0;

    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (!(hwhs_rd(h, HWHS_STATUS) & HWHS_ST_STACK_UP)) {
        if (now_ns() > deadline) {
            fprintf(stderr, "hwhs: BALBOA did not come out of reset (ctl status 0x%08x)\n",
                    h->gpio_ctl[GPIO2_DATA / 4]);
            return -1;
        }
        usleep(100);
    }
    return 0;
}

/* NET_CTRL pushes are taken in a few cycles; this only guards against a stack held in reset */
static int net_push(struct hwhs *h, uint32_t bits, int timeout_ms)
{
    hwhs_wr(h, HWHS_NET_CTRL, bits);
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (hwhs_rd(h, HWHS_NET_PEND) & bits) {
        if (now_ns() > deadline) {
            fprintf(stderr, "hwhs: BALBOA did not take NET_CTRL 0x%x (NET_PEND 0x%x)\n", bits,
                    hwhs_rd(h, HWHS_NET_PEND));
            return -1;
        }
    }
    return 0;
}

int hwhs_set_addr(struct hwhs *h, uint32_t ip, const uint8_t mac[6])
{
    hwhs_wr(h, HWHS_LOCAL_IP, ip);
    hwhs_wr(h, HWHS_LOCAL_MAC_LO, ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                                  ((uint32_t)mac[4] << 8) | mac[5]);
    hwhs_wr(h, HWHS_LOCAL_MAC_HI, ((uint32_t)mac[0] << 8) | mac[1]);
    return net_push(h, HWHS_NET_SET_IP | HWHS_NET_SET_MAC, 100);
}

int hwhs_arp(struct hwhs *h, uint32_t ip, int timeout_ms)
{
    uint32_t rx0 = hwhs_rd(h, HWHS_NET_STATS + 4 * HWHS_STAT_ARP_RX);
    hwhs_wr(h, HWHS_ARP_IP, ip);
    if (net_push(h, HWHS_NET_ARP, 100)) return -1;

    /* BALBOA keeps the answer to itself; an ARP frame coming in is the sign it has it */
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (hwhs_rd(h, HWHS_NET_STATS + 4 * HWHS_STAT_ARP_RX) == rx0) {
        if (now_ns() > deadline) return -1;
        usleep(100);
    }
    usleep(1000);   /* the reply reaches the ARP table a few cycles after the counter */
    return 0;
}

/* `width` bits of v into the 184-bit context at bit `lsb` */
static void put_bits(uint32_t w[6], unsigned lsb, unsigned width, uint64_t v)
{
    for (unsigned i = 0; i < width; i++) {
        unsigned b = lsb + i;
        if ((v >> i) & 1) w[b / 32] |= 1u << (b % 32);
    }
}

static uint32_t ip_first_octet_low(uint32_t ip)
{
    return ((ip & 0xFFu) << 24) | ((ip & 0xFF00u) << 8) | ((ip >> 8) & 0xFF00u) | (ip >> 24);
}

void hwhs_qp_ctx_words(const struct hwhs_qp *qp, uint32_t w[6])
{
    memset(w, 0, 6 * sizeof(w[0]));
    put_bits(w, 0, 32, 0);                          /* new_state, as Coyote's driver writes it */
    put_bits(w, 32, 24, qp->local_qpn);             /* qp_num */
    put_bits(w, 56, 24, qp->psn);                   /* remote_psn: the PSN BALBOA sends with */
    put_bits(w, 80, 24, qp->psn);                   /* local_psn: the PSN BALBOA expects */
    put_bits(w, 104, 32, qp->rkey);                 /* r_key */
    put_bits(w, 136, 48, qp->ring_iova);            /* vaddr */
}

void hwhs_conn_ctx_words(const struct hwhs_qp *qp, uint32_t w[6])
{
    memset(w, 0, 6 * sizeof(w[0]));
    put_bits(w, 0, 16, qp->local_qpn);              /* local_qpn */
    put_bits(w, 16, 24, qp->remote_qpn);            /* remote_qpn */
    for (unsigned i = 0; i < 4; i++)                /* remote_ip_address */
        put_bits(w, 40 + 32 * i, 32, ip_first_octet_low(qp->remote_ip));
    put_bits(w, 168, 16, qp->udp_sport);            /* remote_udp_port */
}

int hwhs_qp_connect(struct hwhs *h, const struct hwhs_qp *qp, int timeout_ms)
{
    uint32_t q[6], c[6];
    hwhs_qp_ctx_words(qp, q);
    hwhs_conn_ctx_words(qp, c);
    for (unsigned i = 0; i < 6; i++) {
        hwhs_wr(h, HWHS_QP_CTX + 4 * i, q[i]);
        hwhs_wr(h, HWHS_CONN_CTX + 4 * i, c[i]);
    }
    return net_push(h, HWHS_NET_QP_PUSH | HWHS_NET_CONN_PUSH, timeout_ms);
}

int hwhs_engine_setup(struct hwhs *h, const struct hwhs_ring *r)
{
    unsigned log2 = 0;
    while ((1u << log2) < r->stride) log2++;
    if (r->stride < HWHS_SLOT_BYTES || (1u << log2) != r->stride || log2 > 15 ||
        r->slots == 0 || r->slots > HWHS_SLOTS || r->req_bytes == 0 || r->req_bytes > HWHS_SLOT_BYTES) {
        fprintf(stderr, "hwhs: bad ring (stride %u, slots %u, request %u B)\n", r->stride, r->slots,
                r->req_bytes);
        return -1;
    }
    hwhs_wr(h, HWHS_CTRL, HWHS_CTRL_SOFT_RESET);
    hwhs_wr(h, HWHS_CTRL, 0);
    hwhs_wr(h, HWHS_REPLY_TO, r->reply_to);
    hwhs_wr(h, HWHS_RING_LEN, r->slots);
    hwhs_wr(h, HWHS_FLAGS, 1);
    hwhs_wr(h, HWHS_LOCAL_QPN, r->qpn);
    hwhs_wr(h, HWHS_REQ_LEN, r->req_bytes);
    hwhs_wr(h, HWHS_RMT_BASE_LO, (uint32_t)r->rmt_base);
    hwhs_wr(h, HWHS_RMT_BASE_HI, (uint32_t)(r->rmt_base >> 32));
    hwhs_wr(h, HWHS_RMT_STRIDE, log2);
    return 0;
}

void hwhs_net_stats(const struct hwhs *h, uint32_t out[HWHS_NET_STATS_N])
{
    for (unsigned i = 0; i < HWHS_NET_STATS_N; i++) out[i] = hwhs_rd(h, HWHS_NET_STATS + 4 * i);
}

/* ------------------------------------------------------------------------------------ */
/* Rounds                                                                               */
/* ------------------------------------------------------------------------------------ */

static void copy_to_dev64(volatile uint8_t *dst, const void *src, size_t len)
{
    uint64_t buf[HWHS_SLOT_BYTES / 8] = {0};
    memcpy(buf, src, len > HWHS_SLOT_BYTES ? HWHS_SLOT_BYTES : len);
    volatile uint64_t *d = (volatile uint64_t *)dst;
    for (size_t i = 0; i < HWHS_SLOT_BYTES / 8; i++) d[i] = buf[i];
}

static void copy_from_dev64(void *dst, const volatile uint8_t *src)
{
    uint64_t buf[HWHS_SLOT_BYTES / 8];
    const volatile uint64_t *s = (const volatile uint64_t *)src;
    for (size_t i = 0; i < HWHS_SLOT_BYTES / 8; i++) buf[i] = s[i];
    memcpy(dst, buf, HWHS_SLOT_BYTES);
}

int hwhs_round(struct hwhs *h, const void *req, size_t len, void *reply, uint64_t *rtt_cycles)
{
    uint32_t idx = hwhs_rd(h, HWHS_RING_IDX);
    uint32_t timeouts = hwhs_rd(h, HWHS_TIMEOUT_CNT);
    uint32_t done = HWHS_ST_DONE_CNT(hwhs_rd(h, HWHS_STATUS));
    volatile uint8_t *slot = h->slot + (size_t)idx * HWHS_SLOT_BYTES;

    copy_to_dev64(slot, req, len);
    /* The slot RAM (LPD) and CTRL (FPD) are on different AXI paths: read the slot back so
     * the payload has landed before START can reach the engine. */
    io_barrier();
    (void)((volatile uint64_t *)slot)[HWHS_SLOT_BYTES / 8 - 1];
    hwhs_wr(h, HWHS_CTRL, HWHS_CTRL_START);

    uint32_t reply_to = hwhs_rd(h, HWHS_REPLY_TO);
    uint64_t limit_ns = reply_to ? (uint64_t)(2e9 * reply_to / h->clk_hz) + 10000000ull : 5000000000ull;
    uint64_t deadline = now_ns() + limit_ns;
    while (HWHS_ST_DONE_CNT(hwhs_rd(h, HWHS_STATUS)) == done) {
        if (now_ns() > deadline) {
            fprintf(stderr, "hwhs: round did not finish (STATUS 0x%08x)\n", hwhs_rd(h, HWHS_STATUS));
            return -1;
        }
    }
    if (hwhs_rd(h, HWHS_TIMEOUT_CNT) != timeouts) return 1;

    if (rtt_cycles) *rtt_cycles = hwhs_rd(h, HWHS_RTT_LO) | ((uint64_t)hwhs_rd(h, HWHS_RTT_HI) << 32);
    if (reply) copy_from_dev64(reply, h->reply + (size_t)idx * HWHS_SLOT_BYTES);
    return 0;
}

int hwhs_demo_run(struct hwhs *h, const void *syndromes, const void *expected, uint32_t table,
                  uint32_t n, uint64_t period, uint32_t span_mask, int timeout_ms,
                  struct hwhs_demo_result *r)
{
    if (table == 0 || 2ull * table * HWHS_SLOT_BYTES > HWHS_SIZE_DEMO) {
        fprintf(stderr, "hwhs: demo table of %u entries does not fit\n", table);
        return -1;
    }
    hwhs_wr(h, HWHS_DEMO_CTRL, HWHS_DEMO_RESET);
    for (uint32_t i = 0; i < table; i++) {
        copy_to_dev64(h->demo + (size_t)i * HWHS_SLOT_BYTES,
                      (const uint8_t *)syndromes + (size_t)i * HWHS_SLOT_BYTES, HWHS_SLOT_BYTES);
        copy_to_dev64(h->demo + (size_t)(table + i) * HWHS_SLOT_BYTES,
                      (const uint8_t *)expected + (size_t)i * HWHS_SLOT_BYTES, HWHS_SLOT_BYTES);
    }
    io_barrier();
    (void)((volatile uint64_t *)h->demo)[0];

    uint32_t rounds0 = hwhs_rd(h, HWHS_ROUND_CNT);
    uint32_t timeouts0 = hwhs_rd(h, HWHS_TIMEOUT_CNT);
    hwhs_wr(h, HWHS_SYN_DEPTH, table * HWHS_SLOT_BYTES);
    hwhs_wr(h, HWHS_CMD_CNT, n);
    hwhs_wr(h, HWHS_FREQ_LO, (uint32_t)period);
    hwhs_wr(h, HWHS_FREQ_HI, (uint32_t)(period >> 32));
    hwhs_wr(h, HWHS_FREQ_SPAN, span_mask);
    hwhs_wr(h, HWHS_TRACE_CTRL, 1);              /* clear, then record */
    hwhs_wr(h, HWHS_TRACE_CTRL, 0);
    hwhs_wr(h, HWHS_DEMO_CTRL, 0);               /* reset_demo off */
    hwhs_wr(h, HWHS_DEMO_CTRL, HWHS_DEMO_START);

    int rc = 0;
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (!(hwhs_rd(h, HWHS_DEMO_STS) & 1)) {
        if (now_ns() > deadline) {
            fprintf(stderr, "hwhs: demo did not finish: %u of %u rounds\n",
                    hwhs_rd(h, HWHS_ROUND_CNT) - rounds0, n);
            rc = -1;
            break;
        }
        usleep(100);
    }

    r->rounds    = hwhs_rd(h, HWHS_ROUND_CNT) - rounds0;
    r->timeouts  = hwhs_rd(h, HWHS_TIMEOUT_CNT) - timeouts0;
    r->errors    = hwhs_rd(h, HWHS_STS1);
    r->trace_cnt = hwhs_rd(h, HWHS_TRACE_CNT);
    hwhs_wr(h, HWHS_DEMO_CTRL, HWHS_DEMO_RESET);  /* stop the engine's demo mode */
    return rc;
}

void hwhs_trace_read(const struct hwhs *h, uint16_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 2) {
        uint32_t w = h->trace[i / 2];
        out[i] = (uint16_t)w;
        if (i + 1 < n) out[i + 1] = (uint16_t)(w >> 16);
    }
}
