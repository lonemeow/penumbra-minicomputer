# Penumbra Minicomputer — Build System
# Usage:
#   make smoke          — build & run smoke test (verify toolchain)
#   make sim MOD=<name> — build & run testbench for a module
#   make wave MOD=<name>— open waveform in GTKWave
#   make clean          — remove build artifacts

# ── Configuration ──────────────────────────────────────────────
# Docker-based Verilator (avoids host install, works on WSL2)
DOCKER_IMAGE ?= verilator/verilator:latest
DOCKER_RUN   = docker run --rm -v $(CURDIR):/work -w /work

# Verilator runs inside the container; its entrypoint IS verilator.
# For commands that aren't verilator (like running the built binary),
# we override the entrypoint.
VERILATOR_FLAGS = --cc --exe --build -Wall --trace \
                  -CFLAGS "-std=c++17" \
                  -Ihw/rtl/core -Ihw/rtl/bus -Ihw/rtl/mmu -Ihw/rtl/io -Ihw/rtl/soc

BUILD_DIR   = build
WAVE_DIR    = waves

# ── Smoke test ─────────────────────────────────────────────────
.PHONY: smoke
smoke: $(BUILD_DIR)/Vsmoke_adder
	@echo "── Running smoke test ──"
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/Vsmoke_adder $(DOCKER_IMAGE)

$(BUILD_DIR)/Vsmoke_adder: hw/rtl/core/smoke_adder.sv hw/sim/tb_smoke_adder.cpp
	@mkdir -p $(BUILD_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--Mdir $(BUILD_DIR)/smoke_adder.verilator \
		-o ../Vsmoke_adder \
		hw/rtl/core/smoke_adder.sv hw/sim/tb_smoke_adder.cpp

# ── Generic module simulation ──────────────────────────────────
# Usage: make sim MOD=alu  (expects hw/rtl/**/alu.sv and hw/sim/tb_alu.cpp)
#        make sim MOD=machine_sim PROG=test_mem TB=tb_cpu_mem
MOD  ?=
PROG ?= test_add
TB   ?= tb_$(MOD)

# Shared package — always included. --top-module tells Verilator which
# module is the DUT (otherwise it picks the first file = the package).
PKG_SV = hw/rtl/core/penumbra_pkg.sv

# ── Assembler tools ──────────────────────────────────────────
PASM  = python3 sw/tools/pasm.py
UASM  = python3 hw/tools/uasm.py

# ── LLVM toolchain (for C boot ROM) ─────────────────────────
# Override with: make simulate LLVM_PREFIX=/path/to/llvm-build
LLVM_PREFIX ?= $(CURDIR)/build/llvm
CC        = $(LLVM_PREFIX)/bin/clang --target=penumbra-unknown-none
MC        = $(LLVM_PREFIX)/bin/llvm-mc -triple=penumbra
LD        = $(LLVM_PREFIX)/bin/ld.lld
OBJCOPY   = $(LLVM_PREFIX)/bin/llvm-objcopy
BIN2HEX   = python3 sw/tools/bin2hex.py

.PHONY: sim
sim:
ifndef MOD
	$(error Set MOD=<module_name>, e.g. make sim MOD=alu)
endif
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--top-module $(MOD) \
		--Mdir $(BUILD_DIR)/$(MOD).verilator \
		-o ../V$(MOD) \
		$(PKG_SV) $$(find hw/rtl -name '$(MOD).sv') hw/sim/$(TB).cpp
	@# Assemble program and microcode for $readmemh
	@rm -f program.hex microcode.hex
	@if test -f hw/sim/programs/$(PROG).s; then $(PASM) --org 0xFFFFE000 hw/sim/programs/$(PROG).s -o program.hex; \
	else echo "ERROR: hw/sim/programs/$(PROG).s not found" >&2; exit 1; fi
	@if test -f hw/microcode/microcode.uasm; then $(UASM) hw/microcode/microcode.uasm -o microcode.hex; fi
	@echo "── Running $(MOD) testbench ──"
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$(MOD) $(DOCKER_IMAGE)

.PHONY: wave
wave:
ifndef MOD
	$(error Set MOD=<module_name>, e.g. make wave MOD=alu)
endif
	gtkwave $(WAVE_DIR)/$(MOD).vcd &

# ── Run all program tests ──────────────────────────────────────
# Discovers all hw/sim/programs/test_*.s files, runs each through
# tb_cpu_prog on machine_sim, reports pass/fail summary.
TEST_PROGS := $(sort $(basename $(notdir $(wildcard hw/sim/programs/test_*.s))))

.PHONY: test
test:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	@# Build machine_sim + tb_cpu_prog once (reuse for all programs)
	@$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--top-module machine_sim \
		--Mdir $(BUILD_DIR)/machine_sim.verilator \
		-o ../Vmachine_sim \
		$(PKG_SV) $$(find hw/rtl -name 'machine_sim.sv') hw/sim/tb_cpu_prog.cpp
	@# Assemble microcode once (shared by all programs)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@# Remove program.hex so make simulate's ROM build doesn't see stale pasm output
	@rm -f program.hex
	@pass=0; fail=0; failed=""; \
	for prog in $(TEST_PROGS); do \
		if ! $(PASM) --org 0xFFFFE000 hw/sim/programs/$$prog.s -o program.hex; then \
			printf "  \033[31mFAIL\033[0m  %s (assembler error)\n" "$$prog"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$prog"; \
			continue; \
		fi; \
		if $(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/Vmachine_sim $(DOCKER_IMAGE) \
			> /dev/null 2>&1; then \
			printf "  \033[32mPASS\033[0m  %s\n" "$$prog"; \
			pass=$$((pass + 1)); \
		else \
			printf "  \033[31mFAIL\033[0m  %s\n" "$$prog"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$prog"; \
		fi; \
	done; \
	echo ""; \
	total=$$((pass + fail)); \
	echo "$$pass/$$total tests passed"; \
	rm -f program.hex; \
	if [ $$fail -gt 0 ]; then \
		echo "  *** $$fail FAILED:$$failed ***"; \
		exit 1; \
	fi

# ── Interactive simulation ─────────────────────────────────────
# Builds machine_sim with interactive testbench and boot ROM.
# Bridges stdin/stdout to UART for terminal interaction.
# Usage: make simulate
#        make simulate LLVM_PREFIX=/other/llvm/build
DOCKER_RUN_IT = docker run --rm -it -v $(CURDIR):/work -w /work

.PHONY: simulate
simulate:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--top-module machine_sim \
		--Mdir $(BUILD_DIR)/machine_sim_interactive.verilator \
		-o ../Vmachine_sim_interactive \
		$(PKG_SV) $$(find hw/rtl -name 'machine_sim.sv') hw/sim/tb_interactive.cpp
	@rm -f program.hex
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@$(DOCKER_RUN_IT) --entrypoint ./$(BUILD_DIR)/Vmachine_sim_interactive $(DOCKER_IMAGE)

# ── Cleanup ────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(WAVE_DIR)
