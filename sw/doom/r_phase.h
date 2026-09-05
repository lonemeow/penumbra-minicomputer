// Per-phase instruction and cycle attribution for the renderer.
//
// R_RenderPlayerView splits cleanly into setup, BSP traversal, plane
// drawing and sprite drawing, and the four are what a profiler would
// separate if this target had one -- MKPROFILE is off for penumbra, so
// the phases are marked by hand.

#ifndef PENUMBRA_R_PHASE_H
#define PENUMBRA_R_PHASE_H

enum {
    R_PHASE_REST = 0,   // everything outside R_RenderPlayerView
    R_PHASE_SETUP,      // R_SetupFrame and the clears
    R_PHASE_BSP,        // R_RenderBSPNode: traversal, segs, wall columns
    R_PHASE_PLANES,     // R_DrawPlanes: floors and ceilings
    R_PHASE_MASKED,     // R_DrawMasked: sprites and masked walls
    R_PHASE_WALL,       // R_StoreWallRange: seg setup + R_RenderSegLoop
    R_PHASE_COUNT
};

// Close the running phase and open the given one.  Deltas are short
// enough that the free-running 32-bit counters cannot wrap between two
// marks, which is what makes the accumulated totals trustworthy over a
// run long enough to wrap a single snapshot pair.
#ifdef PENUMBRA_PERFCTR
void R_PhaseMark(int phase);

// R_StoreWallRange runs ~112 times a frame; bracketing every call costs
// more than it measures.  Sample one call in R_WALL_SAMPLE and scale the
// accumulated total, leaving the unsampled calls attributed to bsp.
#define R_WALL_SAMPLE 16
extern unsigned long r_wall_seen, r_wall_sampled;

// Counted per call rather than per pixel, so the drawers keep their inner
// loops intact and the measurement costs one increment per invocation.
// 32 bits is ample: a 150-frame run draws on the order of 10^7 pixels.
// One table rather than named globals, so adding a probe is one enum entry
// and one line in the report.
enum {
    R_CNT_COL_CALLS, R_CNT_COL_PX,
    R_CNT_SPAN_CALLS, R_CNT_SPAN_PX,
    R_CNT_FIXEDDIV, R_CNT_FIXEDMUL,
    R_CNT_BSPNODE, R_CNT_SUBSECTOR, R_CNT_ADDLINE, R_CNT_STOREWALL,
    R_CNT_FINDPLANE, R_CNT_MAPPLANE, R_CNT_MAKESPANS, R_CNT_VISPLANES,
    R_CNT_PROJSPRITE,
    R_CNT_POINTTOANGLE, R_CNT_CHECKBBOX,
    R_CNT_CLIPSOLID, R_CNT_CLIPPASS,
    R_CNT_SOLIDSEG_SCAN, R_CNT_SOLIDSEG_MOVE, R_CNT_SEGLOOP_ITER,
    R_CNT_COUNT
};

extern unsigned long r_counts[R_CNT_COUNT];

#define R_COUNT(i)         (r_counts[i]++)
#define R_COUNT_N(i, n)    (r_counts[i] += (unsigned long)(n))

#define R_COUNT_COLUMN(n)  (R_COUNT(R_CNT_COL_CALLS), R_COUNT_N(R_CNT_COL_PX, n))
#define R_COUNT_SPAN(n)    (R_COUNT(R_CNT_SPAN_CALLS), R_COUNT_N(R_CNT_SPAN_PX, n))
#define R_COUNT_FIXEDDIV() R_COUNT(R_CNT_FIXEDDIV)
#define R_COUNT_FIXEDMUL() R_COUNT(R_CNT_FIXEDMUL)
#else
#define R_PhaseMark(phase) ((void)0)
#define R_COUNT(i)         ((void)0)
#define R_COUNT_N(i, n)    ((void)0)
#define R_COUNT_COLUMN(n)  ((void)0)
#define R_COUNT_SPAN(n)    ((void)0)
#define R_COUNT_FIXEDDIV() ((void)0)
#define R_COUNT_FIXEDMUL() ((void)0)
#endif

#endif /* PENUMBRA_R_PHASE_H */
