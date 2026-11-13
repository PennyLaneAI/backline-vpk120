// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// hwhs_regs.v
// MMIO control/status window of hwhs-balboa (PS 0xA400_0000, 64 KB, 32-bit accesses).
//
//===----------------------------------------------------------------------===//
///
/// Register map (byte offsets):
///------------------------------------------------------------------------------
///   0x00 CTRL        W1P [0]=start (one sw-loop round)
///                    RW  [1]=abort       (level: engine back to IDLE and held there)
///                    RW  [2]=soft_reset  (level: engine state and counters held at reset)
///   0x04 STATUS      RO  [3:0]=state [4]=busy [5]=done [6]=reply_seen [7]=timeout
///                        [8]=post_busy (a work request or payload still owed to BALBOA)
///                        [16]=stack_up [17]=demo_run [31:24]=DONE_CNT[7:0]
///   0x08 RING_IDX    RW  slot used by the next round (advances after every round)
///   0x0C NEXT_SEQ    RW  seq the engine stamps into the next request
///   0x10 RING_LEN    RW  slots in the ring, 1..2^SLOT_AW (default 256), on both sides
///   0x14 FLAGS       RW  [0]=seq_stamp (1: engine writes NEXT_SEQ at SEQ_OFF and expects it back;
///                                       0: the seq already in the payload is expected back)
///   0x18 LAST_SEQ    RO  seq of the last request sent
///   0x1C DONE_CNT    RO  rounds finished (reply or timeout); STATUS[31:24] = its low 8 bits,
///                        so software sees completion with one STATUS read
///------------------------------------------------------------------------------
///   0x30 REPLY_TO    RW  reply timeout in net clock cycles (0: wait forever; ABORT recovers)
///   0x34 ROUND_CNT   RO  rounds that completed with a matching reply
///   0x38 RTT_LO      RO  last round's RTT in net clock cycles [31:0]
///   0x3C RTT_HI      RO                                       [63:32]
///   0x40 CLK_HZ      RO  net clock frequency
///   0x44 STALE_CNT   RO  incoming WRITEs dropped (seq mismatch, or no round waiting)
///   0x48 TIMEOUT_CNT RO  rounds that timed out
///   0x4C TX_CNT      RO  requests posted (work request and payload taken by BALBOA)
///   0x50 RX_CNT      RO  incoming WRITEs
///   0x64 NET_STATUS  RO  platform status (see backline_roce_top: [0]=stack_up ...)
///   0x74 SEQ_OFF     RW  byte offset of the seq dword in request and reply (default 12)
///   0x78 LAST_RD     RO  seq dword of the last incoming WRITE
///   0x7C BUILD_ID    RO  build marker (yymmddHH of the Vivado run)
///------------------------------------------------------------------------------
///   0x80 NET_CTRL    W1P [0]=set_ip [1]=set_mac [2]=arp_lookup [3]=qp_push [4]=conn_push
///   0x84 NET_PEND    RO  the same bits: pushed, not yet taken by BALBOA
///   0x88 LOCAL_IP    RW  the stack's IPv4 address, 0xC0A80002 = 192.168.0.2 (set_ip applies it)
///   0x8C LOCAL_MAC_LO RW the stack's MAC [31:0]  (0x0293C6484D8E = 02:93:c6:48:4d:8e)
///   0x90 LOCAL_MAC_HI RW                [15:0]  (set_mac applies both)
///   0x94 ARP_IP      RW  IPv4 address arp_lookup resolves
///   0x98 LOCAL_QPN   RW  [15:0] the QP the requests go out on, {vfid[3:0], pid[5:0]} (default 2)
///   0x9C REQ_LEN     RW  bytes of the slot each request carries, 1..64 (default 64)
///   0xA0 RMT_BASE_LO RW  coprocessor ring, address of slot 0 [31:0]
///   0xA4 RMT_BASE_HI RW                                     [63:32]
///------------------------------------------------------------------------------
///   0xA8 DEMO_CTRL   W1P [0]=start_demo (sets demo_run)  RW [1]=reset_demo (level)
///   0xAC FREQ_LO     RW  pacer interval in net clock cycles [31:0]
///   0xB0 FREQ_HI     RW                                     [63:32]
///   0xB4 DEMO_STS    RO  [0]=demo_done
///   0xB8 CMD_CNT     RW  syndromes per demo run (syn_sum)
///   0xBC STS1        RO  demo_err: replies that did not match the expected table
///   0xC0 SYN_DEPTH   RW  bytes of syndrome table (expected replies follow it)
///   0xC4 FREQ_SPAN   RW  pacer jitter mask (0 = fixed interval)
///   0xC8 LFSR_SEED   RW  pacer jitter seed (0 = default)
///   0xCC TRACE_CTRL  RW  [0]=trace_clear (default 1: hold the trace empty)
///   0xD0 TRACE_CNT   RO  RTT trace entries recorded
///   0xD4 TRACE_STS   RO  [0]=full [1]=saturated (some round > 0xFFFF cycles)
///------------------------------------------------------------------------------
///   0xD8 RMT_STRIDE  RW  [3:0] log2 of the coprocessor ring's slot stride (default 6: 64 B)
///   0xDC ACK_CNT     RO  requests the coprocessor's NIC acknowledged
///   0xE0 WR_CNT      RO  incoming WRITE commands
///   0xE4 RD_CNT      RO  incoming READs, answered with zeros
///   0xE8 RETX_WR     RO  payload copies kept for retransmission
///   0xEC RETX_RD     RO  payload copies read back: retransmissions
///   0xF0-0x120 NET_STATS RO BALBOA's counters (network_stack m_net_stats), in this order:
///                    rx frames, tx frames, ARP rx, ARP tx, ICMP rx, ICMP tx, RoCE rx, RoCE tx,
///                    IB rx, IB tx, PSN drops, retransmissions, [0] stream down
///   0x140-0x154 QP_CTX   RW BALBOA rdma_qp_ctx_t, 184 bits, word i = bits [32i+31:32i]
///   0x160-0x174 CONN_CTX RW BALBOA rdma_qp_conn_t, the same way
///                    (qp_push / conn_push send a copy, so these can be rewritten at once)
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none

