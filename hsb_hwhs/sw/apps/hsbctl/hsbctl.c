/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * hsbctl - drive hwhs-hsb and HSB from the VPK120 A72.
 *
 *   hsbctl status                      build marker, clocks, HSB and engine state
 *   hsbctl reg <off> [<val>]           read / write an hwhs register
 *   hsbctl ecb-rd <addr>               read an HSB register over ECB
 *   hsbctl ecb-wr <addr> <val>         write an HSB register over ECB
 *   hsbctl hsb-reset                   pulse HSB's reset and wait for init_done
 *   hsbctl run -H <coprocessor ip> [-p port] [-n rounds] [-m sw|hw] [-P period_us]
 *              [-t timeout_us] [-F fpga_ip] [-o rtt.csv]
 *                                      session with hsb-echo: handshake, program HSB, run
 *   hsbctl trace <file> [count]        dump the RTT trace (hif cycles) as text
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "hsb_handshake.h"
#include "hwhs.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME_BYTES 64u /* one 512-bit beat */
#define REPLY_BYTES 16u /* {u64 value; u32 decoder_id; u32 seq} */

/* The request each round sends. */
struct request {
    uint64_t value;
    uint32_t decoder_id;
    uint32_t seq; /* stamped by hwhs (FLAGS.seq_stamp) */
    uint32_t round_id;
    uint32_t attempt;
    uint8_t pad[FRAME_BYTES - 24];
};
_Static_assert(sizeof(struct request) == FRAME_BYTES, "request layout");

static uint32_t parse_u32(const char *s) { return (uint32_t)strtoul(s, NULL, 0); }

