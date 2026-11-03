/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * hwhs.c - userspace driver for hwhs-hsb and the HSB behind it (VPK120 A72).
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
    static const uint8_t ps_mac[6] = HWHS_PS_MAC;
    static const uint8_t hsb_mac1[6] = HWHS_HSB_MAC1;

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

    /* The GPIO is in the always-on pl0 domain; hwhs needs the hif clock. Check before
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

    memcpy(h->ps_mac, ps_mac, 6);
    memcpy(h->hsb_mac, hsb_mac1, 6);
    h->ps_ip = HWHS_PS_IP;
    h->ps_port = HWHS_PS_PORT;
    h->hsb_ip = HSB_DEFAULT_IP + 1;      /* host port 1 */
    h->ecb_seq = 1;
    h->ecb_timeout_ms = 100;
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
/* ECB over the mailbox (HSB host port 1)                                               */
/* ------------------------------------------------------------------------------------ */

static void put_be16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get_be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t ip_checksum(const uint8_t *hdr, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i += 2) s += get_be16(hdr + i);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/*
 * Ethernet / IPv4 / UDP / ECB request, padded to the 60-byte minimum (the MAC would add
 * the FCS; HSB host ports carry frames without it).
 */
static size_t ecb_frame(const struct hwhs *h, uint8_t *f, uint8_t cmd, uint16_t seq,
                        uint32_t addr, const uint32_t *val)
{
    size_t ecb = 10 + (val ? 4 : 0);       /* cmd, flags, seq, 2 reserved, addr [, value] */
    size_t udp = 8 + ecb;
    size_t ip = 20 + udp;
    size_t n = 14 + ip;

    memset(f, 0, HWHS_ECB_BUF_BYTES);
    memcpy(f + 0, h->hsb_mac, 6);
    memcpy(f + 6, h->ps_mac, 6);
    f[12] = 0x08; f[13] = 0x00;

    uint8_t *iph = f + 14;
    iph[0] = 0x45;
    put_be16(iph + 2, (uint32_t)ip);
    iph[6] = 0x40;                          /* DF */
    iph[8] = 64;                            /* TTL */
    iph[9] = 17;                            /* UDP */
    put_be32(iph + 12, h->ps_ip);
    put_be32(iph + 16, h->hsb_ip);
    put_be16(iph + 10, ip_checksum(iph, 20));

    uint8_t *udph = iph + 20;
    put_be16(udph + 0, h->ps_port);
    put_be16(udph + 2, HSB_ECB_UDP_PORT);
    put_be16(udph + 4, (uint32_t)udp);      /* checksum 0: not used over IPv4 */

    uint8_t *e = udph + 8;
    e[0] = cmd;
    e[1] = HSB_ECB_FLAG_ACK_REQUEST;
    put_be16(e + 2, seq);
    put_be32(e + 6, addr);
    if (val) put_be32(e + 10, *val);

    return n < 60 ? 60 : n;
}

