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

# ── Audio device: out of the CPU cone's placement region ────────────
#
# Why this exists: the CPU clock's critical path is not deep, it is long.
# Measured over ten seeds, its hops average 14.2 tiles against 4.6 for the
# 60 MHz domain and 1.5 for the SDRAM one, at a *better* ns-per-tile than
# either — so consecutive cells in the cone are simply placed far apart,
# not routed badly. They are far apart because the band they occupy is
# full: in columns X75..X89 the core has some 3000 cells, the L1s 1500,
# and the audio device 1800 — a device with 12-18% margin on its own
# domains taking a third of the sites in the region that has none.
#
# The placer cannot know that. It minimizes total wirelength, and every
# module reaches every other through the bus, so it clumps everything into
# the same band and leaves X2..X47 empty — 46 columns of fabric that a
# small design does use (the audio bring-up top places 2080 of its 2272
# logic cells there). A region is the only way to say which module can
# afford the distance.
#
# What moves is the whole output-clock side: the mixer, the queues, and
# the synchronizer stages that feed them. Keeping that group together
# matters more than keeping it near the bus, because its internal paths
# are the ones with a 16.7 ns budget. What stays behind is the register
# tier — decode, the rate/gain register file, the read mux, the level and
# interrupt logic — which the bus reaches on a 26.7 ns budget and which
# would be the wrong thing to send away.
#
# The link between the two halves is the one path built to absorb it: a
# channel's rate and gain cross through two synchronizer stages with the
# walk's remaining cycles to settle, so distance there costs nothing.
#
# Bounds are measured, not sacred. The span covers ten EBR columns for the
# queues' eight block RAMs; if a routed build shows the group straining to
# reach the register tier, slide it right toward X50.
AUDIO_REGION = "audio_dsp"
AX0, AY0, AX1, AY1 = 24, 18, 50, 72
AUDIO_PREFIXES = ("u_audio.u_mix", "u_audio.g_chan",
                  "u_audio.inc_a", "u_audio.vol_a",
                  "u_audio.mvol_a", "u_audio.chan_en_a")

# ── SDRAM control: dense placement in the pad corridor ──────────────
REGION = "sdram_ctl"

# Corridor between the core's right edge (X113) and the pad edge (X126),
# height centred on the middle of the SDRAM pad bank.
X0, Y0, X1, Y1 = 114, 46, 126, 66

# The stretched FSM and the CDC stage that starts its critical path. Names are
# instance-prefixed and survive synthesis because these modules carry
# keep_hierarchy — that boundary is what makes the cells addressable here.
PREFIXES = ("u_sdram_ctrl", "u_sdram_cdc")

ctx.createRectangularRegion(REGION, X0, Y0, X1, Y1)
ctx.createRectangularRegion(AUDIO_REGION, AX0, AY0, AX1, AY1)

n = 0
na = 0
for cell_name, cell in ctx.cells:
    if cell_name.startswith(PREFIXES):
        ctx.constrainCellToRegion(cell_name, REGION)
        n += 1
    elif cell_name.startswith(AUDIO_PREFIXES):
        ctx.constrainCellToRegion(cell_name, AUDIO_REGION)
        na += 1

print("floorplan: constrained %d cells to region %s (X%dY%d..X%dY%d)"
      % (n, REGION, X0, Y0, X1, Y1))
print("floorplan: constrained %d cells to region %s (X%dY%d..X%dY%d)"
      % (na, AUDIO_REGION, AX0, AY0, AX1, AY1))

# A prefix left behind by an instance rename matches nothing, which would
# drop its region silently and hand the placer back the layout this file
# exists to prevent — so an empty region stops the build instead.
if n == 0 or na == 0:
    raise RuntimeError("floorplan: a region matched no cells (%s: %d, %s: %d)"
                       " — do its instance-name prefixes still exist?"
                       % (REGION, n, AUDIO_REGION, na))