static int cmd_status(struct hwhs *h)
{
    uint32_t ctl = h->gpio_ctl[GPIO2_DATA / 4];
    uint32_t st = hwhs_rd(h, HWHS_STATUS);
    uint32_t ver = 0;

    printf("BUILD_ID      0x%08x (gpio 0x%08x)\n", hwhs_rd(h, HWHS_BUILD_ID),
           h->gpio_id[GPIO_DATA / 4]);
    printf("hif clock     %u Hz\n", h->clk_hz);
    printf("platform      clk_locked=%u hsb_init_done=%u hsb_in_reset=%u gt_reset_done=0x%x\n",
           !!(ctl & CTL_IN_CLK_LOCKED), !!(ctl & CTL_IN_HSB_INIT_DONE),
           !!(ctl & CTL_IN_HSB_IN_RESET), CTL_IN_GT_RST_DONE(ctl));
    printf("engine        state=%u busy=%u done=%u reply_seen=%u timeout=%u demo_run=%u\n",
           HWHS_ST_STATE(st), !!(st & HWHS_ST_BUSY), !!(st & HWHS_ST_DONE),
           !!(st & HWHS_ST_REPLY_SEEN), !!(st & HWHS_ST_TIMEOUT), !!(st & HWHS_ST_DEMO_RUN));
    printf("ring          idx=%u len=%u next_seq=%u last_seq=%u seq_off=%u reply_to=%u cycles\n",
           hwhs_rd(h, HWHS_RING_IDX), hwhs_rd(h, HWHS_RING_LEN), hwhs_rd(h, HWHS_NEXT_SEQ),
           hwhs_rd(h, HWHS_LAST_SEQ), hwhs_rd(h, HWHS_SEQ_OFF), hwhs_rd(h, HWHS_REPLY_TO));
    printf("counters      rounds=%u done=%u timeouts=%u stale=%u tx=%u rx=%u last_rd=0x%08x\n",
           hwhs_rd(h, HWHS_ROUND_CNT), hwhs_rd(h, HWHS_DONE_CNT), hwhs_rd(h, HWHS_TIMEOUT_CNT),
           hwhs_rd(h, HWHS_STALE_CNT), hwhs_rd(h, HWHS_TX_CNT), hwhs_rd(h, HWHS_RX_CNT),
           hwhs_rd(h, HWHS_LAST_RD));
    printf("ecb mailbox   status=0x%08x kept=%u dropped=%u overflow=%u\n",
           hwhs_rd(h, HWHS_ECB_STATUS), hwhs_rd(h, HWHS_ECB_RX_CNT), hwhs_rd(h, HWHS_ECB_DROP),
           hwhs_rd(h, HWHS_ECB_OVF));
    if (st & HWHS_ST_HSB_INIT_DONE) {
        if (hwhs_ecb_read(h, HSB_IP_VERSION, &ver) == 0) {
            printf("HSB           IP version 0x%04x, build 0x%04x\n", ver & 0xFFFF, ver >> 16);
        }
    }
    else {
        printf("HSB           not initialized\n");
    }
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Session with the coprocessor                                                         */
/* ------------------------------------------------------------------------------------ */

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) {
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) {
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int tcp_connect(const char *ip, int port)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        fprintf(stderr, "bad address %s\n", ip);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        fprintf(stderr, "connect %s:%d: %s\n", ip, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void print_stats(const struct hwhs *h, uint64_t *rtt, uint32_t n)
{
    if (!n) {
        return;
    }
    qsort(rtt, n, sizeof(*rtt), cmp_u64);
    printf("RTT (ns) over %u rounds: min %.1f  p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n", n,
           hwhs_ns(h, rtt[0]), hwhs_ns(h, rtt[n / 2]), hwhs_ns(h, rtt[(uint64_t)n * 99 / 100]),
           hwhs_ns(h, rtt[(uint64_t)n * 999 / 1000]), hwhs_ns(h, rtt[n - 1]));
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A72 round time: the engine's RTT plus the CPU work around it (slot write, START, polling,
 * reply read). */
static void print_a72(uint64_t *ns, uint32_t n)
{
    if (!n) {
        return;
    }
    qsort(ns, n, sizeof(*ns), cmp_u64);
    printf("A72 round (ns) over %u rounds: min %" PRIu64 "  p50 %" PRIu64 "  p99 %" PRIu64
           "  p99.9 %" PRIu64 "  max %" PRIu64 "\n",
           n, ns[0], ns[n / 2], ns[(uint64_t)n * 99 / 100], ns[(uint64_t)n * 999 / 1000],
           ns[n - 1]);
}

static int run_sw(struct hwhs *h, uint32_t rounds, const char *csv)
{
    uint64_t *rtt = calloc(rounds, sizeof(*rtt));
    uint64_t *a72 = calloc(rounds, sizeof(*a72));
    uint32_t ok = 0, timeouts = 0, bad = 0;
    FILE *out = csv ? fopen(csv, "w") : NULL;
    if (!rtt || !a72 || (csv && !out)) {
        return -1;
    }
    if (out) {
        fprintf(out, "round,rtt_cycles,rtt_ns,a72_ns\n");
    }

    for (uint32_t i = 0; i < rounds; i++) {
        struct request q = {
            .value = 0x1000000000ull + i, .decoder_id = 0, .round_id = i, .attempt = 0};
        struct request r;
        uint64_t cycles = 0;
        uint64_t t0 = mono_ns();
        int rc = hwhs_round(h, &q, sizeof(q), &r, &cycles);
        uint64_t t1 = mono_ns();
        if (rc < 0) {
            free(rtt);
            free(a72);
            if (out) {
                fclose(out);
            }
            return -1;
        }
        if (rc == 1) {
            timeouts++;
            continue;
        }
        if (r.value != q.value) {
            bad++;
        }
        a72[ok] = t1 - t0;
        rtt[ok++] = cycles;
        if (out) {
            fprintf(out, "%u,%" PRIu64 ",%.1f,%" PRIu64 "\n", i, cycles, hwhs_ns(h, cycles),
                    t1 - t0);
        }
    }
    printf("sw-loop: %u replies, %u timeouts, %u value mismatches, %u stale packets\n", ok,
           timeouts, bad, hwhs_rd(h, HWHS_STALE_CNT));
    print_stats(h, rtt, ok);
    print_a72(a72, ok);
    free(rtt);
    free(a72);
    if (out) {
        fclose(out);
    }
    return timeouts || bad ? 1 : 0;
}

static int run_hw(struct hwhs *h, uint32_t rounds, double period_us, const char *csv)
{
    const uint32_t table = 256;
    struct request *syn = calloc(table, sizeof(*syn));
    struct request *exp = calloc(table, sizeof(*exp));
    struct hwhs_demo_result res;
    if (!syn || !exp) {
        return -1;
    }

    for (uint32_t i = 0; i < table; i++) {
        syn[i].value = 0x2000000000ull + i;
        syn[i].round_id = i;
        /* an echoing coprocessor returns the first 16 bytes; transport_demo masks the seq */
        exp[i].value = syn[i].value;
    }
    uint64_t period = (uint64_t)(period_us * h->clk_hz / 1e6);
    int timeout_ms = (int)(rounds * (period_us / 1000.0 + 1.0)) + 2000;
    int rc = hwhs_demo_run(h, syn, exp, table, rounds, period, 0, timeout_ms, &res);
    printf("hw-loop: %u replies, %u timeouts, %u compare errors, %u trace entries\n", res.rounds,
           res.timeouts, res.errors, res.trace_cnt);

    uint32_t n = res.trace_cnt < HWHS_TRACE_ENTRIES ? res.trace_cnt : HWHS_TRACE_ENTRIES;
    uint16_t *t = calloc(n ? n : 1, sizeof(*t));
    uint64_t *rtt = calloc(n ? n : 1, sizeof(*rtt));
    hwhs_trace_read(h, t, n);
    FILE *out = csv ? fopen(csv, "w") : NULL;
    if (out) {
        fprintf(out, "round,rtt_cycles,rtt_ns\n");
    }
    for (uint32_t i = 0; i < n; i++) {
        rtt[i] = t[i];
        if (out) {
            fprintf(out, "%u,%u,%.1f\n", i, t[i], hwhs_ns(h, t[i]));
        }
    }
    if (out) {
        fclose(out);
    }
    print_stats(h, rtt, n);
    free(syn);
    free(exp);
    free(t);
    free(rtt);
    return rc ? rc : (res.timeouts || res.errors ? 1 : 0);
}

static int cmd_run(struct hwhs *h, int argc, char **argv)
{
    const char *host = NULL, *csv = NULL, *mode = "sw", *fpga_ip = "192.168.0.2";
    int port = HSB_HS_PORT, c;
    uint32_t rounds = 10000;
    double period_us = 10.0, timeout_us = 1000.0;
    static const uint8_t fpga_mac[6] = HWHS_HSB_MAC0;

    optind = 1;
    while ((c = getopt(argc, argv, "H:p:n:m:P:t:F:o:")) != -1) {
        switch (c) {
        case 'H':
            host = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 'n':
            rounds = parse_u32(optarg);
            break;
        case 'm':
            mode = optarg;
            break;
        case 'P':
            period_us = atof(optarg);
            break;
        case 't':
            timeout_us = atof(optarg);
            break;
        case 'F':
            fpga_ip = optarg;
            break;
        case 'o':
            csv = optarg;
            break;
        default:
            return 2;
        }
    }
    if (!host) {
        fprintf(stderr, "run: -H <coprocessor management ip> is required\n");
        return 2;
    }

    /* 1-3: handshake */
    int fd = tcp_connect(host, port);
    if (fd < 0) {
        return 1;
    }
    struct hsb_hs_host hi;
    if (read_full(fd, &hi, sizeof(hi)) || hi.magic != HSB_HS_MAGIC ||
        hi.version != HSB_HS_VERSION) {
        fprintf(stderr, "run: bad handshake from %s\n", host);
        close(fd);
        return 1;
    }
    struct in_addr fa;
    inet_pton(AF_INET, fpga_ip, &fa);
    struct hsb_hs_fpga fo = {
        .magic = HSB_HS_MAGIC,
        .version = HSB_HS_VERSION,
        .ip = ntohl(fa.s_addr),
        .qpn = HSB_STX_QPN(0),
        .frame_bytes = FRAME_BYTES,
        .reply_bytes = REPLY_BYTES,
        .seq_off = hwhs_rd(h, HWHS_SEQ_OFF),
    };
    memcpy(fo.mac, fpga_mac, 6);
    struct hsb_hs_ready rd;
    if (write_full(fd, &fo, sizeof(fo)) || read_full(fd, &rd, sizeof(rd)) ||
        rd.magic != HSB_HS_MAGIC) {
        fprintf(stderr, "run: coprocessor did not get ready\n");
        close(fd);
        return 1;
    }
    printf("coprocessor: qpn 0x%06x rkey 0x%08x ring %u x %u B @ 0x%" PRIx64 ", %u.%u.%u.%u %s\n",
           hi.qpn, hi.rkey, hi.ring_slots, hi.ring_pitch, hi.ring_iova, hi.ip >> 24,
           (hi.ip >> 16) & 255, (hi.ip >> 8) & 255, hi.ip & 255,
           hi.imm ? "WRITE_WITH_IMM" : "WRITE");

    /* 4: HSB from reset (slot pointer and PSN back to 0), then the data plane */
    struct hwhs_dp dp = {
        .qpn = hi.qpn,
        .rkey = hi.rkey,
        .ring_iova = hi.ring_iova,
        .ring_pitch = hi.ring_pitch,
        .ring_slots = hi.ring_slots,
        .host_ip = hi.ip,
        .frame_bytes = FRAME_BYTES,
    };
    memcpy(dp.host_mac, hi.mac, 6);
    if (hwhs_hsb_reset(h, 1000) || hwhs_hsb_configure(h, &dp)) {
        close(fd);
        return 1;
    }

    /* engine: fresh counters, seq from 1, stamping on */
    hwhs_wr(h, HWHS_CTRL, HWHS_CTRL_SOFT_RESET);
    hwhs_wr(h, HWHS_CTRL, 0);
    hwhs_wr(h, HWHS_REPLY_TO, (uint32_t)(timeout_us * h->clk_hz / 1e6));
    hwhs_wr(h, HWHS_RING_LEN, 256);
    hwhs_wr(h, HWHS_FLAGS, 1);

    int rc = strcmp(mode, "hw") == 0 ? run_hw(h, rounds, period_us, csv) : run_sw(h, rounds, csv);

    hwhs_hsb_stop(h);
    close(fd);
    return rc < 0 ? 1 : rc;
}

static int cmd_trace(struct hwhs *h, const char *file, uint32_t n)
{
    uint32_t cnt = hwhs_rd(h, HWHS_TRACE_CNT);
    if (n == 0 || n > cnt) {
        n = cnt;
    }
    uint16_t *t = calloc(n ? n : 1, sizeof(*t));
    FILE *out = fopen(file, "w");
    if (!t || !out) {
        return 1;
    }
    hwhs_trace_read(h, t, n);
    fprintf(out, "round,rtt_cycles,rtt_ns\n");
    for (uint32_t i = 0; i < n; i++) {
        fprintf(out, "%u,%u,%.1f\n", i, t[i], hwhs_ns(h, t[i]));
    }
    fclose(out);
    free(t);
    printf("%u entries -> %s\n", n, file);
    return 0;
}

static void usage(void)
{
    fprintf(
        stderr,
        "usage: hsbctl status\n"
        "       hsbctl reg <off> [<val>]\n"
        "       hsbctl ecb-rd <addr>\n"
        "       hsbctl ecb-wr <addr> <val>\n"
        "       hsbctl hsb-reset\n"
        "       hsbctl run -H <coprocessor ip> [-p port] [-n rounds] [-m sw|hw] [-P period_us]\n"
        "                  [-t timeout_us] [-F fpga_ip] [-o rtt.csv]\n"
        "       hsbctl trace <file> [count]\n");
}

int main(int argc, char **argv)
{
    struct hwhs h;
    uint32_t v;
    int rc;

    if (argc < 2) {
        usage();
        return 2;
    }
    if (hwhs_open(&h)) {
        return 1;
    }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "status")) {
        rc = cmd_status(&h);
    }
    else if (!strcmp(cmd, "reg") && argc >= 3) {
        uint32_t off = parse_u32(argv[2]);
        if (argc >= 4) {
            hwhs_wr(&h, off, parse_u32(argv[3]));
        }
        printf("0x%03x = 0x%08x\n", off, hwhs_rd(&h, off));
        rc = 0;
    }
    else if (!strcmp(cmd, "ecb-rd") && argc >= 3) {
        rc = hwhs_ecb_read(&h, parse_u32(argv[2]), &v);
        if (!rc) {
            printf("0x%08x = 0x%08x\n", parse_u32(argv[2]), v);
        }
        rc = rc ? 1 : 0;
    }
    else if (!strcmp(cmd, "ecb-wr") && argc >= 4) {
        rc = hwhs_ecb_write(&h, parse_u32(argv[2]), parse_u32(argv[3])) ? 1 : 0;
    }
    else if (!strcmp(cmd, "hsb-reset")) {
        rc = hwhs_hsb_reset(&h, 1000) ? 1 : 0;
    }
    else if (!strcmp(cmd, "run")) {
        rc = cmd_run(&h, argc - 1, argv + 1);
    }
    else if (!strcmp(cmd, "trace") && argc >= 3) {
        rc = cmd_trace(&h, argv[2], argc >= 4 ? parse_u32(argv[3]) : 0);
    }
    else {
        usage();
        rc = 2;
    }
    hwhs_close(&h);
    return rc;
}
