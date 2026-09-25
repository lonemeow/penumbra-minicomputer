# UberDDR3

An open-source DDR3 controller and PHY for 7-series FPGAs, vendored
verbatim. It is the Nexys Video's memory controller: Penumbra never
writes its own DDR3 controller, and owns only the seam between this
one and the Penumbra bus.

- Upstream: https://github.com/AngeloJacobo/UberDDR3
- Pinned commit: `79d8fd3e30ebba6acd84eecac2fa57b7f95f4544`
- License: GPL-3.0-or-later (`LICENSE`, from the same commit), except
  the two ECC files, which carry ROA Logic's permissive notice: use
  and redistribution are unrestricted as long as the notice and its
  disclaimer stay in the file and in any derivative.

| File | What it is |
|------|------------|
| `rtl/ddr3_top.v` | top: controller plus PHY, Wishbone request port, `o_calib_complete`, BIST |
| `rtl/ddr3_controller.v` | command scheduling, calibration, refresh |
| `rtl/ddr3_phy.v` | 7-series I/O: ISERDES/OSERDES, IDELAY/ODELAY |
| `rtl/ecc/ecc_enc.sv`, `rtl/ecc/ecc_dec.sv` | Hamming ECC, ROA Logic; instantiated inside generate branches even with ECC off, so they must be present |

Not taken: `rtl/axi/` (an AXI front end, which would add a bridge the
Penumbra design avoids), `rtl/spd/`, `rtl/ecp5_phy/`, the example
designs, and the testbench. The testbench's Micron DDR3 model reserves
all rights, so it is never vendored; simulation fetches it at test time
into `build/`.

## The license boundary

Penumbra's own files stay BSD 2-Clause. Exactly one Penumbra file may
instantiate `ddr3_top`: the Nexys-specific wrapper that exposes it as a
plain Wishbone port, itself licensed GPL-3.0-or-later. Everything
behind that port knows nothing of UberDDR3. A Nexys Video bitstream
incorporating this directory is a combined work, GPL-3.0 as a whole if
it is ever distributed, with its Corresponding Source the public
repository at the exact commit. ULX3S bitstreams never contain it.

## Local modifications

None. The controller is configured through its parameters only. A
change that cannot be made that way gets, in the changed file, the
"modified by …, <date>" notice GPL-3 section 5a requires, and an entry
in this section.
