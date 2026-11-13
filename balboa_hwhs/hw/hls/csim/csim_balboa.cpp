// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// csim_balboa.cpp - BALBOA's rocev2 HLS core, run as C++, against what hwhs and the
// coprocessor feed it on the board.
//
// The core is fpga-network-stack's own C++ (third_party/balboa, unmodified), called once per
// step the way its HLS dataflow runs. What goes in is what the board's RTL and software
// produce, packed the same way:
//   - the QP and connection contexts from the driver's own packing (hwhs_qp_ctx_words,
//     hwhs_conn_ctx_words in sw/lib/hwhs/hwhs.c), unpacked as roce_stack hands the 184-bit
//     words to the core;
//   - the work request as roce_stack builds it from balboa_net's dreq_t;
//   - the coprocessor's packets as an RC verbs peer sends them: its ACK of BALBOA's WRITE,
//     its reply WRITE, and one WRITE with the wrong PSN.
// It checks the WRITE BALBOA puts on the wire (addresses, ports, QP, PSN, RETH, payload),
// that the ACK reaches roce_stack's ACK channel, that the reply comes out as a memory write
// with its data and is acknowledged, that the PSNs advance, and that a wrong PSN is not
// delivered.
//
// Build and run with hw/hls/csim/run.sh. Exit status 0 = all checks passed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include "rocev2_config.hpp"
#include "rocev2.hpp"

extern "C" {
#include "hwhs.h"
}

// rocev2.cpp's top function, outside __VITIS_HLS__ (rocev2.hpp declares only the template)
void rocev2_top(hls::stream<net_axis<DATA_WIDTH> > &s_axis_rx_data,
                hls::stream<net_axis<DATA_WIDTH> > &m_axis_tx_data, hls::stream<txMeta> &s_axis_sq_meta,
                hls::stream<ackMeta> &m_axis_rx_ack_meta, hls::stream<memCmd> &m_axis_mem_write_cmd,
                hls::stream<memCmd> &m_axis_mem_read_cmd,
                hls::stream<net_axis<DATA_WIDTH> > &m_axis_mem_write_data,
                hls::stream<net_axis<DATA_WIDTH> > &s_axis_mem_read_data,
                hls::stream<qpContext> &s_axis_qp_interface, hls::stream<ifConnReq> &s_axis_qp_conn_interface,
                ap_uint<128> local_ip_address, hls::stream<psnPkg> &m_axis_dbg_0,
                hls::stream<psnPkg> &m_axis_dbg_1, hls::stream<psnPkg> &m_axis_dbg_2,
                ap_uint<32> &regCrcDropPkgCount, ap_uint<32> &regInvalidPsnDropCount,
                ap_uint<32> &regRetransCount, ap_uint<32> &regIbvCountRx, ap_uint<32> &regIbvCountTx);

static int errors = 0;
static void check(bool ok, const char *what)
{
    if (!ok) {
        errors++;
        std::fprintf(stderr, "  FAIL: %s\n", what);
    }
}

// ---- the session ----------------------------------------------------------------------------
static const uint32_t FPGA_IP   = HWHS_FPGA_IP;     // 192.168.0.2
static const uint32_t HOST_IP   = 0xC0A80001u;      // 192.168.0.1
static const uint32_t HOST_QPN  = 0x000ABCu;
static const uint32_t PSN       = 0x123456u;
static const uint32_t HOST_RKEY = 0x1234ABCDu;
static const uint64_t RING_IOVA = 0x7F0012340000ull;
static const uint16_t SRC_PORT  = 0xC000;
static const uint32_t REQ_BYTES = 64;

// ---- the core's ports -----------------------------------------------------------------------
typedef net_axis<DATA_WIDTH> word_t;
static hls::stream<word_t>    s_rx("s_rx"), m_tx("m_tx"), m_wr_data("m_wr_data"), s_rd_data("s_rd_data");
static hls::stream<txMeta>    s_sq("s_sq");
static hls::stream<ackMeta>   m_ack("m_ack");
static hls::stream<memCmd>    m_wr_cmd("m_wr_cmd"), m_rd_cmd("m_rd_cmd");
static hls::stream<qpContext> s_qp("s_qp");
static hls::stream<ifConnReq> s_conn("s_conn");
static hls::stream<psnPkg>    dbg0("dbg0"), dbg1("dbg1"), dbg2("dbg2");
static ap_uint<128>           local_ip;
static ap_uint<32>            reg_crc, reg_psn_drop, reg_retrans, reg_rx, reg_tx;

