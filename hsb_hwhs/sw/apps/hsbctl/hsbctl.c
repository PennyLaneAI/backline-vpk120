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
 *              [-t timeout_us] [-F fpga_ip] [-o rtt.csv] [-f bl16|cuqr] [-b arg_len]
 *              [-i function] [-v]
 *                                      session with hsb-echo: handshake, program HSB, run
 *   hsbctl run -f cuqr -E '<endpoint>' -A <rth ip> -M <rth mac> [...]
 *                                      the same against a cudaq-realtime host (no handshake)
 *   hsbctl trace <file> [count]        dump the RTT trace (hif cycles) as text
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "cuqr.h"
#include "hsb_handshake.h"
#include "hwhs.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
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

/* What a round sends: a BL16 request (struct request) or a CUQR frame (cuqr.h). */
struct frame {
    int cuqr;
    uint32_t function_id; /* CUQR */
    uint32_t arg_len;     /* CUQR argument bytes, <= CUQR_MAX_ARG */
    int verbose;          /* print the first request and reply */
};

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

/* CUQR request for round `i` */
static void cuqr_request(const struct frame *fr, uint8_t *f, uint32_t i)
{
    // header and arguments are changed every round
    struct cuqr_header hd = {
        .magic = CUQR_MAGIC, .function_id = fr->function_id, .arg_len = fr->arg_len};
    memset(f, 0, FRAME_BYTES);
    memcpy(f, &hd, sizeof(hd));
    for (uint32_t k = 0; k < fr->arg_len; k++) {
        f[sizeof(hd) + k] = (uint8_t)(i + k);
    }
}

/* 0 if `r` answers `q` */
static int cuqr_check(const uint8_t *q, const uint8_t *r)
{
    // Check if the reply is a valid CUQS response
    struct cuqr_header qh;
    struct cuqr_response rh;
    memcpy(&qh, q, sizeof(qh));
    memcpy(&rh, r, sizeof(rh));
    if (rh.magic != CUQS_MAGIC || rh.status != 0 || rh.result_len != qh.arg_len) {
        return -1;
    }

    // Check if the reply is a valid RPC_INCREMENT response
    for (uint32_t k = 0; qh.function_id == CUQR_RPC_INCREMENT && k < qh.arg_len; k++) {
        if (r[sizeof(rh) + k] != (uint8_t)(q[sizeof(qh) + k] + 1)) {
            return -1;
        }
    }
    return 0;
}

static void print_bytes(const uint8_t *f, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        printf("%s%02x", k % 16 ? " " : "\n    ", f[k]);
    }
    printf("\n");
}

static void cuqr_print(const uint8_t *q, const uint8_t *r)
{
    struct cuqr_header qh;
    struct cuqr_response rh;
    memcpy(&qh, q, sizeof(qh));
    memcpy(&rh, r, sizeof(rh));
    printf("request: magic 0x%08x function_id 0x%08x arg_len %u request_id (stamped by hwhs) "
           "ptp_timestamp %" PRIu64,
           qh.magic, qh.function_id, qh.arg_len, qh.ptp_timestamp);
    print_bytes(q, (uint32_t)sizeof(qh) + qh.arg_len);
    uint32_t n = rh.result_len <= CUQR_MAX_ARG ? rh.result_len : 0;
    printf("reply:   magic 0x%08x status %d result_len %u request_id %u ptp_timestamp %" PRIu64,
           rh.magic, rh.status, rh.result_len, rh.request_id, rh.ptp_timestamp);
    print_bytes(r, (uint32_t)sizeof(rh) + n);
}

