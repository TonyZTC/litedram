// DPI-backed per-bank storage for the generated LiteDRAM sim core.
//
// scripts/litedram_gen.py swaps the sim PHY's full-capacity Migen
// memories (2GB per bank at the 32GB config) for instances of this module;
// the actual bytes live in the testbench's sparse DramDpiStore
// (include/dram_dpi_store.hpp), which must be
// compiled into any simulation that links this module.
//
// Port shape mirrors the stock BankModel memory ports: a byte-granular
// synchronous write port and an asynchronous (combinational) read port, both
// already gated by the bank's active/read/write state in the generated code.
// One ADDR_W word = one 512-bit beat = one DRAM BL8 burst.
module dram_dpi_mem #(
  parameter int unsigned BANK   = 0,
  parameter int unsigned DATA_W = 512,
  parameter int unsigned ADDR_W = 25
) (
  input  logic                  clk,
  input  logic [DATA_W/8-1:0]   we,
  input  logic [ADDR_W-1:0]     wadr,
  input  logic [DATA_W-1:0]     wdat,
  input  logic                  rd_en,
  input  logic [ADDR_W-1:0]     radr,
  output logic [DATA_W-1:0]     rdat
);

  import "DPI-C" function void dram_dpi_write(input int     bank,
                                              input longint adr,
                                              input bit [511:0] data,
                                              input longint strb);
  import "DPI-C" function void dram_dpi_read (input int     bank,
                                              input longint adr,
                                              input bit     wr_tgl,
                                              output bit [511:0] data);

  initial begin
    if (DATA_W != 512)
      $fatal(1, "dram_dpi_mem: DATA_W must be 512 (DPI signature), got %0d", DATA_W);
  end

  // Flips on every write to this bank. The DPI read result is opaque to the
  // simulator's dependency tracking, so the toggle is threaded through the
  // read call to force re-evaluation of a combinationally-held read after
  // the underlying store changes.
  logic wr_tgl = 1'b0;

  always_ff @(posedge clk) begin
    if (|we) begin
      dram_dpi_write(int'(BANK), longint'(wadr), 512'(wdat), longint'(we));
      wr_tgl <= ~wr_tgl;
    end
  end

  logic [511:0] rdat_full;
  always_comb begin
    rdat_full = '0;
    if (rd_en) dram_dpi_read(int'(BANK), longint'(radr), wr_tgl, rdat_full);
  end
  assign rdat = rdat_full[DATA_W-1:0];

endmodule
