// Standalone smoke test for the generated LiteDRAM DPI core (USE_DRAM_CTRL).
//
// Verilates litedram_core.v + dram_dpi_mem.sv alone (no host design) and
// checks, front-door through the spare "tb" AXI port:
//   1. user ports are blocked until the single-Wishbone-write init
//      (ddrctrl_init_done <- 1) completes and init_done rises;
//   2. an AXI write lands in the sparse DramDpiStore at exactly the AXI byte
//      address (validates the ROW_BANK_COL inversion in dram_dpi_store.hpp);
//   3. an AXI read returns store contents / 0xBAADDEADBEEFDEAD for
//      unpopulated dwords;
//   4. partial-strobe writes and 2-beat bursts (the host master's shapes)
//      round-trip correctly.
// Run via `make` in this directory; exits 0 on success.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <verilated.h>

#include "Vlitedram_core.h"
// Relative to this file (dram_ctrl/smoke/): the DPI store lives in the sibling
// include/ dir. Resolved by the preprocessor against this .cpp's location, so
// no -I / absolute path is needed.
#include "../include/dram_dpi_store.hpp"

namespace {

Vlitedram_core* dut = nullptr;
uint64_t cycles = 0;
int failures = 0;

// clk is left at 0 between cycles; inputs are set, settled with eval(), and
// outputs sampled pre-edge, so a valid/ready pair seen high together here
// handshakes at the next rising edge.
void settle() { dut->eval(); }

void tick() {
  dut->clk = 1;
  dut->eval();
  dut->clk = 0;
  dut->eval();
  cycles++;
}

void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("[SMOKE] FAIL: %s (cycle %" PRIu64 ")\n", what, cycles);
    failures++;
  } else {
    std::printf("[SMOKE] pass: %s\n", what);
  }
}

bool wb_write32(uint32_t byte_address, uint32_t data, int timeout = 1000) {
  dut->wb_ctrl_adr = byte_address >> 2;
  dut->wb_ctrl_dat_w = data;
  dut->wb_ctrl_sel = 0xf;
  dut->wb_ctrl_we = 1;
  dut->wb_ctrl_cyc = 1;
  dut->wb_ctrl_stb = 1;
  while (timeout-- > 0) {
    settle();
    const bool ack = dut->wb_ctrl_ack;
    tick();
    if (ack) {
      dut->wb_ctrl_cyc = 0;
      dut->wb_ctrl_stb = 0;
      dut->wb_ctrl_we = 0;
      tick();
      return true;
    }
  }
  return false;
}

// One 512-bit beat = 64 bytes; beats[i] points at 64 bytes.
bool axi_write(uint64_t address, unsigned num_beats, const uint8_t* beats,
               const uint64_t* strobes, int timeout = 20000) {
  dut->user_port_tb_awvalid = 1;
  dut->user_port_tb_awaddr = address;
  dut->user_port_tb_awlen = num_beats - 1;
  dut->user_port_tb_awsize = 6;
  dut->user_port_tb_awburst = 1;  // INCR
  dut->user_port_tb_awid = 5;
  for (;;) {
    settle();
    const bool fire = dut->user_port_tb_awready;
    tick();
    if (fire) break;
    if (timeout-- <= 0) return false;
  }
  dut->user_port_tb_awvalid = 0;

  for (unsigned beat = 0; beat < num_beats; beat++) {
    dut->user_port_tb_wvalid = 1;
    dut->user_port_tb_wlast = (beat == num_beats - 1);
    dut->user_port_tb_wstrb = strobes[beat];
    for (unsigned chunk = 0; chunk < 16; chunk++) {
      uint32_t value = 0;
      std::memcpy(&value, beats + beat * 64 + chunk * 4, 4);
      dut->user_port_tb_wdata[chunk] = value;
    }
    for (;;) {
      settle();
      const bool fire = dut->user_port_tb_wready;
      tick();
      if (fire) break;
      if (timeout-- <= 0) return false;
    }
  }
  dut->user_port_tb_wvalid = 0;
  dut->user_port_tb_wlast = 0;

  dut->user_port_tb_bready = 1;
  for (;;) {
    settle();
    const bool fire = dut->user_port_tb_bvalid;
    const uint32_t bid = dut->user_port_tb_bid;
    const uint32_t bresp = dut->user_port_tb_bresp;
    tick();
    if (fire) {
      dut->user_port_tb_bready = 0;
      check(bid == 5, "BID echoes AWID");
      check(bresp == 0, "BRESP is OKAY");
      return true;
    }
    if (timeout-- <= 0) return false;
  }
}

