# Floorplan for ulx3s_penumbra2_top — run by nextpnr via --pre-pack.
#
# Why this exists: the gen2 SDRAM controller's clk_sdram paths close at only
# ~101 MHz, where the identical controller RTL reaches ~140 MHz on the sparse
# gen1 design. Measured from a routed build, the cause is geometric, not
# logical. The penumbra2 core fills fabric columns X75..113, leaving the SDRAM
# stack only the ~13-column corridor between the core's right edge and the
# right-edge SDRAM pads at X126. With nowhere to spread horizontally, the
# placer stacked u_sdram_ctrl into a 4-wide x 37-tall sliver (X120..124,
# Y39..76); the cdc -> ctrl -> phy_a control path then routes vertically across
# that sliver, which is where the 0.9-1.4 ns route hops come from. gen1's small
# core never crowds this corridor, so the same logic packs compactly there.
#
# Fix: confine the SDRAM control logic (the FSM in u_sdram_ctrl plus the
# u_sdram_cdc that feeds its critical path) to a compact rectangle in that
# corridor, centred on the pad bank (pads span Y14..92). Bounding the box caps
# the vertical spread and forces dense placement, shortening the routes.
#
# The PHY is deliberately NOT constrained: it is IOLOGIC that must follow its
# pads up and down the whole edge, and boxing it would only move the pressure
# onto the ctrl -> pad paths.
#
# This is design intent and touches no board truth — pin sites and I/O
# standards live solely in the vendor LPF, which a Python region script cannot
# express even by accident. Bounds are measured, not sacred: if a routed build
# still shows a tall ctrl bbox, tighten Y; if it turns unroutable, widen it.

REGION = "sdram_ctl"

# Corridor between the core's right edge (X113) and the pad edge (X126),
# height centred on the middle of the SDRAM pad bank.
X0, Y0, X1, Y1 = 114, 46, 126, 66

# The stretched FSM and the CDC stage that starts its critical path. Names are
# instance-prefixed and survive synthesis because these modules carry
# keep_hierarchy — that boundary is what makes the cells addressable here.
PREFIXES = ("u_sdram_ctrl", "u_sdram_cdc")

ctx.createRectangularRegion(REGION, X0, Y0, X1, Y1)

n = 0
for cell_name, cell in ctx.cells:
    if cell_name.startswith(PREFIXES):
        ctx.constrainCellToRegion(cell_name, REGION)
        n += 1

print("floorplan: constrained %d cells to region %s (X%dY%d..X%dY%d)"
      % (n, REGION, X0, Y0, X1, Y1))
