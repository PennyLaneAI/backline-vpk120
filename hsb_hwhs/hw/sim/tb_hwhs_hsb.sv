// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// tb_hwhs_hsb.sv - hwhs-hsb against HSB IP (HOLOLINK_top):
//
//   tb "A72"  --AXI-Lite/BRAM-->  hwhs  --sensor RX0/TX0-->  HOLOLINK_top
//                                   \--ECB mailbox--> host port 1
//   coprocessor model  <--host port 0 (RoCE UC)-->  HOLOLINK_top
//
// Run with hw/sim/run.sh.

`timescale 1ps / 1ps
`include "HOLOLINK_def.svh"

module tb_hwhs_hsb;
  import HOLOLINK_pkg::*;

  // --------------------------------------------------------------------------------------
  // Clocks: hif 250 MHz, apb 100 MHz, ptp 100 MHz
  // --------------------------------------------------------------------------------------
  logic hif_clk = 1'b0, apb_clk = 1'b0, ptp_clk = 1'b0;
  always #2000 hif_clk = ~hif_clk;
  always #5000 apb_clk = ~apb_clk;
  initial begin
    #1300;
    forever #5000 ptp_clk = ~ptp_clk;
  end

  int unsigned cyc = 0;
  always @(posedge hif_clk) cyc <= cyc + 1;

  bit plain_write = 1'b0; // +plain_write: requests come as UC WRITE_ONLY (0x2A),
  initial plain_write = $test$plusargs("plain_write");

  logic sys_rst = 1'b1;
  logic hwhs_rstn = 1'b0;

  int errors = 0;
  task automatic check(input bit cond, input string what);
    if (!cond) begin
      errors++;
      $display("  FAIL: %s", what);
    end
  endtask

  // --------------------------------------------------------------------------------------
  // Addresses
  // --------------------------------------------------------------------------------------
  localparam logic [47:0] HSB_MAC0  = 48'h0293_C648_4D8E;
  localparam logic [47:0] HSB_MAC1  = 48'h0293_C648_4D8F;
  localparam logic [47:0] PS_MAC    = 48'h0293_C648_4D90;
  localparam logic [31:0] HSB_IP0   = 32'hC0A8_0002;
  localparam logic [31:0] HSB_IP1   = 32'hC0A8_0003;
  localparam logic [31:0] PS_IP     = 32'hC0A8_00FE;
  localparam logic [47:0] HOST_MAC  = 48'hA088_C2D4_25E9;
  localparam logic [31:0] HOST_IP   = 32'hC0A8_0001;
  localparam logic [23:0] HOST_QPN  = 24'h00_0123;
  localparam logic [31:0] HOST_RKEY = 32'h0000_ABCD;
  localparam int          SLOTS     = 256;

  // --------------------------------------------------------------------------------------
  // HSB
  // --------------------------------------------------------------------------------------
  logic [47:0] mac_addr [`HOST_IF_INST-1:0];
  assign mac_addr[0] = HSB_MAC0;
  assign mac_addr[1] = HSB_MAC1;

  logic [31:0] apb_prdata [`REG_INST-1:0];
  initial for (int i = 0; i < `REG_INST; i++) apb_prdata[i] = '0;

  logic [`HOST_IF_INST-1:0]   hif_rx_tvalid, hif_rx_tlast, hif_rx_tready;
  logic [`HOST_WIDTH-1:0]     hif_rx_tdata [`HOST_IF_INST-1:0];
  logic [`HOSTKEEP_WIDTH-1:0] hif_rx_tkeep [`HOST_IF_INST-1:0];
  logic [`HOSTUSER_WIDTH-1:0] hif_rx_tuser [`HOST_IF_INST-1:0];
  logic [`HOST_IF_INST-1:0]   hif_tx_tvalid, hif_tx_tlast, hif_tx_tready;
  logic [`HOST_WIDTH-1:0]     hif_tx_tdata [`HOST_IF_INST-1:0];
  logic [`HOSTKEEP_WIDTH-1:0] hif_tx_tkeep [`HOST_IF_INST-1:0];
  logic [`HOSTUSER_WIDTH-1:0] hif_tx_tuser [`HOST_IF_INST-1:0];

  logic [0:0]   sif_rx_tvalid, sif_rx_tlast, sif_rx_tready;
  logic [511:0] sif_rx_tdata [0:0];
  logic [63:0]  sif_rx_tkeep [0:0];
  logic [0:0]   sif_rx_tuser [0:0];
  logic [0:0]   sif_tx_tvalid, sif_tx_tlast, sif_tx_tready;
  logic [511:0] sif_tx_tdata [0:0];
  logic [63:0]  sif_tx_tkeep [0:0];
  logic [0:0]   sif_tx_tuser [0:0];

  logic hsb_init_done;

  assign hif_rx_tuser[0] = '0;
  assign hif_rx_tuser[1] = '0;
  assign hif_tx_tready   = '1;
  assign sif_rx_tuser[0] = '0;

  HOLOLINK_top #(.BUILD_REV(48'h0000_2610_2115)) u_hsb (
    .i_sys_rst         ( sys_rst        ),
    .i_apb_clk         ( apb_clk        ),
    .o_apb_rst         (                ),
    .o_apb_psel        (                ),
    .o_apb_penable     (                ),
    .o_apb_paddr       (                ),
    .o_apb_pwdata      (                ),
    .o_apb_pwrite      (                ),
    .i_apb_pready      ( '1             ),
    .i_apb_prdata      ( apb_prdata     ),
    .i_apb_pserr       ( '0             ),
    .i_mac_addr        ( mac_addr       ),
    .i_board_sn        ( '0             ),
    .i_enum_vld        ( 1'b1           ),
    .o_init_done       ( hsb_init_done  ),
    .i_sif_rx_clk      ( hif_clk        ),
    .o_sif_rx_rst      (                ),
    .i_sif_axis_tvalid ( sif_rx_tvalid  ),
    .i_sif_axis_tlast  ( sif_rx_tlast   ),
    .i_sif_axis_tdata  ( sif_rx_tdata   ),
    .i_sif_axis_tkeep  ( sif_rx_tkeep   ),
    .i_sif_axis_tuser  ( sif_rx_tuser   ),
    .o_sif_axis_tready ( sif_rx_tready  ),
    .i_sif_tx_clk      ( hif_clk        ),
    .o_sif_tx_rst      (                ),
    .o_sif_axis_tvalid ( sif_tx_tvalid  ),
    .o_sif_axis_tlast  ( sif_tx_tlast   ),
    .o_sif_axis_tdata  ( sif_tx_tdata   ),
    .o_sif_axis_tkeep  ( sif_tx_tkeep   ),
    .o_sif_axis_tuser  ( sif_tx_tuser   ),
    .i_sif_axis_tready ( sif_tx_tready  ),
    .i_sif_event       ( 16'd0          ),
    .i_hif_clk         ( hif_clk        ),
    .o_hif_rst         (                ),
    .i_hif_axis_tvalid ( hif_rx_tvalid  ),
    .i_hif_axis_tlast  ( hif_rx_tlast   ),
    .i_hif_axis_tdata  ( hif_rx_tdata   ),
    .i_hif_axis_tkeep  ( hif_rx_tkeep   ),
    .i_hif_axis_tuser  ( hif_rx_tuser   ),
    .o_hif_axis_tready ( hif_rx_tready  ),
    .o_hif_axis_tvalid ( hif_tx_tvalid  ),
    .o_hif_axis_tlast  ( hif_tx_tlast   ),
    .o_hif_axis_tdata  ( hif_tx_tdata   ),
    .o_hif_axis_tkeep  ( hif_tx_tkeep   ),
    .o_hif_axis_tuser  ( hif_tx_tuser   ),
    .i_hif_axis_tready ( hif_tx_tready  ),
    .o_spi_csn         (                ),
    .o_spi_sck         (                ),
    .i_spi_sdio        ( '{4'b0}        ),
    .o_spi_sdio        (                ),
    .o_spi_oen         (                ),
    .i_i2c_scl         ( '1             ),
    .i_i2c_sda         ( '1             ),
    .o_i2c_scl_en      (                ),
    .o_i2c_sda_en      (                ),
    .i_gpio            ( '0             ),
    .o_gpio            (                ),
    .o_gpio_dir        (                ),
    .o_sw_sys_rst      (                ),
    .o_sw_sen_rst      (                ),
    .i_ptp_clk         ( ptp_clk        ),
    .o_ptp_rst         (                ),
    .o_ptp_sec         (                ),
    .o_ptp_nanosec     (                ),
    .o_pps             (                )
  );

  // --------------------------------------------------------------------------------------
  // hwhs
  // --------------------------------------------------------------------------------------
  logic [15:0]  awaddr = '0, araddr = '0;
  logic         awvalid = 1'b0, wvalid = 1'b0, arvalid = 1'b0;
  wire          bready = 1'b1, rready = 1'b1;
  logic [31:0]  wdata = '0;
  wire          awready, wready, bvalid, arready, rvalid;
  wire  [1:0]   bresp, rresp;
  wire  [31:0]  rdata;

  logic         slot_en = 1'b0, reply_en = 1'b0, trace_en = 1'b0;
  logic [63:0]  slot_we = '0;
  logic [31:0]  slot_addr = '0, reply_addr = '0, trace_addr = '0;
  logic [511:0] slot_din = '0;
  wire  [511:0] slot_dout, reply_dout;
  wire  [31:0]  trace_dout;

  logic [511:0] demo_tx_tdata = '0;   // "transport_demo" -> hwhs
  logic         demo_tx_tvalid = 1'b0;
  wire          demo_tx_tready;
  wire  [511:0] demo_rx_tdata;        // hwhs -> "transport_demo"
  wire          demo_rx_tvalid, demo_rx_tlast;
  logic         demo_rx_tready = 1'b0;
  wire          start_demo, reset_demo;
  wire  [31:0]  syn_sum, syn_depth, reply_seq_off, freq_span_mask, lfsr_seed;
  wire  [63:0]  freq_num;

  wire [31:0] hsb_status = {30'd0, sys_rst, hsb_init_done};

  hwhs #(.CLK_HZ(250000000), .BUILD_ID(32'h2610_2115)) u_hwhs (
    .clk               ( hif_clk                ),
    .rstn              ( hwhs_rstn              ),
    .s_axi_awaddr      ( awaddr                 ),
    .s_axi_awvalid     ( awvalid                ),
    .s_axi_awready     ( awready                ),
    .s_axi_wdata       ( wdata                  ),
    .s_axi_wstrb       ( 4'hF                   ),
    .s_axi_wvalid      ( wvalid                 ),
    .s_axi_wready      ( wready                 ),
    .s_axi_bresp       ( bresp                  ),
    .s_axi_bvalid      ( bvalid                 ),
    .s_axi_bready      ( bready                 ),
    .s_axi_araddr      ( araddr                 ),
    .s_axi_arvalid     ( arvalid                ),
    .s_axi_arready     ( arready                ),
    .s_axi_rdata       ( rdata                  ),
    .s_axi_rresp       ( rresp                  ),
    .s_axi_rvalid      ( rvalid                 ),
    .s_axi_rready      ( rready                 ),
    .slot_bram_rst     ( 1'b0                   ),
    .slot_bram_en      ( slot_en                ),
    .slot_bram_we      ( slot_we                ),
    .slot_bram_addr    ( slot_addr              ),
    .slot_bram_wrdata  ( slot_din               ),
    .slot_bram_rddata  ( slot_dout              ),
    .reply_bram_rst    ( 1'b0                   ),
    .reply_bram_en     ( reply_en               ),
    .reply_bram_we     ( 64'd0                  ),
    .reply_bram_addr   ( reply_addr             ),
    .reply_bram_wrdata ( 512'd0                 ),
    .reply_bram_rddata ( reply_dout             ),
    .trace_bram_rst    ( 1'b0                   ),
    .trace_bram_en     ( trace_en               ),
    .trace_bram_we     ( 4'd0                   ),
    .trace_bram_addr   ( trace_addr             ),
    .trace_bram_wrdata ( 32'd0                  ),
    .trace_bram_rddata ( trace_dout             ),
    .s_axis_tdata      ( demo_tx_tdata          ),
    .s_axis_tvalid     ( demo_tx_tvalid         ),
    .s_axis_tready     ( demo_tx_tready         ),
    .s_axis_tlast      ( 1'b1                   ),
    .m_axis_tdata      ( demo_rx_tdata          ),
    .m_axis_tvalid     ( demo_rx_tvalid         ),
    .m_axis_tready     ( demo_rx_tready         ),
    .m_axis_tlast      ( demo_rx_tlast          ),
    .start_demo        ( start_demo             ),
    .reset_demo        ( reset_demo             ),
    .syn_sum           ( syn_sum                ),
    .freq_num          ( freq_num               ),
    .syn_depth         ( syn_depth              ),
    .reply_seq_off     ( reply_seq_off          ),
    .freq_span_mask    ( freq_span_mask         ),
    .lfsr_seed         ( lfsr_seed              ),
    .cmp_error         ( 1'b0                   ),
    .cmp_valid         ( 1'b0                   ),
    .demo_done         ( 1'b0                   ),
    .sif_rx_tdata      ( sif_rx_tdata[0]        ),
    .sif_rx_tkeep      ( sif_rx_tkeep[0]        ),
    .sif_rx_tlast      ( sif_rx_tlast[0]        ),
    .sif_rx_tvalid     ( sif_rx_tvalid[0]       ),
    .sif_rx_tready     ( sif_rx_tready[0]       ),
    .sif_tx_tdata      ( sif_tx_tdata[0]        ),
    .sif_tx_tkeep      ( sif_tx_tkeep[0]        ),
    .sif_tx_tlast      ( sif_tx_tlast[0]        ),
    .sif_tx_tvalid     ( sif_tx_tvalid[0]       ),
    .sif_tx_tready     ( sif_tx_tready[0]       ),
    .ecb_rx_tdata      ( hif_rx_tdata[1]        ),
    .ecb_rx_tkeep      ( hif_rx_tkeep[1]        ),
    .ecb_rx_tlast      ( hif_rx_tlast[1]        ),
    .ecb_rx_tvalid     ( hif_rx_tvalid[1]       ),
    .ecb_rx_tready     ( hif_rx_tready[1]       ),
    .ecb_tx_tdata      ( hif_tx_tdata[1]        ),
    .ecb_tx_tkeep      ( hif_tx_tkeep[1]        ),
    .ecb_tx_tlast      ( hif_tx_tlast[1]        ),
    .ecb_tx_tvalid     ( hif_tx_tvalid[1]       ),
    .ecb_tx_tready     (                        ),
    .hsb_status        ( hsb_status             )
  );

  // --------------------------------------------------------------------------------------
  // A72 side: AXI-Lite and BRAM-controller transactions
  // --------------------------------------------------------------------------------------
  task automatic wr(input logic [15:0] a, input logic [31:0] d);
    bit aw_hs, w_hs;
    @(negedge hif_clk);
    awaddr = a; awvalid = 1'b1; wdata = d; wvalid = 1'b1;
    while (awvalid || wvalid) begin
      aw_hs = awvalid && awready;
      w_hs  = wvalid && wready;
      @(negedge hif_clk);
      if (aw_hs) awvalid = 1'b0;
      if (w_hs) wvalid = 1'b0;
    end
    while (!bvalid) @(negedge hif_clk);
    @(negedge hif_clk);
  endtask

  task automatic rd(input logic [15:0] a, output logic [31:0] d);
    bit ar_hs;
    @(negedge hif_clk);
    araddr = a; arvalid = 1'b1;
    while (arvalid) begin
      ar_hs = arvalid && arready;
      @(negedge hif_clk);
      if (ar_hs) arvalid = 1'b0;
    end
    while (!rvalid) @(negedge hif_clk);
    d = rdata;
    @(negedge hif_clk);
  endtask

  task automatic slot_write(input int idx, input logic [511:0] data);
    @(negedge hif_clk);
    slot_en = 1'b1; slot_we = '1; slot_addr = idx * 64; slot_din = data;
    @(negedge hif_clk);
    slot_en = 1'b0; slot_we = '0;
  endtask

  task automatic trace_read(input int word, output logic [31:0] data);
    @(negedge hif_clk);
    trace_en = 1'b1; trace_addr = word * 4;
    repeat (9) @(negedge hif_clk);         // READ_LATENCY 8
    data = trace_dout;
    trace_en = 1'b0;
  endtask

  task automatic reply_read(input int idx, output logic [511:0] data);
    @(negedge hif_clk);
    reply_en = 1'b1; reply_addr = idx * 64;
    repeat (3) @(negedge hif_clk);         // READ_LATENCY 2
    data = reply_dout;
    reply_en = 1'b0;
  endtask

  // --------------------------------------------------------------------------------------
  // ECB through the mailbox
  // --------------------------------------------------------------------------------------
  logic [15:0] ecb_seq = 16'd1;

  function automatic logic [15:0] ip_csum(input logic [7:0] h [20]);
    logic [31:0] s = 0;
    for (int i = 0; i < 20; i += 2) s += {h[i], h[i + 1]};
    while (s[31:16] != 0) s = s[15:0] + s[31:16];
    return ~s[15:0];
  endfunction

  task automatic ecb(input logic [7:0] cmd, input logic [31:0] addr, input logic [31:0] val,
                     output logic [31:0] rval, output bit ok);
    logic [7:0]  f [256];
    logic [7:0]  ip [20];
    logic [31:0] st, w;
    int          n, ecb_len, udp_len, ip_len, len;
    logic [15:0] seq = ecb_seq;
    bit          is_wr = (cmd == 8'h04);
    ecb_seq = ecb_seq + 1;
    for (int i = 0; i < 256; i++) f[i] = 8'h00;
    ecb_len = is_wr ? 14 : 10;
    udp_len = 8 + ecb_len;
    ip_len  = 20 + udp_len;
    n       = 14 + ip_len;
    for (int i = 0; i < 6; i++) begin
      f[i]     = HSB_MAC1[47 - 8 * i -: 8];
      f[6 + i] = PS_MAC[47 - 8 * i -: 8];
    end
    f[12] = 8'h08; f[13] = 8'h00;
    ip[0] = 8'h45; ip[1] = 0; ip[2] = ip_len[15:8]; ip[3] = ip_len[7:0]; ip[4] = 0; ip[5] = 0;
    ip[6] = 8'h40; ip[7] = 0; ip[8] = 64; ip[9] = 17; ip[10] = 0; ip[11] = 0;
    for (int i = 0; i < 4; i++) begin
      ip[12 + i] = PS_IP[31 - 8 * i -: 8];
      ip[16 + i] = HSB_IP1[31 - 8 * i -: 8];
    end
    {ip[10], ip[11]} = ip_csum(ip);
    for (int i = 0; i < 20; i++) f[14 + i] = ip[i];
    f[34] = 8'h30; f[35] = 8'h39;                       // UDP source 12345
    f[36] = 8'h20; f[37] = 8'h00;                       // UDP destination 8192
    f[38] = udp_len[15:8]; f[39] = udp_len[7:0];
    f[42] = cmd; f[43] = 8'h01; f[44] = seq[15:8]; f[45] = seq[7:0];
    for (int i = 0; i < 4; i++) begin
      f[48 + i] = addr[31 - 8 * i -: 8];
      f[52 + i] = val[31 - 8 * i -: 8];
    end
    if (n < 60) n = 60;

    rd(16'h084, st);
    if (st[1]) wr(16'h080, 32'h2);                      // stale reply: release
    for (int i = 0; i < (n + 3) / 4; i++)
      wr(16'h400 + 16'(4 * i), {f[4 * i + 3], f[4 * i + 2], f[4 * i + 1], f[4 * i]});
    wr(16'h088, 32'(n));
    wr(16'h080, 32'h1);

    ok = 0;
    rval = '0;
    for (int t = 0; t < 2000 && !ok; t++) begin
      rd(16'h084, st);
      if (st[1]) begin
        automatic logic [7:0] r [256];
        len = int'(st[24:16]);
        for (int i = 0; i < (len + 3) / 4; i++) begin
          rd(16'h500 + 16'(4 * i), w);
          {r[4 * i + 3], r[4 * i + 2], r[4 * i + 1], r[4 * i]} = w;
        end
        wr(16'h080, 32'h2);
        if ({r[44], r[45]} == seq) begin
          ok = (r[46] == 8'h00);
          rval = {r[52], r[53], r[54], r[55]};
          if (!ok) $display("  ECB %s 0x%08x: response code 0x%02x", is_wr ? "write" : "read", addr, r[46]);
        end
      end
    end
    if (!ok) $display("  ECB %s 0x%08x: no reply (ECB_STATUS 0x%08x)", is_wr ? "write" : "read", addr, st);
  endtask

  task automatic ecb_wr(input logic [31:0] addr, input logic [31:0] val);
    logic [31:0] dummy;
    bit ok;
    ecb(8'h04, addr, val, dummy, ok);
    check(ok, $sformatf("ECB write 0x%08x", addr));
  endtask

  task automatic ecb_rd(input logic [31:0] addr, output logic [31:0] val);
    bit ok;
    ecb(8'h14, addr, 32'h0, val, ok);
    check(ok, $sformatf("ECB read 0x%08x", addr));
  endtask

  // --------------------------------------------------------------------------------------
  // Coprocessor model on host port 0
  // --------------------------------------------------------------------------------------
  int          reply_delay = 100;   // hif cycles from request to reply
  int          drop_next   = 0;     // requests to drop
  int          late_next   = 0;     // requests to answer after the timeout
  int          n_writes = 0, n_other = 0;
  int unsigned t_tx_first, t_tx_last, t_rx_first;
  logic [7:0]  txf [2048];
  int          txn = 0;
  logic [7:0]  host_ring [SLOTS * 128];

  // queued replies
  logic [127:0] rq_pay [$];
  int unsigned  rq_at  [$];

  function automatic logic [31:0] be32(input int o);
    return {txf[o], txf[o + 1], txf[o + 2], txf[o + 3]};
  endfunction

  always @(negedge hif_clk) begin
    if (hif_tx_tvalid[0] && hif_tx_tready[0]) begin
      if (txn == 0) t_tx_first = cyc;
      for (int k = 0; k < 64; k++)
        if (hif_tx_tkeep[0][k]) begin txf[txn] = hif_tx_tdata[0][8 * k +: 8]; txn++; end
      if (hif_tx_tlast[0]) begin
        t_tx_last = cyc;
        // UC RDMA WRITE_ONLY_WITH_IMM, or WRITE_ONLY with +plain_write
        if ({txf[12], txf[13]} == 16'h0800 && txf[23] == 8'h11 && {txf[36], txf[37]} == 16'd4791 &&
            txf[42] == (plain_write ? 8'h2A : 8'h2B)) begin
          automatic int          po   = plain_write ? 70 : 74;   // payload offset: no ImmDt
          automatic logic [23:0] qpn  = {txf[47], txf[48], txf[49]};
          automatic logic [63:0] va   = {be32(54), be32(58)};
          automatic logic [31:0] rkey = be32(62);
          automatic logic [31:0] dlen = be32(66);
          automatic logic [31:0] imm  = be32(70);
          automatic int          slot = plain_write ? int'(va / 128) : int'(imm[11:0]);
          n_writes++;
          if (qpn != HOST_QPN || rkey != HOST_RKEY || dlen != 64 || va != 64'(slot * 128) ||
              txn != po + 64 + 4) begin
            errors++;
            $display("  FAIL: request frame: qpn 0x%06x rkey 0x%08x len %0d va 0x%0x slot %0d bytes %0d",
                     qpn, rkey, dlen, va, slot, txn);
          end
          for (int i = 0; i < 64; i++) host_ring[slot * 128 + i] = txf[po + i];
          if (drop_next > 0) begin
            drop_next--;
          end else begin
            automatic logic [127:0] pay = '0;
            for (int i = 0; i < 16; i++) pay[8 * i +: 8] = txf[po + i];
            rq_pay.push_back(pay);
            rq_at.push_back(cyc + 32'(late_next > 0 ? 6000 : reply_delay));
            if (late_next > 0) late_next--;
          end
        end else begin
          n_other++;                                    // non-RoCE, e.g. BOOTP
        end
        txn = 0;
      end
    end
  end

  // UC SEND_ONLY (0x24) to QP 2 = HSB sensor-TX port 0
  logic [7:0] rxf [128];
  int         rxn = 0, rxpos = 0;
  logic [23:0] host_psn = 24'd0;

  task automatic build_send(input logic [127:0] pay);
    logic [7:0] ip [20];
    int ip_len = 20 + 8 + 12 + 16 + 4;
    int udp_len = 8 + 12 + 16 + 4;
    for (int i = 0; i < 6; i++) begin
      rxf[i]     = HSB_MAC0[47 - 8 * i -: 8];
      rxf[6 + i] = HOST_MAC[47 - 8 * i -: 8];
    end
    rxf[12] = 8'h08; rxf[13] = 8'h00;
    ip[0] = 8'h45; ip[1] = 0; ip[2] = 8'(ip_len >> 8); ip[3] = 8'(ip_len); ip[4] = 0; ip[5] = 0;
    ip[6] = 8'h40; ip[7] = 0; ip[8] = 64; ip[9] = 17; ip[10] = 0; ip[11] = 0;
    for (int i = 0; i < 4; i++) begin
      ip[12 + i] = HOST_IP[31 - 8 * i -: 8];
      ip[16 + i] = HSB_IP0[31 - 8 * i -: 8];
    end
    {ip[10], ip[11]} = ip_csum(ip);
    for (int i = 0; i < 20; i++) rxf[14 + i] = ip[i];
    rxf[34] = 8'hC0; rxf[35] = 8'h00; rxf[36] = 8'h12; rxf[37] = 8'hB7;
    rxf[38] = 8'(udp_len >> 8); rxf[39] = 8'(udp_len); rxf[40] = 0; rxf[41] = 0;
    rxf[42] = 8'h24; rxf[43] = 0; rxf[44] = 8'hFF; rxf[45] = 8'hFF;    // BTH: UC SEND_ONLY
    rxf[46] = 0; rxf[47] = 0; rxf[48] = 0; rxf[49] = 8'h02;           // dest QP 2
    rxf[50] = 0; rxf[51] = host_psn[23:16]; rxf[52] = host_psn[15:8]; rxf[53] = host_psn[7:0];
    for (int i = 0; i < 16; i++) rxf[54 + i] = pay[8 * i +: 8];
    for (int i = 0; i < 4; i++) rxf[70 + i] = 8'hEE;                  // ICRC, not checked by HSB
    rxn = 74;
    host_psn = host_psn + 1;
  endtask

  initial begin
    hif_rx_tvalid[0] = 1'b0; hif_rx_tlast[0] = 1'b0; hif_rx_tdata[0] = '0; hif_rx_tkeep[0] = '0;
    forever begin
      @(negedge hif_clk);
      if (rxpos == 0) begin
        for (int q = 0; q < rq_at.size(); q++)
          if (cyc >= rq_at[q]) begin        // earliest due reply
            build_send(rq_pay[q]);
            rq_pay.delete(q);
            rq_at.delete(q);
            t_rx_first = cyc;
            rxpos = 1;
            break;
          end
      end
      if (rxpos > 0) begin
        automatic int           b = rxpos - 1;
        automatic logic [511:0] d = '0;
        automatic logic [63:0]  k = '0;
        for (int i = 0; i < 64; i++)
          if (b * 64 + i < rxn) begin d[8 * i +: 8] = rxf[b * 64 + i]; k[i] = 1'b1; end
        hif_rx_tvalid[0] = 1'b1;
        hif_rx_tdata[0]  = d;
        hif_rx_tkeep[0]  = k;
        hif_rx_tlast[0]  = ((b + 1) * 64 >= rxn);
        rxpos = ((b + 1) * 64 >= rxn) ? 0 : rxpos + 1;
      end else begin
        hif_rx_tvalid[0] = 1'b0;
        hif_rx_tlast[0]  = 1'b0;
      end
    end
  end

  // --------------------------------------------------------------------------------------
  // Stage timestamps (hif cycles)
  // --------------------------------------------------------------------------------------
  int unsigned t_sif_rx, t_sif_tx;
  always @(negedge hif_clk) begin
    if (sif_rx_tvalid[0] && sif_rx_tready[0]) t_sif_rx = cyc;
    if (sif_tx_tvalid[0] && sif_tx_tready[0]) t_sif_tx = cyc;
  end

  // --------------------------------------------------------------------------------------
  // Test
  // --------------------------------------------------------------------------------------
  function automatic logic [511:0] request(input int i);
    logic [511:0] q = '0;
    q[63:0]    = 64'h0000_0010_0000_0000 + 64'(i);   // value
    q[95:64]   = 32'h0;                              // decoder_id
    q[127:96]  = 32'h0;                              // seq: stamped by hwhs
    q[159:128] = 32'(i);                             // round_id
    return q;
  endfunction

  task automatic sw_round(input int i, input bit expect_timeout);
    logic [31:0] st, st2, idx, rtt, to0, to1, seq;
    logic [511:0] rep;
    rd(16'h008, idx);
    rd(16'h048, to0);
    rd(16'h004, st);
    slot_write(int'(idx), request(i));
    wr(16'h000, 32'h1);
    do rd(16'h004, st2); while (st2[31:24] == st[31:24]);
    rd(16'h048, to1);
    rd(16'h038, rtt);
    rd(16'h018, seq);
    if (expect_timeout) begin
      check(to1 == to0 + 1, $sformatf("round %0d: expected a timeout", i));
      $display("  round %0d: timeout as expected (seq %0d)", i, seq);
    end else begin
      check(to1 == to0, $sformatf("round %0d: unexpected timeout", i));
      reply_read(int'(idx), rep);
      check(rep[63:0] == request(i)[63:0], $sformatf("round %0d: reply value 0x%016x", i, rep[63:0]));
      check(rep[127:96] == seq, $sformatf("round %0d: reply seq %0d, sent %0d", i, rep[127:96], seq));
      $display("  round %0d: slot %0d seq %0d RTT %0d cycles | HSB TX %0d (first %0d) | coproc %0d | HSB RX %0d",
               i, idx, seq, rtt, t_tx_last - t_sif_rx, t_tx_first - t_sif_rx, reply_delay,
               t_sif_tx - t_rx_first);
    end
  endtask

  initial begin
    logic [31:0] v;
    logic [511:0] beat;
    int unsigned t0;

    repeat (20) @(negedge apb_clk);
    sys_rst = 1'b0;
    hwhs_rstn = 1'b1;
    t0 = cyc;
    while (!hsb_init_done && cyc - t0 < 100000) @(posedge hif_clk);
    check(hsb_init_done, "HSB init_done");
    $display("HSB init_done after %0d hif cycles", cyc - t0);
    repeat (2000) @(posedge hif_clk);    // let BOOTP go out

    rd(16'h07C, v);
    check(v == 32'h2610_2115, "BUILD_ID");
    rd(16'h040, v);
    check(v == 250000000, "CLK_HZ");

    $display("-- ECB through the mailbox (HSB host port 1)");
    ecb_rd(32'h0000_0080, v);
    $display("  HSB_IP_VERSION 0x%08x", v);
    check(v[15:0] == 16'h2609, "HSB IP revision 0x2609");
    check(v[31:16] == 16'h2115, "HSB BUILD_REV[15:0] reported");

    $display("-- program the data plane");
    ecb_rd(32'h0200_030C, v);
    ecb_wr(32'h0200_030C, v & ~32'h1);
    ecb_wr(32'h0200_0304, 32'd8);
    ecb_wr(32'h0200_0308, 32'd12288);
    ecb_wr(32'h0000_1018, 32'd64);
    ecb_wr(32'h0000_1020, HOST_MAC[31:0]);
    ecb_wr(32'h0000_1024, {16'h0, HOST_MAC[47:32]});
    ecb_wr(32'h0000_1028, HOST_IP);
    ecb_wr(32'h0000_102C, 32'd4791 | (32'd1 << 16) | (32'(plain_write) << 17));
    ecb_wr(32'h0000_1034, 32'h0);
    ecb_wr(32'h0000_1000, {8'h0, HOST_QPN});
    ecb_wr(32'h0000_1004, HOST_RKEY);
    ecb_wr(32'h0000_1008, 32'h0);
    ecb_wr(32'h0000_100C, 32'h0);
    ecb_wr(32'h0000_1010, 32'd1);
    ecb_wr(32'h0000_1014, 32'(SLOTS - 1));
    ecb_wr(32'h0120_0004, 32'h0);
    ecb_wr(32'h0120_0008, 32'h0);
    ecb_wr(32'h0120_0000, 32'h1);
    ecb_wr(32'h0120_0000, 32'h5);
    ecb_rd(32'h0200_030C, v);
    ecb_wr(32'h0200_030C, v | 32'h1);
    ecb_rd(32'h0000_1000, v);
    check(v[23:0] == HOST_QPN, "DP_QP read back");
    rd(16'h090, v);
    $display("  mailbox: kept %0d frames, dropped %0d (BOOTP etc.), coprocessor saw %0d non-RoCE frames", v,
             u_hwhs.u_ecb.drop_cnt, n_other);
    check(u_hwhs.u_ecb.drop_cnt >= 1, "the mailbox dropped HSB's port-1 BOOTP");

    $display("-- sw-loop rounds");
    wr(16'h030, 32'd5000);         // REPLY_TO: 20 us
    for (int i = 0; i < 6; i++) sw_round(i, 0);

    $display("-- request lost: timeout, then the next round goes through with a fresh seq");
    drop_next = 1;
    sw_round(6, 1);
    sw_round(7, 0);

    $display("-- late reply: times out, arrives during the next round and is counted stale");
    late_next = 1;
    sw_round(8, 1);
    sw_round(9, 0);
    repeat (7000) @(posedge hif_clk);
    rd(16'h044, v);
    check(v >= 1, $sformatf("STALE_CNT %0d", v));
    $display("  STALE_CNT %0d", v);

    $display("-- hw-loop (driven as transport_demo drives it), RTT trace on");
    wr(16'h0CC, 32'h0);             // TRACE_CTRL: record
    wr(16'h0A8, 32'h1);             // demo_run
    for (int i = 0; i < 4; i++) begin
      automatic logic [31:0] rtt;
      @(negedge hif_clk);
      demo_tx_tdata = request(100 + i);
      demo_tx_tvalid = 1'b1;
      while (!demo_tx_tready) @(negedge hif_clk);
      @(negedge hif_clk);
      demo_tx_tvalid = 1'b0;
      demo_rx_tready = 1'b1;
      while (!demo_rx_tvalid) @(negedge hif_clk);
      beat = demo_rx_tdata;
      @(negedge hif_clk);
      demo_rx_tready = 1'b0;
      check(beat[63:0] == request(100 + i)[63:0], $sformatf("hw-loop %0d: reply value", i));
      repeat (5) @(posedge hif_clk);
      rd(16'h038, rtt);
      $display("  hw-loop %0d: seq %0d RTT %0d cycles", i, beat[127:96], rtt);
    end
    wr(16'h0A8, 32'h2);             // reset_demo: leave demo mode
    rd(16'h004, v);
    check(!v[17], "demo_run cleared");

    rd(16'h034, v);
    $display("ROUND_CNT %0d, coprocessor saw %0d requests", v, n_writes);
    rd(16'h0D0, v);
    check(v == 4, $sformatf("TRACE_CNT %0d, expected 4", v));
    begin
      logic [31:0] t0w, t1w, last_rtt;
      rd(16'h038, last_rtt);
      trace_read(0, t0w);
      trace_read(1, t1w);
      $display("RTT trace: %0d %0d %0d %0d cycles", t0w[15:0], t0w[31:16], t1w[15:0], t1w[31:16]);
      check(t1w[31:16] == last_rtt[15:0], "last trace entry = RTT_LO");
      check(t0w[15:0] != 0 && t0w[31:16] != 0 && t1w[15:0] != 0, "trace entries recorded");
    end

    if (errors == 0) begin
      $display("PASS");
      $finish;
    end else begin
      $fatal(1, "FAILED: %0d check(s)", errors);
    end
  end

  initial begin
    #2000000000;
    $fatal(1, "FAIL: global timeout");
  end
endmodule
