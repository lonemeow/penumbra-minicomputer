# Penumbra ISS — Instruction Set Simulator
#
# Builds a standalone executable with no Verilator dependency.
# Usage:
#   make                  — build penumbra-iss
#   make clean            — remove build artifacts
#   make run              — build and run with default ROM
#   make run SDCARD=path  — run with SD card image

CXX      ?= g++
CXXFLAGS  = -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter
TARGET    = penumbra-iss

# Default ROM (from make simulate pipeline)
ROM      ?= ../../build/program.hex
SDCARD   ?=
TRACE    ?=

ARGS = $(ROM)
ifneq ($(SDCARD),)
  ARGS += +sdcard=$(SDCARD)
endif
ifneq ($(TRACE),)
  ARGS += +trace=$(TRACE)
endif

$(TARGET): penumbra_iss.cpp ../../hw/sim/usb_device_sim.h
	$(CXX) $(CXXFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET) $(ARGS)

clean:
	rm -f $(TARGET)

.PHONY: run clean
