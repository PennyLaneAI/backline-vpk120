/**
 * This file is part of the Coyote <https://github.com/fpgasystems/Coyote>
 *
 * MIT Licence
 * Copyright (c) 2021-2025, Systems Group, ETH Zurich
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// backline-roce: Coyote's lynxTypes package, rendered for this build.
//
// Rendered from third_party/balboa/coyote/hw/templates/common/lynx_pkg_tmplt.txt (Coyote
// 15e5f59, MIT, above) the way Coyote's CMake renders it, with Coyote's defaults except:
//   en_rdma = 1, en_stats = 1, vitis_hls = 1, every other en_* = 0 (no TCP, sniffer, memory,
//   host streams); fdev = "vpk120", fpga_arch = "versal"; n_reg = 1; no card memory.
// Edits after rendering:
//   - DEF_MAC_ADDRESS and DEF_IP_ADDRESS are the board's own, so the stack answers ARP right
//     after a reset (HSB host port 0's address: a coprocessor set up for the HSB build needs no
//     change), and DEF_MAC_ADDRESS is 48 bits wide instead of a 32-bit integer;
//   - the testbench timing constants (CLK_PERIOD ... OUTPUT_TIMING, 1step) are dropped: only
//     Coyote's own simulation environment uses them, and Verilator does not parse them.
//
// The defines below select what the vendored network_stack.sv and roce_stack.sv build, so this
// file is compiled first.

`timescale 1ns / 1ps

`define EN_RDMA
`define EN_STATS
`define VITIS_HLS
	
package lynxTypes;

    // ========================---------------------------------------------
    // Util functions
    // ========================---------------------------------------------
    function integer clog2s;
    input [31:0] v;
    reg [31:0] value;
    begin
        value = v;
        if (value == 1) begin
            clog2s = 1;
        end
        else begin
            value = value-1;
            for (clog2s=0; value>0; clog2s=clog2s+1)
                value = value>>1;
        end
    end
    endfunction

    // ========================---------------------------------------------
    // Static
    // ========================---------------------------------------------

    localparam string BUILD_DIR = "";

    // QDMA configuration (Versal only)
    localparam integer QDMA_N_QUEUES = 512;
    localparam integer QDMA_PR_QUEUE_IDX = 0;
    localparam integer QDMA_RD_QUEUE_START_IDX = 1;
    localparam integer QDMA_WR_QUEUE_START_IDX = QDMA_N_QUEUES / 2;
    localparam integer PCIE_GT_BITS = 16;

    // AXI
    localparam integer AXIL_DATA_BITS = 64;
    localparam integer AVX_DATA_BITS = 256;
    localparam integer AXI_DATA_BITS = 512;
    localparam integer AXI_ADDR_BITS = 64;
    localparam integer AXI_NET_BITS = 512;
    localparam integer AXI_DDR_BITS = 512;
    localparam integer AXI_TLB_BITS = 128;
    localparam integer AXI_ID_BITS = 6;
    localparam integer AXI_BPSS_BAR_BITS = 28;

    // TLB ram
    localparam integer TLB_S_ORDER = 10;
    localparam integer PG_S_BITS = 12;
    localparam integer N_S_ASSOC = 4;

    localparam integer TLB_L_ORDER = 9;
    localparam integer PG_L_BITS = 21;
    localparam integer N_L_ASSOC = 2;

    localparam integer TLB_DATA_BITS = 104;
    localparam integer N_TLB_ACTV = 16;

    localparam integer TLB_TMR_REF_CLR = 100000;
    localparam integer CACHE_MAX_SIZE = 32;

    // IRQ
    localparam integer IRQ_OFFL = 0;
    localparam integer IRQ_SYNC = 1;
    localparam integer IRQ_INVLDT = 2;
    localparam integer IRQ_PFAULT = 3;
    localparam integer IRQ_NOTIFY = 4;
    localparam integer IRQ_RCNFG = 5;

    // Data
    localparam integer ADDR_BITS = 64;
    localparam integer PADDR_BITS = 44;
    localparam integer VADDR_BITS = 48;
    localparam integer LEN_BITS = 28;
    localparam integer DEST_BITS = 4;
    localparam integer PID_BITS = 6;
    localparam integer HPID_BITS = 32;
    localparam integer USER_BITS = 4;
    localparam integer NOTIFY_BITS = 32;
    localparam integer BEAT_LOG_BITS = $clog2(AXI_DATA_BITS/8);
    localparam integer BLEN_BITS = LEN_BITS - BEAT_LOG_BITS;
    localparam integer OFFS_BITS = 6;
    localparam integer OPCODE_BITS = 5;
    localparam integer STRM_BITS = 2;

    localparam integer LOCAL_READ = 1;
    localparam integer LOCAL_WRITE = 2;
    localparam integer LOCAL_TRANSFER = 3;
    localparam integer LOCAL_OFFLOAD = 4;
    localparam integer LOCAL_SYNC = 5;

    localparam integer STRM_CARD = 0;
    localparam integer STRM_HOST = 1;
    localparam integer STRM_RDMA = 2;
    localparam integer STRM_TCP = 3;

    // Queue depth
    localparam integer QUEUE_DEPTH = 8;

    // Slices
    localparam integer N_REG_STAT_S0 = 2;
    localparam integer N_REG_STAT_S1 = 2;
    localparam integer N_REG_SHELL_S0 = 3;
    localparam integer N_REG_SHELL_S1 = 2;
    localparam integer N_REG_DYN_HOST_S0 = 3;
    localparam integer N_REG_DYN_HOST_S1 = 3;
    localparam integer N_REG_DYN_CARD_S0 = 3;
    localparam integer N_REG_DYN_CARD_S1 = 3;
    localparam integer N_REG_DYN_NET_S0 = 3;
    localparam integer N_REG_DYN_NET_S1 = 3;
    localparam integer N_REG_NET_S0 = 6;
    localparam integer N_REG_NET_S1 = 4;
    localparam integer N_REG_NET_S2 = 5;
    localparam integer N_REG_STA_DCPL = 3;
    localparam integer N_REG_DYN_DCPL = 3;
    localparam integer N_REG_PR = 4;
    localparam integer NET_STATS_DELAY = 4;
    localparam integer HDMA_STATS_DELAY = 4;

    // LEGACY Enzian support
    localparam integer N_REG_ECI_S0 = 3; // 3
    localparam integer N_REG_ECI_S1 = 2; // 2

    // Network
    localparam integer ARP_LUP_REQ_BITS = 32;
    localparam integer ARP_LUP_RSP_BITS = 56;
    localparam integer IP_ADDR_BITS = 32;
    localparam integer MAC_ADDR_BITS = 48;
    localparam logic [47:0] DEF_MAC_ADDRESS = 48'h8E4D48C69302; // LSB first, 02:93:C6:48:4D:8E
    localparam integer DEF_IP_ADDRESS = 32'h0200A8C0; // LSB first, 192.168.0.2
    localparam integer NET_STRM_DOWN_THRS = 256;

    // Network buffers
    localparam integer MEM_CMD_BITS = 96;
    localparam integer MEM_STS_BITS = 8;

    // Network RDMA
    localparam integer APP_READ = 0;
    localparam integer APP_WRITE = 1;
    localparam integer APP_SEND = 2;
    localparam integer APP_IMMED = 3;

    localparam integer RC_SEND_FIRST = 5'h0;
    localparam integer RC_SEND_MIDDLE = 5'h1;
    localparam integer RC_SEND_LAST = 5'h2;
    localparam integer RC_SEND_ONLY = 5'h4;
    localparam integer RC_RDMA_WRITE_FIRST = 5'h6;
    localparam integer RC_RDMA_WRITE_MIDDLE = 5'h7;
    localparam integer RC_RDMA_WRITE_LAST = 5'h8;
    localparam integer RC_RDMA_WRITE_LAST_WITH_IMD = 5'h9;
    localparam integer RC_RDMA_WRITE_ONLY = 5'hA;
    localparam integer RC_RDMA_WRITE_ONLY_WIT_IMD = 5'hB;
    localparam integer RC_RDMA_READ_REQUEST = 5'hC;
    localparam integer RC_RDMA_READ_RESP_FIRST = 5'hD;
    localparam integer RC_RDMA_READ_RESP_MIDDLE = 5'hE;
    localparam integer RC_RDMA_READ_RESP_LAST = 5'hF;
    localparam integer RC_RDMA_READ_RESP_ONLY = 5'h10;
    localparam integer RC_ACK = 5'h11;

    localparam integer RDMA_ACK_BITS = 64;
    localparam integer RDMA_ACK_QPN_BITS = 10;
    localparam integer RDMA_ACK_SYNDROME_BITS = 8;
    localparam integer RDMA_ACK_PSN_BITS = 24;
    localparam integer RDMA_ACK_MSN_BITS = 24;
    localparam integer RDMA_BASE_REQ_BITS = 144;
    localparam integer RDMA_VADDR_BITS = 64;
    localparam integer RDMA_LEN_BITS = 32;
    localparam integer RDMA_REQ_BITS = 248;
    localparam integer RDMA_OPCODE_BITS = 5;
    localparam integer RDMA_QPN_BITS = 16;
    localparam integer RDMA_IMM_BITS = 32;
    localparam integer RDMA_QP_INTF_BITS = 168;
    localparam integer RDMA_QP_CONN_BITS = 184;
    localparam integer RDMA_LVADDR_OFFS = 0;
    localparam integer RDMA_RVADDR_OFFS = RDMA_VADDR_BITS;
    localparam integer RDMA_LEN_OFFS = 2*RDMA_VADDR_BITS;
    localparam integer RDMA_PARAMS_OFFS = 2*RDMA_VADDR_BITS + RDMA_LEN_BITS;
    localparam integer RDMA_MSN_BITS = 24;
    localparam integer RDMA_SNDRM_BITS = 8;
    localparam integer RDMA_N_RD_OUTSTANDING = 8;
    localparam integer RDMA_N_WR_OUTSTANDING = 16;
    localparam integer RDMA_MAX_SINGLE_READ = 32 * 1024;
    localparam integer RDMA_MODE_PARSE = 0;
    localparam integer RDMA_MODE_RAW = 1;
    localparam integer RDMA_WR_NET_THRS = 256; // beats
    localparam integer RDMA_MEM_SHIFT = 27;

    // Network TCP/IP
    localparam integer N_TCP_CHANNELS = 2;
    localparam integer TCP_PORT_OFFS = 49152;
    localparam integer TCP_PORT_ORDER = 10;
    localparam integer TCP_RSESSION_BITS = DEST_BITS + PID_BITS + DEST_BITS;
    localparam integer TCP_PORT_TABLE_DATA_BITS = 16;
    localparam integer TCP_PORT_REQ_BITS = 16;
    localparam integer TCP_PORT_RSP_BITS = 8;
    localparam integer TCP_OPEN_CONN_REQ_BITS = 48;
    localparam integer TCP_OPEN_CONN_RSP_BITS = 72;
    localparam integer TCP_CLOSE_CONN_REQ_BITS = 16;
    localparam integer TCP_NOTIFY_BITS = 88;
    localparam integer TCP_RD_PKG_REQ_BITS = 32;
    localparam integer TCP_RX_META_BITS = 16;
    localparam integer TCP_TX_META_BITS = 32;
    localparam integer TCP_TX_STAT_BITS = 64;

    localparam integer TCP_IP_ADDRESS_BITS = 32;
    localparam integer TCP_IP_PORT_BITS = 16;
    localparam integer TCP_SESSION_BITS = 16;
    localparam integer TCP_SUCCESS_BITS = 8;
    localparam integer TCP_LEN_BITS = 16;
    localparam integer TCP_REM_SPACE_BITS = 30;
    localparam integer TCP_ERROR_BITS = 2;
    localparam integer TCP_MEM_SHIFT = 0;
    localparam integer TCP_SID_BITS = 10;
    localparam integer TCP_OPCODE = 0;

    // Legacy Enzian support - ECI
    localparam integer N_LANES = 12;
    localparam integer N_LANES_GRPS = 3;
    localparam integer ECI_DATA_BITS = 1024;
    localparam integer ECI_ADDR_BITS = 40;
    localparam integer ECI_ID_BITS = 5;
    localparam integer ECI_WORD_BITS = 64;

    // ========================---------------------------------------------
    // Dynamic
    // ========================---------------------------------------------

    // SIM

    // Flow
    localparam string  FDEV = "vpk120";
    localparam string  FPGA_ARCH = "versal";

    localparam integer N_HCHAN = 1;
    localparam integer N_HCHAN_BITS = clog2s(N_HCHAN);
    localparam integer N_SCHAN = 1;
    localparam integer N_SCHAN_BITS = clog2s(1);
    localparam integer N_CHAN = 1;
    localparam integer N_CHAN_BITS = clog2s(1);
    
    localparam integer STAT_PROBE = 0;
    localparam integer SHELL_PROBE = 1044942;
    
    localparam integer N_REGIONS = 1;
    localparam integer N_REGIONS_BITS = clog2s(1);

    localparam integer N_OUTSTANDING = 8;
    localparam integer N_OUTSTANDING_REGION = 32;

    localparam integer PMTU_BYTES = 4096;
    
    localparam longint MEM_OFFSET = 64'd0;
    localparam integer DDR_SIZE = 0;
    localparam integer HBM_SIZE = 0;
    localparam integer MC_SIZE = 0;

    localparam integer N_DDR_CHAN = 0;
    localparam integer N_DDR_CHAN_BITS = clog2s(0);
    localparam integer N_MEM_CHAN = 0;
    localparam integer N_MEM_CHAN_BITS = clog2s(0);
    localparam integer N_STRIPE_CHAN = 0;
    localparam integer N_STRIPE_CHAN_BITS = clog2s(0);
    localparam integer STRIPE_FRAG_SIZE = 1024;
    localparam integer PR_FLOW = 0;
    localparam integer SHELL_PBLOCK_EN = 0;
    localparam integer RECONFIG_EOS_TIME = 1000000;

    localparam integer AVX_FLOW = 0;
    localparam integer BPSS_FLOW = 1;
    localparam integer WB_FLOW = 0;
    localparam integer STRM_FLOW = 0;
    localparam integer MEM_FLOW = 0;
    localparam integer RDMA_FLOW = 1;
    localparam integer TCP_FLOW = 0;
    localparam integer N_WBS = 1;
    localparam integer QSFP = 0;

    localparam integer N_CARD_AXI = 1;
    localparam integer N_STRM_AXI = 1;
    localparam integer N_RDMA_AXI = 1;

    localparam integer BLOCK_MEM_IMPL_EN = 0;

    // ========================---------------------------------------------
    // Structs
    // ========================---------------------------------------------

    // 
    // REQ
    //
    
    typedef struct packed {
        // Opcode
        logic [OPCODE_BITS-1:0] opcode; // One of the values of coyote::CoyoteOper
        logic [STRM_BITS-1:0] strm;     // One of STRM_CARD, STRM_HOST, STRM_TCP, or STRM_RDMA (this determines the where this request lands)
        logic mode;                     // In the STRM_RDMA case, controls whether to skip the request splitter in the dreq_rdma_parser_wr module
        logic rdma;
        logic remote;

        // ID
        logic [DEST_BITS-1:0] vfid; // rsrvd
        logic [PID_BITS-1:0] pid;
        logic [DEST_BITS-1:0] dest; // The index of the AXI stream that data arrives at/departs from

        // FLAGS
        logic last; // Only if last is high, the corresponding AXI stream will have a last signal at the end. Otherwise, the stream will end without a last signal, the checkCompleted counter will not be incremented and no acknowledgement will be sent on the corresponding cq_* interface

        // DESC
        logic [VADDR_BITS-1:0] vaddr;
        logic [LEN_BITS-1:0] len;

        // RSRVD
        logic actv; // rsrvd
        logic host; // rsrvd
        logic [OFFS_BITS-1:0] offs; // rsrvd

        logic [128-OFFS_BITS-2-VADDR_BITS-LEN_BITS-1-2*DEST_BITS-PID_BITS-3-STRM_BITS-OPCODE_BITS-1:0] rsrvd;
    } req_t;

    typedef struct packed {
        req_t req_1; // rd, local
        req_t req_2; // wr, remote
    } dreq_t;

    typedef struct packed {
        logic [OPCODE_BITS-1:0] opcode; // One of the values of coyote::CoyoteOper
        logic [STRM_BITS-1:0] strm;     // One of STRM_CARD, STRM_HOST, STRM_TCP, or STRM_RDMA (this determines the where this request lands)
        logic remote;
        logic host;                     // Whether the corresponding request came from the corresponding vFPGA sq_* interface or a host-side invoke
        logic [DEST_BITS-1:0] dest;     // The index of the AXI stream that data arrives at/departs from
        logic [PID_BITS-1:0] pid;
        logic [DEST_BITS-1:0] vfid;
        logic [32-OPCODE_BITS-STRM_BITS-2-DEST_BITS-PID_BITS-DEST_BITS-1:0] rsrvd;
    } ack_t;

    typedef struct packed {
        ack_t ack;
        logic last;
    } dack_t;

    typedef struct packed {
        logic [DEST_BITS-1:0] dest;
        logic [BLEN_BITS-1:0] len;
        logic [PID_BITS-1:0] pid;
    } mux_user_t;

    typedef struct packed {
        logic [N_REGIONS_BITS-1:0] vfid;
        logic [BLEN_BITS-1:0] len;
        logic last;
    } mux_host_t;

    typedef struct packed {
        logic [N_CHAN_BITS-1:0] chan;
        logic [BLEN_BITS-1:0] len;
        logic last;
    } mux_shell_t;

    // 
    // IRQ
    //

    typedef struct packed {
        logic success;
    } pf_t;

    typedef struct packed {
        logic lock;
        logic last;
        logic [HPID_BITS-1:0] hpid;
        logic [VADDR_BITS-1:0] vaddr;
        logic [LEN_BITS-1:0] len;
    } inv_t;

    typedef struct packed {
        logic [VADDR_BITS-1:0] vaddr;
        logic [LEN_BITS-1:0] len;
        logic [STRM_BITS-1:0] strm;
        logic [PID_BITS-1:0] pid;
    } irq_pft_t;

    typedef struct packed {
        logic [HPID_BITS-1:0] hpid;
    } irq_inv_t;

    typedef struct packed {
        logic [PID_BITS-1:0] pid;
        logic [31:0] value;
    } irq_not_t;

    //
    // WB
    //

    typedef struct packed {
        logic [PADDR_BITS-1:0] paddr;
        logic [32-1:0] value;
        logic [96-32-PADDR_BITS-1:0] rsrvd;
    } wback_t;

    //
    // DMA
    //

    typedef struct packed {
        // Req
        logic [PADDR_BITS-1:0] paddr;
        logic [LEN_BITS-1:0] len;
        logic last;

        logic [96-PADDR_BITS-LEN_BITS-1-1:0] rsrvd;
    } dma_req_t;

    typedef struct packed {
        logic done;
    } dma_rsp_t;

    typedef struct packed {
        // Req
        logic [PADDR_BITS-1:0] paddr_host;
        logic [PADDR_BITS-1:0] paddr_card;
        logic [LEN_BITS-1:0] len;
        logic last;

        // Completion
        logic [128-1-LEN_BITS-2*PADDR_BITS-1:0] rsrvd;
    } dma_isr_req_t;

    typedef struct packed {
        logic done;
    } dma_isr_rsp_t;

    //
    // QDMA
    //

    // QDMA H2C Data Stream
    typedef struct packed {
        logic [AXI_DATA_BITS-1:0] 	tdata;      
        logic [31:0] 				tcrc;       
        logic [11:0] 				qid;        
        logic [2:0]					port_id;    
        logic						err;        
        logic [31:0]			 	mdata;      
        logic [5:0]			 		mty;        
        logic 						zero_byte;  
        logic 						rsrvd;      
    } qdma_h2c_data_t;                          

    // QDMA H2C Command Stream
    typedef struct packed {
        logic [63:0] 	addr;       
        logic [15:0] 	cidx;       
        logic  			eop;        
        logic  			error;      
        logic [11:0] 	func;       
        logic [15:0] 	len;        
        logic  			mrkr_req;   
        logic  			no_dma;     
        logic [2:0] 	port_id;    
        logic [11:0] 	qid;        
        logic  			sdi;        
        logic  			sop;        
        logic [6:0]     rsrvd;      
    } qdma_h2c_cmd_t;              

    // QDMA C2H Data Stream
    typedef struct packed {
        logic [AXI_DATA_BITS-1:0] 	tdata;      
        logic [31:0] 				tcrc;       
        logic [15:0]				len;        
        logic [11:0] 				qid;        
        logic						has_cmpt;   
        logic						marker;     
        logic [2:0]					port_id;    
        logic [6:0]					ecc;        
        logic [5:0]			 		mty;        
        logic [9:0]					rsrvd;      
    } qdma_c2h_data_t;                          

    // QDMA C2H Command Stream
    typedef struct packed {
        logic [63:0] 	addr;       
        logic [11:0] 	qid;        
        logic  			error;      
        logic [11:0] 	func;       
        logic [2:0] 	port_id;    
        logic [6:0]		pfch_tag;   
        logic [4:0]     rsvrd;      
    } qdma_c2h_cmd_t;         

    // QDMA H2C Memory Mapped Bypass Descriptor
    typedef struct packed {
        logic [63:0] 	raddr;    
        logic [63:0] 	waddr;       
        logic [15:0] 	cidx;       
        logic  			error;      
        logic [11:0] 	func;       
        logic [15:0] 	len;        
        logic  			mrkr_req;   
        logic  			no_dma;     
        logic [2:0] 	port_id;    
        logic [11:0] 	qid;        
        logic  			sdi;        
    } qdma_h2c_mm_desc;              

    //
    // RDMA
    //

    typedef struct packed {
        logic [47:0] vaddr;
        logic [31:0] r_key;
        logic [23:0] local_psn;
        logic [23:0] remote_psn;
        logic [23:0] qp_num;
        logic [31:0] new_state;
    } rdma_qp_ctx_t;

    typedef struct packed {
        logic [15:0] remote_udp_port;
        logic [127:0] remote_ip_address;
        logic [23:0] remote_qpn;
        logic [15:0] local_qpn;
    } rdma_qp_conn_t;

    //
    // TCP
    //

    typedef struct packed {
        logic [TCP_IP_PORT_BITS-1:0] ip_port;
    } tcp_listen_req_t;

    typedef struct packed {
        logic [DEST_BITS-1:0] dest;
        logic [PID_BITS-1:0] pid;
        logic [DEST_BITS-1:0] vfid;
        logic [TCP_IP_PORT_BITS-1:0] ip_port;
    } tcp_listen_req_r_t;

    typedef struct packed {
        logic [TCP_SUCCESS_BITS-1:0] open_port_success;
    } tcp_listen_rsp_t;

    typedef struct packed {
        logic [DEST_BITS-1:0] vfid;
        logic [TCP_SUCCESS_BITS-1:0] open_port_success;
    } tcp_listen_rsp_r_t;

    typedef struct packed {
        logic [TCP_IP_PORT_BITS-1:0] ip_port;
        logic [TCP_IP_ADDRESS_BITS-1:0] ip_address;
    } tcp_open_req_t;

    typedef struct packed {
        logic rsrvd;
        logic close;
        logic [DEST_BITS-1:0] dest;
        logic [PID_BITS-1:0] pid;
        logic [DEST_BITS-1:0] vfid;
        logic [TCP_IP_PORT_BITS-1:0] ip_port; 
        logic [TCP_IP_ADDRESS_BITS-1:0] ip_address;
    } tcp_open_req_r_t;

    typedef struct packed {
        logic [TCP_IP_PORT_BITS-1:0] ip_port;
        logic [TCP_IP_ADDRESS_BITS-1:0] ip_address;
        logic [TCP_SUCCESS_BITS-1:0] success;
        logic [TCP_SESSION_BITS-1:0] sid;
    } tcp_open_rsp_t;

    typedef struct packed {
        logic [PID_BITS-1:0] pid; //
        logic [DEST_BITS-1:0] vfid; //
        logic [TCP_SUCCESS_BITS-1:0] success; // 
    } tcp_open_rsp_r_t;

    typedef struct packed {
        logic [TCP_SESSION_BITS-1:0] sid;
    } tcp_close_req_t;

    typedef struct packed {
        logic [5:0] rsrvd;
        logic opened;
        logic closed;
        logic [TCP_IP_PORT_BITS-1:0] dst_port;
        logic [TCP_IP_ADDRESS_BITS-1:0] ip_address;
        logic [TCP_LEN_BITS-1:0] len;
        logic [TCP_SESSION_BITS-1:0] sid;
    } tcp_notify_t;

    typedef struct packed {
        logic [TCP_LEN_BITS-1:0] len;
        logic [TCP_SESSION_BITS-1:0] sid;
    } tcp_rd_pkg_t; 

    typedef struct packed {
        logic [TCP_LEN_BITS-1:0] len;
        logic [DEST_BITS-1:0] dest;
        logic [PID_BITS-1:0] pid;
        logic [DEST_BITS-1:0] vfid;
    } tcp_meta_r_t;

    typedef struct packed {
        logic [TCP_LEN_BITS-1:0] len;
        logic [TCP_SESSION_BITS-1:0] sid;
    } tcp_meta_t;

    typedef struct packed {
        logic [TCP_LEN_BITS-1:0] len;
        logic [TCP_SESSION_BITS-1:0] sid;
    } tcp_rx_meta_t;

    typedef struct packed {
        logic [TCP_LEN_BITS-1:0] len;
        logic [TCP_SESSION_BITS-1:0] sid;
    } tcp_tx_meta_t;

    typedef struct packed {
        logic [TCP_ERROR_BITS-1:0] error;
        logic [TCP_REM_SPACE_BITS-1:0] remaining_space;
        logic [TCP_LEN_BITS-1:0] len;
        logic [TCP_SESSION_BITS-1:0] sid;
    } tcp_tx_stat_t;

    typedef struct packed {
        logic [31:0] bpss_h2c_req_counter;
        logic [31:0] bpss_c2h_req_counter;
        logic [31:0] bpss_h2c_cmpl_counter;
        logic [31:0] bpss_c2h_cmpl_counter;
        logic [31:0] bpss_h2c_axis_counter;
        logic [31:0] bpss_c2h_axis_counter;
    } hdma_stat_t;

    typedef struct packed {
        logic [31:0] rx_pkg_counter;
        logic [31:0] tx_pkg_counter;
        logic [31:0] arp_rx_pkg_counter;
        logic [31:0] arp_tx_pkg_counter;
        logic [31:0] icmp_rx_pkg_counter;
        logic [31:0] icmp_tx_pkg_counter;
        logic [31:0] tcp_rx_pkg_counter;
        logic [31:0] tcp_tx_pkg_counter;
        logic [31:0] roce_rx_pkg_counter;
        logic [31:0] roce_tx_pkg_counter;
        logic [31:0] ibv_rx_pkg_counter;
        logic [31:0] ibv_tx_pkg_counter;
        logic [31:0] roce_psn_drop_counter;
        logic [31:0] roce_retrans_counter;
        logic [15:0] tcp_session_counter;
        logic axis_stream_down;
        logic [512 - (14*32+16+1) - 1:0] rsrvd; 
    } net_stat_t;

    // -----------------------------------------------------------------
    // Additional functions
    // -----------------------------------------------------------------
    function logic is_strm_local;
    input [STRM_BITS-1:0] strm;
    begin
        if (strm == STRM_HOST || strm == STRM_CARD) begin
            is_strm_local = 1'b1;
        end
        else begin
            is_strm_local = 1'b0;
        end
    end
    endfunction
    
    function logic is_opcode_rd_req;
    input [OPCODE_BITS-1:0] opcode;
    begin
        if (opcode == RC_RDMA_READ_REQUEST) begin
            is_opcode_rd_req = 1'b1;
        end
        else begin
            is_opcode_rd_req = 1'b0;
        end
    end
    endfunction

    function logic is_opcode_ack;
    input [OPCODE_BITS-1:0] opcode;
    begin
        if (opcode == RC_ACK) begin
            is_opcode_ack = 1'b1;
        end
        else begin
            is_opcode_ack = 1'b0;
        end
    end
    endfunction

    function logic is_opcode_rd_resp;
    input [OPCODE_BITS-1:0] opcode;
    begin
        if (opcode == RC_RDMA_READ_RESP_FIRST ||
            opcode == RC_RDMA_READ_RESP_MIDDLE ||
            opcode == RC_RDMA_READ_RESP_LAST ||
            opcode == RC_RDMA_READ_RESP_ONLY) begin
            is_opcode_rd_resp = 1'b1;
        end
        else begin
            is_opcode_rd_resp = 1'b0;
        end
    end
    endfunction

    function logic is_opcode_wr;
    input [OPCODE_BITS-1:0] opcode;
    begin
        if (opcode == RC_RDMA_WRITE_FIRST ||
            opcode == RC_RDMA_WRITE_MIDDLE ||
            opcode == RC_RDMA_WRITE_LAST ||
            opcode == RC_RDMA_WRITE_ONLY) begin
            is_opcode_wr = 1'b1;
        end
        else begin
            is_opcode_wr = 1'b0;
        end
    end
    endfunction

    function logic is_opcode_send;
    input [OPCODE_BITS-1:0] opcode;
    begin
        if (opcode == RC_SEND_FIRST ||
            opcode == RC_SEND_MIDDLE ||
            opcode == RC_SEND_LAST ||
            opcode == RC_SEND_ONLY) begin
            is_opcode_send = 1'b1;
        end
        else begin
            is_opcode_send = 1'b0;
        end
    end
    endfunction

endpackage
