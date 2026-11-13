// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// hwhs_regs.v - hwhs-hsb registers (PS 0xA400_0000, 64 KB, 32-bit accesses).
//
//===----------------------------------------------------------------------===//
///
/// Register map (byte offsets):
///------------------------------------------------------------------------------
///   0x00 CTRL        W1P [0]=start (one sw-loop round)
///                    RW  [1]=abort       (level: engine back to IDLE once its AXIS handshakes finish,
///                                         and held there)
///                    RW  [2]=soft_reset  (level: engine state and counters held at reset)
///   0x04 STATUS      RO  [3:0]=state [4]=busy [5]=done [6]=reply_seen [7]=timeout
///                        [16]=hsb_init_done [17]=demo_run [18]=ecb_rx_valid [31:24]=DONE_CNT[7:0]
///   0x08 RING_IDX    RW  slot used by the next round (advances after every round)
///   0x0C NEXT_SEQ    RW  seq the engine stamps into the next request
///   0x10 RING_LEN    RW  slots in the ring, 1..2^SLOT_AW (default 256)
///   0x14 FLAGS       RW  [0]=seq_stamp (1: engine writes NEXT_SEQ at SEQ_OFF and expects it back;
///                                       0: the seq already in the payload is expected back)
///   0x18 LAST_SEQ    RO  seq of the last request sent
///   0x1C DONE_CNT    RO  rounds finished (reply or timeout); low 8 bits also in STATUS[31:24]
///------------------------------------------------------------------------------
///   0x30 REPLY_TO    RW  reply timeout in hif cycles (0: wait forever; ABORT recovers)
///   0x34 ROUND_CNT   RO  rounds that completed with a matching reply
///   0x38 RTT_LO      RO  last round's RTT in hif cycles [31:0]
///   0x3C RTT_HI      RO                                 [63:32]
///   0x40 CLK_HZ      RO  hif clock frequency
///   0x44 STALE_CNT   RO  reply packets dropped (seq mismatch, or no round waiting)
///   0x48 TIMEOUT_CNT RO  rounds that timed out
///   0x4C TX_CNT      RO  request beats accepted by HSB
///   0x50 RX_CNT      RO  packets received from HSB sensor-TX 0
///   0x64 HSB_STATUS  RO  platform status (see backline_hsb_top: [0]=init_done ...)
///   0x74 SEQ_OFF     RW  byte offset of the seq dword in request and reply (default 12)
///   0x78 LAST_RD     RO  seq dword of the last packet received from HSB
///   0x7C BUILD_ID    RO  build marker (yymmddHH of the Vivado run)
///------------------------------------------------------------------------------
///   0x80 ECB_CTRL    W1P [0]=send the TX frame [1]=release the RX frame
///   0x84 ECB_STATUS  RO  [0]=tx_busy [1]=rx_valid [2]=rx_trunc [24:16]=rx_len (bytes)
///   0x88 ECB_TX_LEN  RW  TX frame length in bytes (1..256)
///   0x8C ECB_PORT    RW  UDP source port of the frames the mailbox keeps (default 8192)
///   0x90 ECB_RX_CNT  RO  frames kept
///   0x94 ECB_DROP    RO  frames dropped because they are not ECB replies (BOOTP, ARP, ...)
///   0x98 ECB_OVF     RO  ECB replies dropped because the RX buffer was still full
///------------------------------------------------------------------------------
///   0xA8 DEMO_CTRL   W1P [0]=start_demo (sets demo_run)  RW [1]=reset_demo (level)
///   0xAC FREQ_LO     RW  pacer interval in hif cycles [31:0]
///   0xB0 FREQ_HI     RW                               [63:32]
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
///   0x400-0x4FC ECB_TX  W  TX frame, byte 4i+k in bits [8k+7:8k] of word i
///   0x500-0x5FC ECB_RX  R  RX frame, same packing
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none