static int run_sw(struct hwhs *h, const struct frame *fr, uint32_t rounds, const char *csv)
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
        uint8_t q[FRAME_BYTES], r[HWHS_SLOT_BYTES];
        if (fr->cuqr) {
            cuqr_request(fr, q, i);
        }
        else {
            struct request rq = {
                .value = 0x1000000000ull + i, .decoder_id = 0, .round_id = i, .attempt = 0};
            memcpy(q, &rq, sizeof(rq));
        }
        uint64_t cycles = 0;
        uint64_t t0 = mono_ns();
        int rc = hwhs_round(h, q, sizeof(q), r, &cycles);
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
        if (fr->cuqr && fr->verbose && ok == 0) {
            cuqr_print(q, r);
        }
        if (fr->cuqr ? cuqr_check(q, r) != 0 : memcmp(r, q, 8) != 0) { /* BL16: the u64 value */
            bad++;
        }
        a72[ok] = t1 - t0;
        rtt[ok++] = cycles;
        if (out) {
            fprintf(out, "%u,%" PRIu64 ",%.1f,%" PRIu64 "\n", i, cycles, hwhs_ns(h, cycles),
                    t1 - t0);
        }
    }
    printf("sw-loop: %u replies, %u timeouts, %u %s, %u stale packets\n", ok, timeouts, bad,
           fr->cuqr ? "bad replies" : "value mismatches", hwhs_rd(h, HWHS_STALE_CNT));
    print_stats(h, rtt, ok);
    print_a72(a72, ok);
    free(rtt);
    free(a72);
    if (out) {
        fclose(out);
    }
    return timeouts || bad ? 1 : 0;
}

