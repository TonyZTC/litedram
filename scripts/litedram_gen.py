#!/usr/bin/env python3
# Fork of litedram's standalone core generator (litedram/gen.py).
#
# Three deviations from the stock generator, all applied by rebinding names
# before delegating to the unmodified gen.main():
#
# 1. A fake 32Gb x8 DDR4 die (MT40A4G8) is registered in litedram.modules so
#    the YAML can request a 32GB single-rank 64-bit-bus configuration (the
#    catalog tops out at 16Gb dies and the sim PHY model has no multi-rank
#    support). Sim-only geometry; timings inherit MT40A2G8.
#
# 2. litedram.phy.model.BankModel is replaced with a DPI-backed variant: the
#    per-bank full-capacity Migen Memory (2GB/bank here -- unbuildable under
#    Verilator) becomes an Instance of the hand-written dram_dpi_mem SV module
#    (../dram_dpi_mem.sv), which calls into the testbench's sparse C++ store.
#    Bank index = creation order, which matches the DFI bank number
#    (SDRAMPHYModel wires banks via enumerate()). All activate/precharge row
#    tracking and address math is copied verbatim from the stock BankModel so
#    the controller-visible behavior is identical.
#
# 3. The AXI user-port buffers are deepened (see below) so the port can hold
#    the host master's full outstanding-write depth without dropping ids.
#
# Run via regen_litedram.sh (this script's sibling; pinned submodules + venv).

from migen import Module, Signal, Instance, ClockSignal, Replicate, If, log2_int

import litedram.modules as litedram_modules
import litedram.phy.model as phy_model


# 1) Fake 32Gb x8 DDR4 die: 2^18 rows x 2^10 cols x 16 banks x 8 bits.
#    x8 dies on a 64-bit bus -> 32GB total, 35-bit AXI user-port address.
class MT40A4G8(litedram_modules.MT40A2G8):
    nrows = 262144


litedram_modules.MT40A4G8 = MT40A4G8


# 2) DPI-backed BankModel. Same interface and internal row/active tracking as
#    litedram.phy.model.BankModel; only the storage differs.
class DPIBankModel(Module):
    _bank_counter = 0

    def __init__(self, data_width, nrows, ncols, burst_length, nphases, we_granularity, init):
        assert init in (None, []), "bank init contents are unsupported with DPI storage"
        assert we_granularity == 8, "dram_dpi_mem assumes byte write granularity"
        bank_index = DPIBankModel._bank_counter
        DPIBankModel._bank_counter += 1

        self.activate     = Signal()
        self.activate_row = Signal(max=nrows)
        self.precharge    = Signal()

        self.write        = Signal()
        self.write_col    = Signal(max=ncols)
        self.write_data   = Signal(data_width)
        self.write_mask   = Signal(data_width//8)

        self.read         = Signal()
        self.read_col     = Signal(max=ncols)
        self.read_data    = Signal(data_width)

        # # #

        active = Signal()
        row    = Signal(max=nrows)

        self.sync += \
            If(self.precharge,
                active.eq(0),
            ).Elif(self.activate,
                active.eq(1),
                row.eq(self.activate_row)
            )

        bank_mem_len = nrows*ncols//(burst_length*nphases)
        wraddr       = Signal(max=bank_mem_len)
        rdaddr       = Signal(max=bank_mem_len)

        self.comb += [
            wraddr.eq((row*ncols | self.write_col)[log2_int(burst_length*nphases):]),
            rdaddr.eq((row*ncols | self.read_col)[log2_int(burst_length*nphases):]),
        ]

        we    = Signal(data_width//8)
        rd_en = Signal()
        self.comb += \
            If(active,
                we.eq(Replicate(self.write, data_width//8) & ~self.write_mask),
                rd_en.eq(self.read),
            )

        rdat = Signal(data_width)
        self.specials += Instance("dram_dpi_mem",
            p_BANK   = bank_index,
            p_DATA_W = data_width,
            p_ADDR_W = len(wraddr),
            i_clk    = ClockSignal(),
            i_we     = we,
            i_wadr   = wraddr,
            i_wdat   = self.write_data,
            i_rd_en  = rd_en,
            i_radr   = rdaddr,
            o_rdat   = rdat,
        )
        self.comb += If(rd_en, self.read_data.eq(rdat))


phy_model.BankModel = DPIBankModel


# 3) Deepen the AXI user-port buffers.
#
# LiteDRAMAXI2Native's write id/resp/data buffers default to depth 16, and the
# write path does NOT gate aw.ready on id_buffer space (frontend/axi.py: aw.ready
# is driven purely by the controller cmd handshake). So once more than 16 writes
# are outstanding, the id_buffer overflows: the AW is still accepted but its id
# is dropped, and the B channel then echoes mismatched ids -- orphaning the
# write whose id was lost (its bid never returns) and erroring another.
#
# The host AXI master can issue up to 128 outstanding writes, so it easily
# exceeds 16 under heavy traffic. Deepen the buffers to cover the master's full
# outstanding capacity. Sim-only; these FIFOs are cheap.
import litedram.gen as _gen  # noqa: E402

_AXI_BUFFER_DEPTH = 128
_orig_axi2native = _gen.LiteDRAMAXI2Native


def _deep_axi2native(axi, port, w_buffer_depth=_AXI_BUFFER_DEPTH,
                     r_buffer_depth=_AXI_BUFFER_DEPTH,
                     base_address=0x00000000, with_read_modify_write=False):
    return _orig_axi2native(axi, port, w_buffer_depth=w_buffer_depth,
                            r_buffer_depth=r_buffer_depth,
                            base_address=base_address,
                            with_read_modify_write=with_read_modify_write)


_gen.LiteDRAMAXI2Native = _deep_axi2native

from litedram.gen import main  # noqa: E402  (import after patches on purpose)

if __name__ == "__main__":
    main()
