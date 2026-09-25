# Digilent Vivado board files

Board files from Digilent, vendored verbatim: the facts of a board that
its master XDC leaves out. The Nexys Video's master XDC
(`../digilent-xdc/`) has no DDR3 pins, because Vivado's memory
interface generator (MIG) supplies them from this project file.

- Upstream: https://github.com/Digilent/vivado-boards
- Pinned commit: `36f34ab687b7fa9c778b779d027f3bce63b3ace9`
- License: MIT (`License.txt`, from the same commit)

| File | Upstream path | What it states |
|------|---------------|----------------|
| `nexys_video_mig.prj` | `new/board_files/nexys_video/A.0/1.2/mig.prj` | the Nexys Video's DDR3 part (MT41K256M16, x16), its pins and I/O standards, and the internal VREF its bank needs |

These files are never edited. The xc7 flow reads the DDR3 pins in the
project file as board facts beside the master XDC's
(`hw/tools/xdc_check.py`), so a top's DDR3 pin lines are checked
against the manufacturer's the same way its other pins are.

No local modifications.
