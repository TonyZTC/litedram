// LiteDRAM controller init driver for USE_DRAM_CTRL builds.
//
// The generated core powers up with the DFII already in hardware control
// (its CSR resets to sel=1) and the behavioral PHY model needs no mode
// registers, so the whole init sequence is one Wishbone CSR write:
// ddrctrl_init_done <- 1, which unblocks the core's AXI user ports
// (block_until_ready). This driver performs that write over the wb_ctrl_*
// top-level ports during the INITIALIZATION phase and holds the phase open
// until dram_init_done is observed high (verified ~6 cycles total by the
// smoke test).

#ifndef DRAM_CTRL_DRIVER_HPP
#define DRAM_CTRL_DRIVER_HPP

#include "driver.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

class LiteDramInitTx : public tvm::Transaction {
 public:
  LiteDramInitTx() = default;
  explicit LiteDramInitTx(const nlohmann::json&) {}
  void randomize(const tvm::RandFunc&) override {}
  bool operator==(const tvm::Transaction& other) const override {
    return dynamic_cast<const LiteDramInitTx*>(&other) != nullptr;
  }
  nlohmann::json to_json() const override { return nlohmann::json::object(); }
  std::string name() const override { return "LiteDramInitTx"; }
};

class LiteDramInitDrv : public tvm::Driver<LiteDramInitTx>,
                        public tvm::BulkLoadDriverInterface {
  // CSR byte address of ddrctrl_init_done, from dram_ctrl/generated/csr.csv.
  // Re-check after regenerating the core (the ddrctrl block is emitted first,
  // so this has been stable at 0x0).
  static constexpr uint32_t kInitDoneCsrByteAddr = 0x0;

  enum class State { kIssueWrite, kWaitAck, kWaitInitDone, kDone };
  State state = State::kIssueWrite;
  uint64_t cycle = 0;

 public:
  tvm::DriverPhase get_phase() const override {
    return tvm::DriverPhase::INITIALIZATION;
  }

  void drive(Vdut&, const LiteDramInitTx&) const override {}
  void queue_transaction(const tvm::Transaction&) override {}
  void finish_loading() override {}
  bool is_complete() const override { return state == State::kDone; }

  void initialize_outputs(Vdut& dut) override {
    dut.wb_ctrl_cyc = 0;
    dut.wb_ctrl_stb = 0;
    dut.wb_ctrl_we = 0;
    dut.wb_ctrl_adr = 0;
    dut.wb_ctrl_dat_w = 0;
    dut.wb_ctrl_sel = 0;
  }

  void advance_protocol(Vdut& dut) override {
    cycle++;
    switch (state) {
      case State::kIssueWrite:
        dut.wb_ctrl_adr = kInitDoneCsrByteAddr >> 2;  // word-addressed bus
        dut.wb_ctrl_dat_w = 1;
        dut.wb_ctrl_sel = 0xf;
        dut.wb_ctrl_we = 1;
        dut.wb_ctrl_cyc = 1;
        dut.wb_ctrl_stb = 1;
        state = State::kWaitAck;
        break;

      case State::kWaitAck:
        if (dut.wb_ctrl_ack) {
          dut.wb_ctrl_cyc = 0;
          dut.wb_ctrl_stb = 0;
          dut.wb_ctrl_we = 0;
          state = State::kWaitInitDone;
        }
        break;

      case State::kWaitInitDone:
        if (dut.dram_init_error) {
          std::cerr << "[DRAM_CTRL] ERROR: litedram init_error asserted"
                    << std::endl;
          std::exit(1);
        }
        if (dut.dram_init_done) {
          std::cout << "[DRAM_CTRL] litedram init_done at init cycle " << cycle
                    << std::endl;
          state = State::kDone;
        }
        break;

      case State::kDone:
        break;
    }
  }
};

#endif  // DRAM_CTRL_DRIVER_HPP
