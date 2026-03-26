# Penumbra ISA - Architecture Overview

## Design Philosophy

Penumbra is a 32-bit load-store RISC-like architecture. While inspired by the aesthetics and operational feel of 1970s/80s minicomputers like the Data General Eclipse and DEC VAX, the ISA itself is a clean design that avoids the accumulated complexity of those machines.

Key principles:
- **Load-store:** Only load and store instructions access memory; all computation operates on registers
- **Fixed-width or few-width instruction encoding:** Keep decode simple
- **Orthogonal design:** Minimize special cases and irregular encodings
- **Sufficient registers:** Avoid the register pressure problems of real vintage architectures

## Registers

_To be defined._ Considerations:
- General-purpose register count (16 or 32)
- Dedicated PC, SP, status/flags register
- Whether to have a separate link register or use a GPR convention
- Supervisor vs. user register banks (if any)

## Instruction Formats

_To be defined._ Considerations:
- Fixed 32-bit instructions vs. 16/32 mixed encoding
- Encoding space for opcode, register fields, immediates
- Branch offset range and addressing modes

## Addressing Modes

As a load-store architecture, addressing modes apply only to load/store instructions:
- Register + immediate offset (the workhorse)
- Register + register (useful for array indexing)
- PC-relative (for position-independent code and literal pools)

## Privilege Levels

_To be defined._ At minimum:
- User mode
- Supervisor/kernel mode
- Controlled transitions via trap/syscall instruction

## Exception and Interrupt Model

_To be defined._ Considerations:
- Vectored vs. non-vectored interrupts
- Precise exceptions for virtual memory support
- Trap instruction for system calls

## Memory Model

- 32-bit virtual address space (4 GB)
- Page-based virtual memory managed by the MMU
- Memory protection (read/write/execute per page, user/supervisor)
- Physical address space size TBD (dependent on ULX3S SDRAM and I/O mapping)
