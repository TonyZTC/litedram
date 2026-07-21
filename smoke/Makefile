# Standalone smoke test for the generated LiteDRAM DPI core (USE_DRAM_CTRL).
# Verilates litedram_core.v + dram_dpi_mem.sv alone and runs smoke_tb.cpp.
#   make        - build + run
#   make build  - build only
#   make clean

# All paths relative to this dir (dram_ctrl/smoke/). The lint waivers in
# dram_lint_waivers.vlt match by file name, so the relative "../generated/..."
# names are fine. smoke_tb.cpp includes the DPI store via a relative
# "../include/..." path, so no C++ -I is needed here.
DCTL    := ..
SV_FILES  := $(DCTL)/dram_lint_waivers.vlt $(DCTL)/generated/litedram_core.v $(DCTL)/dram_dpi_mem.sv
# The store header is an #include of smoke_tb.cpp; make it a build dependency
# too, or editing the store silently reuses a stale binary and "passes".
CPP_HEADERS := $(DCTL)/include/dram_dpi_store.hpp
OBJ_DIR   := obj_dir
EXE       := $(OBJ_DIR)/Vlitedram_core

VERILATOR_FLAGS := --cc --exe --build -j 8 -O1 \
	--top-module litedram_core \
	--Mdir $(OBJ_DIR) \
	-CFLAGS "-std=c++17 -O1"

.PHONY: test build clean
test: build
	./$(EXE)

build: $(EXE)

$(EXE): $(SV_FILES) smoke_tb.cpp $(CPP_HEADERS)
	verilator $(VERILATOR_FLAGS) $(SV_FILES) smoke_tb.cpp

clean:
	rm -rf $(OBJ_DIR)
