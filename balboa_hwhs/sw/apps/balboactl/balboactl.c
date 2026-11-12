/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * balboactl - drive hwhs-balboa and the BALBOA RoCE v2 stack from the VPK120 A72.
 *
 *   balboactl status                      build marker, clocks, BALBOA and engine state
 *   balboactl stats                       BALBOA's packet counters
 *   balboactl reg <off> [<val>]           read / write an hwhs register
 *   balboactl stack-reset                 hold BALBOA in reset for a moment (QPs, PSNs, ARP table)
 *   balboactl arp <ip>                    resolve an address with BALBOA's ARP server
 *   balboactl run -H <coprocessor ip> [-p port] [-n rounds] [-m sw|hw] [-P period_us]
 *                 [-t timeout_us] [-F fpga_ip] [-o rtt.csv]
 *                                         session with balboa-echo / balboa-gpu-echo:
 *                                         handshake, connect BALBOA, run
 *   balboactl trace <file> [count]        dump the RTT trace (net clock cycles) as text
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "balboa_handshake.h"
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

#define FRAME_BYTES 64u     /* one 512-bit beat */
#define REPLY_BYTES 16u     /* {u64 value; u32 decoder_id; u32 seq} */
#define UDP_SPORT   49152u  /* source port of BALBOA's packets */

/* The request each round sends: catalyst's PayloadSlot, then round id and attempt. */
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

static int parse_ip(const char *s, uint32_t *ip)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) {
        fprintf(stderr, "bad address %s\n", s);
        return -1;
    }
    *ip = ntohl(a.s_addr);
    return 0;
}

