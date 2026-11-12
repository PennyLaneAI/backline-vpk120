// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
`timescale 1ns / 10ps
`default_nettype none

module transport_demo #(
    // Base addresses
    parameter BRAM_START_ADDR     = 32'h8000_0000 // Start address for syndromes
)(
    input wire clk,
    input wire resetn,

    // FREQ_num is the base counter which is used to set the data transmission rate
    input wire [63:0] freq_num,

    input wire start_demo,
    input wire reset_demo,

    input wire [31:0]  syn_sum,

    // AXI4 Master Interface0 (BRAM Read-Only for TX)
    output reg  [31:0]  m_axi_araddr_0,
    output wire [7:0]   m_axi_arlen_0,
    output wire [2:0]   m_axi_arsize_0,
    output reg          m_axi_arvalid_0,
    input  wire         m_axi_arready_0,

    input  wire [511:0] m_axi_rdata_0,
    input  wire         m_axi_rlast_0,
    input  wire         m_axi_rvalid_0,
    output reg          m_axi_rready_0,

    // AXI4 Master Interface1 (BRAM Read-Only for RX)
    output reg  [31:0]  m_axi_araddr_1,
    output wire [7:0]   m_axi_arlen_1,
    output wire [2:0]   m_axi_arsize_1,
    output reg          m_axi_arvalid_1,
    input  wire         m_axi_arready_1,

    input  wire [511:0] m_axi_rdata_1,
    input  wire         m_axi_rlast_1,
    input  wire         m_axi_rvalid_1,
    output reg          m_axi_rready_1,

    // AXI4-Stream Master
    output reg  [511:0] m_axis_tdata,
    output reg          m_axis_tvalid,
    input  wire         m_axis_tready,
    output wire         m_axis_tlast,    // Tied high for single-beat packets

    // AXI4-Stream Slave
    input  wire [511:0] s_axis_tdata,
    input  wire         s_axis_tvalid,
    output reg          s_axis_tready,
    input  wire         s_axis_tlast,    // Ignored in this single-beat logic

    output reg cmp_error,
    output reg cmp_valid,
    output wire demo_done,

    //depth of syndromes words in BRAM
    input wire [31:0] syn_depth,
    input wire [31:0] reply_seq_off,

    // 200 MHz: 1 ms = 200_000 cycles. mask 0x0FFF_FFFF -> 1 ms .. 1.34 s
    input wire [31:0] freq_span_mask,
    input wire [31:0] lfsr_seed
);

   // Constant AXI values
    assign m_axi_arlen_0  = 8'd0;       // 1 beat per read
    assign m_axi_arsize_0 = 3'b110;     // 64 bytes (512 bits)
    assign m_axi_arlen_1  = 8'd0;       // 1 beat per read
    assign m_axi_arsize_1 = 3'b110;     // 64 bytes (512 bits)
    assign m_axis_tlast = 1'b1;       // Single beat stream packet

    // Internal Registers
    reg [31:0]  bram_addr_main;
    reg [31:0]  bram_addr_arb;
    reg [511:0] fetched_data_512b;
    reg [511:0] rx_fetched_data_512b;
    reg [511:0] axis_rx_data_512b;
    reg [63:0] freq_cnt;
    reg tx_update;
    reg rx_update;

    reg [31:0] syn_count;
    reg [31:0] compl_count;

    assign demo_done = (compl_count == syn_sum);

    // FSM States
    localparam [2:0]
        S_IDLE        = 3'd0,
        S_BRAM_AR1    = 3'd1,
        S_BRAM_R1     = 3'd2,
        S_AXIS_TX     = 3'd3,
        S_AXIS_RX     = 3'd4,
        S_BRAM_AR2    = 3'd5,
        S_BRAM_R2     = 3'd6,
        S_INC         = 3'd7;

    reg [2:0] state_tx;
    reg [2:0] state_rx;

    wire tx_en = tx_update == rx_update;

    // Fib LFSR: 32-bit maximal-length (taps 32,22,2,1), advanced once per send
    // https://en.wikipedia.org/wiki/Linear-feedback_shift_register
    reg  [31:0] lfsr;
    wire [31:0] lfsr_init = (lfsr_seed != 32'd0) ? lfsr_seed : 32'hACE1_2345;
    wire        lfsr_fb   = lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0];
    wire [63:0] next_interval = freq_num + {32'd0, (lfsr & freq_span_mask)};

    // shift per cycle
    always @(posedge clk) begin
        if (!resetn || reset_demo) lfsr <= lfsr_init;
        else                       lfsr <= {lfsr[30:0], lfsr_fb};
    end

    wire [3:0]   seq_dw   = reply_seq_off[5:2];
    wire [511:0] payload_mask = {{384{1'b0}}, {128{1'b1}}};
    wire [511:0] seq_mask = payload_mask & ~({{480{1'b0}}, 32'hFFFF_FFFF} << (seq_dw * 32));

    always @(posedge clk) begin
        if (!resetn) begin
            state_tx           <= S_IDLE;
            bram_addr_main  <= BRAM_START_ADDR;

            m_axi_arvalid_0   <= 1'b0;
            m_axi_rready_0    <= 1'b0;

            m_axis_tvalid   <= 1'b0;
            m_axis_tdata    <= 512'd0;

            freq_cnt        <= 64'b0;
            tx_update       <= 1'b0;
            syn_count       <= 32'd0;
        end else begin
            if (reset_demo) begin
                syn_count <= 32'd0;
                tx_update <= 1'b0;
                freq_cnt <= 64'b0;
                bram_addr_main  <= BRAM_START_ADDR;
            end else begin
                case (state_tx)
                    S_IDLE: begin
                        if (start_demo && (syn_count < syn_sum)) begin
                            if (freq_cnt == 64'b0) begin
                                //prevent TX overruning RX in roundtrip mode
                                if (tx_en) begin
                                    state_tx <= S_BRAM_AR1;
                                end
                            end else begin
                                freq_cnt <= freq_cnt - 1;
                            end
                        end
                    end

                    S_BRAM_AR1: begin
                        m_axi_araddr_0  <= bram_addr_main;
                        m_axi_arvalid_0 <= 1'b1;
                        if (m_axi_arvalid_0 && m_axi_arready_0) begin
                            m_axi_arvalid_0 <= 1'b0;
                            m_axi_rready_0  <= 1'b1;
                            state_tx         <= S_BRAM_R1;
                        end
                    end

                    S_BRAM_R1: begin
                        if (m_axi_rready_0 && m_axi_rvalid_0) begin
                            m_axi_rready_0      <= 1'b0;
                            fetched_data_512b <= m_axi_rdata_0;
                            state_tx             <= S_AXIS_TX;
                        end
                    end

                    S_AXIS_TX: begin
                        m_axis_tdata  <= fetched_data_512b;
                        m_axis_tvalid <= 1'b1;
                        if (m_axis_tvalid && m_axis_tready) begin
                            m_axis_tvalid <= 1'b0;

                            state_tx <= S_INC;
                        end
                    end

                    S_INC: begin
                        if ((bram_addr_main - BRAM_START_ADDR) + 32'd64 >= syn_depth) begin
                            bram_addr_main <= BRAM_START_ADDR;
                        end else begin
                            bram_addr_main <= bram_addr_main + 32'd64;
                        end

                        freq_cnt  <= next_interval;
                        tx_update <= ~tx_update;

                        state_tx <= S_IDLE;
                        syn_count <= syn_count + 1;
                    end

                    default: state_tx <= S_IDLE;
                endcase
            end
        end
    end

    always @(posedge clk) begin
        if (!resetn) begin
            state_rx           <= S_AXIS_RX;
            bram_addr_arb   <= BRAM_START_ADDR + syn_depth;

            m_axi_arvalid_1   <= 1'b0;
            m_axi_rready_1    <= 1'b0;

            s_axis_tready   <= 1'b0;
            rx_update <= 1'b0;
            cmp_valid <= 1'b0;
            compl_count       <= 32'd0;
        end else begin
            if (reset_demo) begin
                cmp_valid <= 1'b0;
                rx_update <= 1'b0;
                bram_addr_arb   <= BRAM_START_ADDR + syn_depth;
                compl_count       <= 32'd0;
            end else begin
                cmp_valid <= 1'b0;

                case (state_rx)

                    S_AXIS_RX: begin
                        s_axis_tready <= 1'b1;
                        if (s_axis_tready && s_axis_tvalid) begin
                            s_axis_tready     <= 1'b0;
                            axis_rx_data_512b <= s_axis_tdata;
                            state_rx             <= S_BRAM_AR2;
                        end
                    end

                    S_BRAM_AR2: begin
                        m_axi_araddr_1  <= bram_addr_arb;
                        m_axi_arvalid_1 <= 1'b1;
                        if (m_axi_arvalid_1 && m_axi_arready_1) begin
                            m_axi_arvalid_1 <= 1'b0;
                            m_axi_rready_1  <= 1'b1;
                            state_rx         <= S_BRAM_R2;
                        end
                    end

                    S_BRAM_R2: begin
                        if (m_axi_rready_1 && m_axi_rvalid_1) begin
                            m_axi_rready_1      <= 1'b0;
                            rx_fetched_data_512b <= m_axi_rdata_1;
                            state_rx             <= S_INC;
                        end
                    end

                    S_INC: begin
                        if ((bram_addr_arb - (BRAM_START_ADDR + syn_depth)) + 32'd64 >= syn_depth) begin
                            bram_addr_arb <= BRAM_START_ADDR + syn_depth;
                        end else begin
                            bram_addr_arb <= bram_addr_arb + 32'd64;
                        end

                        rx_update <= ~rx_update;
                        state_rx <= S_AXIS_RX;

                        // mask off the seq_num DWord
                        if ((axis_rx_data_512b & seq_mask) ==
                            (rx_fetched_data_512b & seq_mask)) begin
                            cmp_valid <= 1'b1;
                            cmp_error <= 1'b0;
                        end else begin
                            cmp_valid <= 1'b1;
                            cmp_error <= 1'b1;
                        end
                        compl_count       <= compl_count + 1;
                    end

                    default: state_rx <= S_AXIS_RX;
                endcase
            end
        end
    end
endmodule

`default_nettype wire
