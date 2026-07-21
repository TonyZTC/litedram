# DPI-backed LiteDRAM DDR4 controller for Verilator

A drop-in, simulation-only AXI4 DRAM controller built from
[LiteDRAM](https://github.com/enjoy-digital/litedram) (BSD-2). It gives a
Verilator testbench a *real* open-source DRAM controller — real AXI4 slave
protocol behavior plus real controller behavior (bank management, refresh,
command scheduling) — in place of a behavioral C++ memory mock, while a DPI
backend keeps a 32GB address space free of resident memory.

```
your AXI4 master
   v  s_axi_* (AXI4, 512-bit data, 64-bit addr)
dram_ctrl_wrap.sv           (glue: addr truncation, ID extend, init gating)
   v  user_port_p0_* (AXI4)
generated/litedram_core.v   (LiteDRAM controller core)
   v  per-bank storage = DPI
dram_dpi_mem.sv x16
   v
DramDpiStore (C++ sparse dword-keyed map: load_memory_from_binary,
              validation, 0xBAADDEADBEEFDEAD uninitialized-read magic)
```

The core is committed under `generated/`, so everyday builds need no Python —
you only compile the RTL and link the C++ store header into your testbench.

## Integrating into a testbench

1. Add these to your Verilator source list (order matters — waivers first):
   `dram_lint_waivers.vlt`, `generated/litedram_core.v`, `dram_dpi_mem.sv`,
   `dram_ctrl_wrap.sv`.
2. Put `include/` on the C++ include path and compile a translation unit that
   pulls in `dram_dpi_store.hpp` (it defines the `dram_dpi_read/write` DPI
   entry points backed by the sparse store).
3. Instantiate `dram_ctrl_wrap` and wire `s_axi_*` to your AXI4 master
   (512-bit data, 64-bit address, <=8-bit ID). Drive the `wb_ctrl_*` Wishbone
   bus and `reset_l`; expose `dram_init_done` / `dram_init_error` /
   `dram_oow_count` to the C++ side.
4. Run the init sequence once at startup: `include/dram_ctrl_driver.hpp`
   provides `LiteDramInitDrv`, a single Wishbone write that releases the
   controller (the user ports hold `ready` low until it completes and
   `dram_init_done` rises). Preload data with
   `DramDpiStore::load_memory_from_binary()` and validate after the run.

`smoke/` is a self-contained example that exercises exactly this flow
(WB init + AXI writes/reads + store validation) against the core alone — a
good reference and a fast build sanity check.

## Files

| File | Role |
|---|---|
| `litedram_dram_ctrl.yml` | generator config: DDR4, fake 32Gb x8 die (`MT40A4G8`) x8 = 32GB, 64-bit bus -> 512-bit AXI, ports `p0` (id 8) + spare `tb` (id 4), `cpu: None`, `ROW_BANK_COL` |
| `generated/litedram_core.v` | committed generator output — everyday builds need no Python |
| `generated/csr.csv` | CSR map; `ddrctrl_init_done` @ 0x0 is hardcoded in `dram_ctrl_driver.hpp` |
| `dram_ctrl_wrap.sv` | glue: 64->35-bit address truncation (+ out-of-window counter), 7->8-bit ID zero-extension, wb_ctrl/init plumbing, spare-port tie-off |
| `dram_dpi_mem.sv` | DPI-backed bank storage (replaces the sim PHY's baked 2GB-per-bank arrays) |
| `dram_lint_waivers.vlt` | lint waivers scoped to the generated core |
| `include/dram_dpi_store.hpp` | sparse store + DPI entry points (compiled into the testbench) |
| `include/dram_ctrl_driver.hpp` | `LiteDramInitDrv`: one Wishbone write in the init phase |
| `smoke/` | standalone smoke test of the core + DPI store (`make` there) |
| `scripts/litedram_gen.py` | generator fork: registers `MT40A4G8`, swaps `BankModel` for the DPI variant, deepens AXI buffers to 128 |
| `scripts/regen_litedram.sh` | venv + pinned submodules + regenerate + copy into `generated/` |

Everything the controller needs is self-contained under this directory (RTL,
generated core, C++ store/driver headers, smoke test, and the regeneration
scripts). The `third_party/{migen,litex,litedram}` submodules are needed only
to regenerate the core.

## How the DPI storage works

LiteDRAM's `--sim` PHY (`SDRAMPHYModel`) stores DRAM contents in per-bank
Migen memories sized to the full DRAM — unbuildable at 32GB. The generator
fork replaces `litedram.phy.model.BankModel` with a copy whose `Memory`
becomes an `Instance("dram_dpi_mem", p_BANK=<creation order>=DFI bank #)`.
Each bank word is one 512-bit beat; `DramDpiStore::linear_byte_address()`
inverts the controller's `ROW_BANK_COL` mapping so the sparse map is keyed by
the original AXI byte address. The smoke test verifies the inversion round
trip front-door (AXI write -> map key == AXI address; backdoor preload -> AXI
read). The store is a dword-keyed sparse map, so
`load_memory_from_binary`, post-sim `validate_memory_dump`, and the
`0xBAADDEADBEEFDEAD` uninitialized-read magic all behave deterministically.

## Behavioral notes

- Latency comes from the controller (row hits/misses, refresh; ~21 cycles for
  an idle-bank read), not from any configurable knob.
- Responses are strictly in-order (LiteDRAM's AXI front-end does not reorder).
- Addresses at or above 32GB alias mod 2^35 (warned once + counted on
  `dram_oow_count`).
- There is no 4KB-boundary special-casing; an INCR burst that crosses a 4KB
  boundary still increments the address correctly.
- The litedram core has **no reset input** (Migen power-on initials); a
  mid-simulation `reset_l` pulse resets the glue's counter but not the
  controller. The `wb_ctrl_*` strobes are gated by `reset_l` in the wrap so a
  garbage CSR write during the reset window cannot corrupt the controller.

## Regenerating the core

```sh
./scripts/regen_litedram.sh   # run from this directory
```

Requires network on first run (venv bootstrap: pyyaml, packaging,
pythondata-misc-tapcfg) and the `third_party/{migen,litex,litedram}`
submodules checked out (pinned: litex/litedram `2026.04`).
After a regen, re-check: the module ports of `litedram_core` (consumed by
`dram_ctrl_wrap.sv`), the `ddrctrl_init_done` CSR address in `csr.csv`
(hardcoded in `include/dram_ctrl_driver.hpp`), and run the smoke test
(`smoke/`) before your full build.

## Simulation-only

The generated core and the DPI storage are simulation-only (behavioral PHY
model, DPI-backed storage, no reset input). Do not include any of this in a
synthesis flow.