static int run_hw(struct hwhs *h, const struct frame *fr, uint32_t rounds, double period_us,
                  const char *csv)
{
    const uint32_t table = 256;
    uint8_t *syn = calloc(table, FRAME_BYTES);
    uint8_t *exp = calloc(table, FRAME_BYTES);
    struct hwhs_demo_result res;
    if (!syn || !exp) {
        return -1;
    }

    /* transport_demo compares the first 16 reply bytes with the seq dword masked */
    for (uint32_t i = 0; i < table; i++) {
        if (fr->cuqr) {
            /* CUQS, status 0, result_len: the results themselves are checked by sw-loop */
            struct cuqr_response e = {.magic = CUQS_MAGIC, .result_len = fr->arg_len};
            cuqr_request(fr, syn + (size_t)i * FRAME_BYTES, i);
            memcpy(exp + (size_t)i * FRAME_BYTES, &e, sizeof(e));
        }
        else {
            /* an echoing coprocessor returns the first 16 bytes */
            struct request q = {.value = 0x2000000000ull + i, .round_id = i};
            struct request e = {.value = q.value};
            memcpy(syn + (size_t)i * FRAME_BYTES, &q, sizeof(q));
            memcpy(exp + (size_t)i * FRAME_BYTES, &e, sizeof(e));
        }
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

/* cudaq-realtime host endpoint. */
static int parse_endpoint(const char *text, struct hwhs_dp *dp, char *peer_ip, size_t peer_len)
{
    char *copy = strdup(text), *save = NULL;
    unsigned seen = 0;
    for (char *t = strtok_r(copy, " \t\n", &save); t; t = strtok_r(NULL, " \t\n", &save)) {
        char *v = strchr(t, '=');
        if (!v) {
            continue;
        }
        *v++ = 0;
        if (!strcmp(t, "qp")) {
            dp->qpn = parse_u32(v);
            seen |= 1;
        }
        else if (!strcmp(t, "rkey")) {
            dp->rkey = parse_u32(v);
            seen |= 2;
        }
        else if (!strcmp(t, "buffer_addr")) {
            dp->ring_iova = strtoull(v, NULL, 0);
            seen |= 4;
        }
        else if (!strcmp(t, "slot_size")) {
            dp->ring_pitch = parse_u32(v);
            seen |= 8;
        }
        else if (!strcmp(t, "num_slots")) {
            dp->ring_slots = parse_u32(v);
            seen |= 16;
        }
        else if (!strcmp(t, "peer_ip")) {
            snprintf(peer_ip, peer_len, "%s", v);
        }
    }
    free(copy);
    if (seen != 31) {
        fprintf(stderr, "run: endpoint needs qp, rkey, buffer_addr, slot_size and num_slots\n");
        return -1;
    }
    return 0;
}

static int cmd_run(struct hwhs *h, int argc, char **argv)
{
    const char *host = NULL, *csv = NULL, *mode = "sw", *fpga_ip = "192.168.0.2";
    const char *endpoint = NULL, *rth_ip = NULL, *rth_mac = NULL;
    int port = HSB_HS_PORT, c;
    uint32_t rounds = 10000;
    double period_us = 10.0, timeout_us = 1000.0;
    static const uint8_t fpga_mac[6] = HWHS_HSB_MAC0;
    struct frame fr = {.function_id = CUQR_RPC_INCREMENT, .arg_len = 8};

    optind = 1;
    while ((c = getopt(argc, argv, "H:p:n:m:P:t:F:o:f:b:i:vE:A:M:")) != -1) {
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
        case 'f':
            if (strcmp(optarg, "bl16") && strcmp(optarg, "cuqr")) {
                fprintf(stderr, "run: -f bl16 or -f cuqr\n");
                return 2;
            }
            fr.cuqr = !strcmp(optarg, "cuqr");
            break;
        case 'b':
            fr.arg_len = parse_u32(optarg);
            break;
        case 'i': /* a number, or a handler name to hash */
            fr.function_id =
                isdigit((unsigned char)optarg[0]) ? parse_u32(optarg) : cuqr_fnv1a(optarg);
            break;
        case 'v':
            fr.verbose = 1;
            break;
        case 'E':
            endpoint = optarg;
            break;
        case 'A':
            rth_ip = optarg;
            break;
        case 'M':
            rth_mac = optarg;
            break;
        default:
            return 2;
        }
    }
    if (!host && !endpoint) {
        fprintf(stderr, "run: -H <coprocessor management ip> or -E <endpoint> is required\n");
        return 2;
    }
    if (endpoint && (!fr.cuqr || !rth_ip || !rth_mac)) {
        fprintf(stderr, "run: -E needs -f cuqr, -A <host RoCE ip> and -M <host RoCE mac>\n");
        return 2;
    }
    if (fr.cuqr && fr.arg_len > CUQR_MAX_ARG) {
        fprintf(stderr, "run: -b %u: a CUQR request carries at most %u bytes in one beat\n",
                fr.arg_len, CUQR_MAX_ARG);
        return 2;
    }
    if (fr.cuqr && hwhs_rd(h, HWHS_SEQ_OFF) != offsetof(struct cuqr_header, request_id)) {
        fprintf(stderr, "run: hwhs stamps its seq at byte %u, CUQR's request_id is at 12\n",
                hwhs_rd(h, HWHS_SEQ_OFF));
        return 1;
    }

    uint32_t reply_bytes =
        fr.cuqr ? (uint32_t)sizeof(struct cuqr_response) + fr.arg_len : REPLY_BYTES;
    struct in_addr fa;
    inet_pton(AF_INET, fpga_ip, &fa);
    struct hwhs_dp dp = {.frame_bytes = FRAME_BYTES};
    int fd = -1;
    if (endpoint) {
        /* cudaq-realtime host: the ring comes from its endpoint, its address from -A and -M */
        char peer_ip[64] = "";
        struct in_addr ra;
        unsigned m[6];
        if (parse_endpoint(endpoint, &dp, peer_ip, sizeof(peer_ip))) {
            return 2;
        }
        if (inet_pton(AF_INET, rth_ip, &ra) != 1 ||
            sscanf(rth_mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
            fprintf(stderr, "run: bad -A %s or -M %s\n", rth_ip, rth_mac);
            return 2;
        }
        if (peer_ip[0] && strcmp(peer_ip, fpga_ip)) {
            fprintf(stderr, "run: the host expects the board at %s, HSB is at %s (-F)\n", peer_ip,
                    fpga_ip);
            return 2;
        }
        dp.host_ip = ntohl(ra.s_addr);
        for (int k = 0; k < 6; k++) {
            dp.host_mac[k] = (uint8_t)m[k];
        }
        dp.imm = 1;
        printf("cudaq-realtime host: qpn 0x%06x rkey 0x%08x ring %u x %u B @ 0x%" PRIx64
               ", %s %s\n",
               dp.qpn, dp.rkey, dp.ring_slots, dp.ring_pitch, dp.ring_iova, rth_ip, rth_mac);
        if (dp.ring_slots && rounds % dp.ring_slots) {
            printf("note: %u rounds leave the host's ring at slot %u; it expects slot 0 next time, "
                   "so restart it before another run\n",
                   rounds, rounds % dp.ring_slots);
        }
    }
    else {
        /* 1-3: handshake */
        fd = tcp_connect(host, port);
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
        if (fr.cuqr && !hi.imm) {
            fprintf(stderr, "run: CUQR needs WRITE_WITH_IMM; start hsb-echo with -f cuqr\n");
            close(fd);
            return 1;
        }
        struct hsb_hs_fpga fo = {
            .magic = HSB_HS_MAGIC,
            .version = HSB_HS_VERSION,
            .ip = ntohl(fa.s_addr),
            .qpn = HSB_STX_QPN(0),
            .frame_bytes = FRAME_BYTES,
            .reply_bytes = reply_bytes,
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
        printf("coprocessor: qpn 0x%06x rkey 0x%08x ring %u x %u B @ 0x%" PRIx64
               ", %u.%u.%u.%u %s\n",
               hi.qpn, hi.rkey, hi.ring_slots, hi.ring_pitch, hi.ring_iova, hi.ip >> 24,
               (hi.ip >> 16) & 255, (hi.ip >> 8) & 255, hi.ip & 255,
               hi.imm ? "WRITE_WITH_IMM" : "WRITE");
        dp.qpn = hi.qpn;
        dp.rkey = hi.rkey;
        dp.ring_iova = hi.ring_iova;
        dp.ring_pitch = hi.ring_pitch;
        dp.ring_slots = hi.ring_slots;
        dp.host_ip = hi.ip;
        dp.imm = hi.imm;
        memcpy(dp.host_mac, hi.mac, 6);
    }
    if (fr.cuqr) {
        printf("CUQR: function_id 0x%08x, %u argument bytes, %u-byte replies\n", fr.function_id,
               fr.arg_len, reply_bytes);
    }

    /* 4: HSB from reset (slot pointer and PSN back to 0), then the data plane */
    if (hwhs_hsb_reset(h, 1000) || hwhs_hsb_configure(h, &dp)) {
        if (fd >= 0) {
            close(fd);
        }
        return 1;
    }

    /* engine: fresh counters, seq from 1, stamping on */
    hwhs_wr(h, HWHS_CTRL, HWHS_CTRL_SOFT_RESET);
    hwhs_wr(h, HWHS_CTRL, 0);
    hwhs_wr(h, HWHS_REPLY_TO, (uint32_t)(timeout_us * h->clk_hz / 1e6));
    hwhs_wr(h, HWHS_RING_LEN, 256);
    hwhs_wr(h, HWHS_FLAGS, 1);

    int rc = strcmp(mode, "hw") == 0 ? run_hw(h, &fr, rounds, period_us, csv)
                                     : run_sw(h, &fr, rounds, csv);

    hwhs_hsb_stop(h);
    if (fd >= 0) {
        close(fd);
    }
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
        "                  [-t timeout_us] [-F fpga_ip] [-o rtt.csv] [-f bl16|cuqr] [-b arg_len]\n"
        "                  [-i function] [-v]\n"
        "       hsbctl run -f cuqr -E '<endpoint>' -A <host ip> -M <host mac> [...]\n"
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
