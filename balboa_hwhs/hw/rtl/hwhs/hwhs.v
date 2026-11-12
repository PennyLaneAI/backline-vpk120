// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// hwhs.v - hwhs-balboa, the hardware-handshake controller in front of BALBOA
//
// Data path of a round: one request out as an RC RDMA WRITE, one reply back as the
// coprocessor's RC RDMA WRITE. [A B] are the RAM ports; slot and reply RAM have the PS on A,
// the trace RAM has it on B.
//
//   PS (A72)                hwhs                                         BALBOA (balboa_net)
//
//   0x9100_0000 --write--> slot RAM  --request--> +-------------+ --sq_*, tx_*--> WRITE ==> coprocessor
//                          [A    B]               |             |
//   0x9300_0000 <--read--- reply RAM <--reply---- | hwhs_engine | <--wr_*-------- WRITE <== coprocessor
//                          [A    B]               |             |
//   0x9200_0000 <--read--- trace RAM <--RTT------ +-------------+
//                          [B    A]                  ^        |
//                                          s_axis    |        |    m_axis
//                                       (request)    |        v    (reply)
//                                             transport_demo (hw-loop)
//
// Around the engine, what BALBOA needs from the memory it would have in Coyote:
//
//   retransmission copies  <--mem_wr_*/mem_rd_*-->  hwhs_retx_ram
//   READ responses         <--rd_*--               zeros (hwhs posts no READ targets)
//   ACKs                   --ack_*-->              counted
//
// Control, and BALBOA's configuration:
//
//   0xA400_0000 --AXI-Lite--> hwhs_regs --control--> hwhs_engine
//                              |     ^  <--status---
//                              v     |
//                      IP, MAC, ARP lookups, QP and connection contexts --> BALBOA
//
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none

