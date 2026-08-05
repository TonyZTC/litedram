// Glue between the host AXI4 master and the generated
// LiteDRAM controller core (USE_DRAM_CTRL builds only; see README.md).
//
// Adaptation handled here:
//  - address: the host issues 64-bit (bus-aligned) addresses; the 32GB core
//    decodes CORE_ADDR_W=35 bits. Upper bits are truncated; requests with any
//    upper bit set are counted on dram_oow_count and warned once.
//  - ID: host IDs (7 bits) are zero-extended to the core's 8-bit port; the
//    core echoes them back, so the top bit of RID/BID is always 0 and the
//    truncation on the response path is lossless.
//  - prot/cache/qos: constant from the host, not modeled by LiteDRAM; dropped.
//  - init: the core's user ports are internally blocked (ready low) until the
//    testbench's init driver completes the wb_ctrl init sequence and
//    dram_init_done rises, so the host just sees backpressure at startup.
//  - the spare "tb" user port and sim_trace are tied off.
//
// The core has no reset input (Migen power-on register initials); reset_l is
// only used by the out-of-window counter.
module dram_ctrl_wrap #(
  parameter int unsigned AXI_ID_WIDTH   = 8,
  parameter int unsigned AXI_DATA_WIDTH = 512,
  parameter int unsigned AXI_ADDR_WIDTH = 64,
  parameter int unsigned CORE_ADDR_W    = 35,
  parameter int unsigned CORE_ID_W      = 8
) (
  input  logic clk,
  input  logic reset_l,

  // Slave side: driven by the host's AXI4 master.
  input  logic [AXI_ID_WIDTH-1:0]         s_axi_arid,
  input  logic [AXI_ADDR_WIDTH-1:0]       s_axi_araddr,
  input  logic [7:0]                      s_axi_arlen,
  input  logic [2:0]                      s_axi_arsize,
  input  logic [1:0]                      s_axi_arburst,
  input  logic                            s_axi_arvalid,
  output logic                            s_axi_arready,
  output logic [AXI_ID_WIDTH-1:0]         s_axi_rid,
  output logic [AXI_DATA_WIDTH-1:0]       s_axi_rdata,
  output logic [1:0]                      s_axi_rresp,
  output logic                            s_axi_rlast,
  output logic                            s_axi_rvalid,
  input  logic                            s_axi_rready,

  input  logic [AXI_ID_WIDTH-1:0]         s_axi_awid,
  input  logic [AXI_ADDR_WIDTH-1:0]       s_axi_awaddr,
  input  logic [7:0]                      s_axi_awlen,
  input  logic [2:0]                      s_axi_awsize,
  input  logic [1:0]                      s_axi_awburst,
  input  logic                            s_axi_awvalid,
  output logic                            s_axi_awready,
  input  logic [AXI_DATA_WIDTH-1:0]       s_axi_wdata,
  input  logic [(AXI_DATA_WIDTH/8)-1:0]   s_axi_wstrb,
  input  logic                            s_axi_wlast,
  input  logic                            s_axi_wvalid,
  output logic                            s_axi_wready,
  output logic [AXI_ID_WIDTH-1:0]         s_axi_bid,
  output logic [1:0]                      s_axi_bresp,
  output logic                            s_axi_bvalid,
  input  logic                            s_axi_bready,

  // Controller status (poked/peeked by the C++ testbench).
  output logic                            dram_init_done,
  output logic                            dram_init_error,
  output logic [31:0]                     dram_oow_count,

  // wb_ctrl CSR bus: the C++ LiteDramInitDrv is the Wishbone master.
  input  logic                            wb_ctrl_cyc,
  input  logic                            wb_ctrl_stb,
  input  logic                            wb_ctrl_we,
  input  logic [29:0]                     wb_ctrl_adr,
  input  logic [31:0]                     wb_ctrl_dat_w,
  input  logic [3:0]                      wb_ctrl_sel,
  output logic                            wb_ctrl_ack,
  output logic                            wb_ctrl_err,
  output logic [31:0]                     wb_ctrl_dat_r
);

  initial begin
    if (AXI_DATA_WIDTH != 512)
      $fatal(1, "dram_ctrl_wrap: the generated litedram core is 512-bit only, got %0d",
             AXI_DATA_WIDTH);
    if (AXI_ID_WIDTH > CORE_ID_W)
      $fatal(1, "dram_ctrl_wrap: host ID width %0d exceeds core ID width %0d",
             AXI_ID_WIDTH, CORE_ID_W);
  end

  logic [CORE_ID_W-1:0] core_rid;
  logic [CORE_ID_W-1:0] core_bid;
  assign s_axi_rid = core_rid[AXI_ID_WIDTH-1:0];
  assign s_axi_bid = core_bid[AXI_ID_WIDTH-1:0];

  // The C++ testbench runs with Verilator's randReset(2), so
  // wb_ctrl_cyc/stb/we/adr/dat_w hold garbage during the reset_l-low window,
  // before the init driver's initialize_outputs() zeroes them. The litedram
  // core itself has no reset input, so an unlucky garbage CSR write during that
  // window (e.g. flipping sdram_dfii_control to software mode) would corrupt
  // the controller permanently. Gate the Wishbone strobes by reset_l -- the
  // same idiom the host uses for arvalid/awvalid/wvalid -- so no transaction
  // can reach the core before the testbench takes over.
  logic wb_ctrl_cyc_gated, wb_ctrl_stb_gated;
  assign wb_ctrl_cyc_gated = wb_ctrl_cyc & reset_l;
  assign wb_ctrl_stb_gated = wb_ctrl_stb & reset_l;

  // Out-of-window observability: addresses whose truncated-away upper bits
  // are non-zero alias into the 32GB window (mod 2^CORE_ADDR_W).
  logic ar_oow, aw_oow;
  assign ar_oow = s_axi_arvalid & s_axi_arready & (|s_axi_araddr[AXI_ADDR_WIDTH-1:CORE_ADDR_W]);
  assign aw_oow = s_axi_awvalid & s_axi_awready & (|s_axi_awaddr[AXI_ADDR_WIDTH-1:CORE_ADDR_W]);
  always_ff @(posedge clk or negedge reset_l) begin
    if (!reset_l) begin
      dram_oow_count <= '0;
    end else if (ar_oow || aw_oow) begin
      if (dram_oow_count == '0)
        $display("[DRAM_CTRL] WARNING: request address above the 32GB window (ar=%b aw=%b) aliases mod 2^%0d",
                 ar_oow, aw_oow, CORE_ADDR_W);
      dram_oow_count <= dram_oow_count + 32'((ar_oow ? 1 : 0) + (aw_oow ? 1 : 0));
    end
  end

  litedram_core u_litedram_core (
    .clk                      (clk),
    .sim_trace                (1'b0),
    .init_done                (dram_init_done),
    .init_error               (dram_init_error),
    .user_clk                 (),
    .user_rst                 (),

    // Host-facing AXI4 user port.
    .user_port_p0_arid    ({{(CORE_ID_W-AXI_ID_WIDTH){1'b0}}, s_axi_arid}),
    .user_port_p0_araddr  (s_axi_araddr[CORE_ADDR_W-1:0]),
    .user_port_p0_arlen   (s_axi_arlen),
    .user_port_p0_arsize  (s_axi_arsize),
    .user_port_p0_arburst (s_axi_arburst),
    .user_port_p0_arvalid (s_axi_arvalid),
    .user_port_p0_arready (s_axi_arready),
    .user_port_p0_rid     (core_rid),
    .user_port_p0_rdata   (s_axi_rdata),
    .user_port_p0_rresp   (s_axi_rresp),
    .user_port_p0_rlast   (s_axi_rlast),
    .user_port_p0_rvalid  (s_axi_rvalid),
    .user_port_p0_rready  (s_axi_rready),
    .user_port_p0_awid    ({{(CORE_ID_W-AXI_ID_WIDTH){1'b0}}, s_axi_awid}),
    .user_port_p0_awaddr  (s_axi_awaddr[CORE_ADDR_W-1:0]),
    .user_port_p0_awlen   (s_axi_awlen),
    .user_port_p0_awsize  (s_axi_awsize),
    .user_port_p0_awburst (s_axi_awburst),
    .user_port_p0_awvalid (s_axi_awvalid),
    .user_port_p0_awready (s_axi_awready),
    .user_port_p0_wdata   (s_axi_wdata),
    .user_port_p0_wstrb   (s_axi_wstrb),
    .user_port_p0_wlast   (s_axi_wlast),
    .user_port_p0_wvalid  (s_axi_wvalid),
    .user_port_p0_wready  (s_axi_wready),
    .user_port_p0_bid     (core_bid),
    .user_port_p0_bresp   (s_axi_bresp),
    .user_port_p0_bvalid  (s_axi_bvalid),
    .user_port_p0_bready  (s_axi_bready),

    // Spare front-door port (used by the standalone smoke test only).
    .user_port_tb_arid        ('0),
    .user_port_tb_araddr      ('0),
    .user_port_tb_arlen       ('0),
    .user_port_tb_arsize      ('0),
    .user_port_tb_arburst     ('0),
    .user_port_tb_arvalid     (1'b0),
    .user_port_tb_arready     (),
    .user_port_tb_rid         (),
    .user_port_tb_rdata       (),
    .user_port_tb_rresp       (),
    .user_port_tb_rlast       (),
    .user_port_tb_rvalid      (),
    .user_port_tb_rready      (1'b0),
    .user_port_tb_awid        ('0),
    .user_port_tb_awaddr      ('0),
    .user_port_tb_awlen       ('0),
    .user_port_tb_awsize      ('0),
    .user_port_tb_awburst     ('0),
    .user_port_tb_awvalid     (1'b0),
    .user_port_tb_awready     (),
    .user_port_tb_wdata       ('0),
    .user_port_tb_wstrb       ('0),
    .user_port_tb_wlast       (1'b0),
    .user_port_tb_wvalid      (1'b0),
    .user_port_tb_wready      (),
    .user_port_tb_bid         (),
    .user_port_tb_bresp       (),
    .user_port_tb_bvalid      (),
    .user_port_tb_bready      (1'b0),

    // CSR bus for the C++ init driver.
    .wb_ctrl_cyc              (wb_ctrl_cyc_gated),
    .wb_ctrl_stb              (wb_ctrl_stb_gated),
    .wb_ctrl_we               (wb_ctrl_we),
    .wb_ctrl_adr              (wb_ctrl_adr),
    .wb_ctrl_dat_w            (wb_ctrl_dat_w),
    .wb_ctrl_sel              (wb_ctrl_sel),
    .wb_ctrl_cti              ('0),
    .wb_ctrl_bte              ('0),
    .wb_ctrl_ack              (wb_ctrl_ack),
    .wb_ctrl_err              (wb_ctrl_err),
    .wb_ctrl_dat_r            (wb_ctrl_dat_r)
  );

endmodule
