# Penumbra Minicomputer — Build System
# Usage:
#   make simulate            — build ROM + ISS, run interactively (fast)
#   make simulate-rtl        — build ROM + Verilator RTL sim (cycle-accurate)
#   make test-iss            — ISA conformance tests on the ISS (fast, no Docker)
#   make test [CORE=<gen>]   — isa/ + <gen>/ tests on RTL sim (slow, Docker)
#   make test-prog CORE=<gen> PROG=<name> — one program test
#   make sim MOD=<name>      — build & run testbench for a module
#   make smoke               — toolchain smoke test
#   make wave MOD=<name>     — open waveform in GTKWave
#   make image               — build the NetBSD SD image (FLAVOR= selects one)
#   make fpga-lint           — Verilator lint check on the FPGA system
#   make fpga BOARD=<b> CORE=<gen> — synthesize + PnR + bitstream
#                              (TOP=<module> is the low-level escape hatch)
#   make flash [same vars]   — fpga + flash to the board via USB
#   make vivado-image        — build the local Vivado image (needs a token)
#   make vivado-authenticate — refresh the AMD web installer's login token
#   make openfpgaloader-image — build the board programmer image
#   make clean               — remove build artifacts

# ── Configuration ──────────────────────────────────────────────
# Docker-based Verilator (avoids host install, works on WSL2)
DOCKER_IMAGE ?= verilator/verilator:latest
DOCKER_RUN   = docker run --rm -u $(shell id -u):$(shell id -g) -v $(CURDIR):/work -w /work

# Verilator runs inside the container; its entrypoint IS verilator.
# For commands that aren't verilator (like running the built binary),
# we override the entrypoint.
#
# OPT_BUILD overrides the host C++ optimization Verilator uses for
# generated simulation code.  Verilator's default is -Os (size); we
# default to -O2 for ~2-3x simulation runtime at modest build-time
# cost.  Long-running sims (NetBSD boot, etc.) benefit further from
# OPT_BUILD="-O3 -flto -march=native" — slower to build, faster to
# run.  Set OPT_BUILD="-Os" to restore the original size-optimized
# build when iterating on testbenches.
OPT_BUILD ?= -O2

VERILATOR_BASE_FLAGS = --cc --exe --build -Wall --assert \
                       $(if $(VCD),--trace) \
                       -CFLAGS "-std=c++17 $(OPT_BUILD)"
VERILATOR_INCLUDES   = -Ihw/rtl/common -Ihw/rtl/penumbra1 -Ihw/rtl/penumbra2 -Ihw/rtl/penumbra3 -Ihw/rtl/machine -Ihw/rtl/bus -Ihw/rtl/io -Ihw/rtl/io/audio -Ihw/rtl/io/i2c -Ihw/rtl/io/sdram -Ihw/rtl/io/video -Ihw/rtl/io/usb -Ihw/rtl/io/eth -Ihw/rtl/io/ps2 -Ihw/rtl/soc -Ihw/rtl/sim

# A core build (program runners, interactive sim) puts its variant's
# directory first, so the variant's forks shadow the base modules they
# replace, and its memory system's (SIM_MEM) next.
VERILATOR_FLAGS = $(VERILATOR_BASE_FLAGS) $(if $(CORE_SUB),-Ihw/rtl/$(CORE) )$(addprefix -I,$(SIM_MEM_DIRS)) $(VERILATOR_INCLUDES)

# A module test builds its module against the base files whatever CORE
# the session selects: a variant fork on the include path would shadow
# the submodules a base module instantiates. A variant's own module
# tests name its directory first themselves (test-modules-variant).
#
# A board top under test instantiates vendor primitives, which the lint
# stubs stand in for; as a -v library file, a stub file's modules only
# elaborate where something instantiates them.
VERILATOR_BOARD_LIBS   = -Ihw/rtl/fpga/nexysvideo -Ihw/rtl/fpga/xc7 -v hw/rtl/fpga/xc7/xc7_prim.sv
VERILATOR_MODULE_FLAGS = $(VERILATOR_BASE_FLAGS) $(VERILATOR_INCLUDES) $(VERILATOR_BOARD_LIBS)

BUILD_DIR   = build
WAVE_DIR    = waves

# ── Generic module simulation ──────────────────────────────────
# Usage: make sim MOD=alu  (expects hw/rtl/**/alu.sv and hw/sim/tb_alu.cpp)
#        make sim MOD=machine_sim PROG=test_mem TB=tb_cpu_mem
# PROG replaces the boot ROM's image for that run; without it, boot_rom
# loads program.hex.
MOD  ?=
PROG ?=
TB   ?= tb_$(MOD)

# Shared packages — always included. --top-module tells Verilator which
# module is the DUT (otherwise it picks the first file = a package).
# Add new packages here as the design grows.
PKG_SV = hw/rtl/common/penumbra_pkg.sv hw/rtl/penumbra2/penumbra2_pkg.sv hw/rtl/penumbra3/penumbra3_pkg.sv hw/rtl/io/sdram/sdram_pkg.sv hw/rtl/io/video/video_pkg.sv hw/rtl/io/usb/usb_pkg.sv hw/rtl/fpga/ecp5_pll_pkg.sv hw/rtl/fpga/xc7/xc7_mmcm_pkg.sv

# Resolving a module by name (module sims, test-modules) must not pick up a
# fork, which redefines a module under the same name in its own directory: a
# core variant's (penumbra2_5/), or a simulation stand-in's (sim/ddr3/,
# sim/nexysvideo/). Module tests target the base modules; the variant forks
# are exercised through the full-machine sim (CORE=penumbra2_5) and their own
# module tests (test-modules-variant), the stand-ins by the builds that put
# their directories on the path.
MOD_FIND_PRUNE = -not -path '*/penumbra2_5/*' -not -path '*/sim/ddr3/*' -not -path '*/sim/nexysvideo/*'

# ── Assembler tools ──────────────────────────────────────────
PASM  = python3 sw/tools/pasm.py
UASM  = python3 hw/tools/uasm.py

# ── Instruction-set simulator ────────────────────────────────
# Defined ahead of every rule that names it as a prerequisite: make
# expands a rule's prerequisites as it reads the rule, so a later
# definition leaves those rules with no dependency on the ISS, and they
# run whatever binary happens to be built.
ISS = sw/sim/penumbra-iss

# ── LLVM toolchain (for C boot ROM) ─────────────────────────
# Override with: make simulate LLVM_PREFIX=/path/to/llvm-build
LLVM_PREFIX ?= $(CURDIR)/build/llvm
CC        = $(LLVM_PREFIX)/bin/clang --target=penumbra-unknown-none
MC        = $(LLVM_PREFIX)/bin/llvm-mc -triple=penumbra
LD        = $(LLVM_PREFIX)/bin/ld.lld
OBJCOPY   = $(LLVM_PREFIX)/bin/llvm-objcopy
BIN2HEX   = python3 sw/tools/bin2hex.py

# A module sim that needs sources off the module path names their
# directories here, searched ahead of it: the Nexys machine top's DDR3
# memory system, its controller's and display clocks' simulation
# stand-ins (shadowing their wrappers), its stack, and the line adapter
# the stack takes (shadowing the word adapter).
SIM_DIRS_nexysvideo_penumbra2_top = hw/rtl/sim/nexysvideo hw/rtl/io/ddr3 hw/rtl/io/sdram_line

# A module sim's own Verilator flags. The Nexys machine top's I2C pads
# are open drain, driven low or let go: without the enables at the top
# level Verilator resolves such a pin inside the model and never reads
# it back, so the testbench could not hold the bus for it.
SIM_FLAGS_nexysvideo_penumbra2_top = --pins-inout-enables

.PHONY: sim
sim:
ifndef MOD
	$(error Set MOD=<module_name>, e.g. make sim MOD=alu)
