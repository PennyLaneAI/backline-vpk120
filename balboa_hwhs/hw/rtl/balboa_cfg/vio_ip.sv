// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// vio_ip.sv - takes the place of the Vivado VIO core that network_stack.sv instantiates to
// show the stack's IP and MAC in the lab. The board reads those from hwhs_regs instead, and a
// debug core would need a debug hub on the Versal NoC, so this module is empty.

`timescale 1ns / 1ps

module vio_ip (
  input logic        clk,
  input logic [31:0] probe_in0,   // IP address
  input logic [47:0] probe_in1    // MAC address
);
endmodule