module hwhs #(
    parameter integer CLK_HZ   = 250000000,
    parameter [31:0]  BUILD_ID = 32'h0,
    parameter integer SLOT_AW  = 10,   // slot/reply RAM: 2^SLOT_AW x 64 B
    parameter integer TRACE_AW = 20,   // RTT trace: 2^TRACE_AW x TRACE_DW bit
    parameter integer N_STATS  = 13,   // BALBOA counters, hwhs_regs NET_STATS
    // Widths fixed by what is on the other side of each port:
    parameter integer BEAT_DW       = 512,  // BALBOA's data path (AXI_NET_BITS) = one slot: 64 B
    parameter integer TRACE_DW      = 16,   // one RTT trace entry (the driver reads 16 bit)
    parameter integer TRACE_BRAM_DW = 32,   // axi_bram_ctrl_trace data width
    parameter integer BRAM_AW       = 32,   // axi_bram_ctrl byte address, as the BD drives it
    parameter integer RETX_OFFS_LSB = 39    // where rdma_mux_retrans puts offs in a memory address
) (
    input  wire                       clk,
    input  wire                       rstn,
    input  wire                       stack_rstn,   // BALBOA's reset: also resets the engine

    // AXI4-Lite slave (registers)
    input  wire [15:0]                s_axi_awaddr,
    input  wire                       s_axi_awvalid,
    output wire                       s_axi_awready,
    input  wire [31:0]                s_axi_wdata,
    input  wire [ 3:0]                s_axi_wstrb,
    input  wire                       s_axi_wvalid,
    output wire                       s_axi_wready,
    output wire [ 1:0]                s_axi_bresp,
    output wire                       s_axi_bvalid,
    input  wire                       s_axi_bready,
    input  wire [15:0]                s_axi_araddr,
    input  wire                       s_axi_arvalid,
    output wire                       s_axi_arready,
    output wire [31:0]                s_axi_rdata,
    output wire [ 1:0]                s_axi_rresp,
    output wire                       s_axi_rvalid,
    input  wire                       s_axi_rready,

    // axi_bram_ctrl ports (PS side of the slot, reply and trace RAMs)
    input  wire                       slot_bram_rst,
    input  wire                       slot_bram_en,
    input  wire [BEAT_DW/8-1:0]       slot_bram_we,
    input  wire [BRAM_AW-1:0]         slot_bram_addr,
    input  wire [BEAT_DW-1:0]         slot_bram_wrdata,
    output wire [BEAT_DW-1:0]         slot_bram_rddata,

    input  wire                       reply_bram_rst,
    input  wire                       reply_bram_en,
    input  wire [BEAT_DW/8-1:0]       reply_bram_we,
    input  wire [BRAM_AW-1:0]         reply_bram_addr,
    input  wire [BEAT_DW-1:0]         reply_bram_wrdata,
    output wire [BEAT_DW-1:0]         reply_bram_rddata,

    input  wire                       trace_bram_rst,
    input  wire                       trace_bram_en,
    input  wire [TRACE_BRAM_DW/8-1:0] trace_bram_we,
    input  wire [BRAM_AW-1:0]         trace_bram_addr,
    input  wire [TRACE_BRAM_DW-1:0]   trace_bram_wrdata,
    output wire [TRACE_BRAM_DW-1:0]   trace_bram_rddata,

    // hw-loop: transport_demo
    input  wire [BEAT_DW-1:0]         s_axis_tdata,
    input  wire                       s_axis_tvalid,
    output wire                       s_axis_tready,
    input  wire                       s_axis_tlast,  // ignored: single-beat messages
    output wire [BEAT_DW-1:0]         m_axis_tdata,
    output wire                       m_axis_tvalid,
    input  wire                       m_axis_tready,
    output wire                       m_axis_tlast,

    output wire                       start_demo,
    output wire                       reset_demo,
    output wire [ 31:0]               syn_sum,
    output wire [ 63:0]               freq_num,
    output wire [ 31:0]               syn_depth,
    output wire [ 31:0]               reply_seq_off,
    output wire [ 31:0]               freq_span_mask,
    output wire [ 31:0]               lfsr_seed,
    input  wire                       cmp_error,
    input  wire                       cmp_valid,
    input  wire                       demo_done,

    // BALBOA: configuration
    output wire                       set_ip_valid,
    output wire [31:0]                set_ip,
    output wire                       set_mac_valid,
    output wire [47:0]                set_mac,
    output wire                       arp_valid,
    input  wire                       arp_ready,
    output wire [31:0]                arp_ip,
    output wire                       qp_valid,
    input  wire                       qp_ready,
    output wire [183:0]               qp_ctx,
    output wire                       conn_valid,
    input  wire                       conn_ready,
    output wire [183:0]               conn_ctx,
    input  wire [32*N_STATS-1:0]      net_stats,

    // BALBOA: requests, as WRITEs (work request + payload)
    output wire                       sq_valid,
    input  wire                       sq_ready,
    output wire [15:0]                sq_qpn,
    output wire [63:0]                sq_raddr,
    output wire [27:0]                sq_len,
    output wire [BEAT_DW-1:0]         tx_tdata,
    output wire [BEAT_DW/8-1:0]       tx_tkeep,
    output wire                       tx_tlast,
    output wire                       tx_tvalid,
    input  wire                       tx_tready,

    // BALBOA: ACKs of the requests
    input  wire                       ack_valid,
    output wire                       ack_ready,

    // BALBOA: READs the coprocessor issues, and their response data
    input  wire                       rd_valid,
    output wire                       rd_ready,
    input  wire [27:0]                rd_len,
    output wire [BEAT_DW-1:0]         rd_tdata,
    output wire [BEAT_DW/8-1:0]       rd_tkeep,
    output wire                       rd_tlast,
    output wire                       rd_tvalid,
    input  wire                       rd_tready,

    // BALBOA: WRITEs the coprocessor issues (the replies)
    input  wire                       wr_valid,
    output wire                       wr_ready,
    input  wire [BEAT_DW-1:0]         wr_tdata,
    input  wire [BEAT_DW/8-1:0]       wr_tkeep,
    input  wire                       wr_tlast,
    input  wire                       wr_tvalid,
    output wire                       wr_tready,

    // BALBOA: retransmission buffer
    input  wire                       mem_wr_valid,
    output wire                       mem_wr_ready,
    input  wire [95:0]                mem_wr_cmd,
    input  wire [BEAT_DW-1:0]         mem_wr_tdata,
    input  wire                       mem_wr_tlast,
    input  wire                       mem_wr_tvalid,
    output wire                       mem_wr_tready,
    input  wire                       mem_rd_valid,
    output wire                       mem_rd_ready,
    input  wire [95:0]                mem_rd_cmd,
    output wire [BEAT_DW-1:0]         mem_rd_tdata,
    output wire [BEAT_DW/8-1:0]       mem_rd_tkeep,
    output wire                       mem_rd_tlast,
    output wire                       mem_rd_tvalid,
    input  wire                       mem_rd_tready,

    // platform status, synchronized to clk by the top ([0] = stack up)
    input  wire [ 31:0]               net_status
);

    localparam integer BEAT_BYTES = BEAT_DW / 8;
    localparam integer BEAT_LOG   = $clog2(BEAT_BYTES);

    // ------------------------------------------------------------ //
    // Registers
    // ------------------------------------------------------------ //
    wire               start, abort, soft_reset, ring_wr, seq_wr, seq_stamp;
    wire [SLOT_AW-1:0] ring_wdata;
    wire [SLOT_AW:0]   ring_len;
    wire [31:0]        seq_wdata, reply_to, seq_off;
    wire [15:0]        local_qpn;
    wire [6:0]         req_len;
    wire [63:0]        rmt_base;
    wire [3:0]         rmt_stride_log2;

    wire [3:0]         sts_state;
    wire               sts_busy, sts_done, sts_reply_seen, sts_timeout, sts_post_busy;
    wire [SLOT_AW-1:0] sts_ring_idx;
    wire [31:0]        sts_next_seq, sts_last_seq, sts_round_cnt, sts_done_cnt, sts_stale_cnt,
                       sts_timeout_cnt, sts_tx_cnt, sts_rx_cnt, sts_last_rd;
    wire [63:0]        sts_rtt;

    wire               demo_start, demo_reset, trace_clear;
    wire [31:0]        sts_trace_cnt;
    wire               sts_trace_full, sts_trace_sat;

    reg                demo_run;
    reg  [31:0]        demo_err;
    reg  [31:0]        ack_cnt, wr_cnt, rd_cnt;
    wire [31:0]        retx_wr_cnt, retx_rd_cnt;

    hwhs_regs #(
        .CLK_HZ  (CLK_HZ),
        .BUILD_ID(BUILD_ID),
        .SLOT_AW (SLOT_AW),
        .N_STATS (N_STATS)
    ) u_regs (
        .clk            (clk),
        .rstn           (rstn),
        .s_axi_awaddr   (s_axi_awaddr),
        .s_axi_awvalid  (s_axi_awvalid),
        .s_axi_awready  (s_axi_awready),
        .s_axi_wdata    (s_axi_wdata),
        .s_axi_wstrb    (s_axi_wstrb),
        .s_axi_wvalid   (s_axi_wvalid),
        .s_axi_wready   (s_axi_wready),
        .s_axi_bresp    (s_axi_bresp),
        .s_axi_bvalid   (s_axi_bvalid),
        .s_axi_bready   (s_axi_bready),
        .s_axi_araddr   (s_axi_araddr),
        .s_axi_arvalid  (s_axi_arvalid),
        .s_axi_arready  (s_axi_arready),
        .s_axi_rdata    (s_axi_rdata),
        .s_axi_rresp    (s_axi_rresp),
        .s_axi_rvalid   (s_axi_rvalid),
        .s_axi_rready   (s_axi_rready),
        .start          (start),
        .abort          (abort),
        .soft_reset     (soft_reset),
        .ring_wr        (ring_wr),
        .ring_wdata     (ring_wdata),
        .seq_wr         (seq_wr),
        .seq_wdata      (seq_wdata),
        .ring_len       (ring_len),
        .seq_stamp      (seq_stamp),
        .reply_to       (reply_to),
        .seq_off        (seq_off),
        .local_qpn      (local_qpn),
        .req_len        (req_len),
        .rmt_base       (rmt_base),
        .rmt_stride_log2(rmt_stride_log2),
        .sts_state      (sts_state),
        .sts_busy       (sts_busy),
        .sts_done       (sts_done),
        .sts_reply_seen (sts_reply_seen),
        .sts_timeout    (sts_timeout),
        .sts_post_busy  (sts_post_busy),
        .sts_ring_idx   (sts_ring_idx),
        .sts_next_seq   (sts_next_seq),
        .sts_last_seq   (sts_last_seq),
        .sts_round_cnt  (sts_round_cnt),
        .sts_done_cnt   (sts_done_cnt),
        .sts_rtt        (sts_rtt),
        .sts_stale_cnt  (sts_stale_cnt),
        .sts_timeout_cnt(sts_timeout_cnt),
        .sts_tx_cnt     (sts_tx_cnt),
        .sts_rx_cnt     (sts_rx_cnt),
        .sts_last_rd    (sts_last_rd),
        .net_status     (net_status),
        .set_ip_valid   (set_ip_valid),
        .local_ip       (set_ip),
        .set_mac_valid  (set_mac_valid),
        .local_mac      (set_mac),
        .arp_valid      (arp_valid),
        .arp_ready      (arp_ready),
        .arp_ip         (arp_ip),
        .qp_valid       (qp_valid),
        .qp_ready       (qp_ready),
        .qp_ctx         (qp_ctx),
        .conn_valid     (conn_valid),
        .conn_ready     (conn_ready),
        .conn_ctx       (conn_ctx),
        .ack_cnt        (ack_cnt),
        .wr_cnt         (wr_cnt),
        .rd_cnt         (rd_cnt),
        .retx_wr_cnt    (retx_wr_cnt),
        .retx_rd_cnt    (retx_rd_cnt),
        .net_stats      (net_stats),
        .demo_start     (demo_start),
        .reset_demo     (demo_reset),
        .freq_num       (freq_num),
        .syn_sum        (syn_sum),
        .syn_depth      (syn_depth),
        .freq_span      (freq_span_mask),
        .lfsr_seed      (lfsr_seed),
        .demo_run       (demo_run),
        .demo_done      (demo_done),
        .demo_err       (demo_err),
        .trace_clear    (trace_clear),
        .trace_cnt      (sts_trace_cnt),
        .trace_full     (sts_trace_full),
        .trace_sat      (sts_trace_sat)
    );

    // A reset of BALBOA (QP state, PSNs) resets the engine with it: a post owed to the old
    // stack must not go to the new one. The registers keep their values.
    wire eng_reset = soft_reset || !stack_rstn;

    // ------------------------------------------------------------ //
    // Demo run flag and error count
    // ------------------------------------------------------------ //
    always @(posedge clk) begin
        if (!rstn || demo_reset || abort || eng_reset) begin
            demo_run <= 1'b0;
        end else if (demo_start) begin
            demo_run <= 1'b1;
        end
    end

    always @(posedge clk) begin
        if (!rstn || demo_reset) demo_err <= 32'd0;
        else if (cmp_valid && cmp_error) demo_err <= demo_err + 32'd1;
    end

    assign start_demo    = demo_run;
    assign reset_demo    = demo_reset;
    assign reply_seq_off = seq_off;

    // ------------------------------------------------------------ //
    // Engine
    // ------------------------------------------------------------ //
    wire [SLOT_AW-1:0] slot_addr, reply_addr;
    wire [BEAT_DW-1:0] slot_dout, reply_din;
    wire               reply_we;
    wire               trace_we;
    wire [TRACE_AW-1:0] trace_addr;
    wire [TRACE_DW-1:0] trace_din;

    hwhs_engine #(
        .C_SLOT_AW (SLOT_AW),
        .C_BEAT_DW (BEAT_DW),
        .C_TRACE_AW(TRACE_AW),
        .C_TRACE_DW(TRACE_DW)
    ) u_engine (
        .clk            (clk),
        .rstn           (rstn),
        .start          (start),
        .abort          (abort),
        .soft_reset     (eng_reset),
        .ring_wr        (ring_wr),
        .ring_wdata     (ring_wdata),
        .seq_wr         (seq_wr),
        .seq_wdata      (seq_wdata),
        .ring_len       (ring_len),
        .seq_stamp      (seq_stamp),
        .reply_to       (reply_to),
        .seq_off        (seq_off),
        .demo_run       (demo_run),
        .trace_clear    (trace_clear),
        .rmt_base       (rmt_base),
        .rmt_stride_log2(rmt_stride_log2),
        .sts_state      (sts_state),
        .sts_busy       (sts_busy),
        .sts_done       (sts_done),
        .sts_reply_seen (sts_reply_seen),
        .sts_timeout    (sts_timeout),
        .sts_post_busy  (sts_post_busy),
        .sts_ring_idx   (sts_ring_idx),
        .sts_next_seq   (sts_next_seq),
        .sts_last_seq   (sts_last_seq),
        .sts_round_cnt  (sts_round_cnt),
        .sts_done_cnt   (sts_done_cnt),
        .sts_rtt        (sts_rtt),
        .sts_stale_cnt  (sts_stale_cnt),
        .sts_timeout_cnt(sts_timeout_cnt),
        .sts_tx_cnt     (sts_tx_cnt),
        .sts_rx_cnt     (sts_rx_cnt),
        .sts_last_rd    (sts_last_rd),
        .sts_trace_cnt  (sts_trace_cnt),
        .sts_trace_full (sts_trace_full),
        .sts_trace_sat  (sts_trace_sat),
        .slot_addr      (slot_addr),
        .slot_dout      (slot_dout),
        .reply_we       (reply_we),
        .reply_addr     (reply_addr),
        .reply_din      (reply_din),
        .s_axis_tdata   (s_axis_tdata),
        .s_axis_tvalid  (s_axis_tvalid),
        .s_axis_tready  (s_axis_tready),
        .m_axis_tdata   (m_axis_tdata),
        .m_axis_tvalid  (m_axis_tvalid),
        .m_axis_tready  (m_axis_tready),
        .sq_valid       (sq_valid),
        .sq_ready       (sq_ready),
        .sq_raddr       (sq_raddr),
        .tx_tvalid      (tx_tvalid),
        .tx_tready      (tx_tready),
        .tx_tdata       (tx_tdata),
        .rx_tdata       (wr_tdata),
        .rx_tkeep       (wr_tkeep),
        .rx_tvalid      (wr_tvalid),
        .rx_tlast       (wr_tlast),
        .trace_we       (trace_we),
        .trace_addr     (trace_addr),
        .trace_din      (trace_din)
    );

    assign m_axis_tlast = 1'b1;    // single-beat messages

    // every request is one beat of REQ_LEN bytes
    assign sq_qpn   = local_qpn;
    assign sq_len   = {21'd0, req_len};
    assign tx_tkeep = req_len[BEAT_LOG] ? {BEAT_BYTES{1'b1}}            // REQ_LEN is 1..64
                                        : ~({BEAT_BYTES{1'b1}} << req_len[BEAT_LOG-1:0]);
    assign tx_tlast = 1'b1;

    // the replies are never back-pressured
    assign wr_ready  = 1'b1;
    assign wr_tready = 1'b1;
    assign ack_ready = 1'b1;

    always @(posedge clk) begin
        if (!rstn || eng_reset) begin
            ack_cnt <= 32'd0;
            wr_cnt  <= 32'd0;
        end else begin
            if (ack_valid) ack_cnt <= ack_cnt + 32'd1;
            if (wr_valid)  wr_cnt  <= wr_cnt + 32'd1;
        end
    end

    // ------------------------------------------------------------ //
    // READs: answered with zeros, so a stray READ cannot hold up the stack's transmit path
    // ------------------------------------------------------------ //
    reg        rd_busy;
    reg [27:0] rd_left;                  // beats still to send, this one included
    reg [BEAT_LOG-1:0] rd_tail;          // bytes in the last beat (0 = a full beat)
    wire [27:0] rd_beats = {{BEAT_LOG{1'b0}}, rd_len[27:BEAT_LOG]}
                         + {27'd0, (rd_len[BEAT_LOG-1:0] != {BEAT_LOG{1'b0}})};

    assign rd_ready  = !rd_busy;
    assign rd_tvalid = rd_busy;
    assign rd_tdata  = {BEAT_DW{1'b0}};
    assign rd_tlast  = (rd_left == 28'd1);
    assign rd_tkeep  = (rd_tlast && rd_tail != {BEAT_LOG{1'b0}}) ? ~({BEAT_BYTES{1'b1}} << rd_tail)
                                                                 : {BEAT_BYTES{1'b1}};

    always @(posedge clk) begin
        if (!stack_rstn) begin
            rd_busy <= 1'b0;
            rd_cnt  <= 32'd0;
        end else if (!rd_busy) begin
            if (rd_valid) begin
                rd_busy <= (rd_len != 28'd0);
                rd_left <= rd_beats;
                rd_tail <= rd_len[BEAT_LOG-1:0];
                rd_cnt  <= rd_cnt + 32'd1;
            end
        end else if (rd_tready) begin
            rd_left <= rd_left - 28'd1;
            if (rd_left == 28'd1) rd_busy <= 1'b0;
        end
    end

    // ------------------------------------------------------------ //
    // RAMs
    // ------------------------------------------------------------ //
    hwhs_ram #(
        .C_SLOT_AW(SLOT_AW),
        .C_SLOT_DW(BEAT_DW),
        .C_BRAM_DW(BEAT_DW),
        .C_BRAM_AW(BRAM_AW)
    ) u_slot_ram (
        .clk        (clk),
        .bram_rst   (slot_bram_rst),
        .bram_en    (slot_bram_en),
        .bram_we    (slot_bram_we),
        .bram_addr  (slot_bram_addr),
        .bram_wrdata(slot_bram_wrdata),
        .bram_rddata(slot_bram_rddata),
        .en_b       (1'b1),
        .we_b       (1'b0),
        .addr_b     (slot_addr),
        .din_b      ({BEAT_DW{1'b0}}),
        .dout_b     (slot_dout)
    );

    hwhs_ram #(
        .C_SLOT_AW(SLOT_AW),
        .C_SLOT_DW(BEAT_DW),
        .C_BRAM_DW(BEAT_DW),
        .C_BRAM_AW(BRAM_AW)
    ) u_reply_ram (
        .clk        (clk),
        .bram_rst   (reply_bram_rst),
        .bram_en    (reply_bram_en),
        .bram_we    (reply_bram_we),
        .bram_addr  (reply_bram_addr),
        .bram_wrdata(reply_bram_wrdata),
        .bram_rddata(reply_bram_rddata),
        .en_b       (reply_we),
        .we_b       (reply_we),
        .addr_b     (reply_addr),
        .din_b      (reply_din),
        .dout_b     ()
    );

    hh_rtt_trace #(
        .C_TRACE_AW(TRACE_AW),
        .C_TRACE_DW(TRACE_DW),
        .C_BRAM_DW (TRACE_BRAM_DW)
    ) u_trace (
        .clk        (clk),
        .trace_we   (trace_we),
        .trace_addr (trace_addr),
        .trace_din  (trace_din),
        .bram_rst   (trace_bram_rst),
        .bram_clk   (clk),
        .bram_en    (trace_bram_en),
        .bram_we    (trace_bram_we),
        .bram_addr  (trace_bram_addr[TRACE_AW:0]),   // byte address of 2^TRACE_AW 2-byte entries
        .bram_wrdata(trace_bram_wrdata),
        .bram_rddata(trace_bram_rddata)
    );

    hwhs_retx_ram #(
        .C_DW      (BEAT_DW),
        .C_OFFS_LSB(RETX_OFFS_LSB)
    ) u_retx (
        .clk         (clk),
        .rstn        (stack_rstn),
        .wr_cmd_valid(mem_wr_valid),
        .wr_cmd_ready(mem_wr_ready),
        .wr_cmd      (mem_wr_cmd),
        .wr_tdata    (mem_wr_tdata),
        .wr_tlast    (mem_wr_tlast),
        .wr_tvalid   (mem_wr_tvalid),
        .wr_tready   (mem_wr_tready),
        .rd_cmd_valid(mem_rd_valid),
        .rd_cmd_ready(mem_rd_ready),
        .rd_cmd      (mem_rd_cmd),
        .rd_tdata    (mem_rd_tdata),
        .rd_tkeep    (mem_rd_tkeep),
        .rd_tlast    (mem_rd_tlast),
        .rd_tvalid   (mem_rd_tvalid),
        .rd_tready   (mem_rd_tready),
        .wr_cnt      (retx_wr_cnt),
        .rd_cnt      (retx_rd_cnt)
    );

endmodule

`default_nettype wire