bool axi_read(uint64_t address, unsigned num_beats, uint8_t* beats,
              int timeout = 20000) {
  dut->user_port_tb_arvalid = 1;
  dut->user_port_tb_araddr = address;
  dut->user_port_tb_arlen = num_beats - 1;
  dut->user_port_tb_arsize = 6;
  dut->user_port_tb_arburst = 1;  // INCR
  dut->user_port_tb_arid = 9;
  for (;;) {
    settle();
    const bool fire = dut->user_port_tb_arready;
    tick();
    if (fire) break;
    if (timeout-- <= 0) return false;
  }
  dut->user_port_tb_arvalid = 0;

  dut->user_port_tb_rready = 1;
  for (unsigned beat = 0; beat < num_beats;) {
    settle();
    const bool fire = dut->user_port_tb_rvalid;
    const bool last = dut->user_port_tb_rlast;
    const uint32_t rid = dut->user_port_tb_rid;
    const uint32_t rresp = dut->user_port_tb_rresp;
    uint32_t data[16];
    for (unsigned chunk = 0; chunk < 16; chunk++) {
      data[chunk] = dut->user_port_tb_rdata[chunk];
    }
    tick();
    if (fire) {
      std::memcpy(beats + beat * 64, data, 64);
      check(rid == 9, "RID echoes ARID");
      check(rresp == 0, "RRESP is OKAY");
      check(last == (beat == num_beats - 1), "RLAST on final beat only");
      beat++;
    }
    if (timeout-- <= 0) return false;
  }
  dut->user_port_tb_rready = 0;
  return true;
}