endif
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(addprefix -I,$(SIM_DIRS_$(MOD))) $(VERILATOR_MODULE_FLAGS) \
		$(SIM_FLAGS_$(MOD)) \
		--top-module $(MOD) \
		--Mdir $(BUILD_DIR)/$(MOD).verilator \
		-o ../V$(MOD) \
		$(PKG_SV) $$(find hw/rtl -name '$(MOD).sv' $(MOD_FIND_PRUNE)) hw/sim/$(TB).cpp
	@# Assemble program (per-program hex under build/hex/, passed to the
	@# RTL via +rom_hex= — the root program.hex belongs to the boot ROM)
	@# and microcode for $readmemh
	@mkdir -p $(BUILD_DIR)/hex
	@if test -n "$(PROG)"; then \
		src=$$(ls hw/sim/programs/*/$(PROG).s 2>/dev/null | head -1); \
		if test -n "$$src"; then \
			$(PASM) --org 0xFFFF0000 $$src -o $(BUILD_DIR)/hex/$(PROG).hex; \
		else echo "ERROR: $(PROG).s not found under hw/sim/programs/" >&2; exit 1; fi; \
	fi
	@if test -f hw/microcode/microcode.uasm; then $(UASM) hw/microcode/microcode.uasm -o microcode.hex; fi
	@echo "── Running $(MOD) testbench ──"
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$(MOD) $(DOCKER_IMAGE) $(if $(PROG),+rom_hex=$(BUILD_DIR)/hex/$(PROG).hex)

.PHONY: wave
wave:
ifndef MOD
	$(error Set MOD=<module_name>, e.g. make wave MOD=alu)
endif
	gtkwave $(WAVE_DIR)/$(MOD).vcd &

# ── Program test suites ────────────────────────────────────────
# Layout per doc/internals/build-system.md: isa/ holds conformance
# tests (must pass on the ISS and on every core generation's runner);
# <core>/ holds microarch-pinned regressions. Programs carry lit-style
# "; RUNNER:" / "; REQUIRES:" header tags; hw/tools/run-prog-tests.py
# scans them, skips unrunnable programs visibly, and reports.
CORE ?= penumbra2_5

# A CORE may name a microarch variant of a base generation as
# penumbra<n>_<sub> (e.g. penumbra2_5 = gen2.5): the same RTL fileset and
# runner config as the base, distinguished only by a build-time core
# parameter wired in below. Split CORE into base + suffix once here; a
# bare core name (penumbra1, penumbra2) is its own base, empty suffix.
CORE_BASE := $(word 1,$(subst _, ,$(CORE)))
CORE_SUB  := $(word 2,$(subst _, ,$(CORE)))

# The one genuinely per-variant fact: which value the variant parameter
# takes. Stated once per variant; bare cores get the baseline (0). Fed to
# both the sim (-D) and fpga (sv2v -D) builds as PENUMBRA_CPU_VARIANT.
CPU_VARIANT_penumbra2_5 := 1
CPU_VARIANT := $(or $(CPU_VARIANT_$(CORE)),0)

# Which memory system backs the sim machine's RAM: `sdram`, the ULX3S's
# (sdram_sim, 32 MiB), or `ddr3`, the Nexys Video's (ddr3_sim, 512 MiB;
# gen2 only). ddr3 builds the fork of the sim wrapper in hw/rtl/sim/ddr3/
# and puts the DDR3 stack, and the line adapter it takes, ahead of the
# base files on the path. The runner and console binaries carry the
# choice in their names, so both builds can sit side by side.
SIM_MEM ?= sdram
SIM_MEM_DIRS_ddr3 := hw/rtl/sim/ddr3 hw/rtl/io/ddr3 hw/rtl/io/sdram_line
SIM_MEM_DIRS      := $(SIM_MEM_DIRS_$(SIM_MEM))
SIM_MEM_TAG       := $(if $(SIM_MEM_DIRS),_$(SIM_MEM))
ifeq ($(filter sdram ddr3,$(SIM_MEM)),)
$(error SIM_MEM must be `sdram` or `ddr3`, got `$(SIM_MEM)`)
endif
ifeq ($(SIM_MEM)-$(filter penumbra2,$(CORE_BASE)),ddr3-)
$(error SIM_MEM=ddr3 has a sim wrapper for gen2 only, not CORE=$(CORE))
endif

# A sim wrapper's source: the memory system's fork of it if there is one,
# else the base file.
sim_wrapper_sv = $(firstword $(wildcard $(addsuffix /$(1).sv,$(SIM_MEM_DIRS) hw/rtl/sim)))

PROG_DIR    = hw/sim/programs
ISA_PROGS  := $(sort $(wildcard $(PROG_DIR)/isa/test_*.s))
# A variant inherits its base's programs and adds any of its own; the
# REQUIRES/PROVIDES tags skip base tests a variant cannot satisfy.
CORE_PROGS := $(sort $(wildcard $(PROG_DIR)/$(CORE_BASE)/test_*.s) \
                     $(wildcard $(PROG_DIR)/$(CORE)/test_*.s))
TEST_PROGS := $(ISA_PROGS) $(CORE_PROGS)

# test-prog narrows the suite to a single program.
ifdef TEST_FILTER
TEST_PROGS := $(filter %/$(TEST_FILTER).s,$(TEST_PROGS))
endif

# RTL runner configuration per core generation: the DUT module, the
# runner testbenches (first entry is the default; the rest are
# selectable via RUNNER tags), and the capability set the integration
# provides for REQUIRES tags.
# mmu-d / mmu-i are the per-side slices of mmu: data / fetch
# translation with TLB miss + protection faults and the
# FAULT_ADDR/FAULT_STATUS commit. Plain mmu is the full MMU including
# non-identity mappings (the gen2 flat memory stand-in is
# vaddr-addressed, so gen2 earns mmu with the VIPT L1's tag compare).
RUNNER_MOD_penumbra1      = machine_sim
RUNNER_TBS_penumbra1      = tb_cpu_prog
# bus-fault = the no-device access-fault return path (an unclaimed address
# traps to VEC_BUS_FAULT). bus = the SYSDEV_BUS controller device, whose
# RST/CFG_EN register test_busctl probes. The autoconfig daisy chain those
# bits drive is a wrapper/board concern that no conformance test exercises:
# gen1's machine_sim wires a chain; gen2's sim wrapper leaves busctl's
# outputs open until the device-discovery / SD path consumes them.
RUNNER_PROVIDES_penumbra1 = mmu mmu-d mmu-i cache l2 uart spi bus bus-fault machid perfctr timer irq wrspr usbhc

RUNNER_MOD_penumbra2      = machine_penumbra2_sim
RUNNER_TBS_penumbra2      = tb_penumbra2_prog tb_penumbra2_intr
# pinned-stalls: the exact cycle/stall-attribution counts the gen2 perfctr tests
# assert. A forwarding/predicting variant changes them, so it drops it (below).
RUNNER_PROVIDES_penumbra2 = mmu mmu-d mmu-i cache l2 wrspr machid perfctr timer irq uart bus bus-fault pinned-stalls

# Runner config keys on the base generation, so a variant inherits the
# DUT wrapper, testbenches, and capability set unchanged. A variant whose
# behavior diverges from its base attaches its capability delta here
# (e.g. dropping a capability whose tests it can no longer satisfy).
RUNNER_MOD      = $(RUNNER_MOD_$(CORE_BASE))
RUNNER_TBS      = $(RUNNER_TBS_$(CORE_BASE))
RUNNER_DEFAULT  = $(firstword $(RUNNER_TBS))
RUNNER_PROVIDES = $(filter-out $(RUNNER_DROPS_$(CORE)),$(RUNNER_PROVIDES_$(CORE_BASE)))

# Per-variant capability delta, filtered from the inherited set above (unset for
# a base core). gen2.5 forwards/predicts → cycle counts move, so it drops pinned-stalls.
RUNNER_DROPS_penumbra2_5 = pinned-stalls

# The ISS models the full machine: it provides every capability the
# gen1 machine does.
ISS_PROVIDES = $(RUNNER_PROVIDES_penumbra1)

# ── Run the program suites on the RTL sim ──────────────────────
# Usage: make test [CORE=penumbra2]
.PHONY: test
test:
	@test -n "$(strip $(RUNNER_MOD))" || \
		{ echo "ERROR: unknown CORE '$(CORE)' — known bases: penumbra1 penumbra2 (+ variants, e.g. penumbra2_5)"; exit 1; }
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR) $(BUILD_DIR)/hex
	@# Build each runner testbench once (binaries keyed by core + testbench:
	@# a variant sets a different PENUMBRA_CPU_VARIANT, so it needs its own)
	@for tb in $(RUNNER_TBS); do \
		$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
			-DPENUMBRA_CPU_VARIANT=$(CPU_VARIANT) \
			--top-module $(RUNNER_MOD) \
			--Mdir $(BUILD_DIR)/$(CORE)$(SIM_MEM_TAG)_$$tb.verilator \
			-o ../V$(CORE)$(SIM_MEM_TAG)_$$tb \
			$(PKG_SV) $(call sim_wrapper_sv,$(RUNNER_MOD)) hw/sim/$$tb.cpp \
			|| exit 1; \
	done
	@# Microcode for $$readmemh (gen1's machine; harmless for other cores)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@python3 hw/tools/run-prog-tests.py --mode rtl \
		--asm "$(PASM) --org 0xFFFF0000" --hexdir $(BUILD_DIR)/hex \
		--provides "$(RUNNER_PROVIDES)" --default-runner $(RUNNER_DEFAULT) \
		$(foreach tb,$(RUNNER_TBS),--bin "$(tb)=$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$(CORE)$(SIM_MEM_TAG)_$(tb) $(DOCKER_IMAGE)") \
		$(TEST_PROGS)

# ── One program test (porcelain over the same runner flow) ─────
# Usage: make test-prog CORE=penumbra2 PROG=test_store_squash
.PHONY: test-prog
test-prog:
	@test -n "$(strip $(filter %/$(PROG).s,$(TEST_PROGS)))" || \
		{ echo "ERROR: no $(PROG).s under $(PROG_DIR)/{isa,$(CORE)}"; exit 1; }
	@$(MAKE) --no-print-directory test TEST_FILTER=$(PROG)

# ── Run all hardware module unit tests ────────────────────────
# Each entry is "MOD" when the testbench filename is the default
# `tb_<MOD>.cpp` and the top module is `<MOD>`.  Use "MOD:TB" when
# the wrapper module name diverges from the testbench filename
# (typically because a `<thing>_test.sv` wrapper drives a smaller
# RTL module — see hw/rtl/sim/).
#
# Integration testbenches (tb_cpu_prog, tb_cpu_top, tb_cpu_mem, the
# tb_penumbra{1,2}_interactive consoles) are not listed here — they are
# covered by `make test`, `make simulate`, etc.
#
# Usage: make test-modules
MODULE_TESTS = \
    alu \
    amux \
    bmux \
    byte_ext \
    byte_rep \
    cond_eval \
    datapath \
    divmul \
    field_ext \
    imm_ext \
    mar \
    mdr \
    pc_mux \
    pc_reg \
    regfile \
    status_reg \
    wmux \
    penumbra2_regfile \
    penumbra2_scoreboard \
    penumbra2_alu \
    penumbra2_regmap \
    penumbra2_decode \
    penumbra2_flag_bypass \
    penumbra2_if1_stage \
    penumbra2_if2_stage \
    penumbra2_id_stage \
    penumbra2_ex_stage \
    penumbra2_mem_stage \
    penumbra2_wb_stage \
    penumbra2_spr_file \
    penumbra2_vecfetch \
    penumbra2_irq \
    penumbra2_spine \
    penumbra2_fetch_buffer \
    cache_bram_vipt \
    txn_arbiter \
    fill_sequencer \
    ecp5_pll_test \
    xc7_mmcm_test \
    nexysvideo_uart_test_top \
    nexysvideo_ddr3_selftest_test \
    nexysvideo_codec_setup_test:tb_nexysvideo_codec_setup \
    onchip_ram \
    unified_mem \
    uart \
    busctl \
    bus_wait_meter \
    tlb \
    tlb_pinned \
    tlb_unit \
    penumbra2_tlb \
    penumbra2_tlb_unit \
    mmu \
    penumbra2_mmu \
    cpu_bus_arbiter \
    spi_fifo \
    video_timing \
    audio_sigma_delta_test \
    audio_test:tb_audio \
    audio_multi_test:tb_audio_multi \
    audio_i2s_out \
    i2c_master_test:tb_i2c_master \
    i2c_shared_master_test:tb_i2c_shared_master \
    timer \
    sdram_test \
    sdram_bl8_test:tb_sdram_bl8_test \
    sdram_cdc \
    cache_test:tb_cache \
    cache_vipt_test:tb_cache_vipt \
    l2_cache \
    autoconfig_test:tb_autoconfig \
    spi_test:tb_spi \
    sdram_adapter_test:tb_sdram_adapter \
    sdram_sim:tb_sdram_sim \
    penumbra3_bus_master_test:tb_penumbra3_bus_master \
    penumbra3_decode_test:tb_penumbra3_decode \
    penumbra3_fetch_buffer \
    penumbra3_fetch_replay \
    penumbra3_if1_stage \
    penumbra3_if2_stage_test:tb_penumbra3_if2_stage \
    penumbra3_id_stage_test:tb_penumbra3_id_stage \
    penumbra3_ex_stage_test:tb_penumbra3_ex_stage \
    penumbra3_mem1_stage_test:tb_penumbra3_mem1_stage \
    penumbra3_mem2_stage_test:tb_penumbra3_mem2_stage \
    penumbra3_wb_stage_test:tb_penumbra3_wb_stage \
    penumbra3_regfile \
    penumbra3_spr_file \
    penumbra3_scratch_file \
    penumbra3_spine_test:tb_penumbra3_spine \
    penumbra3_vecfetch \
    penumbra3_irq \
    penumbra3_core_test:tb_penumbra3_core \
    penumbra3_mmu \
    penumbra3_l1 \
    penumbra3_predecode \
    penumbra3_return_stack \
    penumbra3_direction_table \
    video_pattern_test \
    video_tmds_encoder \
    video_serializer \
    video_chain_test \
    video_font_rom \
    video_cell_ram \
    video_fb_ram \
    video_fb_palette \
    video_fbgen \
    video_textgen \
    video_pixel_chain \
    video_display \
    eth_crc32 \
    eth_mdio_test:tb_eth_mdio \
    eth_tx \
    eth_rx \
    eth_cdc \
    eth_regs \
    eth_loop_test:tb_eth_loop \
    eth_phy_rgmii_xc7_test:tb_eth_phy_rgmii_xc7 \
    eth_phy_reset_test:tb_eth_phy_reset \
    eth_addr_eeprom_test:tb_eth_addr_eeprom \
    ps2_test:tb_ps2 \
    usb_crc5 \
    usb_crc16 \
    usb_bit_stuff_tx \
    usb_nrzi_encode \
    usb_nrzi_decode \
    usb_bit_unstuff_rx \
    usb_serialize_tx \
    usb_deserialize_rx \
    usb_oversample_rx \
    usb_line_state \
    usb_rx_framing \
    usb_rx_test \
    usb_tx_framing \
    usb_tx_test \
    usb_loop_test \
    usbhc_pkt_tx \
    usbhc_pkt_rx \
    usbhc_txn \
    usbhc_frame_test:tb_usbhc_frame \
    usbhc_port \
    usbhc_mac_test:tb_usbhc_mac \
    usb_phy_pair_test:tb_usb_phy_pair \
    usbhc_stack_test:tb_usbhc_stack \
    usbhc_dev_test:tb_usbhc_dev \
    usbhc_wire_test:tb_usbhc_wire

# Variant-fork unit tests: a fork (penumbra2_5/) shares its base module's name,
# so the name-based search in test-modules resolves to the base. Name the source
# explicitly here. Fields: top-module : testbench : source.sv
# <top module>:<testbench>:<explicit sources, comma-separated>.  The
# list carries the top module, which Verilator does not hunt for on the
# -I path, and any forked file, which has to define its module before
# the same-named one on that path does.  Everything else still resolves
# from the library path as usual.
VARIANT_MODULE_TESTS = \
    penumbra2_ex_stage:tb_penumbra2_5_ex_stage:hw/rtl/penumbra2_5/penumbra2_ex_stage.sv \
    penumbra2_btb:tb_penumbra2_5_btb:hw/rtl/penumbra2_5/penumbra2_btb.sv \
    sdram_sim:tb_sdram_line_test:hw/rtl/sim/sdram_sim.sv,hw/rtl/io/sdram_line/sdram_bus_adapter.sv,hw/rtl/io/sdram_line/sdram_stack.sv \
    ddr3_stack_test:tb_ddr3_stack_test:hw/rtl/sim/ddr3_stack_test.sv,hw/rtl/io/sdram_line/sdram_bus_adapter.sv,hw/rtl/io/ddr3/ddr3_stack.sv,hw/rtl/sim/wb_line_mem.sv

# Built-in text-video font ROM image, read by video_font_rom via
# $readmemh: generated from the NetBSD console font bold8x16
# (public-domain glyph data, IBM/CP437 encoding). The root-level name
# follows the program.hex / microcode.hex convention.
font8x16.hex: netbsd/sys/dev/wsfont/bold8x16.h hw/tools/wsfont2hex.py
	python3 hw/tools/wsfont2hex.py $< $@

# Looping PCM clip for the audio bring-up probe, read by
# audio_sample_rom via $readmemh. AUDIO_WAV names source material;
# with none the tool synthesizes an arpeggio, so the probe builds
# without anyone having to supply a file. AUDIO_SAMPLES is the ROM
# depth and must stay a power of two — at 48 kHz the default is 2.7 s.
AUDIO_SAMPLES ?= 131072

# Channels the audio device is built with. Four is what a tracker
# replayer needs and so what the machine is built for; the cost against
# one channel is about 3% of mean CPU fmax and six block RAMs, and it
# lands entirely in placement pressure rather than on any audio path
# (doc/TODO/hw-core.md). Changing this changes the netlist, so the pinned
# placement seeds below only hold for the value set here.
AUDIO_CHANNELS ?= 4
AUDIO_WAV     ?=
audio_sample.hex: hw/tools/wav2sample.py $(AUDIO_WAV)
	python3 hw/tools/wav2sample.py $(AUDIO_WAV) -o $@ -n $(AUDIO_SAMPLES)

# Splash-screen cell image for the CPU-free display bring-up, read by
# video_cell_ram via $readmemh when preloaded (video_pixel_chain).
splash_cells.hex: hw/video/splash.txt hw/tools/text2cells.py
	python3 hw/tools/text2cells.py $< $@

.PHONY: test-modules test-modules-variant
test-modules: test-modules-variant font8x16.hex splash_cells.hex
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	@pass=0; fail=0; failed=""; \
	for entry in $(MODULE_TESTS); do \
		mod=$${entry%%:*}; \
		tb=$${entry#*:}; \
		[ "$$tb" = "$$entry" ] && tb=tb_$$mod; \
		if $(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_MODULE_FLAGS) \
			--top-module $$mod \
			--Mdir $(BUILD_DIR)/$$mod.verilator \
			-o ../V$$mod \
			$(PKG_SV) $$(find hw/rtl -name "$$mod.sv" $(MOD_FIND_PRUNE)) hw/sim/$$tb.cpp \
			>/dev/null 2>&1 && \
		   $(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$$mod $(DOCKER_IMAGE) \
			>/dev/null 2>&1; then \
			printf "  \033[32mPASS\033[0m  %s\n" "$$mod"; \
			pass=$$((pass + 1)); \
		else \
			printf "  \033[31mFAIL\033[0m  %s\n" "$$mod"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$mod"; \
		fi; \
	done; \
	echo ""; \
	total=$$((pass + fail)); \
	echo "$$pass/$$total module tests passed"; \
	if [ $$fail -gt 0 ]; then \
		echo "  *** $$fail FAILED:$$failed ***"; \
		exit 1; \
	fi

# ── Variant-fork unit tests (explicit source — see VARIANT_MODULE_TESTS) ──
test-modules-variant:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	@for e in $(VARIANT_MODULE_TESTS); do \
		m=$${e%%:*}; r=$${e#*:}; tb=$${r%%:*}; s=`echo $${r#*:} | tr , ' '`; \
		$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_BASE_FLAGS) -Ihw/rtl/penumbra2_5 $(VERILATOR_INCLUDES) --top-module $$m --Mdir $(BUILD_DIR)/$$tb.verilator -o ../V$$tb $(PKG_SV) $$s hw/sim/$$tb.cpp >/dev/null 2>&1 || { printf "  \033[31mBUILD FAIL\033[0m  %s\n" "$$tb"; exit 1; }; \
		$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$$tb $(DOCKER_IMAGE) || exit 1; \
	done

# ── Gate-level CDC hazard test (Icarus, via the FPGA container) ──
# Synthesizes each crossing module (sdram_cdc, eth_cdc) to ECP5 cells
# and event-simulates the mapped netlist with a routing delay on every
# cell input.  A clock-domain crossing whose failure mode is a
# combinational glitch is invisible to a zero-delay simulator, which
# evaluates a multi-bit encode atomically; reproducing one needs the
# netlist, per-pin delays and an event simulator together.  Harness in
# hw/sim/gate/, a testbench for each module.
#
# Runs GATE_SEEDS routing-delay draws of GATE_CYCLES system cycles each,
# plus a zero-delay control that must stay clean -- a violation there
# means the harness is measuring itself rather than the design.  A deep
# sweep is GATE_SEEDS=8 GATE_CYCLES=200000.
#
# GATE_WIDTHS lists sdram_cdc's response payload widths to cover.  The
# pointer handshake is width-independent, but the routing a wide payload
# needs is not, so every width the design ships belongs in the list.
GATE_DIR     = $(BUILD_DIR)/gate
GATE_SEEDS  ?= 2
GATE_CYCLES ?= 20000
GATE_WIDTHS ?= 32
GATE_CELLS   = hw/sim/gate/ecp5_cell_delays.v
GATE_TB      = hw/sim/gate/tb_sdram_cdc_gate.v

$(GATE_DIR)/cdc_sv2v.v: hw/rtl/io/sdram/sdram_cdc.sv
	@mkdir -p $(GATE_DIR)
	$(FPGA_TOOLS)/sv2v $< -w $@

# Make treats a pattern rule's output as an intermediate and deletes it
# once the goal is built, which would resynthesize on every run and
# leave no netlist to inspect after a failure.
.SECONDARY: $(GATE_WIDTHS:%=$(GATE_DIR)/cdc_gate_%.v) \
            $(GATE_WIDTHS:%=$(GATE_DIR)/sim_%.vvp)

$(GATE_DIR)/cdc_gate_%.v: $(GATE_DIR)/cdc_sv2v.v
	$(FPGA_TOOLS)/yosys -p "read_verilog $<; chparam -set RSP_BITS $* sdram_cdc; synth_ecp5 -top sdram_cdc; write_verilog -noattr $@"

$(GATE_DIR)/sim_%.vvp: $(GATE_DIR)/cdc_gate_%.v $(GATE_CELLS) $(GATE_TB)
	$(FPGA_TOOLS)/iverilog -g2012 -s tb_sdram_cdc_gate -s ecp5_delay_cfg \
		-Ptb_sdram_cdc_gate.RSP_BITS=$* \
		-o $@ $(GATE_CELLS) $(GATE_DIR)/cdc_gate_$*.v $(GATE_TB)

# eth_cdc.  GATE_ETH_WIDTHS lists the widths of the bundle it carries
# down, for the reason GATE_WIDTHS has: the Ethernet adapter builds a
# narrow crossing for its transmitter and a wide one for its receiver
# (the two structs in eth.sv).
#
# GATE_ETH_CLOCKS lists pairs of clock periods in ns, the bus clock's
# first.  This crossing's two clocks stand in no fixed relation, and
# what a run can catch depends on it: a device clock over three times
# the bus clock's rate always takes a bundle within the bus cycle that
# sent it, before anything on the bus side can change again, so a
# bundle that was not held still for it would pass.  The list holds a
# device clock much faster than the bus clock, one slower, and one just
# off twice its rate, where the phase between the two creeps through
# every alignment.
GATE_ETH_TB      = hw/sim/gate/tb_eth_cdc_gate.v
GATE_ETH_WIDTHS ?= 19 69
GATE_ETH_CLOCKS ?= 26.667:7.993 16.667:40.021 20.000:10.007

$(GATE_DIR)/eth_cdc_sv2v.v: hw/rtl/io/eth/eth_cdc.sv
	@mkdir -p $(GATE_DIR)
	$(FPGA_TOOLS)/sv2v $< -w $@

.SECONDARY: $(GATE_ETH_WIDTHS:%=$(GATE_DIR)/eth_cdc_gate_%.v) \
            $(GATE_ETH_WIDTHS:%=$(GATE_DIR)/eth_cdc_sim_%.vvp)

$(GATE_DIR)/eth_cdc_gate_%.v: $(GATE_DIR)/eth_cdc_sv2v.v
	$(FPGA_TOOLS)/yosys -p "read_verilog $<; chparam -set DOWN_W $* eth_cdc; synth_ecp5 -top eth_cdc; write_verilog -noattr $@"

$(GATE_DIR)/eth_cdc_sim_%.vvp: $(GATE_DIR)/eth_cdc_gate_%.v $(GATE_CELLS) $(GATE_ETH_TB)
	$(FPGA_TOOLS)/iverilog -g2012 -s tb_eth_cdc_gate -s ecp5_delay_cfg \
		-Ptb_eth_cdc_gate.DOWN_W=$* \
		-o $@ $(GATE_CELLS) $(GATE_DIR)/eth_cdc_gate_$*.v $(GATE_ETH_TB)

.PHONY: test-gate
test-gate: $(GATE_WIDTHS:%=$(GATE_DIR)/sim_%.vvp) \
           $(GATE_ETH_WIDTHS:%=$(GATE_DIR)/eth_cdc_sim_%.vvp)
	@fail=0; \
	for w in $(GATE_WIDTHS); do \
		for s in $$(seq 1 $(GATE_SEEDS)) zero; do \
			if [ "$$s" = zero ]; then a="+zero"; else a="+seed=$$s"; fi; \
			$(FPGA_TOOLS)/vvp $(GATE_DIR)/sim_$$w.vvp $$a +cycles=$(GATE_CYCLES) || fail=1; \
		done; \
	done; \
	for w in $(GATE_ETH_WIDTHS); do \
		for c in $(GATE_ETH_CLOCKS); do \
			for s in $$(seq 1 $(GATE_SEEDS)) zero; do \
				if [ "$$s" = zero ]; then a="+zero"; else a="+seed=$$s"; fi; \
				$(FPGA_TOOLS)/vvp $(GATE_DIR)/eth_cdc_sim_$$w.vvp $$a +cycles=$(GATE_CYCLES) \
					+tbus=$${c%%:*} +tdev=$${c##*:} || fail=1; \
			done; \
		done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "  *** gate-level CDC test FAILED ***"; exit 1; fi

# ── Aggregate: program tests + module tests ───────────────────
# Usage: make test-all
.PHONY: test-all
test-all: test test-modules test-rom test-gate


# ── Boot ROM host unit tests (fast, no Docker, no toolchain) ──
# The ROM's device-independent layers (FAT32) build natively against
# synthesized images.  See hw/rom/tests/.
# Usage: make test-rom
.PHONY: test-rom
test-rom:
	@$(MAKE) -C hw/rom test


# ── Run the conformance suite on the ISS (fast, no Docker) ────
# isa/ only: the ISS is the generation-independent ISA reference, so
# microarch-pinned suites do not gate it.
# Usage: make test-iss
.PHONY: test-iss
test-iss: $(ISS)
	@mkdir -p $(BUILD_DIR)/hex
	@python3 hw/tools/run-prog-tests.py --mode iss \
		--asm "$(PASM) --org 0xFFFF0000" --hexdir $(BUILD_DIR)/hex \
		--provides "$(ISS_PROVIDES)" --timeout 5 \
		--bin "iss=./$(ISS)" \
		$(ISA_PROGS)

# ── ISS option tests (fast, no Docker) ────────────────────────
# The ISS's own options (+core=, ...) against programs with known
# answers, the expected values written from the hardware's parameters.
# See sw/sim/tests/.
# Usage: make test-iss-options
.PHONY: test-iss-options
test-iss-options: $(ISS)
	@python3 sw/sim/tests/run_iss_tests.py --iss ./$(ISS) \
		--asm "$(PASM) --org 0xFFFF0000" --outdir $(BUILD_DIR)/iss-tests

# ── libc string routine tests (fast, no Docker) ───────────────
# The hand-written routines libc and the kernel share
# (netbsd/common/lib/libc/arch/penumbra/string/), checked on the ISS
# against reference loops at every alignment, with guard bytes around
# each destination.  See test/libc/.
# Usage: make test-libc
.PHONY: test-libc
test-libc: $(ISS)
	@python3 test/libc/run-string-test.py --cc "$(CC)" --objcopy "$(OBJCOPY)" \
		--iss ./$(ISS) --bin2hex sw/tools/bin2hex.py \
		--harness-dir $(HARNESS_DIR) \
		--src-dir netbsd/common/lib/libc/arch/penumbra/string \
		--build-dir $(BUILD_DIR)/test-libc

# ── Kernel MD routine tests (fast, no Docker) ─────────────────
# The port's routines that touch only registers and sysregs, assembled
# as the kernel build does and run on the ISS against their contracts.
# See test/kernel/.
# Usage: make test-kernel
.PHONY: test-kernel
test-kernel: $(ISS)
	@python3 test/kernel/run-kernel-test.py --cc "$(CC)" --objcopy "$(OBJCOPY)" \
		--iss ./$(ISS) --bin2hex sw/tools/bin2hex.py \
		--harness-dir $(HARNESS_DIR) \
		--port-dir netbsd/sys/arch/penumbra \
		--build-dir $(BUILD_DIR)/test-kernel

# ── Compiler Correctness Tests ─────────────────────────────────
# Runs curated tests from llvm-test-suite on ISS +hosted mode.
# Usage: make test-compiler [OPT="-O2"]
TEST_COMPILER_DIR = test/compiler
HARNESS_DIR      = $(TEST_COMPILER_DIR)/harness
LLVM_TEST_SUITE  = $(TEST_COMPILER_DIR)/llvm-test-suite
OPT             ?= -O2

# Link the compiler-rt archive built at the same optimization level as
# the tests, so the runtime exercises the same codegen paths the suite
# is checking (a runtime pinned to one level masks codegen bugs at the
# others).  Archives are built per level by sw/tools/setup-compiler-rt.sh.
RT_OPT := $(filter -O%,$(OPT))
ifeq ($(RT_OPT),)
RT_OPT := -O2
endif
COMPILER_RT_BUILTINS = build/compiler-rt-builtins-$(patsubst -%,%,$(RT_OPT))/lib/linux/libclang_rt.builtins-penumbra.a

# We only run UnitTests and Regression for now
# to keep the runtime reasonable.

# Exclude patterns live in test/compiler/excludes.txt (organized by
# category with comments).  See that file for what's excluded and why.
COMPILER_EXCLUDES_FILE := $(TEST_COMPILER_DIR)/excludes.txt
COMPILER_TEST_FLAGS_FILE := $(TEST_COMPILER_DIR)/test-flags.txt

# If COMPILER_TESTS is set, run only those specific files.  Otherwise
# walk the full UnitTests and Regression trees.
ifdef COMPILER_TESTS
COMPILER_TEST_ARGS := $(foreach t,$(COMPILER_TESTS),--test-file "$(t)")
else
COMPILER_TEST_ARGS := \
	--test-dir "$(LLVM_TEST_SUITE)/UnitTests" \
	--test-dir "$(LLVM_TEST_SUITE)/Regression" \
	--test-dir "$(LLVM_TEST_SUITE)/Benchmarks/Stanford" \
	--test-dir "$(TEST_COMPILER_DIR)/penumbra-abi" \
	--test-dir "$(TEST_COMPILER_DIR)/penumbra-codegen"
endif

.PHONY: test-compiler
test-compiler: $(ISS)
	@test -f "$(COMPILER_RT_BUILTINS)" || { \
		echo "error: $(COMPILER_RT_BUILTINS) not found —" \
		     "build it with: sw/tools/setup-compiler-rt.sh $(RT_OPT)"; \
		exit 1; }
	@$(PYTHON) $(TEST_COMPILER_DIR)/run-tests.py \
		$(COMPILER_TEST_ARGS) \
		"--opt=$(OPT)" \
		--harness-dir "$(HARNESS_DIR)" \
		--build-dir "$(BUILD_DIR)/test-compiler" \
		--iss "./$(ISS)" \
		--cc "$(CC)" \
		--objcopy "$(OBJCOPY)" \
		--bin2hex "sw/tools/bin2hex.py" \
		--builtins "$(COMPILER_RT_BUILTINS)" \
		--resource-dir "$$($(CC) -print-resource-dir)" \
		--exclude-file "$(COMPILER_EXCLUDES_FILE)" \
		--flags-file "$(COMPILER_TEST_FLAGS_FILE)" \
		--report "$(BUILD_DIR)/test-compiler-report.txt" \
		--jobs $$(nproc)

# PIC leg: same harness, tests compiled -fPIC and linked static at a
# fixed address (lld resolves the GOT at link time — no runtime
# relocator), so GOT-indirect global/jump-table/block-address codegen
# is exercised that the default static suite never reaches.  Curated
# tests live in test/compiler/penumbra-pic/.  Disjoint from the main
# suite — its excludes/flags files don't apply.
COMPILER_PIC_DIR := $(TEST_COMPILER_DIR)/penumbra-pic

.PHONY: test-compiler-pic
test-compiler-pic: $(ISS)
	@test -f "$(COMPILER_RT_BUILTINS)" || { \
		echo "error: $(COMPILER_RT_BUILTINS) not found —" \
		     "build it with: sw/tools/setup-compiler-rt.sh $(RT_OPT)"; \
		exit 1; }
	@$(PYTHON) $(TEST_COMPILER_DIR)/run-tests.py \
		--test-dir "$(COMPILER_PIC_DIR)" \
		"--opt=$(OPT)" \
		--pic \
		--harness-dir "$(HARNESS_DIR)" \
		--build-dir "$(BUILD_DIR)/test-compiler-pic" \
		--iss "./$(ISS)" \
		--cc "$(CC)" \
		--objcopy "$(OBJCOPY)" \
		--bin2hex "sw/tools/bin2hex.py" \
		--builtins "$(COMPILER_RT_BUILTINS)" \
		--resource-dir "$$($(CC) -print-resource-dir)" \
		--report "$(BUILD_DIR)/test-compiler-pic-report.txt" \
		--jobs $$(nproc)

# ── Interactive simulation (ISS — fast, instruction-level) ─────
# Builds boot ROM and runs through the ISS. No Docker needed.
# Usage: make simulate                    (interactive, default)
#        make simulate SDCARD=build/boot.img
#        make simulate TRACE=build/trace.log
#        make simulate TRACE=build/t.log TRACE_WINDOW=1000000 HALT_ON=DBLFLT
#        make simulate RAW=1                  (full raw TTY for job control)
#        make simulate ETH=loop               (attach an Ethernet adapter)
#        make simulate RAM=512M               (another board's RAM; also benchmark)
#        make simulate LLVM_PREFIX=/other/llvm/build

.PHONY: simulate
simulate: $(ISS)
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX)
	@$(ISS) program.hex $(if $(RAM),+ram=$(RAM)) $(if $(SDCARD),+sdcard=$(SDCARD)) $(if $(USBDEV),+usbdev=$(USBDEV)) $(if $(ETH),+eth=$(ETH)) $(if $(TRACE),+trace=$(TRACE)) $(if $(TRACE_WINDOW),+trace_window=$(TRACE_WINDOW)) $(if $(HALT_ON),'+halt_on=$(HALT_ON)') $(if $(RAW),+raw)

$(ISS): sw/sim/penumbra_iss.cpp sw/sim/penumbra_cores.h sw/sim/cache_model.h sw/sim/tlb_model.h sw/sim/sb_model.h sw/sim/bp_model.h sw/sim/issue_op.h sw/sim/pair_model.h sw/sim/loaduse_model.h hw/sim/usb_device_sim.h hw/sim/usb_msc_sim.h
	@$(MAKE) -C sw/sim

# ── RTL simulation (Verilator — cycle-accurate, slow) ─────────
# Full RTL simulation via Docker/Verilator. Use for hardware
# verification or when cycle-accurate behavior matters.
# Usage: make simulate-rtl
#        make simulate-rtl INTERACTIVE=0   (non-interactive, piped input)
#        make simulate-rtl SDCARD=build/boot.img
#        make simulate-rtl TRACE=build/trace.log TRACE_WINDOW=1000000
#                                              (rolling last-N-instructions trace)
#        make simulate-rtl HALT_ON='user signal:'
#                                              (auto-exit on UART pattern match)
#        make simulate-rtl STDIN_FILE=boot.txt
#                                              (deterministic keystroke replay
#                                               from file at fixed cycle cadence,
#                                               then falls back to live stdin)
ifeq ($(INTERACTIVE),0)
DOCKER_RUN_IT = docker run --rm -u $(shell id -u):$(shell id -g) -i -v $(CURDIR):/work -w /work
else
DOCKER_RUN_IT = docker run --rm -u $(shell id -u):$(shell id -g) -it -v $(CURDIR):/work -w /work
endif

# simulate-rtl and benchmark-rtl are CORE-aware: both cores share the
# sim_console.cpp frontend behind a per-core shim. gen1 uses machine_sim +
# tb_penumbra1_interactive and needs microcode.hex; gen2 uses
# machine_penumbra2_sim + tb_penumbra2_interactive and has no microcode (CORE
# defaults to penumbra2_5, set above). Both wrappers load the boot ROM from
# program.hex (their INIT_FILE default), built by hw/rom.
ifeq ($(CORE_BASE),penumbra2)
SIMRTL_TOP    := machine_penumbra2_sim
SIMRTL_OUT    := V$(CORE)$(SIM_MEM_TAG)_machine_penumbra2_sim_interactive
SIMRTL_TOPSV  := $(call sim_wrapper_sv,machine_penumbra2_sim)
SIMRTL_TB     := hw/sim/tb_penumbra2_interactive.cpp
# gen2's machine_penumbra2_sim backs RAM with the full sdram_sim stack (32 MB;
# ddr3_sim's 512 MB under SIM_MEM=ddr3), so a real boot image fits with no RAM
# parameter override. The CPU variant flows in via the same -D as the
# test/fpga builds, so penumbra2_5 reports "Penumbra/2.5" in the ROM banner
# here too.
SIMRTL_GFLAGS := -DPENUMBRA_CPU_VARIANT=$(CPU_VARIANT)
else
SIMRTL_TOP    := machine_sim
SIMRTL_OUT    := V$(CORE)_machine_sim_interactive
SIMRTL_TOPSV  := hw/rtl/sim/machine_sim.sv
SIMRTL_TB     := hw/sim/tb_penumbra1_interactive.cpp
SIMRTL_GFLAGS :=
endif

.PHONY: simulate-rtl
simulate-rtl:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) $(SIMRTL_GFLAGS) \
		--top-module $(SIMRTL_TOP) \
		--Mdir $(BUILD_DIR)/$(CORE)$(SIM_MEM_TAG)_$(SIMRTL_TOP)_interactive.verilator \
		-o ../$(SIMRTL_OUT) \
		$(PKG_SV) $(SIMRTL_TOPSV) $(SIMRTL_TB) hw/sim/sim_console.cpp
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX)
ifneq ($(CORE_BASE),penumbra2)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
endif
	@$(DOCKER_RUN_IT) --entrypoint ./$(BUILD_DIR)/$(SIMRTL_OUT) $(DOCKER_IMAGE) $(if $(SDCARD),+sdcard=$(SDCARD)) $(if $(TRACE),+trace=$(TRACE)) $(if $(TRACE_WINDOW),+trace_window=$(TRACE_WINDOW)) $(if $(HALT_ON),'+halt_on=$(HALT_ON)') $(if $(STDIN_FILE),+stdin_file=$(STDIN_FILE)) $(if $(PIPE_LO),+pipe_lo=$(PIPE_LO)) $(if $(PIPE_HI),+pipe_hi=$(PIPE_HI))

# ── Artifacts the SD images are built from ─────────────────────
# Prerequisites: kernel and bootloader already built (see CLAUDE.md),
# and `build.sh distribution` run for anything carrying a root
# filesystem.
BOOT_ELF   := $(BUILD_DIR)/netbsd-obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF
# Which kernel config the SD images carry.  GENERIC.DEBUG (consistency
# checks on) is the development default; build with KERNCONF=GENERIC
# for demo/performance images, KERNCONF=MINIMAL for the floor config.
KERNCONF   ?= GENERIC.DEBUG
KERNEL     := $(BUILD_DIR)/netbsd-obj/sys/arch/penumbra/compile/$(KERNCONF)/netbsd
DESTDIR    := $(BUILD_DIR)/netbsd-dest

# Location of the cross-built NetBSD-hosted binaries, staged into the
# rootfs via the overlay step below: pbench here, the graphics demos
# under their own directory.
NETBSD_BENCH_DIR := $(BUILD_DIR)/netbsd-bench
DEMOS_DIR        := $(BUILD_DIR)/demos

# ── Custom NetBSD userland overlays ───────────────────────────────
# Each custom utility installs into a shared overlay fake-root via its
# own `overlay' target; the image build copies the whole tree into the
# image.  Add a utility by giving it an `overlay' target and adding it
# to NETBSD_OVERLAYS.
OVERLAY_ROOT    := $(BUILD_DIR)/netbsd-overlay
NETBSD_OVERLAYS := benchmark-overlay demos-overlay penmon-overlay \
                   exhibit-launcher-overlay doom-overlay pmusic-test-overlay \
                   modplay-overlay fasttelnetd-overlay local-overlay

.PHONY: netbsd-overlay benchmark-overlay demos-overlay penmon-overlay penmon
.PHONY: demos
.PHONY: exhibit-launcher-overlay exhibit-launcher
.PHONY: doom-overlay doom
.PHONY: pmusic-test-overlay pmusic-test local-overlay
.PHONY: modplay-overlay modplay modplay-dump
.PHONY: fasttelnetd-overlay
netbsd-overlay:
	rm -rf $(OVERLAY_ROOT)
	@$(MAKE) $(NETBSD_OVERLAYS)

benchmark-overlay:
	@$(MAKE) -C benchmark/netbsd-bench LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

demos-overlay:
	@$(MAKE) -C sw/demos LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

penmon-overlay:
	@$(MAKE) -C sw/penmon LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

exhibit-launcher-overlay:
	@$(MAKE) -C sw/exhibit-launcher LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

doom-overlay:
	@$(MAKE) -C sw/doom LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

pmusic-test-overlay:
	@$(MAKE) -C sw/pmusic-test LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

modplay-overlay:
	@$(MAKE) -C sw/modplay LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

fasttelnetd-overlay:
	@$(MAKE) -C sw/fasttelnetd LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

# Files for the image that stay out of the repository: whatever sits
# under local-overlay/ lands at the same path in the root filesystem.
# The image's file list takes one path per word, so a name with
# whitespace is refused here rather than split there.
LOCAL_OVERLAY := local-overlay
local-overlay:
	@spaced=$$(find $(LOCAL_OVERLAY) -name '*[[:space:]]*'); \
	if [ -n "$$spaced" ]; then \
		echo "local-overlay: rename these, the image can't take" \
		    "whitespace in a name:" >&2; \
		echo "$$spaced" >&2; \
		exit 1; \
	fi
	@install -d $(OVERLAY_ROOT)
	@tar -C $(LOCAL_OVERLAY) --exclude=./.gitignore -cf - . | \
		tar -C $(OVERLAY_ROOT) -xf -

demos:
	@$(MAKE) -C sw/demos LLVM_PREFIX=$(LLVM_PREFIX) DESTDIR=$(abspath $(DESTDIR))
	@echo "demo binaries: $(DEMOS_DIR)/<name>{,-static}"

doom:
	@$(MAKE) -C sw/doom LLVM_PREFIX=$(LLVM_PREFIX) DESTDIR=$(abspath $(DESTDIR))
	@echo "doom binary: $(BUILD_DIR)/doom/doom"

pmusic-test:
	@$(MAKE) -C sw/pmusic-test LLVM_PREFIX=$(LLVM_PREFIX) DESTDIR=$(abspath $(DESTDIR))
	@echo "pmusic-test binary: $(BUILD_DIR)/pmusic-test/pmusic-test"

modplay:
	@$(MAKE) -C sw/modplay LLVM_PREFIX=$(LLVM_PREFIX) DESTDIR=$(abspath $(DESTDIR))
	@echo "modplay binary: $(BUILD_DIR)/modplay/modplay"

# The same parser and replayer on the build host, walking a song
# without playing it: `make modplay-dump', then
# build/modplay/modplay-dump [-r] file.mod
modplay-dump:
	@$(MAKE) -C sw/modplay dump

penmon:
	@$(MAKE) -C sw/penmon LLVM_PREFIX=$(LLVM_PREFIX) DESTDIR=$(abspath $(DESTDIR))

exhibit-launcher:
	@$(MAKE) -C sw/exhibit-launcher LLVM_PREFIX=$(LLVM_PREFIX) DESTDIR=$(abspath $(DESTDIR))

# ── NetBSD SD image via the in-tree mkimage machinery ─────────────
# distrib/utils/embedded/mkimage builds the entire card — MBR, FAT32
# boot partition, FFS root and disklabel — from the distribution in
# DESTDIR, driven by distrib/utils/embedded/conf/penumbra.conf.  The
# cross-tools it shells out to are passed through the TOOL_* variables
# it resolves them with, so nothing depends on host packages.
MKIMAGE   := netbsd/distrib/utils/embedded/mkimage
NBTOOLBIN := $(abspath $(BUILD_DIR)/netbsd-tools/bin)
# FLAVOR selects a conf under distrib/utils/embedded/conf/.  They all
# carry a penumbra_ prefix, which the flavor name leaves off; an empty
# FLAVOR is the base image the others are derived from:
#   make image                   — boots to a single-user shell
#   make image FLAVOR=multiuser  — runs /etc/rc, getty per tty
#   make image FLAVOR=guest      — multiuser, on the network: a DHCP
#                                  client, telnetd and a guest account
FLAVOR     ?=
IMAGE_CONF := penumbra$(if $(FLAVOR),_$(FLAVOR))
PENIMAGE   ?= $(BUILD_DIR)/$(IMAGE_CONF).img

# The makefs spec comes from the set lists under DESTDIR/etc/mtree, which
# the distribution build regenerates as its last step; makefs refuses to
# read a file whose size disagrees with its spec entry.  Installing into
# DESTDIR without a full distribution build leaves the lists describing
# the previous one, and that surfaces here as a size mismatch on whichever
# binaries changed (DEVELOP.md covers repairing it).
.PHONY: image
image: netbsd-overlay bench-elfs demoscene-elfs
	@HOST_SH=/bin/sh MACHINE=penumbra MACHINE_ARCH=penumbra \
	    TOOL_MAKEFS=$(NBTOOLBIN)/nbmakefs \
	    TOOL_MTREE=$(NBTOOLBIN)/nbmtree \
	    TOOL_DISKLABEL=$(NBTOOLBIN)/nbdisklabel \
	    TOOL_GPT=$(NBTOOLBIN)/nbgpt \
	    TOOL_FDISK=$(NBTOOLBIN)/penumbra-unknown-netbsd-fdisk \
	    TOOL_PWD_MKDB=$(NBTOOLBIN)/nbpwd_mkdb \
	    penboot=$(abspath $(BOOT_ELF)) \
	    kernel=$(abspath $(KERNEL)) \
	    rootoverlay=$(abspath $(OVERLAY_ROOT)) \
	    bootextras=$(abspath $(BOOTEXTRAS_DIR)) \
	    sh $(MKIMAGE) -h $(IMAGE_CONF) -B le \
		-D $(abspath $(DESTDIR)) $(abspath $(PENIMAGE))
	@echo "SD image: $(PENIMAGE)"

# Cross-built NetBSD-hosted benchmark suite (pbench).
# Builds dynamic and static binaries against the NetBSD sysroot.
# `make image` already overlays the dynamic binaries into
# /usr/local/bin via the netbsd-overlay step; run this target on its own
# only to build/measure the binaries standalone.
#
#   make benchmark-netbsd   — build dynamic + static binaries
.PHONY: benchmark-netbsd
benchmark-netbsd:
	@$(MAKE) -C benchmark/netbsd-bench LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR))
	@echo "pbench binaries: $(NETBSD_BENCH_DIR)/pbench{,-static}"

# ── Benchmark SD image and runners ───────────────────────────
# Builds benchmark ELFs and creates an SD image containing them.
# Usage:
#   make sdimage-bench                       — build benchmarks + SD image
#   make benchmark                           — run all benchmarks on ISS (fast)
#   make benchmark-rtl                       — run all benchmarks on Verilator (cycle-accurate)
#   make benchmark BENCH_ITERS=10            — override Dhrystone iteration count
#
# BENCH_ITERS is an opt-in passthrough.  The actual default lives in
# benchmark/Makefile (DHRYSTONE_ITERATIONS) so there's a single source
# of truth — top-level only forwards when the user supplies a value.
BENCH_IMG      := $(BUILD_DIR)/bench.img

# Bare-metal ELFs for an SD card's FAT32 root, beside PENBOOT.ELF: both the
# standalone bench image (sdimage-bench) and the NetBSD image's boot
# partition (image) are built from it, so both depend on every target
# that stages into it.
BOOTEXTRAS_DIR := $(BUILD_DIR)/bootextras

# Build the bare-metal benchmark ELFs and stage them in BOOTEXTRAS_DIR.
# Always rebuilds: the sources are small, compile quickly, and `make` can't
# see when the compiler itself has changed under it.
.PHONY: bench-elfs
bench-elfs:
	@$(MAKE) -C benchmark clean
	@$(MAKE) -C benchmark $(if $(BENCH_ITERS),DHRYSTONE_ITERATIONS=$(BENCH_ITERS)) LLVM_PREFIX=$(LLVM_PREFIX)
	@mkdir -p $(BOOTEXTRAS_DIR)
	@cp $(BUILD_DIR)/benchmark/*.ELF $(BOOTEXTRAS_DIR)/ 2>/dev/null || true

# Bare-metal demoscene productions, each self-contained under sw/demoscene/
# with its own `stage' target.  Add one by listing it in DEMOSCENE.
# Always rebuilds, for the same reason as bench-elfs.
DEMOSCENE := sw/demoscene/skrolli2026

.PHONY: demoscene-elfs
demoscene-elfs:
	@for d in $(DEMOSCENE); do \
	    $(MAKE) -C $$d clean && \
	    $(MAKE) -C $$d LLVM_PREFIX=$(LLVM_PREFIX) \
	        STAGE_DIR=$(abspath $(BOOTEXTRAS_DIR)) stage || exit 1; \
	done

.PHONY: sdimage-bench
sdimage-bench: bench-elfs demoscene-elfs
	@sw/tools/mksdimage.sh -o $(BENCH_IMG) -e $(BOOTEXTRAS_DIR) -v
	@echo "Benchmark SD image: $(BENCH_IMG)"

# List of benchmark ELF names (FAT32 8.3 format, no path).
# MEMTEST is a correctness check; MEMBENCH and DHRYSTON are perf measurements.
BENCH_ELFS := DHRYSTON.ELF MEMTEST.ELF MEMBENCH.ELF

# COREMARK sizes its own runs to the ten seconds its run rules ask for:
# seconds of ISS time, where it checks its results against known CRCs,
# and hours of RTL simulation, so only the ISS loop runs it.
BENCH_ELFS_ISS := $(BENCH_ELFS) COREMARK.ELF

.PHONY: benchmark
benchmark: sdimage-bench $(ISS)
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX)
	@# +halt-on-break: each benchmark ends with a BREAK "done" sentinel.
	@# Without it the interactive ISS traps that BREAK into the ROM monitor
	@# and blocks on stdin, so the loop would never reach the next ELF.
	@for elf in $(BENCH_ELFS_ISS); do \
		echo "═══ Running $$elf on ISS ═══"; \
		echo "boot sd:0,0/$$elf" | $(ISS) program.hex $(if $(RAM),+ram=$(RAM)) +sdcard=$(BENCH_IMG) +halt-on-break \
			|| echo "*** $$elf FAILED ***"; \
		echo ""; \
	done

.PHONY: benchmark-rtl
benchmark-rtl: sdimage-bench
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) $(SIMRTL_GFLAGS) \
		--top-module $(SIMRTL_TOP) \
		--Mdir $(BUILD_DIR)/$(CORE)$(SIM_MEM_TAG)_$(SIMRTL_TOP)_interactive.verilator \
		-o ../$(SIMRTL_OUT) \
		$(PKG_SV) $(SIMRTL_TOPSV) $(SIMRTL_TB) hw/sim/sim_console.cpp
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX)
ifneq ($(CORE_BASE),penumbra2)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
endif
	@for elf in $(BENCH_ELFS); do \
		echo "═══ Running $$elf on RTL sim ═══"; \
		echo "boot sd:0,0/$$elf" | \
			docker run --rm -u $(shell id -u):$(shell id -g) -i -v $(CURDIR):/work -w /work \
			--entrypoint ./$(BUILD_DIR)/$(SIMRTL_OUT) \
			$(DOCKER_IMAGE) +sdcard=$(BENCH_IMG) \
			|| echo "*** $$elf FAILED ***"; \
		echo ""; \
	done

# ── FPGA build (OSS CAD Suite via Docker wrappers) ────────────
FPGA_TOOLS = hw/tools/oss-cad-suite/bin
FPGA_RTL   = hw/rtl/fpga

# ── Vivado image (Xilinx 7-series boards) ──────────────────────
# AMD publishes no Vivado image and its EULA forbids redistributing one,
# so this builds one locally with your own AMD login, and it is never
# pushed. The image is the archive of the working toolchain (`docker
# save` backs it up): rebuilding it needs AMD to still serve the pinned
# release. AMD's installer only ever runs in the Dockerfile's `base`
# stage, never on the host.
VIVADO_TOOLS = hw/tools/vivado
include $(VIVADO_TOOLS)/image.env

# VIVADO_DIR keeps everything per-release outside the repo: the web
# installer .bin as downloaded, its unpacked copy, and the HOME where
# the installer keeps its login token.
VIVADO_DIR        ?= $(HOME)/xilinx/$(VIVADO_VERSION)
VIVADO_BASE_IMAGE  = penumbra-vivado-base:$(VIVADO_VERSION)
VIVADO_WEB_BIN     = $(wildcard $(VIVADO_DIR)/FPGAs_AdaptiveSoCs_Unified_*_$(VIVADO_VERSION)_*_Lin64.bin)
VIVADO_TOKEN       = $(VIVADO_DIR)/home/.Xilinx/wi_authentication_key
VIVADO_SETUP_RUN   = docker run --rm -u $(shell id -u):$(shell id -g) \
                     -e HOME=/x/home -v $(VIVADO_DIR):/x -w /x

.PHONY: vivado-base-image vivado-authenticate vivado-image
vivado-base-image:
	docker build --quiet --target base -t $(VIVADO_BASE_IMAGE) $(VIVADO_TOOLS) >/dev/null

# Unpacked once per release directory, so the archive's own (older)
# mtimes never make it look stale against the .bin. Unpacked beside the
# target and renamed, so an interrupted run leaves nothing half-done.
$(VIVADO_DIR)/webinstaller/xsetup: | vivado-base-image
	@test $(words $(VIVADO_WEB_BIN)) = 1 || { \
		echo "$(VIVADO_DIR) must hold exactly one Vivado $(VIVADO_VERSION) Linux web installer .bin" >&2; \
		exit 1; }
	rm -rf $(VIVADO_DIR)/webinstaller $(VIVADO_DIR)/webinstaller.tmp
	$(VIVADO_SETUP_RUN) --network none $(VIVADO_BASE_IMAGE) \
		sh /x/$(notdir $(VIVADO_WEB_BIN)) --keep --noexec --target /x/webinstaller.tmp >/dev/null
	mv $(VIVADO_DIR)/webinstaller.tmp $(VIVADO_DIR)/webinstaller

# The token the web installer's downloads need, valid for a week, so
# only an image rebuild ever needs a fresh one. It asks for your AMD
# account credentials, so it needs a terminal. The installer leaves the
# token read-only; make it writable so a refresh can replace it.
vivado-authenticate: vivado-base-image $(VIVADO_DIR)/webinstaller/xsetup
	@mkdir -p $(VIVADO_DIR)/home
	@chmod -f u+w $(VIVADO_TOKEN) || true
	$(VIVADO_SETUP_RUN) -it $(VIVADO_BASE_IMAGE) /x/webinstaller/xsetup -b AuthTokenGen

# The install downloads only the Artix-7 modules install_config.txt
# selects. Its last step takes a trivial design for our part to a
# bitstream, so a wrong release, a missing device family or a license
# prompt fails the image build rather than the first real one. zstd
# because the install layer is tens of GB, which single-threaded gzip
# compresses slowly.
vivado-image: $(VIVADO_DIR)/webinstaller/xsetup
	@test -f $(VIVADO_TOKEN) || { \
		echo "no AMD login token in $(VIVADO_DIR); run make vivado-authenticate" >&2; \
		exit 1; }
	docker build --build-context installer=$(VIVADO_DIR)/webinstaller \
		--secret id=amd_token,src=$(VIVADO_TOKEN) \
		--build-arg VIVADO_VERSION=$(VIVADO_VERSION) \
		--output type=image,name=$(VIVADO_IMAGE),compression=zstd \
		$(VIVADO_TOOLS)

# ── openFPGALoader image (every board's programmer) ────────────
# Built from a pinned upstream commit rather than taken from the OSS
# CAD Suite image, whose release cannot parse current Vivado
# bitstreams.
OFL_TOOLS = hw/tools/openfpgaloader
include $(OFL_TOOLS)/image.env

.PHONY: openfpgaloader-image
openfpgaloader-image:
	docker build --build-arg OFL_COMMIT=$(OFL_COMMIT) -t $(OFL_IMAGE) $(OFL_TOOLS)

# ── FPGA build matrix (doc/internals/build-system.md) ──────────
# Porcelain: make fpga BOARD=<board> CORE=<generation> [VARIANT=<v>]
# expands to TOP=<board>_<core>[_<variant>]_top, whose file lives at
# hw/rtl/fpga/<board>/<top>.sv and whose source set composes from the
# per-axis variables below. TOP=<module> remains the low-level escape
# hatch: registry tops get their composed sources, anything else falls
# back to the bare fpga/*.sv set (simple test tops).

# Per-axis source sets. common/ holds the shared package (must come
# first) and the generation-shared modules both cores instantiate.
SRC_COMMON = hw/rtl/common/penumbra_pkg.sv \
             $(filter-out %/penumbra_pkg.sv, $(wildcard hw/rtl/common/*.sv))

# The audio device, board-neutral down to the frames it hands an output
# stage and the frame requests it takes back.
SRC_AUDIO = hw/rtl/io/audio/audio.sv \
            hw/rtl/io/audio/audio_chan_fifo.sv \
            hw/rtl/io/audio/audio_mixer.sv

# The Ethernet device up to the seam a board's PHY adapter attaches at.
SRC_ETH = hw/rtl/io/eth/eth.sv \
          hw/rtl/io/eth/eth_regs.sv \
          hw/rtl/io/eth/eth_mdio.sv \
          hw/rtl/io/eth/eth_cdc.sv \
          hw/rtl/io/eth/eth_slot_ram.sv \
          hw/rtl/io/eth/eth_rx.sv \
          hw/rtl/io/eth/eth_tx.sv \
          hw/rtl/io/eth/eth_crc32.sv

# The ULX3S's output stage: frames requested off a fixed clock, the
# modulator and the resistor ladder. The probe's tone and clip sources
# are deliberately absent: they belong to the bring-up top, and
# audio_sample_rom would want a hex image no machine build produces.
SRC_AUDIO_LADDER = hw/rtl/io/audio/audio_rate_divider.sv \
                   hw/rtl/io/audio/audio_sigma_delta.sv \
                   hw/rtl/io/audio/audio_ladder_out.sv

# ── SDRAM stack selection ───────────────────────────────────────
# Which shape of SDRAM stack a top builds:
#   word — a controller command answers one word; the adapter hides
#          latency by speculating on the next address.
#   line — a command answers a whole burst; the adapter holds it, so
#          the rest of a sequential run needs no controller traffic.
#
# The two define the same modules with the same ports, so choosing is a
# fileset substitution: no board top mentions it and none has to change.
# SDRAM_LINE_FORKED names which files the line variant overrides, and
# the filter-out drops those from the base set so the variant's copies
# are the only definitions — a missing entry surfaces as a duplicate
# module error rather than a silently mixed build.
# Which stack a given top defaults to is pinned beside its placement
# seed, since both are answers to the same question about that artifact.
SDRAM_LINE_FORKED := sdram_bus_adapter sdram_stack

SRC_SDRAM_word = $(filter-out %/sdram_pkg.sv, $(wildcard hw/rtl/io/sdram/*.sv))
SRC_SDRAM_line = $(filter-out $(SDRAM_LINE_FORKED:%=hw/rtl/io/sdram/%.sv),$(SRC_SDRAM_word)) \
                 $(wildcard hw/rtl/io/sdram_line/*.sv)

# An unknown stack names no fileset, which would otherwise synthesize a
# top with no SDRAM at all.  The check expands with the variable rather
# than at parse time, because the default depends on TOP.
SRC_SDRAM = $(or $(SRC_SDRAM_$(SDRAM_STACK)),\
                 $(error SDRAM_STACK must be `word` or `line`, got `$(SDRAM_STACK)`))

# The display adapter, board-neutral down to the DDR bit pairs a board
# top's output cells take.
SRC_VIDEO = hw/rtl/io/video/video_pkg.sv \
            $(filter-out %/video_pkg.sv, $(wildcard hw/rtl/io/video/*.sv))

SRC_FABRIC = hw/rtl/io/sdram/sdram_pkg.sv \
             $(SRC_AUDIO) \
             $(SRC_AUDIO_LADDER) \
             hw/rtl/io/usb/usb_pkg.sv \
             hw/rtl/io/video/video_pkg.sv \
             $(wildcard hw/rtl/soc/*.sv) \
             $(wildcard hw/rtl/io/*.sv) \
             $(SRC_SDRAM) \
             $(filter-out %/usb_pkg.sv, $(wildcard hw/rtl/io/usb/*.sv)) \
             $(filter-out %/video_pkg.sv, $(wildcard hw/rtl/io/video/*.sv))

SRC_CORE_penumbra1 = $(wildcard hw/rtl/penumbra1/*.sv)
SRC_CORE_penumbra2 = hw/rtl/penumbra2/penumbra2_pkg.sv \
                     $(filter-out %/penumbra2_pkg.sv, $(wildcard hw/rtl/penumbra2/*.sv))

# gen3 (penumbra3): a full generation fork -- its own core AND its own
# on-chip memory hierarchy down to a registered bus master (see
# doc/internals/penumbra3/overview.md). Not a composition sub-variant of
# gen2: it shares only common/, the bus devices, and the board shell. pkg
# first, then the rest of the directory, mirroring gen2.
SRC_CORE_penumbra3 = hw/rtl/penumbra3/penumbra3_pkg.sv \
                     $(filter-out %/penumbra3_pkg.sv, $(wildcard hw/rtl/penumbra3/*.sv))

# gen2.5 (penumbra2_5): composition over gen2 — share gen2's leaf cells and
# fork only the integration chain into penumbra2_5/. PENUMBRA2_FORKED is the
# single list naming which gen2 files the variant overrides; the filter-out
# drops them from the gen2 set so the variant's same-named copies are the only
# definitions (a missing entry surfaces as a loud duplicate-module error).
PENUMBRA2_FORKED := penumbra2_core penumbra2_spine penumbra2_id_stage penumbra2_ex_stage penumbra2_if2_stage
SRC_CORE_penumbra2_5 = $(filter-out $(PENUMBRA2_FORKED:%=hw/rtl/penumbra2/%.sv),$(SRC_CORE_penumbra2)) \
                       $(wildcard hw/rtl/penumbra2_5/*.sv)

# CORE-aware core fileset for the fpga tops: a sub-variant overrides, a bare
# core falls back to its base set. (The sim build resolves modules from the -I
# library path instead, with the variant dir prepended above, so penumbra2_5/
# shadows penumbra2/ there without any list.)
SRC_CORE = $(or $(SRC_CORE_$(CORE)),$(SRC_CORE_$(CORE_BASE)))

# The gen2 machine's closure: the integration module plus everything it
# binds beyond the core — the sysreg devices + L2 from the shared soc/
# directory (also wildcarded by SRC_FABRIC — a top composing both should
# $(sort) its source set to dedupe). The gen2 MMU stack now lives in
# penumbra2/ (covered by SRC_CORE_penumbra2), so it is not listed here.
SRC_MACHINE_penumbra2 = hw/rtl/machine/machine_penumbra2.sv \
                        hw/rtl/soc/cpuid.sv hw/rtl/soc/machid.sv \
                        hw/rtl/soc/timer.sv hw/rtl/soc/busctl.sv \
                        hw/rtl/soc/l2_cache.sv \
                        hw/rtl/soc/cache_perfctr.sv

# Board-common helpers only — each registry entry names its own top
# file, so sibling tops never leak into each other's builds. The PLL
# config package comes first (both ULX3S tops import it to derive the
# EHXPLLL dividers from their clock targets).
SRC_BOARD_ulx3s = $(FPGA_RTL)/ecp5_pll_pkg.sv
LPF_ulx3s       = hw/constraints/ulx3s_v20.lpf

# The Nexys Video's clocking, power-on reset and thermal guard, shared
# by its tops; the MMCM package comes first because the clock/reset
# module imports it.
SRC_BOARD_nexysvideo = $(FPGA_RTL)/xc7/xc7_mmcm_pkg.sv \
                       $(FPGA_RTL)/nexysvideo/nexysvideo_clk_rst.sv \
                       $(FPGA_RTL)/xc7/xc7_thermal_guard.sv

# Every Nexys machine top's constraints, whatever its core: the board
# ports the machine tops share, then the DDR3's.
XDC_MACHINE_nexysvideo = hw/constraints/nexysvideo_machine.xdc $(XDC_DDR3_nexysvideo)

# Each board's device family picks the flow that builds it: ecp5
# (yosys, nextpnr-ecp5, ecppack) or xc7 (Vivado). Every board is
# programmed with openFPGALoader.
FAMILY_ulx3s      = ecp5
FAMILY_nexysvideo = xc7

# The name openFPGALoader knows each board by, and a Xilinx board's
# Vivado part.
OFL_BOARD_ulx3s      = ulx3s
OFL_BOARD_nexysvideo = nexysVideo
PART_nexysvideo      = xc7a200tsbg484-1

# An xc7 board's master XDC holds the vendor's pin constraints, all
# commented out, and its memory interface generator project the DRAM's
# pins the master XDC leaves out. Vivado flags a constraint on a port
# the top lacks, so each top has its own XDC_<top> with the master lines
# it uses, uncommented, plus its design constraints;
# hw/tools/xdc_check.py checks those lines against the masters on every
# xc7 build.
XDC_MASTER_nexysvideo = hw/third_party/digilent-xdc/Nexys-Video-Master.xdc \
                        hw/third_party/digilent-boards/nexys_video_mig.prj
XDC_MASTER = $(XDC_MASTER_$(FPGA_BOARD))
XDC = $(if $(XDC_$(TOP)),$(XDC_$(TOP)),$(XDC_$(TOP_MODULE)))

# The registry: every valid (board, core[, variant]) top, its composed
# source set, and which hex images it embeds. Adding a combination
# means adding its top file under hw/rtl/fpga/<board>/ and its
# entries here — an unknown combination is a hard error, not a
# silently empty source list.
FPGA_TOPS = ulx3s_penumbra1_top ulx3s_penumbra2_probe_top ulx3s_penumbra2_top \
            ulx3s_penumbra2_5_top ulx3s_penumbra3_probe_memtlb_top \
            ulx3s_penumbra3_probe_issue_top ulx3s_penumbra3_probe_decode_top \
            ulx3s_penumbra3_probe_l1tag_top \
            nexysvideo_penumbra2_top nexysvideo_penumbra2_5_top

FPGA_SRC_ulx3s_penumbra1_top = $(SRC_COMMON) $(SRC_CORE_penumbra1) \
                               $(SRC_FABRIC) $(SRC_BOARD_ulx3s) \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra1_top.sv

# The gen2 probe is the machine (core + MMU + VIPT L1s + arbiter +
# fill sequencer + L2) against a small BRAM bus memory — the timing
# instrument that puts the IF2 tag-compare / way-mux path and the rest
# of the memory system in front of nextpnr (no board fabric/devices).
FPGA_SRC_ulx3s_penumbra2_probe_top = $(SRC_COMMON) $(SRC_CORE) \
                                     $(SRC_MACHINE_penumbra2) \
                                     hw/rtl/soc/bus_response_fabric.sv \
                                     hw/rtl/soc/bus_device_simple.sv \
                                     hw/rtl/sim/unified_bus_mem.sv \
                                     $(FPGA_RTL)/ulx3s/ulx3s_penumbra2_probe_top.sv

# The gen2 full board system: machine_penumbra2 wired to real board fabric
# (SDRAM v2 stack + boot ROM + UART + SPI/autoconfig). SRC_FABRIC already
# wildcards mmu/ + soc/ + io/, so it covers the machine's whole closure and
# the peripherals; only machine_penumbra2.sv itself is added on top. No
# $(sort) needed — the per-axis sets keep their package files first.
FPGA_SRC_ulx3s_penumbra2_top = $(SRC_COMMON) $(SRC_CORE) \
                               $(SRC_FABRIC) $(SRC_BOARD_ulx3s) \
                               hw/rtl/machine/machine_penumbra2.sv \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra2_top.sv

# gen3 Phase-0 probe P0.3: the MEM1/MEM2 + BRAM-TLB translate cone in front
# of nextpnr (make timing BOARD=ulx3s CORE=penumbra3 VARIANT=probe_memtlb).
# Core fileset + the board shell; no fabric/devices.
FPGA_SRC_ulx3s_penumbra3_probe_memtlb_top = $(SRC_COMMON) $(SRC_CORE_penumbra3) \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra3_probe_memtlb_top.sv

# gen3 Phase-0 probe P0.1: the back-end stall cone (scoreboard +
# load-completion + issue gate) in front of nextpnr
# (make timing BOARD=ulx3s CORE=penumbra3 VARIANT=probe_issue).
FPGA_SRC_ulx3s_penumbra3_probe_issue_top = $(SRC_COMMON) $(SRC_CORE_penumbra3) \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra3_probe_issue_top.sv

# gen3 Phase-0 probe P0.2: the decode->issue cone (registered bundle ->
# regmap -> scoreboard -> issue) in front of nextpnr
# (make timing BOARD=ulx3s CORE=penumbra3 VARIANT=probe_decode).
FPGA_SRC_ulx3s_penumbra3_probe_decode_top = $(SRC_COMMON) $(SRC_CORE_penumbra3) \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra3_probe_decode_top.sv

# gen3 Phase-0 probe P0.5: the L1 physical-tag compare chained behind the
# BRAM TLB verdict, on both sides -- MEM2 into the load result, IF2 into
# the fetch FIFO and the predicted redirect into the PC
# (make timing BOARD=ulx3s CORE=penumbra3 VARIANT=probe_l1tag).
FPGA_SRC_ulx3s_penumbra3_probe_l1tag_top = $(SRC_COMMON) $(SRC_CORE_penumbra3) \
                               hw/rtl/soc/cache_perfctr.sv \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra3_probe_l1tag_top.sv

# Text-video bring-up probe: the sim-verified output chain in front of
# the video PLL, ODDRX1F cells, and GPDI pads — no CPU or bus, so it is
# built via the TOP= escape hatch (make fpga TOP=ulx3s_video_test_top)
# rather than registered as a BOARD/CORE machine.
FPGA_SRC_ulx3s_video_test_top = $(SRC_VIDEO) \
                                hw/rtl/common/build_check.sv \
                                $(FPGA_RTL)/ecp5_pll_pkg.sv \
                                $(FPGA_RTL)/ulx3s/ulx3s_video_test_top.sv

# Audio bring-up probe: the modulator and tone source in front of the
# board's resistor ladder and the audio PLL chain — no CPU or bus, so it
# is built via the TOP= escape hatch (make fpga TOP=ulx3s_audio_test_top)
# rather than registered as a BOARD/CORE machine.
# Named rather than wildcarded: the probe wants the output stage and the
# stimulus, not the bus device that sits in the same directory.
FPGA_SRC_ulx3s_audio_test_top = hw/rtl/io/audio/audio_sigma_delta.sv \
                                hw/rtl/io/audio/audio_sine_rom.sv \
                                hw/rtl/io/audio/audio_tone_gen.sv \
                                hw/rtl/io/audio/audio_sample_rom.sv \
                                hw/rtl/common/build_check.sv \
                                $(FPGA_RTL)/ecp5_pll_pkg.sv \
                                $(FPGA_RTL)/ulx3s/ulx3s_audio_test_top.sv

# Nexys Video console bring-up: the shared MMCM and power-on reset in
# front of a bare serial shifter on the UART port — no CPU or bus, so it
# is built via the TOP= escape hatch (make fpga TOP=nexysvideo_uart_test_top).
FPGA_SRC_nexysvideo_uart_test_top = hw/rtl/common/build_check.sv \
                                    $(SRC_BOARD_nexysvideo) \
                                    $(FPGA_RTL)/nexysvideo/nexysvideo_uart_test_top.sv
XDC_nexysvideo_uart_test_top = hw/constraints/nexysvideo_uart_test_top.xdc

# The Nexys Video's DDR3 controller: UberDDR3 as vendored, behind the one
# GPL wrapper that instantiates it (nexysvideo_ddr3.sv). Its files set
# `default_nettype none, which reaches every file after them in one
# sv2v run, so a source list puts them last.
SRC_UBERDDR3 = hw/third_party/uberddr3/rtl/ecc/ecc_enc.sv \
               hw/third_party/uberddr3/rtl/ecc/ecc_dec.sv \
               hw/third_party/uberddr3/rtl/ddr3_phy.v \
               hw/third_party/uberddr3/rtl/ddr3_controller.v \
               hw/third_party/uberddr3/rtl/ddr3_top.v
SRC_DDR3_nexysvideo = $(FPGA_RTL)/nexysvideo/nexysvideo_ddr3_clk.sv \
                      $(FPGA_RTL)/nexysvideo/nexysvideo_ddr3.sv
# Its pins and I/O constraints, which every top carrying it lists after
# its own XDC.
XDC_DDR3_nexysvideo = hw/constraints/nexysvideo_ddr3.xdc

# Nexys Video DDR3 bring-up: the controller alone at the machine's
# configuration, calibrating and self-testing the whole chip in a loop
# with a line per run on the console — no CPU or bus, built via the
# TOP= escape hatch (make fpga TOP=nexysvideo_ddr3_test_top).
FPGA_SRC_nexysvideo_ddr3_test_top = hw/rtl/common/build_check.sv \
                                    $(FPGA_RTL)/xc7/xc7_thermal_guard.sv \
                                    $(SRC_DDR3_nexysvideo) \
                                    $(FPGA_RTL)/nexysvideo/nexysvideo_ddr3_selftest.sv \
                                    $(FPGA_RTL)/nexysvideo/nexysvideo_ddr3_test_top.sv \
                                    $(SRC_UBERDDR3)
XDC_nexysvideo_ddr3_test_top = hw/constraints/nexysvideo_ddr3_test_top.xdc $(XDC_DDR3_nexysvideo)

# The gen2 machine on the Nexys Video, growing one device per bring-up
# step. Its devices are named rather than taken from SRC_FABRIC, whose
# wildcards bring the ECP5 PHYs; the display's set has none, since
# its output cells are the top's. Its RAM is the DDR3 behind the line
# stack's adapter and crossing, with UberDDR3 last (see SRC_UBERDDR3).
FPGA_SRC_nexysvideo_penumbra2_top = $(SRC_COMMON) $(SRC_CORE) \
                                    $(SRC_MACHINE_penumbra2) \
                                    hw/rtl/soc/bus_devsel.sv \
                                    hw/rtl/soc/bus_device_simple.sv \
                                    hw/rtl/soc/bus_response_fabric.sv \
                                    hw/rtl/soc/bus_wait_meter.sv \
                                    hw/rtl/soc/boot_rom.sv \
                                    hw/rtl/soc/autoconfig_dev.sv \
                                    hw/rtl/io/spi_fifo.sv \
                                    hw/rtl/io/uart.sv \
                                    hw/rtl/io/spi.sv \
                                    hw/rtl/io/sdram/sdram_cdc.sv \
                                    hw/rtl/io/sdram_line/sdram_bus_adapter.sv \
                                    hw/rtl/io/ddr3/ddr3_stack.sv \
                                    $(SRC_VIDEO) \
                                    $(SRC_AUDIO) \
                                    hw/rtl/io/audio/audio_i2s_out.sv \
                                    hw/rtl/io/i2c/i2c_master.sv \
                                    hw/rtl/io/i2c/i2c_shared_master.sv \
                                    $(SRC_ETH) \
                                    hw/rtl/io/eth/eth_phy_rgmii_xc7.sv \
                                    hw/rtl/io/eth/eth_phy_reset.sv \
                                    hw/rtl/io/eth/eth_addr_eeprom.sv \
                                    hw/rtl/io/ps2/ps2.sv \
                                    hw/rtl/io/ps2/ps2_link.sv \
                                    hw/rtl/io/ps2/ps2_line_filter.sv \
                                    $(SRC_BOARD_nexysvideo) \
                                    $(SRC_DDR3_nexysvideo) \
                                    $(FPGA_RTL)/nexysvideo/nexysvideo_video_clk.sv \
                                    $(FPGA_RTL)/nexysvideo/nexysvideo_codec_mclk.sv \
                                    $(FPGA_RTL)/nexysvideo/nexysvideo_codec_setup.sv \
                                    $(FPGA_RTL)/nexysvideo/nexysvideo_eth.sv \
                                    $(FPGA_RTL)/nexysvideo/nexysvideo_penumbra2_top.sv \
                                    $(SRC_UBERDDR3)
XDC_nexysvideo_penumbra2_top = $(XDC_MACHINE_nexysvideo)

# Tops that embed the boot ROM and/or microcode: their hex images are
# generated before synthesis and inlined by inline_hex.py.
# gen2 embeds the boot ROM (no microcode — the gen2 core is hardwired).
FPGA_ROM_TOPS   = ulx3s_penumbra1_top ulx3s_penumbra2_top ulx3s_penumbra2_5_top \
                  nexysvideo_penumbra2_top nexysvideo_penumbra2_5_top
FPGA_UCODE_TOPS = ulx3s_penumbra1_top
# Tops that embed the display font image: it must exist at the repo
# root before yosys resolves its $readmemh.  Every top carrying a
# display belongs here — one left off builds only while some other
# top's run happens to have left the file behind, and fails on a clean
# tree.
FPGA_VIDEO_TOPS = ulx3s_video_test_top ulx3s_penumbra1_top \
                  ulx3s_penumbra2_top ulx3s_penumbra2_5_top \
                  nexysvideo_penumbra2_top nexysvideo_penumbra2_5_top
# Tops that also embed a preloaded cell image — the CPU-free bring-up
# probe only. A machine draws its own screen from software.
FPGA_SPLASH_TOPS = ulx3s_video_test_top
# Tops embedding the audio probe's PCM clip.
FPGA_AUDIO_TOPS = ulx3s_audio_test_top

# Per-top default nextpnr placement seed. Some board/core[/variant]
# combinations only close timing on a particular seed; pin it here so a
# plain `make fpga BOARD=… CORE=…` reproduces the good placement without
# anyone having to remember the number. Keyed by the artifact TOP name
# (like FPGA_ROM_TOPS), so a microarch variant can pin its own seed
# distinct from its base. Resolution (NEXTPNR_SEED ?= …, below the TOP
# derivation): an explicit NEXTPNR_SEED on the command line or environment
# always wins; `NEXTPNR_SEED=` forces a random placement; a top with no
# entry here gets no --seed.
#
# A pin is the seed with the widest *worst* margin across all five
# domains in a hw/tools/seed-sweep.sh run, not the best CPU-clock one: a
# seed clearing one constraint says nothing about the rest. The sweep
# behind a pin belongs in the commit message that sets it.
#
# A seed is a property of the netlist and its timing targets, so any
# change to either invalidates every pin: an RTL change, a non-default
# AUDIO_CHANNELS, or a new CPU_HZ, whose target reweights nextpnr's
# per-domain criticality and whose CLK_FREQ retunes every derived
# divider.
DEFAULT_SEED_ulx3s_penumbra1_top   := 2
DEFAULT_SEED_ulx3s_penumbra2_top   := 5
DEFAULT_SEED_ulx3s_penumbra2_5_top := 23

# Per-top default SDRAM stack (see SDRAM_STACK above).  Which stack a
# top closes timing under is a placement question rather than a logical
# one, so it is pinned per artifact TOP and re-measured the way the
# placement seed is.
SDRAM_STACK_ulx3s_penumbra1_top   := word
SDRAM_STACK_ulx3s_penumbra2_top   := line
SDRAM_STACK_ulx3s_penumbra2_5_top := line
SDRAM_STACK ?= $(or $(SDRAM_STACK_$(TOP)),word)


# BOARD/CORE porcelain → TOP derivation (CORE defaults to penumbra2_5
# in the test-suite section above).
ifneq ($(strip $(BOARD)),)
TOP := $(BOARD)_$(CORE)$(if $(VARIANT),_$(VARIANT))_top
# A microarch variant (e.g. penumbra2_5) is the SAME RTL top as its base,
# built with the variant parameter set. TOP names the artifact (.bit /
# .json); TOP_MODULE names the module yosys actually synthesizes.
TOP_MODULE := $(BOARD)_$(CORE_BASE)$(if $(VARIANT),_$(VARIANT))_top
endif

# Escape hatch (TOP=<module> directly): the artifact is its own module.
TOP_MODULE := $(if $(strip $(TOP_MODULE)),$(TOP_MODULE),$(TOP))

# The board a build is for: BOARD, or under the escape hatch the
# <board>_ prefix of the top's name. A name with no known board prefix
# is one of the bare fpga/*.sv tests, which are all ECP5.
FPGA_BOARD  := $(or $(strip $(BOARD)),$(firstword $(subst _, ,$(TOP))))
FPGA_FAMILY := $(or $(FAMILY_$(FPGA_BOARD)),ecp5)

# Effective PnR seed: an explicit NEXTPNR_SEED (command line / environment)
# wins via ?=; otherwise fall back to this top's pinned default, if any
# (empty default ⇒ no --seed, the historical behavior).
NEXTPNR_SEED ?= $(DEFAULT_SEED_$(TOP))

ifneq ($(filter fpga flash timing,$(MAKECMDGOALS)),)
ifeq ($(strip $(TOP)),)
$(error BOARD/CORE or TOP is required for '$(MAKECMDGOALS)' — e.g. make fpga BOARD=ulx3s CORE=penumbra1)
endif
ifneq ($(strip $(BOARD)),)
ifeq ($(filter $(TOP),$(FPGA_TOPS)),)
$(error Unknown combination BOARD=$(BOARD) CORE=$(CORE)$(if $(VARIANT), VARIANT=$(VARIANT)) — known tops: $(FPGA_TOPS))
endif
endif
endif

# Source-set and constraints selection. Simple test tops (no registry
# entry) use only fpga/*.sv; unknown boards fall back to the ULX3S
# constraints file.
FPGA_SRC_SIMPLE = $(wildcard $(FPGA_RTL)/*.sv)
# A variant artifact has no source set of its own — it falls back to its
# base module's (TOP_MODULE), so the gen2.5 top reuses gen2's file list.
FPGA_SRC = $(if $(FPGA_SRC_$(TOP)),$(FPGA_SRC_$(TOP)),$(if $(FPGA_SRC_$(TOP_MODULE)),$(FPGA_SRC_$(TOP_MODULE)),$(FPGA_SRC_SIMPLE)))
LPF      = $(if $(LPF_$(BOARD)),$(LPF_$(BOARD)),hw/constraints/ulx3s_v20.lpf)

# Optional per-top design-constraint overlay (floorplan / timing tuning),
# layered on top of the vendor board LPF via a second --lpf. The vendor LPF
# stays board truth and is never edited; the overlay holds only design intent
# (REGION / UGROUP / FREQUENCY NET) and never pin LOCATEs or IOBUFs — a file
# that can't speak that vocabulary can't misclaim a pin. Keyed per-top so a
# floorplan travels with its registry entry, like its source set; tops with
# no entry get a byte-identical nextpnr command line.
LPF_DESIGN_ulx3s_penumbra2_top = hw/constraints/ulx3s_penumbra2_design.lpf
LPF_DESIGN = $(if $(LPF_DESIGN_$(TOP)),$(LPF_DESIGN_$(TOP)),$(LPF_DESIGN_$(TOP_MODULE)))

# Optional per-top pre-pack floorplan (nextpnr Python API). Region/placement
# constraints cannot be expressed in nextpnr's LPF — it only supports
# LOCATE COMP (exact per-cell sites), not LOCATE UGROUP / REGION — so a
# floorplan lives in a design-owned Python script run via --pre-pack: a
# separate file and mechanism from the vendor LPF, so it cannot misstate a
# board fact. Keyed per-top, like the source set and the LPF overlay.
PREPACK_ulx3s_penumbra2_top = hw/constraints/ulx3s_penumbra2_floorplan.py
PREPACK = $(if $(PREPACK_$(TOP)),$(PREPACK_$(TOP)),$(PREPACK_$(TOP_MODULE)))

# Primitive stubs, per device family — for Verilator lint only, not
# synthesis.
FPGA_LINT_STUBS     = $(FPGA_RTL)/ecp5_prim.sv
FPGA_LINT_STUBS_xc7 = $(FPGA_RTL)/xc7/xc7_prim.sv

# A Nexys top's sources as lint sees them: its DDR3 controller wrapper,
# and UberDDR3 behind it, swapped for the controller's simulation
# stand-in (the stand-in's file says why).
FPGA_LINT_DDR3_nexysvideo = hw/rtl/sim/nexysvideo/nexysvideo_ddr3.sv hw/rtl/sim/wb_line_mem.sv
fpga_lint_src_nexysvideo  = $(filter-out $(SRC_UBERDDR3) $(FPGA_RTL)/nexysvideo/nexysvideo_ddr3.sv,$(1)) \
                            $(FPGA_LINT_DDR3_nexysvideo)

# Ladder linearity sweep — drives the modulator against a swept model
# of the board's resistor ladder and reports in-band distortion per
# configuration. An investigation tool, not a regression test: it prints
# numbers rather than passing or failing, and its conclusions live in
# doc/TODO/hw-devices.md under the audio device entry.
.PHONY: audio-sweep
audio-sweep:
	@mkdir -p $(BUILD_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) --cc --exe --build -Wall \
		-CFLAGS "-std=c++17 $(OPT_BUILD)" -Ihw/rtl/io/audio \
		--top-module audio_sigma_delta_test \
		--Mdir $(BUILD_DIR)/audio_ladder_sweep.verilator \
		-o ../Vaudio_ladder_sweep \
		hw/rtl/sim/audio_sigma_delta_test.sv hw/sim/audio_ladder_sweep.cpp
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/Vaudio_ladder_sweep $(DOCKER_IMAGE)

.PHONY: fpga flash fpga-lint

# Lint covers every registered top (plus the bring-up probes)
# regardless of TOP. Use Verilator --lint-only with each family's
# primitive stubs.
fpga-lint: $(FPGA_SRC_ulx3s_penumbra1_top) $(FPGA_SRC_ulx3s_penumbra2_probe_top) $(FPGA_SRC_ulx3s_penumbra2_top) $(FPGA_SRC_ulx3s_video_test_top) $(FPGA_SRC_ulx3s_audio_test_top) $(FPGA_LINT_STUBS) \
           $(FPGA_SRC_nexysvideo_uart_test_top) $(FPGA_SRC_nexysvideo_penumbra2_top) \
           $(FPGA_LINT_STUBS_xc7)
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_ulx3s_penumbra1_top) $(FPGA_LINT_STUBS) --top ulx3s_penumbra1_top
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_ulx3s_penumbra2_probe_top) $(FPGA_LINT_STUBS) --top ulx3s_penumbra2_probe_top
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_ulx3s_penumbra2_top) $(FPGA_LINT_STUBS) --top ulx3s_penumbra2_top
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_ulx3s_video_test_top) $(FPGA_LINT_STUBS) --top ulx3s_video_test_top
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_ulx3s_audio_test_top) $(FPGA_LINT_STUBS) --top ulx3s_audio_test_top
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_nexysvideo_uart_test_top) $(FPGA_LINT_STUBS_xc7) --top nexysvideo_uart_test_top
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(call fpga_lint_src_nexysvideo,$(FPGA_SRC_nexysvideo_penumbra2_top)) \
		$(FPGA_LINT_STUBS_xc7) --top nexysvideo_penumbra2_top

fpga: $(BUILD_DIR)/$(TOP).bit
	@echo "Bitstream: $(BUILD_DIR)/$(TOP).bit$(if $(filter ecp5,$(FPGA_FAMILY)), (PHASE_DEG=$(PHASE_DEG)))"

# ── SDRAM phase sweep knob ──────────────────────────────────────
# Used by the ULX3S board tops to set CLKOS2 phase shift (the
# SDRAM-clock pin clock).  Valid values: 0, 45, 90, 135, 180, 225, 270, 315.
# Default 180° is the step-4 baseline; the bring-up sweep iterates
# all 8 to find the centred working window.  See
# doc/internals/sdram-controller.md § Step-5 phase sweep.
PHASE_DEG ?= 180

# Stamp file invalidates downstream artefacts when PHASE_DEG changes.
# We bake the value into the filename, so switching phase makes the
# previous stamp file vanish from the dep graph and forces a rebuild
# from sv2v onward.  Old stamps are wiped on each new value so the
# build dir stays tidy.
PHASE_STAMP = $(BUILD_DIR)/.phase-$(PHASE_DEG)
$(PHASE_STAMP):
	@mkdir -p $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.phase-*
	@touch $@

# Switching stacks swaps which files are in the source set without
# touching any file's timestamp, so the synthesis result would otherwise
# look up to date against a set it was not built from.
SDRAM_STAMP = $(BUILD_DIR)/.sdram-$(SDRAM_STACK)
$(SDRAM_STAMP):
	@mkdir -p $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.sdram-*
	@touch $@

# The synthesis step, one rule for every family. Its front half is
# shared, so both flows read the same Verilog: build the hex images
# and convert SV→V. sv2v converts full SystemVerilog (module-level
# imports, packages) to Verilog-2005 that Yosys reads natively.
# -D SDRAM_PHASE_DEG=N overrides the `define inside the board top for
# the current build. The family's synthesizer runs last:
#   ecp5  yosys, to the netlist JSON nextpnr places (.config, .bit below)
#   xc7   one Vivado run from synthesis to bitstream
#         (hw/tools/vivado/build.tcl, which also writes the reports)
#
# For tops listed in FPGA_ROM_TOPS / FPGA_UCODE_TOPS, the boot-ROM and
# microcode hex images are inlined into the generated Verilog via
# inline_hex.py, so synthesis must rerun whenever their sources
# change.  Conditional prerequisites keep other TOPs from being
# spuriously rebuilt by unrelated edits.
#
# _sv2v.v is deliberately not a target of its own: inline_hex.py
# rewrites it in place, so as a target it could survive a failed
# inline looking up to date, and the next build would synthesize a
# boot ROM with no contents. Inside one rule, a failure anywhere fails
# the whole step. Only the family's output and last command vary; the
# shared lines stay literal in the recipe, where make recognizes the
# recursive $(MAKE).
UCODE_SRC = hw/microcode/microcode.uasm
ROM_SRCS  = $(wildcard hw/rom/*.c hw/rom/*.h hw/rom/*.s hw/rom/*.ld hw/rom/Makefile)

FPGA_SYNTH_OUT_ecp5 = $(BUILD_DIR)/$(TOP).json
FPGA_SYNTH_CMD_ecp5 = $(FPGA_TOOLS)/yosys -p "read_verilog $(BUILD_DIR)/$(TOP)_sv2v.v; synth_ecp5 -top $(TOP_MODULE) -json $@"

FPGA_SYNTH_OUT_xc7  = $(BUILD_DIR)/$(TOP).bit
FPGA_SYNTH_DEPS_xc7 = $(XDC) $(XDC_MASTER) hw/tools/xdc_check.py $(VIVADO_TOOLS)/build.tcl \
                      $(VIVADO_TOOLS)/gates.tcl
FPGA_VIVADO_LOG     = $(BUILD_DIR)/$(TOP)_vivado.log
FPGA_SYNTH_CMD_xc7  = python3 hw/tools/xdc_check.py $(addprefix --master ,$(XDC_MASTER)) $(XDC) && \
                      $(VIVADO_TOOLS)/bin/vivado -mode batch -nojournal \
                      -log $(FPGA_VIVADO_LOG) \
                      -source $(VIVADO_TOOLS)/build.tcl \
                      -tclargs $(TOP_MODULE) $(PART_$(FPGA_BOARD)) $(BUILD_DIR)/$(TOP) \
                               $(FPGA_VIVADO_LOG) $(BUILD_DIR)/$(TOP)_sv2v.v $(XDC)

$(FPGA_SYNTH_OUT_$(FPGA_FAMILY)): $(FPGA_SRC) $(PHASE_STAMP) $(SDRAM_STAMP) \
    $(if $(filter $(TOP),$(FPGA_UCODE_TOPS)),$(UCODE_SRC)) \
    $(if $(filter $(TOP),$(FPGA_ROM_TOPS)),$(ROM_SRCS)) \
    $(if $(filter $(TOP),$(FPGA_VIDEO_TOPS)),font8x16.hex) \
    $(if $(filter $(TOP),$(FPGA_SPLASH_TOPS)),splash_cells.hex) \
    $(if $(filter $(TOP),$(FPGA_AUDIO_TOPS)),audio_sample.hex) \
    $(FPGA_SYNTH_DEPS_$(FPGA_FAMILY))
	@mkdir -p $(BUILD_DIR)
	$(if $(filter $(TOP),$(FPGA_UCODE_TOPS)),$(UASM) hw/microcode/microcode.uasm -o microcode.hex)
	$(if $(filter $(TOP),$(FPGA_ROM_TOPS)),$(MAKE) -C hw/rom)
	$(FPGA_TOOLS)/sv2v -D SDRAM_PHASE_DEG=$(PHASE_DEG) -D PENUMBRA_CPU_VARIANT=$(CPU_VARIANT) -D AUDIO_SAMPLES=$(AUDIO_SAMPLES) -D AUDIO_CHANNELS=$(AUDIO_CHANNELS) $(FPGA_SRC) -w $(BUILD_DIR)/$(TOP)_sv2v.v
	$(if $(filter $(TOP),$(FPGA_ROM_TOPS) $(FPGA_UCODE_TOPS) $(FPGA_AUDIO_TOPS)),python3 hw/tools/inline_hex.py $(BUILD_DIR)/$(TOP)_sv2v.v $(BUILD_DIR)/$(TOP)_sv2v.v)
	$(FPGA_SYNTH_CMD_$(FPGA_FAMILY))

# The nextpnr invocation shared by the normal PnR rule and the
# seed-sweep pattern rule below (device, constraints, prepack).
NEXTPNR_BASE = $(FPGA_TOOLS)/nextpnr-ecp5 --85k --package CABGA381 --speed 6 \
		--timing-allow-fail --lpf $(LPF) \
		$(if $(LPF_DESIGN),--lpf $(LPF_DESIGN)) \
		$(if $(PREPACK),--pre-pack $(PREPACK))

# Seed and VIS change the nextpnr command line but no input file, so
# they ride a stamp filename to stay in the dep graph (as PHASE_STAMP).
PNR_STAMP = $(BUILD_DIR)/.pnr-$(if $(strip $(NEXTPNR_SEED)),s$(NEXTPNR_SEED),rand)-vis$(if $(filter 1,$(VIS)),1,0)
$(PNR_STAMP):
	@mkdir -p $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.pnr-*
	@touch $@

# Which floorplan applies is the same kind of input: naming a different
# script, or none, changes placement without changing any file the rule
# already depends on, and the file dependency alone cannot see it — the
# script is unchanged, or drops out of the graph entirely.  Comparing a
# floorplan against no floorplan is the reason to reach for that, so it
# rides a stamp of its own.  Separate from PNR_STAMP so the seed and the
# floorplan can vary independently, and named after the script so two
# floorplans are distinguishable and not merely present-or-absent.
FP_STAMP = $(BUILD_DIR)/.fp-$(if $(strip $(PREPACK)),$(basename $(notdir $(PREPACK))),none)
$(FP_STAMP):
	@mkdir -p $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.fp-*
	@touch $@

$(BUILD_DIR)/$(TOP).config: $(BUILD_DIR)/$(TOP).json $(LPF) $(LPF_DESIGN) $(PREPACK) $(PNR_STAMP) $(FP_STAMP)
	@echo "[fpga] PnR seed: $(if $(strip $(NEXTPNR_SEED)),$(NEXTPNR_SEED)$(if $(filter $(NEXTPNR_SEED),$(DEFAULT_SEED_$(TOP))), (pinned default for $(TOP))),random — no --seed)"
	$(NEXTPNR_BASE) \
		$(if $(NEXTPNR_SEED),--seed $(NEXTPNR_SEED)) \
		--json $< --textcfg $@ \
		--write $(BUILD_DIR)/$(TOP)_routed.json \
		--report $(BUILD_DIR)/$(TOP)_timing.json --detailed-timing-report \
		--log $(BUILD_DIR)/$(TOP)_pnr.log \
		$(NEXTPNR_VIS)

# ── Headless place/route visualization ──────────────────────────
# VIS=1 makes the PnR step also render the placement to
# <top>_placed.svg — file output, so it works over ssh and opens in
# any browser.  nextpnr can also emit --routed-svg (~700 MB on this
# design) and --router2-heatmap congestion grids; the latter needs
# --router router2, which this design is not routed with.
ifeq ($(VIS),1)
NEXTPNR_VIS = --placed-svg $(BUILD_DIR)/$(TOP)_placed.svg
endif

# ── Parallel seed sweeps ────────────────────────────────────────
# Seed-tagged PnR artifacts: build/<top>.s<seed>.config places the
# shared synthesis JSON with --seed <seed> and writes a seed-tagged
# timing report alongside.  Disjoint outputs per seed let one
# `make -jN` invocation run many seeds concurrently — this is what
# hw/tools/seed-sweep.sh drives.  No routed JSON and no bitstream:
# a sweep wants timing verdicts, and the winning seed is then pinned
# as DEFAULT_SEED_<top> and rebuilt through the normal fpga target.
#
# Each seed's nextpnr output goes to its own build/<top>.s<seed>.log:
# concurrent runs share one stdout, and nextpnr's progress lines are
# long-running and unlabelled, so interleaved they cannot be attributed
# back to a seed. A failing seed prints its tail so the redirect never
# swallows the reason.
#
# PNR_TIMEOUT bounds each run. nextpnr's router can fail to terminate on a
# particular (netlist, seed) pair — and since placement is deterministic that
# seed stays poisoned for that netlist, so waiting never pays. Without a bound
# one such seed blocks a whole sweep. A run that trips it deletes its outputs:
# they would otherwise be a previous sweep's, and the summary reads them back
# as this one's result.
PNR_TIMEOUT ?= 900
$(BUILD_DIR)/$(TOP).s%.config: $(BUILD_DIR)/$(TOP).json $(LPF) $(LPF_DESIGN) $(PREPACK) $(FP_STAMP)
	@echo "[sweep] seed $* → $(BUILD_DIR)/$(TOP).s$*.log"
	@timeout $(PNR_TIMEOUT) $(NEXTPNR_BASE) \
		--seed $* \
		--json $< --textcfg $@ \
		--report $(BUILD_DIR)/$(TOP).s$*_timing.json --detailed-timing-report \
		> $(BUILD_DIR)/$(TOP).s$*.log 2>&1; \
	st=$$?; \
	if [ $$st -ne 0 ]; then \
	     rm -f $@ $(BUILD_DIR)/$(TOP).s$*_timing.json; \
	fi; \
	if [ $$st -eq 124 ]; then \
	     echo "[sweep] seed $* TIMED OUT after $(PNR_TIMEOUT)s" \
	        | tee -a $(BUILD_DIR)/$(TOP).s$*.log; exit 1; \
	elif [ $$st -ne 0 ]; then \
	     echo "[sweep] seed $* FAILED — tail of $(BUILD_DIR)/$(TOP).s$*.log:"; \
	     tail -20 $(BUILD_DIR)/$(TOP).s$*.log; exit 1; \
	fi

# Echo the resolved artifact TOP name (consumed by seed-sweep.sh).
.PHONY: print-top
print-top:
	@echo $(TOP)

# The bitstream goes to the FPGA's configuration RAM: it runs until
# power-off, and the board's flash is left alone.
#
# openFPGALoader opens the board's FTDI chip by USB ID alone, so it
# cannot reach another board attached to the same host. fujprog, the
# OSS CAD Suite's ULX3S programmer, falls back to any FT232R when it
# finds no ULX3S by name, and drives JTAG into whatever it opened: a
# Nexys Video's UART is one.
flash: $(BUILD_DIR)/$(TOP).bit
	$(OFL_TOOLS)/bin/openFPGALoader -b $(OFL_BOARD_$(FPGA_BOARD)) $<

# Packing and the timing report are per family. An xc7 bitstream is
# the synthesis step's own output (Vivado runs through bitgen), so
# only ecp5 has a packing rule here.
ifeq ($(FPGA_FAMILY),xc7)

# Vivado's own timing summary: the design totals and the per-clock
# tables. The worst paths are in full in the report itself.
.PHONY: timing
timing:
	@sed -n '/^| Design Timing Summary/,/^| Inter Clock Table/p' $(BUILD_DIR)/$(TOP)_timing_summary.rpt | head -n -2

else

$(BUILD_DIR)/$(TOP).bit: $(BUILD_DIR)/$(TOP).config
	$(FPGA_TOOLS)/ecppack $< $@

# Pretty-print fmax + top critical paths from the nextpnr JSON report
# produced by the .config rule.  Doesn't trigger a build — operates
# on whatever the last FPGA build left in $(BUILD_DIR).  Override the
# path count via TOP_N=10.  DETAIL=rollup adds a per-path module rollup
# (which subsystem owns each path); DETAIL=full prints the full hop-by-hop
# trace (every LUT and route).  Default DETAIL=none.
.PHONY: timing
TOP_N ?= 5
DETAIL ?= none
timing:
	@hw/tools/timing-report.sh --detail=$(DETAIL) $(BUILD_DIR)/$(TOP)_timing.json $(TOP_N)

endif

# Pretty-print high-fanout nets from the yosys synth JSON, one section
# per clock domain.  Useful for diagnosing nextpnr routing-congestion
# failures: signals with hundreds to thousands of sinks routed through
# general fabric (rather than ECP5's dedicated global routing) saturate
# the router.  Like `timing`, operates on whatever the last FPGA build
# left behind — no rebuild.  Override entries per domain via
# FANOUT_N=30, threshold via FANOUT_MIN=20; FANOUT_DOMAIN=clk_sdram
# restricts the report to one domain (names as in the section headers).
# Nets on the global network are marked from nextpnr's own log — the
# fpga build's <top>_pnr.log or any seed-sweep log of the same netlist.
.PHONY: fanout
FANOUT_N      ?= 25
FANOUT_MIN    ?= 50
FANOUT_DOMAIN ?=
fanout:
	@python3 hw/tools/list_fanout.py $(BUILD_DIR)/$(TOP).json -n $(FANOUT_N) -m $(FANOUT_MIN) $(foreach d,$(FANOUT_DOMAIN),--domain $(d)) \
		$(foreach l,$(wildcard $(BUILD_DIR)/$(TOP)_pnr.log $(BUILD_DIR)/$(TOP).s*.log),--pnr-log $(l))

# ── Cleanup ────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(WAVE_DIR)
	@$(MAKE) -C sw/sim clean 2>/dev/null || true
