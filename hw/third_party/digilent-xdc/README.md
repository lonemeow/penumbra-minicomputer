# Digilent master XDC files

Board constraint files from Digilent, vendored verbatim: the pin,
I/O-standard and oscillator facts of each board, as its manufacturer
states them.

- Upstream: https://github.com/Digilent/digilent-xdc
- Pinned commit: `00a3404901f35aa9567b01ecb3f2c233b6efe9f4`
- License: MIT (`License.txt`, from the same commit)

| File | Board |
|------|-------|
| `Nexys-Video-Master.xdc` | Digilent Nexys Video (XC7A200T-1SBG484C), board rev. A |

These files are never edited. A board top's own XDC copies the lines
for the pins it uses, uncommented and otherwise unchanged, and the xc7
flow checks every pin and board-clock line in it against the master
before synthesis (`hw/tools/xdc_check.py`), so a mistyped pin or I/O
standard fails the build instead of reaching the board.

No local modifications.