module hwhs_regs #(
    parameter integer CLK_HZ   = 250000000,
    parameter [31:0]  BUILD_ID = 32'h0,
    parameter integer SLOT_AW  = 10,
    parameter integer N_STATS  = 13
) (
    input  wire        clk,
    input  wire        rstn,

    input  wire [15:0] s_axi_awaddr,
    input  wire        s_axi_awvalid,
    output reg         s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [ 3:0] s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output reg         s_axi_wready,
    output reg  [ 1:0] s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [15:0] s_axi_araddr,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [ 1:0] s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,

    // ENGINE CONTROL
    output reg                start,         // 1-cycle pulse
    output reg                abort,
    output reg                soft_reset,
    output reg                ring_wr,       // 1-cycle pulse with ring_wdata
    output reg [SLOT_AW-1:0]  ring_wdata,
    output reg                seq_wr,        // 1-cycle pulse with seq_wdata
    output reg [31:0]         seq_wdata,
    output reg [SLOT_AW:0]    ring_len,
    output reg                seq_stamp,
    output reg [31:0]         reply_to,
    output reg [31:0]         seq_off,
    output reg [15:0]         local_qpn,
    output reg [ 6:0]         req_len,       // 1..64
    output reg [63:0]         rmt_base,
    output reg [ 3:0]         rmt_stride_log2,

    // ENGINE STATUS
    input wire [ 3:0]         sts_state,
    input wire                sts_busy,
    input wire                sts_done,
    input wire                sts_reply_seen,
    input wire                sts_timeout,
    input wire                sts_post_busy,
    input wire [SLOT_AW-1:0]  sts_ring_idx,
    input wire [31:0]         sts_next_seq,
    input wire [31:0]         sts_last_seq,
    input wire [31:0]         sts_round_cnt,
    input wire [31:0]         sts_done_cnt,
    input wire [63:0]         sts_rtt,
    input wire [31:0]         sts_stale_cnt,
    input wire [31:0]         sts_timeout_cnt,
    input wire [31:0]         sts_tx_cnt,
    input wire [31:0]         sts_rx_cnt,
    input wire [31:0]         sts_last_rd,
    input wire [31:0]         net_status,    // [0] = stack up

    // BALBOA CONFIGURATION (each valid held until BALBOA takes it)
    output reg                set_ip_valid,
    output reg  [31:0]        local_ip,
    output reg                set_mac_valid,
    output wire [47:0]        local_mac,
    output reg                arp_valid,
    input  wire               arp_ready,
    output reg  [31:0]        arp_ip,
    output reg                qp_valid,
    input  wire               qp_ready,
    output reg  [183:0]       qp_ctx,
    output reg                conn_valid,
    input  wire               conn_ready,
    output reg  [183:0]       conn_ctx,

    // BALBOA COUNTERS
    input  wire [31:0]        ack_cnt,
    input  wire [31:0]        wr_cnt,
    input  wire [31:0]        rd_cnt,
    input  wire [31:0]        retx_wr_cnt,
    input  wire [31:0]        retx_rd_cnt,
    input  wire [32*N_STATS-1:0] net_stats,

    // DEMO (transport_demo)
    output reg                demo_start,    // 1-cycle pulse
    output reg                reset_demo,
    output reg  [63:0]        freq_num,
    output reg  [31:0]        syn_sum,
    output reg  [31:0]        syn_depth,
    output reg  [31:0]        freq_span,
    output reg  [31:0]        lfsr_seed,
    input  wire               demo_run,
    input  wire               demo_done,
    input  wire [31:0]        demo_err,

    // RTT TRACE
    output reg                trace_clear,
    input  wire [31:0]        trace_cnt,
    input  wire               trace_full,
    input  wire               trace_sat
);

    localparam [11:0]
        REG_CTRL        = 12'h000,
        REG_STATUS      = 12'h004,
        REG_RING_IDX    = 12'h008,
        REG_NEXT_SEQ    = 12'h00C,
        REG_RING_LEN    = 12'h010,
        REG_FLAGS       = 12'h014,
        REG_LAST_SEQ    = 12'h018,
        REG_DONE_CNT    = 12'h01C,
        REG_REPLY_TO    = 12'h030,
        REG_ROUND_CNT   = 12'h034,
        REG_RTT_LO      = 12'h038,
        REG_RTT_HI      = 12'h03C,
        REG_CLK_HZ      = 12'h040,
        REG_STALE_CNT   = 12'h044,
        REG_TIMEOUT_CNT = 12'h048,
        REG_TX_CNT      = 12'h04C,
        REG_RX_CNT      = 12'h050,
        REG_NET_STATUS  = 12'h064,
        REG_SEQ_OFF     = 12'h074,
        REG_LAST_RD     = 12'h078,
        REG_BUILD_ID    = 12'h07C,
        REG_NET_CTRL    = 12'h080,
        REG_NET_PEND    = 12'h084,
        REG_LOCAL_IP    = 12'h088,
        REG_MAC_LO      = 12'h08C,
        REG_MAC_HI      = 12'h090,
        REG_ARP_IP      = 12'h094,
        REG_LOCAL_QPN   = 12'h098,
        REG_REQ_LEN     = 12'h09C,
        REG_RMT_BASE_LO = 12'h0A0,
        REG_RMT_BASE_HI = 12'h0A4,
        REG_DEMO_CTRL   = 12'h0A8,
        REG_FREQ_LO     = 12'h0AC,
        REG_FREQ_HI     = 12'h0B0,
        REG_DEMO_STS    = 12'h0B4,
        REG_CMD_CNT     = 12'h0B8,
        REG_STS1        = 12'h0BC,
        REG_SYN_DEPTH   = 12'h0C0,
        REG_FREQ_SPAN   = 12'h0C4,
        REG_LFSR_SEED   = 12'h0C8,
        REG_TRACE_CTRL  = 12'h0CC,
        REG_TRACE_CNT   = 12'h0D0,
        REG_TRACE_STS   = 12'h0D4,
        REG_RMT_STRIDE  = 12'h0D8,
        REG_ACK_CNT     = 12'h0DC,
        REG_WR_CNT      = 12'h0E0,
        REG_RD_CNT      = 12'h0E4,
        REG_RETX_WR     = 12'h0E8,
        REG_RETX_RD     = 12'h0EC;

    localparam [11:0] NET_STATS_BASE = 12'h0F0,   // N_STATS words
                      QP_CTX_BASE    = 12'h140,   // 6 words each
                      CONN_CTX_BASE  = 12'h160;

    localparam integer CTX_WORDS = 6;
    localparam [11:0]  CTX_BYTES   = 12'd24;
    localparam [31:0]  STATS_BYTES = 4 * N_STATS;

    // staging copies of the contexts: qp_push / conn_push hand a copy to BALBOA
    reg  [31:0] qp_w   [0:CTX_WORDS-1];
    reg  [31:0] conn_w [0:CTX_WORDS-1];
    reg  [47:0] mac_q;
    assign local_mac = mac_q;

    // which context word an address names, if any
    function automatic [3:0] ctx_word(input [11:0] a, input [11:0] base);
        ctx_word = (a >= base && a < base + CTX_BYTES) ? {1'b0, a[4:2]} : 4'hF;
    endfunction

    integer i;

    // ------------------------------------------------------
    // Write channel
    // ------------------------------------------------------
    reg         aw_done, w_done;
    reg  [11:0] awaddr_q;
    reg  [31:0] wdata_q;
    wire        wr_fire = aw_done && w_done;
    wire [3:0]  qp_word   = ctx_word({awaddr_q[11:2], 2'b00}, QP_CTX_BASE);
    wire [3:0]  conn_word = ctx_word({awaddr_q[11:2], 2'b00}, CONN_CTX_BASE);

    always @(posedge clk) begin
        if (!rstn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            s_axi_bresp   <= 2'b00;
            aw_done       <= 1'b0;
            w_done        <= 1'b0;
            awaddr_q      <= 12'd0;
            wdata_q       <= 32'd0;

            start           <= 1'b0;
            abort           <= 1'b0;
            soft_reset      <= 1'b0;
            ring_wr         <= 1'b0;
            ring_wdata      <= {SLOT_AW{1'b0}};
            seq_wr          <= 1'b0;
            seq_wdata       <= 32'd0;
            ring_len        <= 256;
            seq_stamp       <= 1'b1;
            reply_to        <= CLK_HZ / 1000;   // 1 ms
            seq_off         <= 32'd12;          // PayloadSlot: {u64 value; u32 decoder_id; u32 seq_num}
            local_qpn       <= 16'd0;
            req_len         <= 7'd64;
            rmt_base        <= 64'd0;
            rmt_stride_log2 <= 4'd6;

            set_ip_valid  <= 1'b0;
            local_ip      <= 32'hC0A8_0002;     // 192.168.0.2
            set_mac_valid <= 1'b0;
            mac_q         <= 48'h0293_C648_4D8E;
            arp_valid     <= 1'b0;
            arp_ip        <= 32'd0;
            qp_valid      <= 1'b0;
            qp_ctx        <= 184'd0;
            conn_valid    <= 1'b0;
            conn_ctx      <= 184'd0;
            for (i = 0; i < CTX_WORDS; i = i + 1) begin
                qp_w[i]   <= 32'd0;
                conn_w[i] <= 32'd0;
            end

            demo_start    <= 1'b0;
            reset_demo    <= 1'b0;
            freq_num      <= 64'd0;
            syn_sum       <= 32'd1;
            syn_depth     <= 32'd256;
            freq_span     <= 32'd0;           // no jitter unless asked for
            lfsr_seed     <= 32'd0;
            trace_clear   <= 1'b1;
        end else begin
            start         <= 1'b0;
            ring_wr       <= 1'b0;
            seq_wr        <= 1'b0;
            demo_start    <= 1'b0;
            set_ip_valid  <= 1'b0;            // network_stack takes these every cycle
            set_mac_valid <= 1'b0;
            if (arp_ready)  arp_valid  <= 1'b0;
            if (qp_ready)   qp_valid   <= 1'b0;
            if (conn_ready) conn_valid <= 1'b0;

            // Latch AW into awaddr_q
            if (s_axi_awvalid && !aw_done) begin
                s_axi_awready <= 1'b1;
                awaddr_q      <= s_axi_awaddr[11:0];
                aw_done       <= 1'b1;
            end else begin
                s_axi_awready <= 1'b0;
            end

            // Latch W into wdata_q
            if (s_axi_wvalid && !w_done) begin
                s_axi_wready <= 1'b1;
                wdata_q      <= s_axi_wdata;
                w_done       <= 1'b1;
            end else begin
                s_axi_wready <= 1'b0;
            end

            // Commit write when both AW and W captured and no pending B
            if (wr_fire && !s_axi_bvalid) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;  // OKAY
                aw_done      <= 1'b0;
                w_done       <= 1'b0;
                if (qp_word != 4'hF) begin
                    qp_w[qp_word[2:0]] <= wdata_q;
                end else if (conn_word != 4'hF) begin
                    conn_w[conn_word[2:0]] <= wdata_q;
                end else begin
                    case ({awaddr_q[11:2], 2'b00})
                        REG_CTRL: begin
                            start      <= wdata_q[0];
                            abort      <= wdata_q[1];
                            soft_reset <= wdata_q[2];
                        end
                        REG_RING_IDX: begin
                            ring_wr    <= 1'b1;
                            ring_wdata <= wdata_q[SLOT_AW-1:0];
                        end
                        REG_NEXT_SEQ: begin
                            seq_wr    <= 1'b1;
                            seq_wdata <= wdata_q;
                        end
                        REG_RING_LEN:    ring_len  <= wdata_q[SLOT_AW:0];
                        REG_FLAGS:       seq_stamp <= wdata_q[0];
                        REG_REPLY_TO:    reply_to  <= wdata_q;
                        REG_SEQ_OFF:     seq_off   <= wdata_q;
                        REG_NET_CTRL: begin
                            if (wdata_q[0]) set_ip_valid  <= 1'b1;
                            if (wdata_q[1]) set_mac_valid <= 1'b1;
                            if (wdata_q[2]) arp_valid     <= 1'b1;
                            if (wdata_q[3]) begin
                                qp_valid <= 1'b1;
                                qp_ctx   <= {qp_w[5][23:0], qp_w[4], qp_w[3], qp_w[2], qp_w[1], qp_w[0]};
                            end
                            if (wdata_q[4]) begin
                                conn_valid <= 1'b1;
                                conn_ctx   <= {conn_w[5][23:0], conn_w[4], conn_w[3], conn_w[2], conn_w[1], conn_w[0]};
                            end
                        end
                        REG_LOCAL_IP:    local_ip          <= wdata_q;
                        REG_MAC_LO:      mac_q[31:0]       <= wdata_q;
                        REG_MAC_HI:      mac_q[47:32]      <= wdata_q[15:0];
                        REG_ARP_IP:      arp_ip            <= wdata_q;
                        REG_LOCAL_QPN:   local_qpn         <= wdata_q[15:0];
                        REG_REQ_LEN:     req_len           <= (wdata_q == 32'd0 || wdata_q > 32'd64) ? 7'd64 : wdata_q[6:0];
                        REG_RMT_BASE_LO: rmt_base[31:0]    <= wdata_q;
                        REG_RMT_BASE_HI: rmt_base[63:32]   <= wdata_q;
                        REG_RMT_STRIDE:  rmt_stride_log2   <= wdata_q[3:0];
                        REG_DEMO_CTRL: begin
                            demo_start <= wdata_q[0];
                            reset_demo <= wdata_q[1];
                        end
                        REG_FREQ_LO:    freq_num[31:0]  <= wdata_q;
                        REG_FREQ_HI:    freq_num[63:32] <= wdata_q;
                        REG_CMD_CNT:    syn_sum         <= wdata_q;
                        REG_SYN_DEPTH:  syn_depth       <= wdata_q;
                        REG_FREQ_SPAN:  freq_span       <= wdata_q;
                        REG_LFSR_SEED:  lfsr_seed       <= wdata_q;
                        REG_TRACE_CTRL: trace_clear     <= wdata_q[0];
                        default: ;  // Unmapped writes ignored (BRESP is OKAY)
                    endcase
                end
            end else if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
            end
        end
    end

    // ------------------------------------------------------
    // Read channel
    // ------------------------------------------------------
    reg  [11:0] araddr_q;
    wire [11:0] rd_a      = {araddr_q[11:2], 2'b00};
    wire [3:0]  qp_rword   = ctx_word(rd_a, QP_CTX_BASE);
    wire [3:0]  conn_rword = ctx_word(rd_a, CONN_CTX_BASE);
    wire [11:0] stat_off  = rd_a - NET_STATS_BASE;
    wire        is_stat   = (rd_a >= NET_STATS_BASE) && (rd_a < NET_STATS_BASE + STATS_BYTES[11:0]);

    always @(posedge clk) begin
        if (!rstn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rresp   <= 2'b00;
            s_axi_rdata   <= 32'd0;
            araddr_q      <= 12'd0;
        end else begin
            if (s_axi_arvalid && !s_axi_arready && !s_axi_rvalid) begin
                s_axi_arready <= 1'b1;
                araddr_q      <= s_axi_araddr[11:0];
            end else begin
                s_axi_arready <= 1'b0;
            end

            if (s_axi_arready) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;
                if (qp_rword != 4'hF) begin
                    s_axi_rdata <= qp_w[qp_rword[2:0]];
                end else if (conn_rword != 4'hF) begin
                    s_axi_rdata <= conn_w[conn_rword[2:0]];
                end else if (is_stat) begin
                    s_axi_rdata <= net_stats[stat_off[11:2]*32 +: 32];
                end else begin
                    case (rd_a)
                        REG_CTRL:        s_axi_rdata <= {29'd0, soft_reset, abort, 1'b0};
                        REG_STATUS:
                        s_axi_rdata <= {
                            sts_done_cnt[7:0], // [31:24]
                            6'd0,
                            demo_run,        // [17]
                            net_status[0],   // [16] stack up
                            7'd0,
                            sts_post_busy,   // [8]
                            sts_timeout,     // [7]
                            sts_reply_seen,  // [6]
                            sts_done,        // [5]
                            sts_busy,        // [4]
                            sts_state        // [3:0]
                        };
                        REG_RING_IDX:    s_axi_rdata <= {{(32 - SLOT_AW) {1'b0}}, sts_ring_idx};
                        REG_NEXT_SEQ:    s_axi_rdata <= sts_next_seq;
                        REG_RING_LEN:    s_axi_rdata <= {{(31 - SLOT_AW) {1'b0}}, ring_len};
                        REG_FLAGS:       s_axi_rdata <= {31'd0, seq_stamp};
                        REG_LAST_SEQ:    s_axi_rdata <= sts_last_seq;
                        REG_DONE_CNT:    s_axi_rdata <= sts_done_cnt;
                        REG_REPLY_TO:    s_axi_rdata <= reply_to;
                        REG_ROUND_CNT:   s_axi_rdata <= sts_round_cnt;
                        REG_RTT_LO:      s_axi_rdata <= sts_rtt[31:0];
                        REG_RTT_HI:      s_axi_rdata <= sts_rtt[63:32];
                        REG_CLK_HZ:      s_axi_rdata <= CLK_HZ;
                        REG_STALE_CNT:   s_axi_rdata <= sts_stale_cnt;
                        REG_TIMEOUT_CNT: s_axi_rdata <= sts_timeout_cnt;
                        REG_TX_CNT:      s_axi_rdata <= sts_tx_cnt;
                        REG_RX_CNT:      s_axi_rdata <= sts_rx_cnt;
                        REG_NET_STATUS:  s_axi_rdata <= net_status;
                        REG_SEQ_OFF:     s_axi_rdata <= seq_off;
                        REG_LAST_RD:     s_axi_rdata <= sts_last_rd;
                        REG_BUILD_ID:    s_axi_rdata <= BUILD_ID;
                        REG_NET_PEND:    s_axi_rdata <= {27'd0, conn_valid, qp_valid, arp_valid,
                                                         set_mac_valid, set_ip_valid};
                        REG_LOCAL_IP:    s_axi_rdata <= local_ip;
                        REG_MAC_LO:      s_axi_rdata <= mac_q[31:0];
                        REG_MAC_HI:      s_axi_rdata <= {16'd0, mac_q[47:32]};
                        REG_ARP_IP:      s_axi_rdata <= arp_ip;
                        REG_LOCAL_QPN:   s_axi_rdata <= {16'd0, local_qpn};
                        REG_REQ_LEN:     s_axi_rdata <= {25'd0, req_len};
                        REG_RMT_BASE_LO: s_axi_rdata <= rmt_base[31:0];
                        REG_RMT_BASE_HI: s_axi_rdata <= rmt_base[63:32];
                        REG_DEMO_CTRL:   s_axi_rdata <= {30'd0, reset_demo, demo_run};
                        REG_FREQ_LO:     s_axi_rdata <= freq_num[31:0];
                        REG_FREQ_HI:     s_axi_rdata <= freq_num[63:32];
                        REG_DEMO_STS:    s_axi_rdata <= {31'd0, demo_done};
                        REG_CMD_CNT:     s_axi_rdata <= syn_sum;
                        REG_STS1:        s_axi_rdata <= demo_err;
                        REG_SYN_DEPTH:   s_axi_rdata <= syn_depth;
                        REG_FREQ_SPAN:   s_axi_rdata <= freq_span;
                        REG_LFSR_SEED:   s_axi_rdata <= lfsr_seed;
                        REG_TRACE_CTRL:  s_axi_rdata <= {31'd0, trace_clear};
                        REG_TRACE_CNT:   s_axi_rdata <= trace_cnt;
                        REG_TRACE_STS:   s_axi_rdata <= {30'd0, trace_sat, trace_full};
                        REG_RMT_STRIDE:  s_axi_rdata <= {28'd0, rmt_stride_log2};
                        REG_ACK_CNT:     s_axi_rdata <= ack_cnt;
                        REG_WR_CNT:      s_axi_rdata <= wr_cnt;
                        REG_RD_CNT:      s_axi_rdata <= rd_cnt;
                        REG_RETX_WR:     s_axi_rdata <= retx_wr_cnt;
                        REG_RETX_RD:     s_axi_rdata <= retx_rd_cnt;
                        default:         s_axi_rdata <= 32'd0;
                    endcase
                end
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule

`default_nettype wire