static const char *ip_str(uint32_t ip)
{
    static char buf[4][16];
    static int n;
    char *b = buf[n++ & 3];
    snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

static void print_counters(const struct hwhs *h)
{
    uint32_t s[HWHS_NET_STATS_N];
    hwhs_net_stats(h, s);
    printf("BALBOA        frames rx=%u tx=%u  ARP rx=%u tx=%u  ICMP rx=%u tx=%u%s\n", s[HWHS_STAT_RX_FRAMES],
           s[HWHS_STAT_TX_FRAMES], s[HWHS_STAT_ARP_RX], s[HWHS_STAT_ARP_TX], s[HWHS_STAT_ICMP_RX],
           s[HWHS_STAT_ICMP_TX], s[HWHS_STAT_STREAM_DOWN] ? "  RX STREAM DOWN" : "");
    printf("              RoCE rx=%u tx=%u  IB rx=%u tx=%u  PSN drops=%u retransmissions=%u\n",
           s[HWHS_STAT_ROCE_RX], s[HWHS_STAT_ROCE_TX], s[HWHS_STAT_IB_RX], s[HWHS_STAT_IB_TX],
           s[HWHS_STAT_PSN_DROP], s[HWHS_STAT_RETRANS]);
    printf("              ACKs=%u WRITEs in=%u READs in=%u copies kept=%u read back=%u\n",
           hwhs_rd(h, HWHS_ACK_CNT), hwhs_rd(h, HWHS_WR_CNT), hwhs_rd(h, HWHS_RD_CNT),
           hwhs_rd(h, HWHS_RETX_WR), hwhs_rd(h, HWHS_RETX_RD));
}

static int cmd_status(struct hwhs *h)
{
    uint32_t ctl = h->gpio_ctl[GPIO2_DATA / 4];
    uint32_t st = hwhs_rd(h, HWHS_STATUS);
    uint32_t mac_hi = hwhs_rd(h, HWHS_LOCAL_MAC_HI), mac_lo = hwhs_rd(h, HWHS_LOCAL_MAC_LO);

    printf("BUILD_ID      0x%08x (gpio 0x%08x)\n", hwhs_rd(h, HWHS_BUILD_ID), h->gpio_id[GPIO_DATA / 4]);
    printf("net clock     %u Hz\n", h->clk_hz);
    printf("platform      clk_locked=%u stack_up=%u stack_in_reset=%u gt_reset_done=0x%x\n",
           !!(ctl & CTL_IN_CLK_LOCKED), !!(ctl & CTL_IN_STACK_UP), !!(ctl & CTL_IN_STACK_RESET),
           CTL_IN_GT_RST_DONE(ctl));
    printf("address       %s %02x:%02x:%02x:%02x:%02x:%02x (registers; set_ip/set_mac apply them)\n",
           ip_str(hwhs_rd(h, HWHS_LOCAL_IP)), (mac_hi >> 8) & 255, mac_hi & 255, mac_lo >> 24,
           (mac_lo >> 16) & 255, (mac_lo >> 8) & 255, mac_lo & 255);
    printf("engine        state=%u busy=%u done=%u reply_seen=%u timeout=%u post_busy=%u demo_run=%u\n",
           HWHS_ST_STATE(st), !!(st & HWHS_ST_BUSY), !!(st & HWHS_ST_DONE), !!(st & HWHS_ST_REPLY_SEEN),
           !!(st & HWHS_ST_TIMEOUT), !!(st & HWHS_ST_POST_BUSY), !!(st & HWHS_ST_DEMO_RUN));
    printf("ring          idx=%u len=%u next_seq=%u last_seq=%u seq_off=%u reply_to=%u cycles\n",
           hwhs_rd(h, HWHS_RING_IDX), hwhs_rd(h, HWHS_RING_LEN), hwhs_rd(h, HWHS_NEXT_SEQ),
           hwhs_rd(h, HWHS_LAST_SEQ), hwhs_rd(h, HWHS_SEQ_OFF), hwhs_rd(h, HWHS_REPLY_TO));
    printf("remote        qpn=%u base=0x%08x%08x stride=%u B request=%u B\n", hwhs_rd(h, HWHS_LOCAL_QPN),
           hwhs_rd(h, HWHS_RMT_BASE_HI), hwhs_rd(h, HWHS_RMT_BASE_LO), 1u << hwhs_rd(h, HWHS_RMT_STRIDE),
           hwhs_rd(h, HWHS_REQ_LEN));
    printf("counters      rounds=%u done=%u timeouts=%u stale=%u tx=%u rx=%u last_rd=0x%08x\n",
           hwhs_rd(h, HWHS_ROUND_CNT), hwhs_rd(h, HWHS_DONE_CNT), hwhs_rd(h, HWHS_TIMEOUT_CNT),
           hwhs_rd(h, HWHS_STALE_CNT), hwhs_rd(h, HWHS_TX_CNT), hwhs_rd(h, HWHS_RX_CNT),
           hwhs_rd(h, HWHS_LAST_RD));
    print_counters(h);
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

/* The engine's RTT starts at START and ends at the reply beat; the A72 time adds what the
 * CPU spends around it (slot write and read-back, START, polling for done, reply read). */
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
    printf("sw-loop: %u replies, %u timeouts, %u value mismatches, %u stale WRITEs\n", ok,
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
    const char *host = NULL, *csv = NULL, *mode = "sw", *fpga_ip_s = NULL;
    int port = BALBOA_HS_PORT, c;
    uint32_t rounds = 10000, fpga_ip = HWHS_FPGA_IP;
    double period_us = 10.0, timeout_us = 1000.0;
    static const uint8_t fpga_mac[6] = HWHS_FPGA_MAC;

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
            fpga_ip_s = optarg;
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
    if (fpga_ip_s && parse_ip(fpga_ip_s, &fpga_ip)) {
        return 2;
    }

    /* 1: the coprocessor's QP and ring */
    int fd = tcp_connect(host, port);
    if (fd < 0) {
        return 1;
    }
    struct balboa_hs_host hi;
    if (read_full(fd, &hi, sizeof(hi)) || hi.magic != BALBOA_HS_MAGIC ||
        hi.version != BALBOA_HS_VERSION) {
        fprintf(stderr, "run: bad handshake from %s\n", host);
        close(fd);
        return 1;
    }
    printf("coprocessor: qpn 0x%06x psn 0x%06x rkey 0x%08x ring %u x %u B @ 0x%" PRIx64 ", %s\n",
           hi.qpn, hi.psn, hi.rkey, hi.ring_slots, hi.ring_stride, hi.ring_iova, ip_str(hi.ip));

    /* 2: BALBOA from reset (QP state, PSNs, ARP table), its address, the coprocessor's MAC,
     * the QP; then the engine */
    if (hwhs_stack_reset(h, 1000) || hwhs_set_addr(h, fpga_ip, fpga_mac)) {
        close(fd);
        return 1;
    }
    if (hwhs_arp(h, hi.ip, 1000)) {
        fprintf(stderr, "run: no ARP reply from %s; is its RoCE netdev up, in %s's subnet?\n",
                ip_str(hi.ip), ip_str(fpga_ip));
    }
    struct hwhs_qp qp = {
        .local_qpn = HWHS_FPGA_QPN,
        .remote_qpn = hi.qpn,
        .psn = hi.psn & 0xFFFFFF,
        .rkey = hi.rkey,
        .ring_iova = hi.ring_iova,
        .remote_ip = hi.ip,
        .udp_sport = UDP_SPORT,
    };
    struct hwhs_ring ring = {
        .rmt_base = hi.ring_iova,
        .stride = hi.ring_stride,
        .slots = hi.ring_slots,
        .req_bytes = FRAME_BYTES,
        .qpn = HWHS_FPGA_QPN,
        .reply_to = (uint32_t)(timeout_us * h->clk_hz / 1e6),
    };
    if (hwhs_qp_connect(h, &qp, 100) || hwhs_engine_setup(h, &ring)) {
        close(fd);
        return 1;
    }

    /* 3: BALBOA's side to the coprocessor; it moves its QP to RTS */
    struct balboa_hs_fpga fo = {
        .magic = BALBOA_HS_MAGIC,
        .version = BALBOA_HS_VERSION,
        .qpn = HWHS_FPGA_QPN,
        .psn = qp.psn,
        .ip = fpga_ip,
        .reply_rkey = 0,
        .reply_addr = 0,
        .req_bytes = FRAME_BYTES,
        .reply_bytes = REPLY_BYTES,
        .seq_off = hwhs_rd(h, HWHS_SEQ_OFF),
    };
    memcpy(fo.mac, fpga_mac, 6);
    struct balboa_hs_ready rd;
    if (write_full(fd, &fo, sizeof(fo)) || read_full(fd, &rd, sizeof(rd)) ||
        rd.magic != BALBOA_HS_MAGIC) {
        fprintf(stderr, "run: coprocessor did not get ready\n");
        close(fd);
        return 1;
    }

    /* 4: rounds */
    int rc = strcmp(mode, "hw") == 0 ? run_hw(h, rounds, period_us, csv) : run_sw(h, rounds, csv);
    print_counters(h);
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
    fprintf(stderr,
            "usage: balboactl status\n"
            "       balboactl stats\n"
            "       balboactl reg <off> [<val>]\n"
            "       balboactl stack-reset\n"
            "       balboactl arp <ip>\n"
            "       balboactl run -H <coprocessor ip> [-p port] [-n rounds] [-m sw|hw] [-P period_us]\n"
            "                     [-t timeout_us] [-F fpga_ip] [-o rtt.csv]\n"
            "       balboactl trace <file> [count]\n");
}

int main(int argc, char **argv)
{
    struct hwhs h;
    uint32_t ip;
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
    else if (!strcmp(cmd, "stats")) {
        print_counters(&h);
        rc = 0;
    }
    else if (!strcmp(cmd, "reg") && argc >= 3) {
        uint32_t off = parse_u32(argv[2]);
        if (argc >= 4) {
            hwhs_wr(&h, off, parse_u32(argv[3]));
        }
        printf("0x%03x = 0x%08x\n", off, hwhs_rd(&h, off));
        rc = 0;
    }
    else if (!strcmp(cmd, "stack-reset")) {
        rc = hwhs_stack_reset(&h, 1000) ? 1 : 0;
    }
    else if (!strcmp(cmd, "arp") && argc >= 3) {
        rc = parse_ip(argv[2], &ip) ? 2 : hwhs_arp(&h, ip, 1000) ? 1 : 0;
        printf("%s: %s\n", argv[2], rc == 0 ? "ARP reply received" : "no ARP reply");
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