static int ecb_xfer(struct hwhs *h, uint8_t cmd, uint32_t addr, const uint32_t *wval, uint32_t *rval)
{
    uint8_t f[HWHS_ECB_BUF_BYTES];
    uint16_t seq = h->ecb_seq++;
    size_t n = ecb_frame(h, f, cmd, seq, addr, wval);

    if (!(hwhs_rd(h, HWHS_STATUS) & HWHS_ST_HSB_INIT_DONE)) {
        fprintf(stderr, "hwhs: HSB is not initialized (in reset?)\n");
        return -1;
    }
    if (hwhs_rd(h, HWHS_ECB_STATUS) & HWHS_ECB_ST_RX_VALID)      /* stale reply */
        hwhs_wr(h, HWHS_ECB_CTRL, HWHS_ECB_RELEASE);

    for (size_t i = 0; i < (n + 3) / 4; i++) {
        uint32_t w = f[4 * i] | (f[4 * i + 1] << 8) | (f[4 * i + 2] << 16) | ((uint32_t)f[4 * i + 3] << 24);
        hwhs_wr(h, HWHS_ECB_TX_BUF + 4 * (uint32_t)i, w);
    }
    hwhs_wr(h, HWHS_ECB_TX_LEN, (uint32_t)n);
    io_barrier();
    hwhs_wr(h, HWHS_ECB_CTRL, HWHS_ECB_SEND);

    uint64_t deadline = now_ns() + (uint64_t)h->ecb_timeout_ms * 1000000ull;
    for (;;) {
        uint32_t st = hwhs_rd(h, HWHS_ECB_STATUS);
        if (st & HWHS_ECB_ST_RX_VALID) {
            uint8_t r[HWHS_ECB_BUF_BYTES];
            size_t len = HWHS_ECB_ST_RX_LEN(st);
            for (size_t i = 0; i < (len + 3) / 4; i++) {
                uint32_t w = hwhs_rd(h, HWHS_ECB_RX_BUF + 4 * (uint32_t)i);
                r[4 * i] = w; r[4 * i + 1] = w >> 8; r[4 * i + 2] = w >> 16; r[4 * i + 3] = w >> 24;
            }
            hwhs_wr(h, HWHS_ECB_CTRL, HWHS_ECB_RELEASE);

            /* reply: cmd', flags, seq, code, 0, addr, value, latched seq (after Eth/IPv4/UDP) */
            const uint8_t *e = r + 42;
            if (len < 42 + 6 || get_be16(e + 2) != seq) continue;    /* not ours: keep waiting */
            if (e[4] != HSB_ECB_RESP_SUCCESS) {
                fprintf(stderr, "hwhs: ECB %s 0x%08x: response code 0x%02x\n",
                        wval ? "write" : "read", addr, e[4]);
                return -1;
            }
            if (rval) {
                if (len < 42 + 14) {
                    fprintf(stderr, "hwhs: ECB read 0x%08x: short reply (%zu bytes)\n", addr, len);
                    return -1;
                }
                *rval = get_be32(e + 10);
            }
            return 0;
        }
        if (now_ns() > deadline) {
            fprintf(stderr, "hwhs: ECB %s 0x%08x: no reply in %d ms (ECB_STATUS 0x%08x, drop %u, ovf %u)\n",
                    wval ? "write" : "read", addr, h->ecb_timeout_ms, st,
                    hwhs_rd(h, HWHS_ECB_DROP), hwhs_rd(h, HWHS_ECB_OVF));
            return -1;
        }
    }
}

int hwhs_ecb_read(struct hwhs *h, uint32_t addr, uint32_t *val)
{
    return ecb_xfer(h, HSB_ECB_RD_DWORD, addr, NULL, val);
}

int hwhs_ecb_write(struct hwhs *h, uint32_t addr, uint32_t val)
{
    return ecb_xfer(h, HSB_ECB_WR_DWORD, addr, &val, NULL);
}

/* ------------------------------------------------------------------------------------ */
/* HSB                                                                                  */
/* ------------------------------------------------------------------------------------ */

int hwhs_hsb_reset(struct hwhs *h, int timeout_ms)
{
    h->gpio_ctl[GPIO_DATA / 4] = CTL_OUT_HSB_RESET;
    usleep(1000);
    h->gpio_ctl[GPIO_DATA / 4] = 0;

    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (!(h->gpio_ctl[GPIO2_DATA / 4] & CTL_IN_HSB_INIT_DONE)) {
        if (now_ns() > deadline) {
            fprintf(stderr, "hwhs: HSB init_done did not come back (ctl status 0x%08x)\n",
                    h->gpio_ctl[GPIO2_DATA / 4]);
            return -1;
        }
        usleep(100);
    }
    return 0;
}

#define ECB_WR(a, v) do { if (hwhs_ecb_write(h, (a), (v))) return -1; } while (0)