static void step(int n)
{
    for (int i = 0; i < n; i++) {
        rocev2_top(s_rx, m_tx, s_sq, m_ack, m_wr_cmd, m_rd_cmd, m_wr_data, s_rd_data, s_qp,
                   s_conn, local_ip, dbg0, dbg1, dbg2, reg_crc, reg_psn_drop, reg_retrans,
                   reg_rx, reg_tx);
        while (!dbg0.empty()) dbg0.read();
        while (!dbg1.empty()) dbg1.read();
        while (!dbg2.empty()) dbg2.read();
    }
}

// ---- packets as bytes -----------------------------------------------------------------------
static std::vector<uint8_t> read_packet(int max_steps)
{
    std::vector<uint8_t> b;
    for (int s = 0; s < max_steps; s++) {
        while (!m_tx.empty()) {
            word_t w = m_tx.read();
            for (int k = 0; k < DATA_WIDTH / 8; k++)
                if (w.keep[k]) b.push_back((uint8_t)w.data(k * 8 + 7, k * 8));
            if (w.last) return b;
        }
        step(1);
    }
    return b;
}

static void write_packet(const std::vector<uint8_t> &b)
{
    for (size_t i = 0; i < b.size(); i += DATA_WIDTH / 8) {
        word_t w;
        w.data = 0;
        w.keep = 0;
        for (size_t k = 0; k < DATA_WIDTH / 8 && i + k < b.size(); k++) {
            w.data(k * 8 + 7, k * 8) = b[i + k];
            w.keep[k] = 1;
        }
        w.last = (i + DATA_WIDTH / 8 >= b.size());
        s_rx.write(w);
    }
}

static uint32_t be(const std::vector<uint8_t> &b, size_t at, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | b[at + i];
    return v;
}

static void put_be(std::vector<uint8_t> &b, uint64_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i)));
}

// IPv4 + UDP + BTH (+ RETH or AETH) + payload + 4 ICRC bytes, from the coprocessor to BALBOA
static std::vector<uint8_t> host_packet(uint8_t opcode, uint32_t psn, bool reth, uint64_t vaddr,
                                        uint32_t rkey, const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> b;
    size_t ip_len = 20 + 8 + 12 + (reth ? 16 : 4) + payload.size() + 4;
    put_be(b, 0x4502, 2);
    put_be(b, ip_len, 2);
    put_be(b, 0x00004000, 4);                 // id, DF
    put_be(b, 0x4011, 2);                     // TTL 64, UDP
    put_be(b, 0, 2);                          // checksum, below
    put_be(b, HOST_IP, 4);
    put_be(b, FPGA_IP, 4);
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += be(b, i, 2);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    b[10] = (uint8_t)(~sum >> 8);
    b[11] = (uint8_t)~sum;
    put_be(b, 0xC350, 2);                     // source port
    put_be(b, 4791, 2);
    put_be(b, ip_len - 20, 2);
    put_be(b, 0, 2);
    put_be(b, opcode, 1);
    put_be(b, 0x40, 1);                       // migreq, pad 0, version 0
    put_be(b, 0xFFFF, 2);                     // P_Key
    put_be(b, HWHS_FPGA_QPN, 4);              // reserved, destination QP
    put_be(b, (reth ? 0x80000000u : 0) | psn, 4);   // AckReq, PSN
    if (reth) {
        put_be(b, vaddr, 8);
        put_be(b, rkey, 4);
        put_be(b, payload.size(), 4);
    } else {
        put_be(b, (0x1Fu << 24) | psn, 4);    // AETH: ACK, unlimited credit; MSN
    }
    b.insert(b.end(), payload.begin(), payload.end());
    put_be(b, 0, 4);                          // ICRC: BALBOA does not check it (crc.hpp)
    return b;
}

// a 184-bit context register file -> the core's struct, the way roce_stack passes the words
static ap_uint<184> ctx_bits(const uint32_t w[6])
{
    ap_uint<184> b = 0;
    for (int i = 0; i < 5; i++) b(32 * i + 31, 32 * i) = w[i];
    b(183, 160) = w[5] & 0xFFFFFF;
    return b;
}

