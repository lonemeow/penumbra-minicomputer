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
                  -Irtl/core -Irtl/bus -Irtl/mmu -Irtl/io -Irtl/soc

BUILD_DIR   = build
WAVE_DIR    = waves

# ── Smoke test ─────────────────────────────────────────────────
.PHONY: smoke
smoke: $(BUILD_DIR)/Vsmoke_adder
	@echo "── Running smoke test ──"
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/Vsmoke_adder $(DOCKER_IMAGE)

$(BUILD_DIR)/Vsmoke_adder: rtl/core/smoke_adder.sv sim/tb_smoke_adder.cpp
	@mkdir -p $(BUILD_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--Mdir $(BUILD_DIR)/smoke_adder.verilator \
		-o ../Vsmoke_adder \
		rtl/core/smoke_adder.sv sim/tb_smoke_adder.cpp

# ── Generic module simulation ──────────────────────────────────
# Usage: make sim MOD=alu  (expects rtl/**/alu.sv and sim/tb_alu.cpp)
MOD ?=

.PHONY: sim
sim:
ifndef MOD
	$(error Set MOD=<module_name>, e.g. make sim MOD=alu)
endif
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--Mdir $(BUILD_DIR)/$(MOD).verilator \
		-o ../V$(MOD) \
		$$(find rtl -name '$(MOD).sv') sim/tb_$(MOD).cpp
	@echo "── Running $(MOD) testbench ──"
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$(MOD) $(DOCKER_IMAGE)

.PHONY: wave
wave:
ifndef MOD
	$(error Set MOD=<module_name>, e.g. make wave MOD=alu)
endif
	gtkwave $(WAVE_DIR)/$(MOD).vcd &

# ── Cleanup ────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(WAVE_DIR)