int hwhs_hsb_configure(struct hwhs *h, const struct hwhs_dp *dp)
{
    const uint32_t hif = HSB_HIF(0), vp = HSB_VP(0);
    uint32_t v;

    if (dp->ring_pitch == 0 || dp->ring_pitch % HSB_PAGE_SIZE || dp->ring_slots == 0 || dp->ring_slots > 4096) {
        fprintf(stderr, "hwhs: bad ring (pitch %u, slots %u)\n", dp->ring_pitch, dp->ring_slots);
        return -1;
    }
    if (hwhs_ecb_read(h, HSB_IP_VERSION, &v)) return -1;
    if ((v & 0xFFFF) < HSB_IP_VERSION_MIN) {
        fprintf(stderr, "hwhs: HSB IP version 0x%04x < 0x%04x\n", v & 0xFFFF, HSB_IP_VERSION_MIN);
        return -1;
    }

    /* the VP stays off while its registers change */
    if (hwhs_ecb_read(h, hif + HSB_DP_VP_MASK, &v)) return -1;
    ECB_WR(hif + HSB_DP_VP_MASK, v & ~1u);

    ECB_WR(hif + HSB_DP_PACKET_SIZE, 8);                  /* 8 x 128 B: one packet per message */
    ECB_WR(hif + HSB_DP_PACKET_UDP_PORT, HSB_DATA_SOURCE_UDP_PORT);
    ECB_WR(vp + HSB_DP_BUFFER_LENGTH, dp->frame_bytes);
    ECB_WR(vp + HSB_DP_HOST_MAC_LOW, ((uint32_t)dp->host_mac[2] << 24) | ((uint32_t)dp->host_mac[3] << 16) |
                                     ((uint32_t)dp->host_mac[4] << 8) | dp->host_mac[5]);
    ECB_WR(vp + HSB_DP_HOST_MAC_HIGH, ((uint32_t)dp->host_mac[0] << 8) | dp->host_mac[1]);
    ECB_WR(vp + HSB_DP_HOST_IP, dp->host_ip);
    ECB_WR(vp + HSB_DP_HOST_UDP_PORT, HSB_ROCE_UDP_PORT | HSB_MD_DISABLE);
    ECB_WR(vp + HSB_DP_VLAN, 0);
    ECB_WR(vp + HSB_DP_QP, dp->qpn & 0xFFFFFF);
    ECB_WR(vp + HSB_DP_RKEY, dp->rkey);
    ECB_WR(vp + HSB_DP_PAGE_LSB, (uint32_t)dp->ring_iova);
    ECB_WR(vp + HSB_DP_PAGE_MSB, (uint32_t)(dp->ring_iova >> 32));
    ECB_WR(vp + HSB_DP_PAGE_INC, dp->ring_pitch / HSB_PAGE_SIZE);
    ECB_WR(vp + HSB_DP_MAX_BUFF, dp->ring_slots - 1);

    /* Sensor-TX 0 holds its output until it is 3/4 full unless the thresholds are 0;
     * the 0 -> 1 edge of ctrl bit 2 latches them. */
    ECB_WR(HSB_STX0 + HSB_STX_AFULL_THRESH, 0);
    ECB_WR(HSB_STX0 + HSB_STX_AEMPTY_THRESH, 0);
    ECB_WR(HSB_STX0 + HSB_STX_CTRL, 0x1);
    ECB_WR(HSB_STX0 + HSB_STX_CTRL, 0x5);

    if (hwhs_ecb_read(h, hif + HSB_DP_VP_MASK, &v)) return -1;
    ECB_WR(hif + HSB_DP_VP_MASK, v | 1u);
    return 0;
}

int hwhs_hsb_stop(struct hwhs *h)
{
    uint32_t v;
    if (hwhs_ecb_read(h, HSB_HIF(0) + HSB_DP_VP_MASK, &v)) return -1;
    return hwhs_ecb_write(h, HSB_HIF(0) + HSB_DP_VP_MASK, v & ~1u);
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
