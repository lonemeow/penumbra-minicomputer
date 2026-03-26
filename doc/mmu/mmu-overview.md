# Penumbra MMU - Overview

## Goals

- Page-based virtual-to-physical address translation
- Per-page protection (read, write, execute) with user/supervisor distinction
- TLB for fast translation in the common case
- Support for precise exceptions on page faults to enable demand paging

## Page Size

_To be defined._ 4 KB is the conventional choice and works well with 32-bit addresses (12-bit offset, 20-bit page number). Larger pages (e.g., 8 KB or 16 KB) reduce TLB pressure but waste memory on small allocations.

## Page Table Structure

_To be defined._ Options:
- Two-level page table (like early RISC machines, straightforward)
- Three-level (more flexible for sparse address spaces, but adds a memory access)
- Inverted page table (saves memory but complicates software)

Two-level with 4 KB pages is the likely starting point.

## TLB

- Fully associative or set-associative, small (16-64 entries)
- Software-managed vs. hardware page table walker TBD
- TLB miss handling strategy is a major architectural decision

## Page Table Entry Format

_To be defined._ Expected fields:
- Physical page frame number
- Valid/present bit
- Protection bits (read, write, execute)
- User/supervisor access
- Dirty bit
- Accessed/referenced bit
- Caching control (for memory-mapped I/O regions)

## Address Space Layout

_To be defined._ Typical split:
- User space in lower portion of address space
- Kernel space in upper portion (mapped in all address spaces)
- I/O region mapped at fixed physical addresses
