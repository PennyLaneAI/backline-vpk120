// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// hwhs_retx_ram.v - BALBOA's retransmission buffer, on chip
//
// BALBOA keeps a copy of every WRITE payload it sends until the WRITE is acknowledged, and
// reads it back to retransmit. Coyote puts that copy in card memory behind a datamover; here
// it is a small RAM, since hwhs posts one beat per WRITE.
//
//   wr_cmd, then wr_* beats   copy of a fresh payload       (roce_stack m_rdma_mem_wr_cmd,
//                                                             m_axis_rdma_mem_wr)
//   rd_cmd -> rd_* beats      the copy, for a retransmission (m_rdma_mem_rd_cmd,
//                                                             s_axis_rdma_mem_rd)
//
// A command is {len[31:0], addr[63:0]}. rdma_mux_retrans builds addr as
// ({vfid, pid, offs} << log2(PMTU)) << RDMA_MEM_SHIFT, so a message's copy is found by its
// QP and by its slot in that QP's outstanding window (offs, from rdma_flow); nothing else in
// addr varies. The RAM keeps 2^C_BEATS_LOG beats per {pid[C_PID_W-1:0], offs}; a longer
// message keeps only its first beats, which hwhs never sends.
//
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none

module hwhs_retx_ram #(
    parameter integer C_DW        = 512,
    parameter integer C_OFFS_LSB  = 39,  // offs: RDMA_MEM_SHIFT (27) + log2(PMTU_BYTES) (12)
    parameter integer C_OFFS_W    = 4,   // log2(RDMA_N_WR_OUTSTANDING)
    parameter integer C_PID_W     = 2,   // low pid bits kept: copies for 2^C_PID_W QPs
    parameter integer C_BEATS_LOG = 2    // beats per message copy
) (
    input  wire              clk,
    input  wire              rstn,

    input  wire              wr_cmd_valid,
    output wire              wr_cmd_ready,
    input  wire [95:0]       wr_cmd,
    input  wire [C_DW-1:0]   wr_tdata,
    input  wire              wr_tlast,
    input  wire              wr_tvalid,
    output wire              wr_tready,

    input  wire              rd_cmd_valid,
    output wire              rd_cmd_ready,
    input  wire [95:0]       rd_cmd,
    output wire [C_DW-1:0]   rd_tdata,
    output wire [C_DW/8-1:0] rd_tkeep,
    output wire              rd_tlast,
    output reg               rd_tvalid,
    input  wire              rd_tready,

    output reg  [31:0]       wr_cnt,     // copies written
    output reg  [31:0]       rd_cnt      // copies read back: retransmissions
);

    localparam integer PID_LSB   = C_OFFS_LSB + C_OFFS_W;   // pid sits right above offs
    localparam integer IDX_W     = C_PID_W + C_OFFS_W;
    localparam integer AW        = IDX_W + C_BEATS_LOG;
    localparam integer BEAT_LOG  = $clog2(C_DW / 8);        // 6: bytes per beat
    localparam integer RD_LAT    = 2;

    function [IDX_W-1:0] slot_of(input [95:0] cmd);
        slot_of = {cmd[PID_LSB +: C_PID_W], cmd[C_OFFS_LSB +: C_OFFS_W]};
    endfunction

    // ------------------------------------------------------------ //
    // Write: take a command, then its beats
    // ------------------------------------------------------------ //
    reg                   w_busy;
    reg                   w_full;      // this copy filled its slot; the rest of it is dropped
    reg [IDX_W-1:0]       w_slot;
    reg [C_BEATS_LOG-1:0] w_beat;
    wire                  w_fire  = wr_tvalid && wr_tready;
    wire                  w_store = w_fire && !w_full;

    assign wr_cmd_ready = rstn && !w_busy;
    assign wr_tready    = w_busy;

    always @(posedge clk) begin
        if (!rstn) begin
            w_busy <= 1'b0;
            wr_cnt <= 32'd0;
        end else if (!w_busy) begin
            if (wr_cmd_valid) begin
                w_busy <= 1'b1;
                w_full <= 1'b0;
                w_slot <= slot_of(wr_cmd);
                w_beat <= {C_BEATS_LOG{1'b0}};
                wr_cnt <= wr_cnt + 32'd1;
            end
        end else if (w_fire) begin
            if (wr_tlast) w_busy <= 1'b0;
            if (w_beat == {C_BEATS_LOG{1'b1}}) w_full <= 1'b1;
            else                               w_beat <= w_beat + 1'b1;
        end
    end

    // ------------------------------------------------------------ //
    // Read: take a command, then hand out its beats one at a time
    // ------------------------------------------------------------ //
    reg                   r_busy;
    reg [IDX_W-1:0]       r_slot;
    reg [C_BEATS_LOG-1:0] r_beat;
    reg [31:0]            r_left;      // beats still to send, this one included
    reg [BEAT_LOG-1:0]    r_tail;      // bytes in the last beat (0 = a full beat)
    reg [RD_LAT:0]        r_pipe;      // a RAM read in flight
    reg                   r_issue;

    wire [C_DW-1:0] q;
    assign rd_tdata     = q;
    assign rd_tlast     = (r_left == 32'd1);
    assign rd_tkeep     = (rd_tlast && r_tail != {BEAT_LOG{1'b0}}) ? ~({(C_DW/8){1'b1}} << r_tail)
                                                                   : {(C_DW/8){1'b1}};
    assign rd_cmd_ready = rstn && !r_busy;

    wire [31:0] rd_len = rd_cmd[64 +: 32];

    always @(posedge clk) begin
        if (!rstn) begin
            r_busy    <= 1'b0;
            r_pipe    <= {(RD_LAT+1){1'b0}};
            r_issue   <= 1'b0;
            rd_tvalid <= 1'b0;
            rd_cnt    <= 32'd0;
        end else begin
            r_issue <= 1'b0;
            r_pipe  <= {r_pipe[RD_LAT-1:0], r_issue};
            if (!r_busy) begin
                if (rd_cmd_valid) begin
                    r_busy  <= 1'b1;
                    r_slot  <= slot_of(rd_cmd);
                    r_beat  <= {C_BEATS_LOG{1'b0}};
                    r_left  <= (rd_len + (C_DW / 8 - 1)) >> BEAT_LOG;
                    r_tail  <= rd_len[BEAT_LOG-1:0];
                    r_issue <= (rd_len != 32'd0);
                    rd_cnt  <= rd_cnt + 32'd1;
                    if (rd_len == 32'd0) r_busy <= 1'b0;
                end
            end else begin
                if (r_pipe[RD_LAT-1]) rd_tvalid <= 1'b1;             // the beat is out of the RAM
                if (rd_tvalid && rd_tready) begin
                    rd_tvalid <= 1'b0;
                    r_left    <= r_left - 32'd1;
                    if (r_left == 32'd1) begin
                        r_busy <= 1'b0;
                    end else begin
                        if (r_beat != {C_BEATS_LOG{1'b1}}) r_beat <= r_beat + 1'b1;
                        r_issue <= 1'b1;
                    end
                end
            end
        end
    end

    xpm_memory_sdpram #(
        .MEMORY_SIZE       (C_DW << AW),
        .MEMORY_PRIMITIVE  ("auto"),
        .CLOCKING_MODE     ("common_clock"),
        .MEMORY_INIT_FILE  ("none"),
        .MEMORY_INIT_PARAM (""),
        .USE_MEM_INIT      (0),
        .WAKEUP_TIME       ("disable_sleep"),
        .MESSAGE_CONTROL   (0),
        .ECC_MODE          ("no_ecc"),
        .AUTO_SLEEP_TIME   (0),
        .WRITE_DATA_WIDTH_A(C_DW),
        .BYTE_WRITE_WIDTH_A(C_DW),
        .ADDR_WIDTH_A      (AW),
        .READ_DATA_WIDTH_B (C_DW),
        .ADDR_WIDTH_B      (AW),
        .READ_RESET_VALUE_B("0"),
        .READ_LATENCY_B    (RD_LAT),
        .WRITE_MODE_B      ("read_first")
    ) u_ram (
        .sleep         (1'b0),
        .clka          (clk),
        .ena           (w_store),
        .wea           (w_store),
        .addra         ({w_slot, w_beat}),
        .dina          (wr_tdata),
        .injectsbiterra(1'b0),
        .injectdbiterra(1'b0),
        .clkb          (clk),
        .rstb          (1'b0),
        .enb           (1'b1),
        .regceb        (1'b1),
        .addrb         ({r_slot, r_beat}),
        .doutb         (q),
        .sbiterrb      (),
        .dbiterrb      ()
    );

endmodule

`default_nettype wire
