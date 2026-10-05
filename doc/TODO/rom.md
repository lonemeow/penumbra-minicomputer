# Boot ROM

The boot ROM under `hw/rom/`: bring-up, autoconfig, the monitor and
loading the first program. The loader it starts, `PENBOOT.ELF`, is
NetBSD's and is in `kernel.md`.

## Boot ROM: autoconfig places devices without checking what is there

`doc/system/bus.md` ("Device Addresses") puts one rule on where an
autoconfigured device goes: its base is a multiple of its size, and
its range overlaps neither the fixed ranges nor another device.
`autoconfig()` in `hw/rom/boot_rom.c` keeps the rule only for the
devices today's boards carry. It tracks no ranges:

- **Every memory board gets the base `0x0100_0000`.** That is inside
  base RAM whenever base RAM exceeds 16 MB, as it does on both boards
  (32 MB on the ULX3S, 512 MB on the Nexys Video). Two memory boards
  would also be given the same base, and a board larger than 16 MB
  would get a base that is not a multiple of its size, at which a
  device never answers.
- **Peripherals are placed upward from `0xFF00_1000`** with nothing
  bounding them. Once their windows add up to about 16 MB, the next
  one lands on the boot ROM at `0xFFFF_0000`.
- **A wrong placement is silent.** The ROM prints the base it chose
  and carries on.

Nothing fails today: no machine has a memory-class autoconfigured
device (only the autoconfig testbench does), and the peripherals take
a small part of the space below the ROM. A discrete build is expected
to carry most of its RAM on autoconfigured boards, which is where the
first item bites.

The ROM already has what the fix needs: `detect_ram()` runs before
`autoconfig()`, so the end of base RAM is known. Open: placement that
keeps the ranges it has handed out, starts memory boards past the end
of base RAM at a multiple of their size, keeps every device clear of
the fixed ranges, and stops with a message when a device does not fit
rather than placing it wrongly.

## Boot ROM: the linker script and the ABI describe 16 MB of RAM

`hw/rom/rom.ld` declares a `RAM` region of 16 MB at 0 and says in its
header that the stack grows down from the top of RAM.
`doc/system/abi.md` ("Sections") repeats the region as the memory map.
Neither is true of the machine: base RAM is probed, and is 32 MB on
the ULX3S and 512 MB on the Nexys Video, and the ROM's stack is in the
second page (`crt0.s`), inside the 8 KB that `doc/system/bus.md`
("Minimum RAM Requirement") guarantees.

Nothing breaks, because no section is assigned to the region: the
script discards every writable section. Open: drop the unused region
and the stack remark from `rom.ld`, and correct `abi.md`, whose
section should not present one script's regions as the machine's
memory map.
