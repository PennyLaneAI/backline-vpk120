// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// tb_net.sv - the small modules around BALBOA's stack:
//   eth_pad         a 42-byte ARP frame comes out as 60 bytes, zero-padded; longer frames and
//                   multi-beat frames pass unchanged
//   icmp_server_ip  an echo request comes back as an echo reply (addresses swapped, type 0,
//                   ICMP checksum still valid, payload unchanged), with the output stalled at
//                   times; other ICMP types and IP headers with options are dropped
//   axis_interconnect_512_4to1 / _2to1
//                   one cycle from input to output; with every input busy, the inputs take
//                   turns packet by packet with no idle cycle; with gaps on the inputs and
//                   stalls on the output, every packet comes out whole and in order
//
// Build and run with hw/sim/run.sh (Verilator). Exit status 0 = all checks passed.

`timescale 1ps / 1ps

module tb_net;
  logic clk = 1'b0;
  always #2000 clk = ~clk;
  logic rstn = 1'b0;

  int errors = 0;
  task automatic check(input bit cond, input string what);
    if (!cond) begin
      errors++;
      $display("  FAIL: %s", what);
    end
  endtask

  task automatic tick(input int n);
    repeat (n) @(negedge clk);
  endtask

  // ------------------------------------------------------------------------------------------
  // eth_pad
  // ------------------------------------------------------------------------------------------
  logic [511:0] p_sd = '0;
  logic [63:0]  p_sk = '0;
  logic         p_sl = 1'b0, p_sv = 1'b0;
  wire          p_sr;
  wire  [511:0] p_md;
  wire  [63:0]  p_mk;
  wire          p_ml, p_mv;

  eth_pad u_pad (.clk(clk), .rstn(rstn), .s_tdata(p_sd), .s_tkeep(p_sk), .s_tlast(p_sl),
                 .s_tvalid(p_sv), .s_tready(p_sr), .m_tdata(p_md), .m_tkeep(p_mk),
                 .m_tlast(p_ml), .m_tvalid(p_mv), .m_tready(1'b1));

  // one beat in; what comes out in the same cycle
  task automatic pad_beat(input logic [511:0] d, input logic [63:0] k, input logic l,
                          output logic [511:0] od, output logic [63:0] ok);
    @(negedge clk);
    p_sd = d; p_sk = k; p_sl = l; p_sv = 1'b1;
    #1;
    od = p_md;
    ok = p_mk;
    @(negedge clk);
    p_sv = 1'b0;
  endtask

  // ------------------------------------------------------------------------------------------
  // icmp_server_ip
  // ------------------------------------------------------------------------------------------
  logic [63:0] i_sd = '0;
  logic [7:0]  i_sk = '0;
  logic        i_sl = 1'b0, i_sv = 1'b0, i_mr = 1'b1;
  wire         i_sr, i_mv, i_ml;
  wire  [63:0] i_md;
  wire  [7:0]  i_mk;

  icmp_server_ip u_icmp (
    .ap_clk(clk), .ap_rst_n(rstn),
    .s_axis_TVALID(i_sv), .s_axis_TREADY(i_sr), .s_axis_TDATA(i_sd), .s_axis_TKEEP(i_sk), .s_axis_TLAST(i_sl),
    .udpIn_TVALID(1'b0), .udpIn_TREADY(), .udpIn_TDATA('0), .udpIn_TKEEP('0), .udpIn_TLAST(1'b0),
    .ttlIn_TVALID(1'b0), .ttlIn_TREADY(), .ttlIn_TDATA('0), .ttlIn_TKEEP('0), .ttlIn_TLAST(1'b0),
    .m_axis_TVALID(i_mv), .m_axis_TREADY(i_mr), .m_axis_TDATA(i_md), .m_axis_TKEEP(i_mk), .m_axis_TLAST(i_ml));

  logic [7:0] out_bytes [$];
  int         out_pkts = 0;
  always @(negedge clk) begin
    if (i_mv && i_mr) begin
      for (int b = 0; b < 8; b++) if (i_mk[b]) out_bytes.push_back(i_md[b*8 +: 8]);
      if (i_ml) out_pkts++;
    end
  end

  int stall_every = 0;           // when > 0, m_axis_TREADY drops every stall_every cycles
  int unsigned cyc = 0;
  always @(negedge clk) begin
    cyc++;
    i_mr = (stall_every == 0) || (cyc % stall_every != 0);
  end

  task automatic icmp_send(input logic [7:0] pkt [$]);
    for (int i = 0; i < pkt.size(); i += 8) begin
      @(negedge clk);
      i_sd = '0;
      i_sk = '0;
      for (int b = 0; b < 8 && i + b < pkt.size(); b++) begin
        i_sd[b*8 +: 8] = pkt[i+b];
        i_sk[b] = 1'b1;
      end
      i_sl = (i + 8 >= pkt.size());
      i_sv = 1'b1;
      #1;
      while (!i_sr) begin
        @(negedge clk);
        #1;
      end
    end
    @(negedge clk);
    i_sv = 1'b0;
  endtask

  function automatic logic [15:0] csum(input logic [7:0] b [$], input int from, input int n);
    logic [31:0] s = 0;
    for (int i = 0; i < n; i += 2)
      s += {b[from+i], (i + 1 < n) ? b[from+i+1] : 8'd0};
    while (s[31:16] != 0) s = s[15:0] + s[31:16];
    return ~s[15:0];
  endfunction

  // IPv4 + ICMP: ihl in words, ICMP type, payload bytes
  function automatic void ip_icmp(ref logic [7:0] p [$], input int ihl, input logic [7:0] typ,
                                  input int payload);
    logic [15:0] c;
    int          n = ihl * 4 + 8 + payload;
    p.delete();
    p.push_back(8'h40 | 8'(ihl)); p.push_back(0); p.push_back(8'(n >> 8)); p.push_back(8'(n));
    p.push_back(8'h12); p.push_back(8'h34); p.push_back(8'h40); p.push_back(0);
    p.push_back(64); p.push_back(1); p.push_back(0); p.push_back(0);        // TTL, ICMP, checksum
    p.push_back(192); p.push_back(168); p.push_back(0); p.push_back(1);     // source
    p.push_back(192); p.push_back(168); p.push_back(0); p.push_back(2);     // destination
    for (int i = 20; i < ihl * 4; i++) p.push_back(8'h01);                  // options (NOPs)
    c = csum(p, 0, ihl * 4);
    p[10] = c[15:8]; p[11] = c[7:0];
    p.push_back(typ); p.push_back(0); p.push_back(0); p.push_back(0);       // type, code, checksum
    p.push_back(8'hBE); p.push_back(8'hEF); p.push_back(0); p.push_back(7); // id, seq
    for (int i = 0; i < payload; i++) p.push_back(8'(i * 3 + 1));
    c = csum(p, ihl * 4, 8 + payload);
    p[ihl*4+2] = c[15:8]; p[ihl*4+3] = c[7:0];
  endfunction

  // ------------------------------------------------------------------------------------------
  // axis_interconnect_512_4to1 and _2to1
  // ------------------------------------------------------------------------------------------
  // A beat carries {len, b, p, in}: input, packet number, beat number, packet length.
  logic [3:0]        x_sv = '0, x_sl = '0;
  logic [3:0][511:0] x_sd = '0;
  wire  [3:0]        x_sr;
  logic              x_mr = 1'b1;
  wire               x_mv, x_ml;
  wire  [511:0]      x_md;
  wire  [63:0]       x_mk;

  axis_interconnect_512_4to1 u_m4 (
    .ACLK(clk), .ARESETN(rstn),
    .S00_AXIS_ACLK(clk), .S01_AXIS_ACLK(clk), .S02_AXIS_ACLK(clk), .S03_AXIS_ACLK(clk),
    .S00_AXIS_ARESETN(rstn), .S01_AXIS_ARESETN(rstn), .S02_AXIS_ARESETN(rstn), .S03_AXIS_ARESETN(rstn),
    .S00_AXIS_TVALID(x_sv[0]), .S00_AXIS_TREADY(x_sr[0]), .S00_AXIS_TDATA(x_sd[0]), .S00_AXIS_TKEEP('1), .S00_AXIS_TLAST(x_sl[0]),
    .S01_AXIS_TVALID(x_sv[1]), .S01_AXIS_TREADY(x_sr[1]), .S01_AXIS_TDATA(x_sd[1]), .S01_AXIS_TKEEP('1), .S01_AXIS_TLAST(x_sl[1]),
    .S02_AXIS_TVALID(x_sv[2]), .S02_AXIS_TREADY(x_sr[2]), .S02_AXIS_TDATA(x_sd[2]), .S02_AXIS_TKEEP('1), .S02_AXIS_TLAST(x_sl[2]),
    .S03_AXIS_TVALID(x_sv[3]), .S03_AXIS_TREADY(x_sr[3]), .S03_AXIS_TDATA(x_sd[3]), .S03_AXIS_TKEEP('1), .S03_AXIS_TLAST(x_sl[3]),
    .M00_AXIS_ACLK(clk), .M00_AXIS_ARESETN(rstn),
    .M00_AXIS_TVALID(x_mv), .M00_AXIS_TREADY(x_mr), .M00_AXIS_TDATA(x_md), .M00_AXIS_TKEEP(x_mk), .M00_AXIS_TLAST(x_ml),
    .S00_ARB_REQ_SUPPRESS(1'b0), .S01_ARB_REQ_SUPPRESS(1'b0), .S02_ARB_REQ_SUPPRESS(1'b0), .S03_ARB_REQ_SUPPRESS(1'b0));

  logic [1:0]   y_sv = '0, y_sl = '0;
  logic [511:0] y_sd [2] = '{default: '0};
  wire  [1:0]   y_sr;
  wire          y_mv, y_ml;
  wire  [511:0] y_md;

  axis_interconnect_512_2to1 u_m2 (
    .ACLK(clk), .ARESETN(rstn),
    .S00_AXIS_ACLK(clk), .S01_AXIS_ACLK(clk), .S00_AXIS_ARESETN(rstn), .S01_AXIS_ARESETN(rstn),
    .S00_AXIS_TVALID(y_sv[0]), .S00_AXIS_TREADY(y_sr[0]), .S00_AXIS_TDATA(y_sd[0]), .S00_AXIS_TKEEP('1), .S00_AXIS_TLAST(y_sl[0]),
    .S01_AXIS_TVALID(y_sv[1]), .S01_AXIS_TREADY(y_sr[1]), .S01_AXIS_TDATA(y_sd[1]), .S01_AXIS_TKEEP('1), .S01_AXIS_TLAST(y_sl[1]),
    .M00_AXIS_ACLK(clk), .M00_AXIS_ARESETN(rstn),
    .M00_AXIS_TVALID(y_mv), .M00_AXIS_TREADY(1'b1), .M00_AXIS_TDATA(y_md), .M00_AXIS_TKEEP(), .M00_AXIS_TLAST(y_ml),
    .S00_ARB_REQ_SUPPRESS(1'b0), .S01_ARB_REQ_SUPPRESS(1'b0));

  function automatic logic [511:0] x_beat(input int in, input int p, input int b, input int len);
    return {448'd0, 16'(len), 16'(b), 16'(p), 16'(in)};
  endfunction

  // one packet of `len` beats from input `in`; with `gap`, tvalid drops for a cycle before
  // some beats
  task automatic x_packet(input int in, input int p, input int len, input bit gap);
    for (int b = 0; b < len; b++) begin
      if (gap && (p * 7 + b * 3 + in) % 5 == 0) begin
        @(negedge clk);
        x_sv[in] = 1'b0;
      end
      @(negedge clk);
      x_sd[in] = x_beat(in, p, b, len);
      x_sl[in] = (b == len - 1);
      x_sv[in] = 1'b1;
      #1;
      while (!x_sr[in]) begin
        @(negedge clk);
        #1;
      end
    end
  endtask

  task automatic x_stream(input int in, input int first, input int n, input bit gap);
    for (int p = first; p < first + n; p++) x_packet(in, p, 1 + (p + in) % 4, gap);
    @(negedge clk);
    x_sv[in] = 1'b0;
  endtask

  // What comes out of the 4to1: each packet whole, beats in order, packets of an input in order
  int          x_cur = -1, x_prev_b = 0, x_beats = 0, x_first_cyc = -1, x_last_cyc = 0;
  int          x_next_p [4] = '{default: 0};
  int          x_order [$];
  int unsigned x_cyc = 0;
  always @(posedge clk) begin
    x_cyc++;
    if (x_mv && x_mr) begin
      automatic int in  = int'(x_md[15:0]);
      automatic int p   = int'(x_md[31:16]);
      automatic int b   = int'(x_md[47:32]);
      automatic int len = int'(x_md[63:48]);
      x_beats++;
      if (x_first_cyc < 0) x_first_cyc = int'(x_cyc);
      x_last_cyc = int'(x_cyc);
      if (x_cur < 0) begin
        check(b == 0, $sformatf("input %0d packet %0d starts at beat %0d", in, p, b));
        check(p == x_next_p[in], $sformatf("input %0d: packet %0d, expected %0d", in, p, x_next_p[in]));
        x_next_p[in] = p + 1;
        x_order.push_back(in);
        x_cur = in;
      end else begin
        check(in == x_cur && b == x_prev_b + 1,
              $sformatf("beat %0d of input %0d inside a packet of input %0d", b, in, x_cur));
      end
      x_prev_b = b;
      if (x_ml) begin
        check(b == len - 1, $sformatf("input %0d packet %0d ends at beat %0d of %0d", in, p, b, len));
        x_cur = -1;
      end
    end
  end

  // ------------------------------------------------------------------------------------------
  // Test
  // ------------------------------------------------------------------------------------------
  initial begin
    logic [511:0] od, d;
    logic [63:0]  ok;
    logic [7:0]   req [$], rep [$];

    tick(3);
    rstn = 1'b1;
    tick(3);

    $display("-- eth_pad");
    d = {512{1'b1}};                                  // bytes past the frame are not zero
    pad_beat(d, 64'h0000_03FF_FFFF_FFFF, 1'b1, od, ok);   // 42 bytes
    check(ok == 64'h0FFF_FFFF_FFFF_FFFF, $sformatf("42-byte frame: keep 0x%0h", ok));
    check(od[42*8-1:0] == d[42*8-1:0] && od[60*8-1:42*8] == '0, "frame kept, padding zero");
    pad_beat(d, 64'h0FFF_FFFF_FFFF_FFFF, 1'b1, od, ok);   // 60 bytes
    check(ok == 64'h0FFF_FFFF_FFFF_FFFF && od == d, "60-byte frame unchanged");
    pad_beat(d, 64'hFFFF_FFFF_FFFF_FFFF, 1'b0, od, ok);   // first beat of a longer frame
    check(ok == '1 && od == d, "first beat unchanged");
    pad_beat(d, 64'h0000_0000_0000_000F, 1'b1, od, ok);   // its 4-byte last beat
    check(ok == 64'h0000_0000_0000_000F && od == d, "a short last beat is not padded");

    $display("-- icmp_server_ip: echo request, output stalled at times");
    stall_every = 3;
    ip_icmp(req, 5, 8'd8, 56);                        // ping's default 84-byte packet
    icmp_send(req);
    tick(20);
    check(out_pkts == 1 && out_bytes.size() == req.size(), $sformatf("one reply of %0d bytes (%0d)",
                                                                     req.size(), out_bytes.size()));
    rep = out_bytes;
    if (rep.size() == req.size()) begin
      bit same = 1'b1;
      check(rep[12] == 192 && rep[13] == 168 && rep[14] == 0 && rep[15] == 2 &&
            rep[16] == 192 && rep[17] == 168 && rep[18] == 0 && rep[19] == 1, "addresses swapped");
      check(csum(rep, 0, 20) == 16'h0, "IP checksum still valid");
      check(rep[20] == 8'd0 && rep[21] == 8'd0, "type 0, code 0");
      check(csum(rep, 20, rep.size() - 20) == 16'h0, "ICMP checksum valid");
      for (int i = 24; i < rep.size(); i++) if (rep[i] != req[i]) same = 1'b0;
      check(same, "id, seq and payload unchanged");
    end

    $display("-- icmp_server_ip: what it drops");
    out_bytes.delete();
    ip_icmp(req, 5, 8'd0, 8);                         // an echo reply
    icmp_send(req);
    ip_icmp(req, 6, 8'd8, 8);                         // IP options
    icmp_send(req);
    ip_icmp(req, 5, 8'd8, 0);                         // the smallest echo request: 28 bytes
    icmp_send(req);
    tick(20);
    check(out_pkts == 2 && out_bytes.size() == 28, $sformatf("only the last one answered (%0d bytes)",
                                                             out_bytes.size()));
    if (out_bytes.size() == 28) check(csum(out_bytes, 20, 8) == 16'h0, "its ICMP checksum is valid");

    $display("-- axis_interconnect_512_4to1: one cycle from input to output");
    @(negedge clk);
    x_sd[2] = x_beat(2, 0, 0, 1);
    x_sl[2] = 1'b1;
    x_sv[2] = 1'b1;
    #1;
    check(x_sr[2], "a lone input is taken at once");
    @(negedge clk);
    x_sv[2] = 1'b0;
    check(x_mv && x_ml && x_md == x_beat(2, 0, 0, 1), "and is at the output one cycle later");
    tick(2);

    $display("-- axis_interconnect_512_4to1: every input busy");
    x_order.delete();
    x_beats = 0;
    x_first_cyc = -1;
    fork
      x_stream(0, 0, 12, 1'b0);
      x_stream(1, 0, 12, 1'b0);
      x_stream(2, 1, 12, 1'b0);
      x_stream(3, 0, 12, 1'b0);
    join
    tick(4);
    check(x_order.size() == 48, $sformatf("48 packets (%0d)", x_order.size()));
    for (int i = 4; i < x_order.size(); i++)
      check(x_order[i] == x_order[i-4] && x_order[i] != x_order[i-1],
            $sformatf("packet %0d from input %0d: the inputs take turns", i, x_order[i]));
    check(x_last_cyc - x_first_cyc + 1 == x_beats,
          $sformatf("%0d beats in %0d cycles", x_beats, x_last_cyc - x_first_cyc + 1));

    $display("-- axis_interconnect_512_4to1: gaps on the inputs, output stalled at times");
    x_order.delete();
    fork
      x_stream(0, 12, 20, 1'b1);
      x_stream(1, 12, 20, 1'b1);
      x_stream(2, 13, 20, 1'b1);
      x_stream(3, 12, 20, 1'b1);
      begin
        for (int c = 0; c < 400; c++) begin
          @(negedge clk);
          x_mr = (c % 3 != 2);
        end
        x_mr = 1'b1;
      end
    join
    tick(4);
    check(x_order.size() == 80, $sformatf("80 packets (%0d)", x_order.size()));
    check(x_next_p[0] == 32 && x_next_p[1] == 32 && x_next_p[2] == 33 && x_next_p[3] == 32,
          "every packet of every input");

    $display("-- axis_interconnect_512_2to1");
    @(negedge clk);
    y_sd[0] = 512'hA0; y_sl[0] = 1'b0; y_sv[0] = 1'b1;
    y_sd[1] = 512'hB0; y_sl[1] = 1'b1; y_sv[1] = 1'b1;
    #1;
    check(y_sr == 2'b01, "input 0 goes first after reset");
    @(negedge clk);
    y_sd[0] = 512'hA1; y_sl[0] = 1'b1;
    #1;
    check(y_mv && y_md == 512'hA0 && y_sr == 2'b01, "its packet holds the output");
    @(negedge clk);
    y_sv[0] = 1'b0;
    #1;
    check(y_mv && y_md == 512'hA1 && y_ml && y_sr == 2'b10, "then input 1");
    @(negedge clk);
    y_sv[1] = 1'b0;
    check(y_mv && y_md == 512'hB0 && y_ml, "input 1's beat right after");
    tick(2);

    if (errors == 0) begin
      $display("PASS");
      $finish;
    end else begin
      $fatal(1, "FAILED: %0d check(s)", errors);
    end
  end

  initial begin
    repeat (100000) @(posedge clk);
    $fatal(1, "FAIL: global timeout");
  end
endmodule
