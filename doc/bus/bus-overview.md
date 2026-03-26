# Penumbra System Bus - Overview

## Goals

- Connect the CPU core to memory and I/O devices
- Support DMA transfers (devices reading/writing memory without CPU involvement)
- Simple enough to understand and debug, but realistic enough to learn from

## Bus Architecture

_To be defined._ Options under consideration:
- Simple custom bus (easiest to understand, full control over design)
- Wishbone (open standard, well-documented, widely used in open-source SoCs)
- AXI4-Lite subset (industry standard, good learning value, more complex)

Wishbone is the likely starting point for its balance of simplicity and ecosystem support.

## Bus Topology

_To be defined._ Likely a shared-bus or crossbar connecting:
- CPU (bus master)
- SDRAM controller (main memory)
- Boot ROM
- UART (serial console)
- GPIO
- SPI (SD card, other peripherals)
- DMA controller (additional bus master)
- Timer / interrupt controller

## Arbitration

With multiple bus masters (CPU and DMA controller at minimum), the bus needs an arbiter. Round-robin or fixed-priority arbitration are the simplest starting points.

## Memory Map

_To be defined._ Physical address space layout mapping memory and I/O regions. Will be constrained by the ULX3S hardware (32 MB SDRAM, onboard peripherals).