int main()
{
    std::cout.rdbuf(nullptr);   // the core's own trace

    // the stack's IP, as network_stack.sv hands it to roce_stack: first octet low, four times
    uint32_t ipl = ((FPGA_IP & 0xFF) << 24) | ((FPGA_IP & 0xFF00) << 8) | ((FPGA_IP >> 8) & 0xFF00) | (FPGA_IP >> 24);
    for (int i = 0; i < 4; i++) local_ip(32 * i + 31, 32 * i) = ipl;

    std::fprintf(stderr, "-- contexts, packed by the driver\n");
    struct hwhs_qp qp = {HWHS_FPGA_QPN, HOST_QPN, PSN, HOST_RKEY, RING_IOVA, HOST_IP, SRC_PORT};
    uint32_t qw[6], cw[6];
    hwhs_qp_ctx_words(&qp, qw);
    hwhs_conn_ctx_words(&qp, cw);
    ap_uint<184> q = ctx_bits(qw), c = ctx_bits(cw);
    qpContext ctx;
    ctx.newState = (qpState)(uint32_t)q(31, 0);
    ctx.qp_num = q(55, 32);
    ctx.remote_psn = q(79, 56);
    ctx.local_psn = q(103, 80);
    ctx.r_key = q(135, 104);
    ctx.virtual_address = q(183, 136);
    ifConnReq conn;
    conn.qpn = c(15, 0);
    conn.remote_qpn = c(39, 16);
    conn.remote_ip_address = c(167, 40);
    conn.remote_udp_port = c(183, 168);
    s_qp.write(ctx);
    s_conn.write(conn);
    step(20);

    for (uint32_t round = 0; round < 2; round++) {
        uint32_t psn = (PSN + round) & 0xFFFFFF;
        uint64_t raddr = RING_IOVA + round * 64;
        char what[160];
        std::fprintf(stderr, "-- round %u: BALBOA's WRITE\n", round);

        // the work request, as roce_stack builds it from balboa_net's dreq_t
        txMeta wr(RC_RDMA_WRITE_ONLY, HWHS_FPGA_QPN, 0, 1, round, 0, raddr, REQ_BYTES, 0);
        s_sq.write(wr);
        int s = 0;
        while (m_rd_cmd.empty() && s++ < 200) step(1);
        check(!m_rd_cmd.empty(), "memory read command for the payload");
        if (m_rd_cmd.empty()) break;
        memCmd rc = m_rd_cmd.read();
        snprintf(what, sizeof(what), "read command: op 0x%x qpn %u addr 0x%llx len %u actv %u offs %u",
                 (unsigned)rc.op_code, (unsigned)rc.qpn, (unsigned long long)rc.addr, (unsigned)rc.len,
                 (unsigned)rc.actv, (unsigned)rc.offs);
        check(rc.op_code == RC_RDMA_WRITE_ONLY && rc.qpn == HWHS_FPGA_QPN && rc.addr == 0 &&
              rc.len == REQ_BYTES && rc.actv == 1 && rc.offs == round, what);

        // the payload: what hwhs posts
        std::vector<uint8_t> req(REQ_BYTES);
        for (uint32_t i = 0; i < REQ_BYTES; i++) req[i] = (uint8_t)(i * 7 + round);
        uint32_t seq = round + 1;
        std::memcpy(&req[12], &seq, 4);
        word_t pw;
        pw.data = 0;
        for (uint32_t i = 0; i < REQ_BYTES; i++) pw.data(i * 8 + 7, i * 8) = req[i];
        pw.keep = ~ap_uint<DATA_WIDTH / 8>(0);
        pw.last = 1;
        s_rd_data.write(pw);

        std::vector<uint8_t> p = read_packet(400);
        snprintf(what, sizeof(what), "a packet of %zu bytes came out", p.size());
        check(p.size() >= 20 + 8 + 12 + 16 + REQ_BYTES, what);
        if (p.size() < 20 + 8 + 12 + 16 + REQ_BYTES) break;
        snprintf(what, sizeof(what), "IPv4 0x%02x, UDP, %u.%u.%u.%u -> %u.%u.%u.%u", p[0], p[12], p[13], p[14],
                 p[15], p[16], p[17], p[18], p[19]);
        check(p[0] == 0x45 && p[9] == 17 && be(p, 12, 4) == FPGA_IP && be(p, 16, 4) == HOST_IP, what);
        snprintf(what, sizeof(what), "UDP %u -> %u", be(p, 20, 2), be(p, 22, 2));
        check(be(p, 22, 2) == 4791 && be(p, 20, 2) == SRC_PORT, what);
        snprintf(what, sizeof(what), "BTH opcode 0x%02x, P_Key 0x%04x, QP 0x%06x, PSN 0x%06x", p[28],
                 be(p, 30, 2), be(p, 33, 3), be(p, 37, 3));
        check(p[28] == 0x0A && be(p, 30, 2) == 0xFFFF && be(p, 33, 3) == HOST_QPN && be(p, 37, 3) == psn, what);
        uint64_t va = ((uint64_t)be(p, 40, 4) << 32) | be(p, 44, 4);
        snprintf(what, sizeof(what), "RETH 0x%llx rkey 0x%08x len %u", (unsigned long long)va, be(p, 48, 4),
                 be(p, 52, 4));
        check(va == raddr && be(p, 48, 4) == HOST_RKEY && be(p, 52, 4) == REQ_BYTES, what);
        check(std::memcmp(&p[56], req.data(), REQ_BYTES) == 0, "payload");
        snprintf(what, sizeof(what), "IP length %u for %zu bytes (+4 ICRC from icrc.sv)", be(p, 2, 2), p.size());
        check(be(p, 2, 2) == p.size() + 4, what);

        std::fprintf(stderr, "-- round %u: the coprocessor ACKs it\n", round);
        write_packet(host_packet(0x11, psn, false, 0, 0, {}));
        s = 0;
        while (m_ack.empty() && s++ < 400) step(1);
        check(!m_ack.empty(), "an ACK on m_axis_rx_ack_meta");
        if (!m_ack.empty()) {
            ackMeta a = m_ack.read();
            snprintf(what, sizeof(what), "ACK: qpn %u lst %u", (unsigned)a.qpn, (unsigned)a.lst);
            check(a.qpn == HWHS_FPGA_QPN && a.lst == 1, what);
        }

        std::fprintf(stderr, "-- round %u: the coprocessor's reply WRITE\n", round);
        std::vector<uint8_t> rep(req.begin(), req.begin() + 16);
        write_packet(host_packet(0x0A, psn, true, 0, 0, rep));
        s = 0;
        while ((m_wr_cmd.empty() || m_wr_data.empty()) && s++ < 400) step(1);
        check(!m_wr_cmd.empty() && !m_wr_data.empty(), "memory write command and data");
        if (!m_wr_cmd.empty() && !m_wr_data.empty()) {
            memCmd wc = m_wr_cmd.read();
            word_t wd = m_wr_data.read();
            snprintf(what, sizeof(what), "write command: qpn %u addr 0x%llx len %u", (unsigned)wc.qpn,
                     (unsigned long long)wc.addr, (unsigned)wc.len);
            check(wc.qpn == HWHS_FPGA_QPN && wc.addr == 0 && wc.len == 16, what);
            bool same = (wd.keep == 0xFFFF) && wd.last;
            for (int i = 0; i < 16; i++) same = same && (wd.data(i * 8 + 7, i * 8) == rep[i]);
            check(same, "reply data: 16 bytes, the seq at byte 12");
        }
        std::vector<uint8_t> ack = read_packet(400);
        snprintf(what, sizeof(what), "BALBOA ACKs the reply: %zu bytes, opcode 0x%02x, QP 0x%06x, PSN 0x%06x",
                 ack.size(), ack.size() > 40 ? ack[28] : 0, ack.size() > 40 ? be(ack, 33, 3) : 0,
                 ack.size() > 40 ? be(ack, 37, 3) : 0);
        check(ack.size() >= 44 && ack[28] == 0x11 && be(ack, 33, 3) == HOST_QPN && be(ack, 37, 3) == psn, what);
    }

    std::fprintf(stderr, "-- a WRITE with the wrong PSN is not delivered\n");
    write_packet(host_packet(0x0A, (PSN + 7) & 0xFFFFFF, true, 0, 0, std::vector<uint8_t>(16, 0xEE)));
    step(400);
    check(m_wr_cmd.empty() && m_wr_data.empty(), "no memory write");
    while (!m_tx.empty()) m_tx.read();

    if (errors == 0) {
        std::fprintf(stderr, "PASS\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d check(s)\n", errors);
    return 1;
}