module hwhs_regs #(
    parameter integer CLK_HZ   = 250000000,
    parameter [31:0]  BUILD_ID = 32'h0,
    parameter integer SLOT_AW  = 10
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

    // ENGINE STATUS
    input wire [ 3:0]         sts_state,
    input wire                sts_busy,
    input wire                sts_done,
    input wire                sts_reply_seen,
    input wire                sts_timeout,
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
    input wire [31:0]         hsb_status,    // [0] = HSB init_done

    // ECB MAILBOX
    output reg                ecb_send,      // 1-cycle pulse
    output reg                ecb_release,   // 1-cycle pulse
    output reg  [ 8:0]        ecb_tx_len,
    output reg  [15:0]        ecb_port,
    output reg                ecb_tx_we,     // 1-cycle pulse: TX word write
    output reg  [ 5:0]        ecb_tx_waddr,
    output reg  [31:0]        ecb_tx_wdata,
    output wire [ 5:0]        ecb_rx_raddr,
    input  wire [31:0]        ecb_rx_rdata,
    input  wire               ecb_tx_busy,
    input  wire               ecb_rx_valid,
    input  wire               ecb_rx_trunc,
    input  wire [ 8:0]        ecb_rx_len,
    input  wire [31:0]        ecb_rx_cnt,
    input  wire [31:0]        ecb_drop_cnt,
    input  wire [31:0]        ecb_ovf_cnt,

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
        REG_HSB_STATUS  = 12'h064,
        REG_SEQ_OFF     = 12'h074,
        REG_LAST_RD     = 12'h078,
        REG_BUILD_ID    = 12'h07C,
        REG_ECB_CTRL    = 12'h080,
        REG_ECB_STATUS  = 12'h084,
        REG_ECB_TX_LEN  = 12'h088,
        REG_ECB_PORT    = 12'h08C,
        REG_ECB_RX_CNT  = 12'h090,
        REG_ECB_DROP    = 12'h094,
        REG_ECB_OVF     = 12'h098,
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
        REG_TRACE_STS   = 12'h0D4;

    localparam [3:0] PAGE_ECB_TX = 4'h4,  // 0x400-0x4FC
                     PAGE_ECB_RX = 4'h5;  // 0x500-0x5FC

    // ------------------------------------------------------
    // Write channel
    // ------------------------------------------------------
    reg         aw_done, w_done;
    reg  [11:0] awaddr_q;
    reg  [31:0] wdata_q;
    wire        wr_fire = aw_done && w_done;

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

            start         <= 1'b0;
            abort         <= 1'b0;
            soft_reset    <= 1'b0;
            ring_wr       <= 1'b0;
            ring_wdata    <= {SLOT_AW{1'b0}};
            seq_wr        <= 1'b0;
            seq_wdata     <= 32'd0;
            ring_len      <= 256;
            seq_stamp     <= 1'b1;
            reply_to      <= CLK_HZ / 1000;   // 1 ms
            seq_off       <= 32'd12;          // after u64 value and u32 decoder_id

            ecb_send      <= 1'b0;
            ecb_release   <= 1'b0;
            ecb_tx_len    <= 9'd0;
            ecb_port      <= 16'd8192;
            ecb_tx_we     <= 1'b0;
            ecb_tx_waddr  <= 6'd0;
            ecb_tx_wdata  <= 32'd0;

            demo_start    <= 1'b0;
            reset_demo    <= 1'b0;
            freq_num      <= 64'd0;
            syn_sum       <= 32'd1;
            syn_depth     <= 32'd256;
            freq_span     <= 32'd0;           // no jitter
            lfsr_seed     <= 32'd0;
            trace_clear   <= 1'b1;
        end else begin
            start       <= 1'b0;
            ring_wr     <= 1'b0;
            seq_wr      <= 1'b0;
            ecb_send    <= 1'b0;
            ecb_release <= 1'b0;
            ecb_tx_we   <= 1'b0;
            demo_start  <= 1'b0;

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
                if (awaddr_q[11:8] == PAGE_ECB_TX) begin
                    ecb_tx_we    <= 1'b1;
                    ecb_tx_waddr <= awaddr_q[7:2];
                    ecb_tx_wdata <= wdata_q;
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
                        REG_RING_LEN:   ring_len  <= wdata_q[SLOT_AW:0];
                        REG_FLAGS:      seq_stamp <= wdata_q[0];
                        REG_REPLY_TO:   reply_to  <= wdata_q;
                        REG_SEQ_OFF:    seq_off   <= wdata_q;
                        REG_ECB_CTRL: begin
                            ecb_send    <= wdata_q[0];
                            ecb_release <= wdata_q[1];
                        end
                        REG_ECB_TX_LEN: ecb_tx_len <= wdata_q[8:0];
                        REG_ECB_PORT:   ecb_port   <= wdata_q[15:0];
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
    reg [11:0] araddr_q;
    assign ecb_rx_raddr = araddr_q[7:2];

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
                if (araddr_q[11:8] == PAGE_ECB_RX) begin
                    s_axi_rdata <= ecb_rx_rdata;
                end else begin
                    case ({araddr_q[11:2], 2'b00})
                        REG_CTRL:        s_axi_rdata <= {29'd0, soft_reset, abort, 1'b0};
                        REG_STATUS:
                        s_axi_rdata <= {
                            sts_done_cnt[7:0], // [31:24]
                            5'd0,
                            ecb_rx_valid,    // [18]
                            demo_run,        // [17]
                            hsb_status[0],   // [16] HSB init_done
                            7'd0,
                            1'b0,            // [8] axi_err: no AXI master in hwhs-hsb
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
                        REG_HSB_STATUS:  s_axi_rdata <= hsb_status;
                        REG_SEQ_OFF:     s_axi_rdata <= seq_off;
                        REG_LAST_RD:     s_axi_rdata <= sts_last_rd;
                        REG_BUILD_ID:    s_axi_rdata <= BUILD_ID;
                        REG_ECB_STATUS:
                        s_axi_rdata <= {7'd0, ecb_rx_len, 13'd0, ecb_rx_trunc, ecb_rx_valid, ecb_tx_busy};
                        REG_ECB_TX_LEN:  s_axi_rdata <= {23'd0, ecb_tx_len};
                        REG_ECB_PORT:    s_axi_rdata <= {16'd0, ecb_port};
                        REG_ECB_RX_CNT:  s_axi_rdata <= ecb_rx_cnt;
                        REG_ECB_DROP:    s_axi_rdata <= ecb_drop_cnt;
                        REG_ECB_OVF:     s_axi_rdata <= ecb_ovf_cnt;
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