// Issue `count` single-beat writes back-to-back WITHOUT waiting for B between
// them (distinct AWIDs 0..count-1), keeping bready high to collect B responses
// concurrently, then drain remaining B's. Returns how many B responses arrived
// and fills id_seen[]. This replicates a pipelined sub-beat write burst
// (the host tracks many outstanding writes by ID and only advances the requester
// when each write's B-driven continuation returns).
// Counts TOTAL B handshakes (not distinct IDs — the tb port is only 4-bit ID,
// so IDs wrap mod 16; a correct controller still returns one B per write).
// stride_bytes=64 -> distinct cachelines; 0 -> hammer the SAME cacheline with
// rotating 16-byte quarter strobes (a same-cacheline sub-word pattern). Returns B count.
unsigned axi_write_pipelined(uint64_t base, unsigned count, uint64_t strobe,
                             uint64_t stride_bytes = 64, bool rotate_quarter = false,
                             int timeout = 400000) {
  static const uint64_t quarters[4] = {
      0x000000000000ffffULL, 0x00000000ffff0000ULL,
      0x0000ffff00000000ULL, 0xffff000000000000ULL};
  unsigned issued = 0, bcount = 0;
  dut->user_port_tb_bready = 1;
  auto collect_b = [&]() {
    if (dut->user_port_tb_bvalid) bcount++;  // bready held high => 1 B/cycle
  };
  while (issued < count) {
    dut->user_port_tb_awvalid = 1;
    dut->user_port_tb_awaddr = base + issued * stride_bytes;
    dut->user_port_tb_awlen = 0;
    dut->user_port_tb_awsize = 6;
    dut->user_port_tb_awburst = 1;
    dut->user_port_tb_awid = issued & 0xF;  // 4-bit port
    for (;;) {
      settle();
      const bool awfire = dut->user_port_tb_awready;
      collect_b();
      tick();
      if (awfire) break;
      if (timeout-- <= 0) { dut->user_port_tb_bready = 0; return bcount; }
    }
    dut->user_port_tb_awvalid = 0;
    dut->user_port_tb_wvalid = 1;
    dut->user_port_tb_wlast = 1;
    dut->user_port_tb_wstrb = rotate_quarter ? quarters[issued & 3] : strobe;
    for (unsigned chunk = 0; chunk < 16; chunk++)
      dut->user_port_tb_wdata[chunk] = 0x1000 + issued * 16 + chunk;
    for (;;) {
      settle();
      const bool wfire = dut->user_port_tb_wready;
      collect_b();
      tick();
      if (wfire) break;
      if (timeout-- <= 0) { dut->user_port_tb_bready = 0; return bcount; }
    }
    dut->user_port_tb_wvalid = 0;
    dut->user_port_tb_wlast = 0;
    issued++;
  }
  // Drain outstanding B responses.
  int idle = 0;
  while (bcount < count && idle < 5000 && timeout-- > 0) {
    settle();
    const bool b = dut->user_port_tb_bvalid;
    collect_b();
    tick();
    idle = b ? 0 : idle + 1;
  }
  dut->user_port_tb_bready = 0;
  return bcount;
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vlitedram_core;
  dut->clk = 0;
  dut->sim_trace = 0;
  dut->eval();

  auto& store = DramDpiStore::instance();

  for (int i = 0; i < 10; i++) tick();

  // 1) Ports blocked before init.
  dut->user_port_tb_awvalid = 1;
  dut->user_port_tb_arvalid = 1;
  settle();
  check(dut->user_port_tb_awready == 0 && dut->user_port_tb_arready == 0,
        "AXI port blocked before init_done");
  dut->user_port_tb_awvalid = 0;
  dut->user_port_tb_arvalid = 0;

  // 2) Init = a single Wishbone write (DFII resets to hardware control):
  //    ddrctrl_init_done CSR @ byte 0x0.
  const uint64_t init_start = cycles;
  check(wb_write32(0x0, 1), "wb_ctrl write to ddrctrl_init_done acked");
  tick();
  tick();  // user_enable is registered
  settle();
  check(dut->init_done == 1, "init_done output high");
  check(dut->init_error == 0, "init_error output low");
  std::printf("[SMOKE] init complete in %" PRIu64 " cycles\n",
              cycles - init_start);

  // 3) Backdoor preload -> front-door AXI read (validates the ROW_BANK_COL
  //    inversion in the read direction). 0x8000_0000 is a representative DRAM-window test base.
  const uint64_t addr_a = 0x80000000ULL;
  for (unsigned i = 0; i < 64; i++) {
    store.write_byte(addr_a + i, static_cast<uint8_t>(i * 3 + 1));
  }
  uint8_t rbuf[128];
  const uint64_t read_start = cycles;
  check(axi_read(addr_a, 1, rbuf), "single-beat AXI read completes");
  std::printf("[SMOKE] single read latency %" PRIu64 " cycles\n",
              cycles - read_start);
  bool match = true;
  for (unsigned i = 0; i < 64; i++) {
    match &= rbuf[i] == static_cast<uint8_t>(i * 3 + 1);
  }
  check(match, "AXI read returns backdoor-preloaded bytes");

  // 4) Front-door AXI write -> store contents at the same byte address
  //    (validates the inversion in the write direction).
  const uint64_t addr_b = 0x100000040ULL;  // >4GB to exercise high row bits
  uint8_t wbuf[128];
  for (unsigned i = 0; i < 64; i++) wbuf[i] = static_cast<uint8_t>(0xC0 - i);
  uint64_t strb_all = ~0ULL;
  check(axi_write(addr_b, 1, wbuf, &strb_all), "single-beat AXI write completes");
  match = true;
  for (unsigned dword = 0; dword < 8; dword++) {
    uint64_t expect = 0;
    std::memcpy(&expect, wbuf + dword * 8, 8);
    match &= store.read_word(addr_b + dword * 8) == expect;
  }
  check(match, "AXI write lands at the AXI byte address in the store");

  // 5) Partial strobes: only dword 0 written; dword 1 stays unpopulated.
  const uint64_t addr_c = 0x40000080ULL;
  uint64_t strb_dword0 = 0xffULL;
  check(axi_write(addr_c, 1, wbuf, &strb_dword0), "partial-strobe write completes");
  uint64_t expect0 = 0;
  std::memcpy(&expect0, wbuf, 8);
  check(store.read_word(addr_c) == expect0, "strobed dword updated");
  check(store.read_word(addr_c + 8) == 0xBAADDEADBEEFDEADULL,
        "unstrobed dword stays unpopulated");

  // 6) Unpopulated read returns the magic pattern through AXI.
  check(axi_read(0x200000000ULL, 1, rbuf), "read of unpopulated address completes");
  match = true;
  for (unsigned dword = 0; dword < 8; dword++) {
    uint64_t value = 0;
    std::memcpy(&value, rbuf + dword * 8, 8);
    match &= value == 0xBAADDEADBEEFDEADULL;
  }
  check(match, "uninitialized AXI read returns 0xBAADDEADBEEFDEAD");

  // 7) 2-beat INCR burst (the widest shape the host master issues).
  const uint64_t addr_e = 0x80200000ULL;
  for (unsigned i = 0; i < 128; i++) wbuf[i] = static_cast<uint8_t>(i ^ 0x5A);
  uint64_t strb2[2] = {~0ULL, ~0ULL};
  check(axi_write(addr_e, 2, wbuf, strb2), "2-beat AXI write completes");
  check(axi_read(addr_e, 2, rbuf), "2-beat AXI read completes");
  check(std::memcmp(rbuf, wbuf, 128) == 0, "2-beat burst round-trips");

  // 8) Read-after-write to the same address (exercises the wr_tgl re-eval).
  for (unsigned i = 0; i < 64; i++) wbuf[i] = static_cast<uint8_t>(i + 7);
  check(axi_write(addr_e, 1, wbuf, &strb_all), "overwrite completes");
  check(axi_read(addr_e, 1, rbuf), "read-back completes");
  check(std::memcmp(rbuf, wbuf, 64) == 0, "read-after-write sees new data");

  // 9) flat-preload -> AXI read across row>0 / multi-bank addresses. This is
  //    the exact preload scenario (load_memory_from_binary writes flat, the
  //    kernel reads via the controller). Test 3 only covered offset 0 (row 0,
  //    bank 0), where the ROW_BANK_COL bank-insertion is a no-op; these hit
  //    nonzero rows and several banks. 0x80003600 is the exact address whose
  //    controller-mode read was observed to diverge under heavy traffic.
  const uint64_t sweep_addrs[] = {
      0x80003600ULL,   // a diverging address seen under heavy traffic (row 16384, bank 1)
      0x80200000ULL,   // 2MB offset, high row
      0x80016600ULL,   // a result region base
      0x100002000ULL,  // >4GB, bank-boundary
      0x1FFFFFFC0ULL,  // near 8GB, high row + high bank
      0x400000000ULL,  // 16GB
      0x7FFFFFFC0ULL,  // top of the 32GB window
  };
  for (uint64_t a : sweep_addrs) {
    const uint64_t base = a & ~0x3FULL;  // 64B-aligned beat
    for (unsigned i = 0; i < 64; i++) {
      store.write_byte(base + i, static_cast<uint8_t>((base >> 6) + i * 7 + 3));
    }
    uint8_t sbuf[64];
    char lbl[96];
    std::snprintf(lbl, sizeof(lbl),
                  "flat-preload -> AXI read matches @ 0x%llx (row>0/multi-bank)",
                  (unsigned long long)base);
    if (axi_read(base, 1, sbuf)) {
      bool ok = true;
      for (unsigned i = 0; i < 64; i++) {
        ok &= sbuf[i] == static_cast<uint8_t>((base >> 6) + i * 7 + 3);
      }
      check(ok, lbl);
    } else {
      check(false, lbl);
    }
  }

  // 10) SUB-BEAT (sub-8-word) partial write to an ALREADY-POPULATED beat must
  //     PRESERVE the non-strobed bytes. This is the accumulation pattern
  //     (results updated word-by-word into a populated cacheline) and the gap
  //     in test 5 (which only wrote fresh/unpopulated bytes). If the controller
  //     turns a partial write into a full-beat RMW with a bad read part, the
  //     neighbors get clobbered/zeroed -> all-zero results.
  const uint64_t addr_p = 0x80400000ULL;
  uint8_t full[64], part[64];
  for (unsigned i = 0; i < 64; i++) full[i] = static_cast<uint8_t>(0x11 * ((i % 15) + 1));
  check(axi_write(addr_p, 1, full, &strb_all), "partial-test: seed full beat");
  check(axi_read(addr_p, 1, rbuf), "partial-test: seed read-back");
  check(std::memcmp(rbuf, full, 64) == 0, "partial-test: full beat seeded ok");

  // 10a) overwrite ONLY dword 3 (bytes 24..31); dwords 0-2,4-7 must survive.
  std::memcpy(part, full, 64);
  for (unsigned i = 24; i < 32; i++) part[i] = static_cast<uint8_t>(0xE0 + (i - 24));
  uint64_t strb_dw3 = 0xffULL << 24;
  check(axi_write(addr_p, 1, part, &strb_dw3), "partial-test: write only dword3");
  check(axi_read(addr_p, 1, rbuf), "partial-test: read after dword3 write");
  check(std::memcmp(rbuf, part, 64) == 0,
        "partial-test: dword3 updated AND neighbors preserved (sub-8-word write)");

  // 10b) single-byte write at offset 40; only that byte changes.
  std::memcpy(part, rbuf, 64);
  part[40] = 0x5A;
  uint64_t strb_b40 = 1ULL << 40;
  check(axi_write(addr_p, 1, part, &strb_b40), "partial-test: single-byte write");
  check(axi_read(addr_p, 1, rbuf), "partial-test: read after single-byte write");
  check(std::memcmp(rbuf, part, 64) == 0,
        "partial-test: single byte updated AND 63 neighbors preserved");

  // 10c) non-contiguous strobes: bytes 0 and 63 only.
  std::memcpy(part, rbuf, 64);
  part[0] = 0x77; part[63] = 0x88;
  uint64_t strb_ends = (1ULL << 0) | (1ULL << 63);
  check(axi_write(addr_p, 1, part, &strb_ends), "partial-test: non-contiguous strobe write");
  check(axi_read(addr_p, 1, rbuf), "partial-test: read after non-contiguous write");
  check(std::memcmp(rbuf, part, 64) == 0,
        "partial-test: bytes 0/63 updated AND middle 62 preserved");

  // 10d) second beat of a populated pair, single-word update (bank/row cross).
  const uint64_t addr_q = 0x80400040ULL;  // next 64B beat
  for (unsigned i = 0; i < 64; i++) full[i] = static_cast<uint8_t>(0xC3 ^ i);
  check(axi_write(addr_q, 1, full, &strb_all), "partial-test: seed second beat");
  std::memcpy(part, full, 64);
  part[8] = 0x01; part[9] = 0x02; part[10] = 0x03; part[11] = 0x04;
  part[12] = 0x05; part[13] = 0x06; part[14] = 0x07; part[15] = 0x08;
  uint64_t strb_dw1 = 0xffULL << 8;
  check(axi_write(addr_q, 1, part, &strb_dw1), "partial-test: write dword1 of second beat");
  check(axi_read(addr_q, 1, rbuf), "partial-test: read second beat");
  check(std::memcmp(rbuf, part, 64) == 0,
        "partial-test: second-beat dword1 updated AND neighbors preserved");

  // 11) PIPELINED sub-8-word writes: does every outstanding partial write get
  //     its B response back? This replicates a heavy write burst (the requester only
  //     advances when each write's B-driven continuation returns; a missing B
  //     would hang the kernel exactly as observed).
  {
    const uint64_t pbase = 0x80900000ULL;
    const uint64_t quarter_strb = 0x000000000000ffffULL;  // 16-byte quarter
    for (unsigned n : {4u, 16u, 17u, 32u, 64u, 128u}) {
      unsigned got = axi_write_pipelined(pbase + n * 0x2000, n, quarter_strb);
      char lbl[96];
      std::snprintf(lbl, sizeof(lbl),
                    "pipelined sub-8-word (distinct lines): %u writes -> %u B", n, got);
      check(got == n, lbl);
    }
  }
  // 11b) SAME-cacheline hammer: the host writes 4 rotating quarters to one 64B beat.
  //      Does the controller return one B per AXI write, or coalesce
  //      same-line writes into fewer B's (-> requester never gets all acks -> hang)?
  {
    const uint64_t hbase = 0x80a00000ULL;
    for (unsigned n : {2u, 4u, 8u, 16u}) {
      // all `n` writes target hbase (same cacheline), rotating quarter strobes.
      unsigned got = axi_write_pipelined(hbase + n * 0x40000 * 0 /*same line*/,
                                         n, 0, /*stride=*/0, /*rotate=*/true);
      char lbl[112];
      std::snprintf(lbl, sizeof(lbl),
                    "same-cacheline sub-8-word: %u writes -> %u B responses", n, got);
      check(got == n, lbl);
    }
  }

  std::printf("[SMOKE] %s (%d failure%s, %" PRIu64 " cycles, %zu dwords)\n",
              failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", failures,
              failures == 1 ? "" : "s", cycles, store.populated_dwords());
  delete dut;
  return failures == 0 ? 0 : 1;
}
