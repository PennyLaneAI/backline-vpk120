// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// hwhs.v - hwhs-hsb, the hardware-handshake controller in front of HSB
//
// Data path of a round. [A B] are the RAM ports; the PS is on A, except on the trace RAM.
//
//   PS (A72)                hwhs                                        HSB
//
//   0x9100_0000 --write--> slot RAM  --request--> +-------------+ --sif_rx--> sensor-RX 0 ==> coprocessor
//                          [A    B]               |             |
//   0x9300_0000 <--read--- reply RAM <--reply---- | hwhs_engine | <--sif_tx-- sensor-TX 0 <== coprocessor
//                          [A    B]               |             |
//   0x9200_0000 <--read--- trace RAM <--RTT------ +-------------+
//                          [B    A]                  ^        |
//                                          s_axis    |        |    m_axis
//                                       (request)    |        v    (reply)
//                                             transport_demo (hw-loop)
//
// Control, and HSB's configuration through ECB:
//
//   0xA400_0000 --AXI-Lite--> hwhs_regs --control--> hwhs_engine
//                              |     ^  <--status---
//                     TX frame |     | RX frame
//                              v     |
//                           hwhs_ecb_mbox --ECB request--> HSB host port 1
//                                         <--ECB reply----
//
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none

module hwhs #(
    parameter integer CLK_HZ   = 250000000,
    parameter [31:0]  BUILD_ID = 32'h0,
    parameter integer SLOT_AW  = 10,   // slot/reply RAM: 2^SLOT_AW x 64 B
    parameter integer TRACE_AW = 20,   // RTT trace: 2^TRACE_AW x TRACE_DW bit
    // Fixed by the other side of each port:
    parameter integer BEAT_DW       = 512,  // HSB AXIS beat = one 64 B slot (hwhs_ecb_mbox: 512 only)
    parameter integer TRACE_DW      = 16,   // one RTT trace entry
    parameter integer TRACE_BRAM_DW = 32,   // axi_bram_ctrl_trace data width
    parameter integer BRAM_AW       = 32    // axi_bram_ctrl byte address
) (
    input  wire                       clk,
    input  wire                       rstn,

    // AXI4-Lite slave (registers + ECB mailbox)
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

    // HSB sensor-RX 0
    output wire [BEAT_DW-1:0]         sif_rx_tdata,
    output wire [BEAT_DW/8-1:0]       sif_rx_tkeep,
    output wire                       sif_rx_tlast,
    output wire                       sif_rx_tvalid,
    input  wire                       sif_rx_tready,

    // HSB sensor-TX 0
    input  wire [BEAT_DW-1:0]         sif_tx_tdata,
    input  wire [BEAT_DW/8-1:0]       sif_tx_tkeep,
    input  wire                       sif_tx_tlast,
    input  wire                       sif_tx_tvalid,
    output wire                       sif_tx_tready,

    // HSB host port 1 (ECB mailbox)
    output wire [BEAT_DW-1:0]         ecb_rx_tdata,  // -> HSB i_hif_axis[1]
    output wire [BEAT_DW/8-1:0]       ecb_rx_tkeep,
    output wire                       ecb_rx_tlast,
    output wire                       ecb_rx_tvalid,
    input  wire                       ecb_rx_tready,
    input  wire [BEAT_DW-1:0]         ecb_tx_tdata,  // <- HSB o_hif_axis[1]
    input  wire [BEAT_DW/8-1:0]       ecb_tx_tkeep,
    input  wire                       ecb_tx_tlast,
    input  wire                       ecb_tx_tvalid,
    output wire                       ecb_tx_tready,

    // platform status in the clk domain ([0] = HSB init_done)
    input  wire [ 31:0]               hsb_status
);

    // ------------------------------------------------------------ //
    // Registers
    // ------------------------------------------------------------ //
    wire               start, abort, soft_reset, ring_wr, seq_wr, seq_stamp;
    wire [SLOT_AW-1:0] ring_wdata;
    wire [SLOT_AW:0]   ring_len;
    wire [31:0]        seq_wdata, reply_to, seq_off;

    wire [3:0]         sts_state;
    wire               sts_busy, sts_done, sts_reply_seen, sts_timeout;
    wire [SLOT_AW-1:0] sts_ring_idx;
    wire [31:0]        sts_next_seq, sts_last_seq, sts_round_cnt, sts_done_cnt, sts_stale_cnt,
                       sts_timeout_cnt, sts_tx_cnt, sts_rx_cnt, sts_last_rd;
    wire [63:0]        sts_rtt;

    wire               ecb_send, ecb_release, ecb_tx_we;

    // ECB mailbox, fixed in hwhs_ecb_mbox and hwhs_regs
    localparam integer ECB_WORD_AW = 6;  // each buffer: 2^6 32-bit words = 256 B
    localparam integer ECB_LEN_W   = 9;  // frame length in bytes, 0..256
    wire [ECB_LEN_W-1:0]   ecb_tx_len, ecb_rx_len;
    wire [15:0]            ecb_port;     // UDP source port
    wire [ECB_WORD_AW-1:0] ecb_tx_waddr, ecb_rx_raddr;
    wire [31:0]        ecb_tx_wdata, ecb_rx_rdata, ecb_rx_cnt, ecb_drop_cnt, ecb_ovf_cnt;
    wire               ecb_tx_busy, ecb_rx_valid, ecb_rx_trunc;

    wire               demo_start, demo_reset, trace_clear;
    wire [31:0]        sts_trace_cnt;
    wire               sts_trace_full, sts_trace_sat;

    reg                demo_run;
    reg  [31:0]        demo_err;

    hwhs_regs #(
        .CLK_HZ  (CLK_HZ),
        .BUILD_ID(BUILD_ID),
        .SLOT_AW (SLOT_AW)
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
        .sts_state      (sts_state),
        .sts_busy       (sts_busy),
        .sts_done       (sts_done),
        .sts_reply_seen (sts_reply_seen),
        .sts_timeout    (sts_timeout),
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
        .hsb_status     (hsb_status),
        .ecb_send       (ecb_send),
        .ecb_release    (ecb_release),
        .ecb_tx_len     (ecb_tx_len),
        .ecb_port       (ecb_port),
        .ecb_tx_we      (ecb_tx_we),
        .ecb_tx_waddr   (ecb_tx_waddr),
        .ecb_tx_wdata   (ecb_tx_wdata),
        .ecb_rx_raddr   (ecb_rx_raddr),
        .ecb_rx_rdata   (ecb_rx_rdata),
        .ecb_tx_busy    (ecb_tx_busy),
        .ecb_rx_valid   (ecb_rx_valid),
        .ecb_rx_trunc   (ecb_rx_trunc),
        .ecb_rx_len     (ecb_rx_len),
        .ecb_rx_cnt     (ecb_rx_cnt),
        .ecb_drop_cnt   (ecb_drop_cnt),
        .ecb_ovf_cnt    (ecb_ovf_cnt),
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

    // ------------------------------------------------------------ //
    // Demo run flag and error count
    // ------------------------------------------------------------ //
    always @(posedge clk) begin
        if (!rstn || demo_reset || abort || soft_reset) begin
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
        .soft_reset     (soft_reset),
        .ring_wr        (ring_wr),
        .ring_wdata     (ring_wdata),
        .seq_wr         (seq_wr),
        .seq_wdata      (seq_wdata),
        .ring_len       (ring_len),
        .seq_stamp      (seq_stamp),
        .reply_to       (reply_to),
        .seq_off        (seq_off),
        .demo_run       (demo_run),
        .sts_state      (sts_state),
        .sts_busy       (sts_busy),
        .sts_done       (sts_done),
        .sts_reply_seen (sts_reply_seen),
        .sts_timeout    (sts_timeout),
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
        .sif_rx_tdata   (sif_rx_tdata),
        .sif_rx_tvalid  (sif_rx_tvalid),
        .sif_rx_tready  (sif_rx_tready),
        .sif_tx_tdata   (sif_tx_tdata),
        .sif_tx_tvalid  (sif_tx_tvalid),
        .sif_tx_tlast   (sif_tx_tlast),
        .trace_clear    (trace_clear),
        .sts_trace_cnt  (sts_trace_cnt),
        .sts_trace_full (sts_trace_full),
        .sts_trace_sat  (sts_trace_sat),
        .trace_we       (trace_we),
        .trace_addr     (trace_addr),
        .trace_din      (trace_din)
    );

    assign m_axis_tlast  = 1'b1;    // single-beat messages
    assign sif_rx_tkeep  = {(BEAT_DW/8){1'b1}};
    assign sif_rx_tlast  = 1'b1;
    assign sif_tx_tready = 1'b1;

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

    // ------------------------------------------------------------ //
    // ECB mailbox on HSB host port 1
    // ------------------------------------------------------------ //
    hwhs_ecb_mbox u_ecb (
        .clk       (clk),
        .rstn      (rstn),
        .tx_we     (ecb_tx_we),
        .tx_waddr  (ecb_tx_waddr),
        .tx_wdata  (ecb_tx_wdata),
        .send      (ecb_send),
        .release_rx(ecb_release),
        .tx_len    (ecb_tx_len),
        .ecb_port  (ecb_port),
        .rx_raddr  (ecb_rx_raddr),
        .rx_rdata  (ecb_rx_rdata),
        .tx_busy   (ecb_tx_busy),
        .rx_valid  (ecb_rx_valid),
        .rx_trunc  (ecb_rx_trunc),
        .rx_len    (ecb_rx_len),
        .rx_cnt    (ecb_rx_cnt),
        .drop_cnt  (ecb_drop_cnt),
        .ovf_cnt   (ecb_ovf_cnt),
        .m_tdata   (ecb_rx_tdata),
        .m_tkeep   (ecb_rx_tkeep),
        .m_tlast   (ecb_rx_tlast),
        .m_tvalid  (ecb_rx_tvalid),
        .m_tready  (ecb_rx_tready),
        .s_tdata   (ecb_tx_tdata),
        .s_tkeep   (ecb_tx_tkeep),
        .s_tlast   (ecb_tx_tlast),
        .s_tvalid  (ecb_tx_tvalid)
    );

    assign ecb_tx_tready = 1'b1;

endmodule

`default_nettype wire
