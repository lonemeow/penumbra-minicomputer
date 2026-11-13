#!/usr/bin/env python3
"""Run the ISS option tests: programs with known answers under chosen options.

Each case assembles a program from this directory, runs the ISS on it
with a set of plusargs until the program's BREAK, and checks the
registers the ISS prints at exit.  Expected values are written here from
the hardware's own parameters rather than read back from the ISS's
tables, so a case pins the ISS to the machine it claims to present.

Usage: run_iss_tests.py --iss <penumbra-iss> --asm "<assembler command>"
                        --outdir <dir>
"""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Every case runs a few thousand instructions; a run still going after this
# long is stuck, not slow.
RUN_TIMEOUT = 30


def cache_info(sets, ways, line_bytes, vipt):
    """A cache INFO word in penumbra_pkg.sv's unified layout."""
    return (int(vipt) << 26) | (ways << 21) | (sets << 6) | (line_bytes // 4)


# What each generation presents, from its RTL.  penumbra1: cpu_core's two
# cache_vipt instances at their defaults (64 sets, direct-mapped, 16-byte
# lines) and cpuid's default name "Penumbra/1".  penumbra2 and penumbra2_5:
# machine_penumbra2's cache_bram_vipt pair (4 KiB, 2 ways), with the name
# tagging the variant.  penumbra3: machine_penumbra3's defaults, two
# penumbra3_l1 instances of 2 ways at a page a way, and its uncore's L2 at
# l2_cache's geometry.  Every earlier generation shares l2_cache's defaults.
L2_INFO = cache_info(1024, 4, 16, vipt=False)
PENUMBRA1_L1 = cache_info(64, 1, 16, vipt=True)
PENUMBRA2_L1 = cache_info(128, 2, 16, vipt=True)
PENUMBRA3_L1 = cache_info(256, 2, 16, vipt=True)
CORE_IDENTITY = {
    "penumbra1":   {2: 0x0000312F, 3: PENUMBRA1_L1, 4: PENUMBRA1_L1, 5: L2_INFO},
    "penumbra2":   {2: 0x0000322F, 3: PENUMBRA2_L1, 4: PENUMBRA2_L1, 5: L2_INFO},
    "penumbra2_5": {2: 0x352E322F, 3: PENUMBRA2_L1, 4: PENUMBRA2_L1, 5: L2_INFO},
    "penumbra3":   {2: 0x0000332F, 3: PENUMBRA3_L1, 4: PENUMBRA3_L1, 5: L2_INFO},
}
CTRL_READBACK = {6: 0, 7: 1}


def mmu_config(ver, sets, ways, pinned=8, page_log2=12, asid=8):
    """An MMU_CONFIG word in doc/system/mmu.md's layout."""
    return (ver | pinned << 4 | (sets.bit_length() - 1) << 10 | ways << 15
            | page_log2 << 20 | asid << 25)


# mmu_v2.s under +mmu=2:256x2: the slots read back, nothing past the end,
# and what each invalidation register leaves (the program's comments
# name the entries).
MMU_V2 = {2: mmu_config(2, 256, 2), 3: 0x01012305, 4: 0x10123019, 5: 0x00042099,
          6: 0, 7: 0b10110, 8: 0b110, 9: 0b10000, 11: 0x77, 12: 0x0ABCDE05}

# audio.sv's CAP_VALUE at the board's four channels: version 1, PITCH,
# VOLUME and STATUS.  audio_status.s leaves the class of the chain's
# fourth device in R8 (CLASS_AUDIO) and queues two frames, so a LEVEL of
# DEPTH - 2 means a device that is not up held them.
AUDIO_DEPTH = 1024
AUDIO_CAP = 1 | (1 << 8) | (1 << 9) | (1 << 13) | (4 << 16)
AUDIO_HELD = AUDIO_DEPTH - 2
ST_BUSY, ST_FAULT = 1, 2

# The Ethernet model as the cases attach it: four receive slots and two
# transmit slots of 2048 bytes, a link the device manages itself at
# 100 Mbit/s, and a factory station address.  The values follow
# doc/system/devices/ethernet.md: CAP is version 1 with ADDR and the
# 100 Mbit/s bit of SPEEDS, a GEOM is the slot count under the slot
# size, and LINK is UP with SPEED 1.
ETH_ARGS = ["+eth=loop,rx=4,tx=2"]
ETH_RX_SLOTS, ETH_TX_SLOTS, ETH_SLOT_SIZE = 4, 2, 2048
ETH_CAP = 1 | (1 << 9) | (1 << 17)
ETH_LINK = 1 | (1 << 8)
ETH_IRQ_RX, ETH_IRQ_TX = 1, 2
# The first word of a receive slot holding a frame to the model's
# station address, 02:50:..., padded to the 60-byte minimum.
ETH_PADDED_TO_STATION = 60 | (0x02 << 16) | (0x50 << 24)
# eth_tap_peer.py answers the guest from 02:00:00:00:00:02 with type
# 0x88B5 and the payload "PEER", in a frame of 18 bytes.  The fourth
# word of the slot holds the source's last two bytes and the type, the
# fifth the payload, and the sixth is the padding to 60 bytes.
ETH_TAP_ARGS = ["+eth=tap:tap0,rx=4,tx=2"]
ETH_PEER_TAIL_AND_TYPE = 0x00 | (0x02 << 8) | (0x88 << 16) | (0xB5 << 24)
ETH_PEER_PAYLOAD = int.from_bytes(b"PEER", "little")
ETH_PEER_BURST = 8      # eth_tap_peer.py's BURST
# With a PHY to manage (+eth=...,mdio) CAP gains MDIO and lists every
# speed up to the device's own, here 10 and 100 Mbit/s.  The PHY model
# answers at address 1 with the standard registers: its first ID word,
# and a status register that reads its abilities plus "negotiation
# complete" and "link up" once the link has settled.
ETH_CAP_MANAGED = 1 | (1 << 8) | (1 << 9) | (0b011 << 16)
ETH_PHY_ID1 = 0x5045
ETH_PHY_STATUS_UP = 0x7909 | 0x0020 | 0x0004
ETH_MDIO_BUSY = 1 << 31


def mdio_read(phy, reg, value):
    """MDIO once a read of `reg` at address `phy` has returned `value`."""
    return (phy << 24) | (reg << 16) | value

# A user and network namespace of a case's own, where an unprivileged
# user may make a TAP device.
NAMESPACE = ["unshare", "-rn"]


def can_make_namespace():
    try:
        return subprocess.run(NAMESPACE + ["true"], capture_output=True).returncode == 0
    except OSError:
        return False


def hierarchy(l1i=(0, 0), l1d=(0, 0, 0, 0), l2=(0, 0, 0, 0), fills=None, prediction=None):
    """A cache model's expected counts for one hierarchy over one window:
    L1I (read hits, read misses), L1D and L2 (read hits, read misses,
    write hits, write misses), and the L1 fills by (kind, mode) as
    (from L2, from memory).  Fills not listed must be zero.  `prediction`,
    when given, is the next-line buffer's (served once, served continue,
    look-ups, missing L2, unused)."""
    want = {"L1I": tuple(l1i) + (0, 0), "L1D": tuple(l1d), "L2": tuple(l2),
            "fills": fills or {}}
    if prediction is not None:
        want["prediction"] = tuple(prediction)
    return want


# cache_data.s, phase by phase (see the program's header).  A 16-byte line
# is 4 words, so an L2-served fill counts 4 L2 read hits and a
# memory-served one 1 read miss and 3 read hits.  Phase E's L2 sequence
# takes 5 misses and 3 hits under the L2's tree-PLRU.
DATA_PENUMBRA1 = hierarchy(                 # 1 KiB direct-mapped L1-D
    l1d=(0, 128 + 128 + 1 + 8, 0, 2),       # A, B, D, E all miss; C both miss
    l2=(128 * 3 + 128 * 4 + 4 + (5 * 3 + 3 * 4), 128 + 5, 1, 1),
    fills={("D", "super"): (128 + 1 + 3, 128 + 5)})
DATA_PENUMBRA2 = hierarchy(                 # 4 KiB 2-way L1-D
    l1d=(128, 128 + 1 + 8, 1, 1),           # B hits; C hits 0x1000
    l2=(128 * 3 + 4 + (5 * 3 + 3 * 4), 128 + 5, 1, 1),
    fills={("D", "super"): (1 + 3, 128 + 5)})

# cache_ifetch.s: 33 fetches.  Sharing a set, the loop's two lines miss
# twice a round for ten rounds and the exit hits; apart, each misses once.
IFETCH_PENUMBRA1 = hierarchy(
    l1i=(10 + 3, 20), l2=(2 * 3 + 18 * 4, 2, 0, 0), fills={("I", "super"): (18, 2)})
IFETCH_PENUMBRA2 = hierarchy(
    l1i=(31, 2), l2=(2 * 3, 2, 0, 0), fills={("I", "super"): (0, 2)})

# cache_nextline.s, step by step (see the program's header).  Each pass
# of the 8-line run fetches 29 instructions and misses 8 times; each
# one-instruction snippet misses once.  Misses: run 8 + r1 + run 8 + r0 +
# r1 + last; the passes' other 21 fetches hit.  Look-ups follow every
# miss but last's (19), missing L2 in pass 1 (8), after r1 twice and
# after the warm run's final line; the one r0 loads is invalidated unused.
NEXTLINE = hierarchy(
    l1i=(2 * 21, 8 + 1 + 8 + 1 + 1 + 1),
    l2=(8 * 3 + 3 + 8 * 4 + 3 + 4 + 3, 8 + 1 + 1 + 1, 0, 0),
    fills={("I", "super"): (8 + 1, 8 + 1 + 1 + 1)},
    prediction=(4, 7, 8 + 1 + 8 + 1 + 1, 8 + 1 + 1 + 1, 1))

# cache_window.s: inside the window, 16 re-loaded lines hit and 16 new
# ones miss in both levels.
WINDOW_PENUMBRA1 = hierarchy(
    l1d=(16, 16, 0, 0), l2=(16 * 3, 16, 0, 0), fills={("D", "user"): (0, 16)})


def store_buffer(stores, drained, waits, waited):
    """A store buffer's expected counts over one window: stores as
    (buffered, merged, entries, uncached), entries drained by burst
    length in words (1, 2, 3, 4), waits in model cycles as (buffer full,
    I fills, D fills, uncached), and the accesses behind them as (stores
    that found the buffer full, then I fills, D fills and uncached
    accesses each as (waited, all))."""
    full, (i_waited, i_all), (d_waited, d_all), (u_waited, u_all) = waited
    return {"sb": {"stores": tuple(stores), "drained": tuple(drained), "waits": tuple(waits),
                   "waited": (full, i_waited, i_all, d_waited, d_all, u_waited, u_all)}}


# sb_window.s inside the window, T being the cycle `BL body` issues after
# its fill; each instruction takes one cycle (two in the cpi case).  A's
# four stores land at T+1..T+4, completing it, so it drains from T+4; B's
# store lands at T+5 and C's at T+6, which closes B.  Both loads miss L1
# and L2 (24 cycles each).  D's store follows; the uncached load after it
# waits for D whatever the policy, so the uncached store finds every
# buffer empty.  The window's one I fill, and its first, waits for nothing.
#   8l/all/bus:   the load at T+7 waits for A (to T+17), B (T+22) and C
#                 (T+27): 20.  The uncached load closes D and waits 5.
#   8l/line/bus:  the first load waits only for A in flight: 10.  B starts
#                 after that fill, at T+41, and the load from C at T+42
#                 waits for B and C: to T+51, 9.
#   combine:      A drains in 11 (to T+15): 18 for all; for line 8, then
#                 B from T+39 and C behind it, 9 at T+40: 17.
#   8w/all/bus:   every store is a one-word entry, ready at once and 5
#                 cycles to drain: the first load waits to T+31, 24.  D is
#                 draining already when the uncached load comes: 4.
#   2l/all/bus:   C's store finds both entries taken and waits for A: 11.
#                 The first load then waits for B and C: 9.
#   3:4:4:5:      A drains in 5 and the rest in 3: 8, and D 3.
#   post:         both fills come from memory and need the bus, so posting
#                 changes nothing: 8l/line/bus/post waits as 8l/line/bus.
#   8l/merge/bus: the first load waits for A in flight: 10.  B starts after
#                 that fill and the load from C waits only for it: 4.  C,
#                 still open, drains once D's store closes it, and the
#                 uncached load waits for C and then D: 9.
#   cpi 2:        stores at T+2..T+12, A draining from T+8 to T+21: the
#                 load at T+14 waits to T+31, 17.
SB_LINES = ((7, 3, 4, 1), (3, 0, 0, 1))
SB_FIRST_FILL_WAITS = (0, (0, 1), (1, 2), (1, 2))    # only the first load waits
SB_BOTH_FILLS_WAIT = (0, (0, 1), (2, 2), (1, 2))
SB_WINDOW_DEFAULT = {
    "sb 8l/all/bus": store_buffer(*SB_LINES, (0, 0, 20, 5), SB_FIRST_FILL_WAITS),
    "sb 8l/line/bus": store_buffer(*SB_LINES, (0, 0, 19, 5), SB_BOTH_FILLS_WAIT),
    "sb 8l/all/combine": store_buffer(*SB_LINES, (0, 0, 18, 5), SB_FIRST_FILL_WAITS),
    "sb 8l/line/combine": store_buffer(*SB_LINES, (0, 0, 17, 5), SB_BOTH_FILLS_WAIT),
    "sb 8w/all/bus": store_buffer((7, 0, 7, 1), (7, 0, 0, 0), (0, 0, 24, 4),
                                  SB_FIRST_FILL_WAITS),
}
SB_WINDOW_LISTED = {
    "sb 2l/all/bus": store_buffer(*SB_LINES, (11, 0, 9, 5), (1,) + SB_FIRST_FILL_WAITS[1:]),
    "sb 8l/all/3:4:4:5": store_buffer(*SB_LINES, (0, 0, 8, 3), SB_FIRST_FILL_WAITS),
    "sb 8l/merge/bus": store_buffer(*SB_LINES, (0, 0, 14, 9), SB_BOTH_FILLS_WAIT),
    "sb 8l/line/bus/post": store_buffer(*SB_LINES, (0, 0, 19, 5), SB_BOTH_FILLS_WAIT),
}

# sb_posted.s inside the window, T as in sb_window.s: A completes at T+4
# and drains from then on, B's store lands at T+5 and C's at T+6; the load
# at T+7 hits L2 (5 cycles) and the uncached store follows it.
#   8l/all/bus:        the load waits for A (to T+17), B and C (T+27): 20,
#                      and the uncached store finds the buffer empty.
#   8l/line/bus:       the load waits for A in flight: 10.  B drains after
#                      it, and the uncached store waits for B and C: 9.
#   8l/all/bus/post:   L2 took A's write by T+6 and B's by T+8; C's L2 write
#                      ends at T+10, and the load waits for it: 3.  The
#                      uncached store waits for the bus writes: A, then B
#                      and C from T+17 to T+27: 11.
#   8l/line/bus/post:  the load waits only for B's L2 write, to T+8: 1.
#                      The uncached store at T+14 waits to T+27: 13.
SB_POSTED_STORES = ((6, 3, 3, 1), (2, 0, 0, 1))
SB_POSTED_ONE_FILL = (0, (0, 1), (1, 1), (0, 1))     # the load waits; the store does not
SB_POSTED_BOTH = (0, (0, 1), (1, 1), (1, 1))
SB_POSTED = {
    "sb 8l/all/bus": store_buffer(*SB_POSTED_STORES, (0, 0, 20, 0), SB_POSTED_ONE_FILL),
    "sb 8l/line/bus": store_buffer(*SB_POSTED_STORES, (0, 0, 10, 9), SB_POSTED_BOTH),
    "sb 8l/all/bus/post": store_buffer(*SB_POSTED_STORES, (0, 0, 3, 11), SB_POSTED_BOTH),
    "sb 8l/line/bus/post": store_buffer(*SB_POSTED_STORES, (0, 0, 1, 13), SB_POSTED_BOTH),
}
SB_WINDOW_CPI2 = {"sb 8l/all/bus": store_buffer(*SB_LINES, (0, 0, 17, 5), SB_FIRST_FILL_WAITS)}
SB_WINDOW_ARGS = ["+cachemodel=4k/2:4k/2:64k/4", "+model_start=0xFFFF0800",
                  "+model_stop=0xFFFF0900"]


def predictor(fetch, decode, execute, transfers=None):
    """A branch predictor's expected redirects over one window: at fetch,
    at decode, and at execute by kind as (conditional, direct, calls,
    indirect calls, returns, indirect).  `transfers` is the window's
    (conditional, taken, direct, calls, indirect calls, returns,
    indirect), bp_basic.s's by default."""
    return {"bp": {"transfers": transfers or BP_TRANSFERS,
                   "redirects": (fetch, decode, sum(execute)) + tuple(execute)}}


# bp_basic.s: 6 conditional (4 taken), 0 direct, 2 calls, 1 indirect call,
# 3 returns, 2 indirect.  Every taken transfer costs a redirect somewhere;
# a not-taken one only when predicted taken.  Word n of the program is
# each table's slot n.
#   none:        every taken transfer resolves in EX.
#   btb32:       direct only, taken while held: C1 hits once and then
#                mispredicts its exit, C2 misses and mispredicts its exit;
#                calls miss once each; nothing indirect is held.
#   ibtb32:      as btb32, plus I1 hitting on its second run; each return
#                finds the previous caller's target.
#   dec:         backward conditionals and calls redirect at decode, the
#                loops mispredict their exits and the forward C3 its taken.
#   dec+ras4:    as dec, and the stack returns all three right.
#   dec+bim64:   counters start weakly not taken: C1 learns by its second
#                run and mispredicts its exit; C2 and C3 never predict taken.
#   dec+gs64:    history moves C1's second run to a fresh counter, so only
#                calls redirect at decode.
#   ibtb32+ras4+dec: the buffer takes C1's and I1's second runs at fetch;
#                decode takes the first runs, the calls and every return.
BP_TRANSFERS = (6, 4, 0, 2, 1, 3, 2)
# bp_ras.s: six calls and six returns, nested.  Pre-decode redirects every
# call; a 4-deep ring predicts the four innermost returns and misses the
# other two, stale or empty; an 8-deep one predicts all six.
BP_RAS_TRANSFERS = (0, 0, 0, 6, 0, 6, 0)
BP_RAS = {
    "bp dec": predictor(0, 6, (0, 0, 0, 0, 6, 0), BP_RAS_TRANSFERS),
    "bp dec+ras4": predictor(0, 10, (0, 0, 0, 0, 2, 0), BP_RAS_TRANSFERS),
    "bp dec+ras4e": predictor(0, 10, (0, 0, 0, 0, 2, 0), BP_RAS_TRANSFERS),
    "bp dec+ras8": predictor(0, 12, (0, 0, 0, 0, 0, 0), BP_RAS_TRANSFERS),
}
# bp_recurse.s: six calls, five from one recursive site, six returns, and
# one forward conditional taken once (BTFN misses it).  Every ring slot
# holds the site's address once the recursion wraps a 4-deep ring: the
# plain ring's stale fifth pop is right and the counting ring's empty one
# predicts nothing; both miss the return to _start.
BP_RECURSE_TRANSFERS = (6, 1, 0, 6, 0, 6, 0)
BP_RECURSE = {
    "bp dec+ras4": predictor(0, 11, (1, 0, 0, 0, 1, 0), BP_RECURSE_TRANSFERS),
    "bp dec+ras4e": predictor(0, 10, (1, 0, 0, 0, 2, 0), BP_RECURSE_TRANSFERS),
    "bp dec+ras8": predictor(0, 12, (1, 0, 0, 0, 0, 0), BP_RECURSE_TRANSFERS),
}
BP_BASIC = {
    "bp none": predictor(0, 0, (4, 0, 2, 1, 3, 2)),
    "bp btb32": predictor(1, 0, (5, 0, 2, 1, 3, 2)),
    "bp ibtb32": predictor(2, 0, (5, 0, 2, 1, 3, 1)),
    "bp dec": predictor(0, 5, (3, 0, 0, 1, 3, 2)),
    "bp dec+ras4": predictor(0, 8, (3, 0, 0, 1, 0, 2)),
    "bp dec+bim64": predictor(0, 3, (5, 0, 0, 1, 3, 2)),
    "bp dec+gs64": predictor(0, 2, (4, 0, 0, 1, 3, 2)),
    "bp ibtb32+ras4+dec": predictor(2, 7, (3, 0, 0, 1, 0, 1)),
}


def pairing(insns, pairs=0, fused=None, forward=None, younger=None, alone=None, one=None,
            several=(0, {}), idle=0, fetch=None, sizes=None):
    """An issue stage's expected counts over one window, each a dict of
    the report's names holding only what is not zero: instructions
    `fused` away by kind; `pairs` by the `forward` each relies on ("none"
    for those that need no forward) and by the `younger` op's class;
    singles that had no candidate to pair with, by why (`alone`); singles
    whose candidate was kept out by `one` cause; and `several` as (singles
    kept out by more than one cause, how often each cause was among
    them).  `idle` cycles issue nothing, which takes a fetch stage;
    `fetch` is that stage's fetches, bubbles and cycles with a full
    queue.  `sizes`, checked where given, is the cycles by instructions
    issued."""
    fused = fused or {}
    ops = insns - sum(fused.values())
    singles = ops - 2 * pairs
    counts = {"issued": (insns, ops, pairs + singles + idle, pairs, singles, idle),
              "fused": fused, "pairs by forward": forward or {},
              "pairs by younger": younger or {}, "singles, no candidate": alone or {},
              "singles, one cause": one or {}, "singles, several causes": several}
    if fetch is not None:
        counts["fetch"] = fetch
    if sizes is not None:
        counts["cycles by instructions issued"] = sizes
    return {"pair": counts}


# Every pair_*.s probe ends in a DI, which issues alone, and so does a
# probe's last op when it did not pair: both count under "system", as
# does each setup instruction.  The BREAK that ends the window counts
# under "end".
#
# pair_slots.s: 35 instructions, 13 of them DI or BREAK.  No probe has a
# dependency, so slots, units and memory accesses decide:
#   single:       X issues nothing: A B F H lack only a slot for the older
#                 op; C D E also make two memory accesses and I two branches;
#                 both B's are taken with the next op off the path.
#   dual:         A (MOV; LDW) and F (MOV; B) pair.  B and H start with an op
#                 X cannot issue; C D E I as before.
#   +flex:        B (LDW; MOV) and H (BEQ; MOV) pair with the older op in Y.
#   x:simple:ld:  B pairs in order; C now lacks only a second memory access.
#   +mem:ld2:     C (LDW; LDW) pairs; a store still pairs with no access.
#   x:all+mem:2:  every probe pairs but I, two branches for one branch unit.
PAIR_SLOTS_SEVERAL = (4, {"slot-older": 4, "memory": 3, "unit": 1})
PAIR_SLOTS = {
    "pairing single": pairing(35, alone={"taken": 2, "system": 24, "end": 1},
                              one={"slot-older": 4}, several=PAIR_SLOTS_SEVERAL),
    "pairing dual": pairing(35, 2, forward={"none": 2}, younger={"ld": 1, "br": 1},
                            alone={"taken": 1, "system": 23, "end": 1},
                            one={"slot-older": 2}, several=PAIR_SLOTS_SEVERAL),
    "pairing dual+flex": pairing(35, 4, forward={"none": 4}, younger={"mov": 2, "ld": 1, "br": 1},
                                 alone={"taken": 1, "system": 21, "end": 1},
                                 several=PAIR_SLOTS_SEVERAL),
    "pairing dual+x:simple:ld": pairing(
        35, 3, forward={"none": 3}, younger={"mov": 1, "ld": 1, "br": 1},
        alone={"taken": 1, "system": 22, "end": 1}, one={"slot-older": 1, "memory": 1},
        several=(3, {"slot-older": 3, "memory": 2, "unit": 1})),
    "pairing dual+x:simple:ld+mem:ld2": pairing(
        35, 4, forward={"none": 4}, younger={"mov": 1, "ld": 2, "br": 1},
        alone={"taken": 1, "system": 21, "end": 1}, one={"slot-older": 1},
        several=(3, {"slot-older": 3, "memory": 2, "unit": 1})),
    "pairing dual+x:all+mem:2": pairing(
        35, 7, forward={"none": 7}, younger={"mov": 2, "ld": 3, "st": 1, "br": 1},
        alone={"taken": 1, "system": 18, "end": 1}, one={"unit": 1}),
}
# The default list holds single and dual among its stages.
PAIR_DEFAULT = {k: PAIR_SLOTS[k] for k in ("pairing single", "pairing dual")}

# pair_deps.s: 30 instructions, 11 of them DI or BREAK.  Each probe's
# younger op reads what its older op writes; H (LDW; STW) also starts
# with a load X cannot issue and makes two memory accesses.
#   dual:      nothing forwards: B A G wait for an operand, D E F for a
#              base, C for store data, I for a jump target.
#   fw:mov:    the copies pair: A (operand), D (base), I (target).
#   fw:imm:    B, the constant.
#   fw:data:   C; H's data is a load's, which no forward reaches in time.
#   fw:ea:     E, the base an ADD #imm makes; not D's copy or F's LUI.
#   fw:alu:    every operand: B A G.
#   all of them, with X issuing everything and two accesses a cycle: each
#              probe pairs under its narrowest forward, except F (a LUI
#              result as a base) and H (a load's data).
PAIR_DEPS_H = (1, {"slot-older": 1, "memory": 1, "data": 1})
PAIR_DEPS = {
    "pairing dual": pairing(30, alone={"system": 20, "end": 1},
                            one={"operand": 3, "base": 3, "data": 1, "target": 1},
                            several=PAIR_DEPS_H),
    "pairing dual+fw:mov": pairing(30, 3, forward={"mov": 3}, younger={"alu": 1, "ld": 1, "br": 1},
                                   alone={"system": 17, "end": 1},
                                   one={"operand": 2, "base": 2, "data": 1}, several=PAIR_DEPS_H),
    "pairing dual+fw:imm": pairing(30, 1, forward={"imm": 1}, younger={"mov": 1},
                                   alone={"system": 19, "end": 1},
                                   one={"operand": 2, "base": 3, "data": 1, "target": 1},
                                   several=PAIR_DEPS_H),
    "pairing dual+fw:data": pairing(30, 1, forward={"data": 1}, younger={"st": 1},
                                    alone={"system": 19, "end": 1},
                                    one={"operand": 3, "base": 3, "target": 1},
                                    several=PAIR_DEPS_H),
    "pairing dual+fw:ea": pairing(30, 1, forward={"ea": 1}, younger={"ld": 1},
                                  alone={"system": 19, "end": 1},
                                  one={"operand": 3, "base": 2, "data": 1, "target": 1},
                                  several=PAIR_DEPS_H),
    "pairing dual+fw:alu": pairing(30, 3, forward={"alu": 3}, younger={"alu": 2, "mov": 1},
                                   alone={"system": 17, "end": 1},
                                   one={"base": 3, "data": 1, "target": 1}, several=PAIR_DEPS_H),
    "pairing dual+x:all+mem:2+fw:mov:imm:data:ea:alu": pairing(
        30, 7, forward={"mov": 3, "imm": 1, "data": 1, "ea": 1, "alu": 1},
        younger={"alu": 2, "mov": 1, "ld": 2, "st": 1, "br": 1},
        alone={"system": 13, "end": 1}, one={"base": 1, "data": 1}),
}

# pair_fuse.s: 26 instructions, 9 of them DI or BREAK.  B (LLI R5; LUI R6)
# never fuses and pairs wherever X issues.
#   single+fuse:lilui:  A's and I's LLI+LUI fuse; nothing pairs.  Every
#                       other older op lacks a slot, and all but B's also
#                       feed the op behind them.
#   dual+fuse:lilui:    the fused constants feed A's load and I's jump, C's
#                       LLIS its LUI, and each MOV the op behind it.
#   +fw:imm:            A's load and I's jump pair with the fused constant,
#                       three instructions a cycle, and C pairs.
#   dual+fuse:movop:    D fuses and then issues alone before its DI; E and F
#                       do not fuse; A and I issue as LLI, LUI and a third.
PAIR_FUSE = {
    "pairing single+fuse:lilui": pairing(
        26, fused={"lilui": 2}, alone={"system": 16, "end": 1}, one={"slot-older": 1},
        several=(6, {"slot-older": 6, "operand": 4, "base": 1, "target": 1})),
    "pairing dual+fuse:lilui": pairing(
        26, 1, fused={"lilui": 2}, forward={"none": 1}, younger={"mov": 1},
        alone={"system": 15, "end": 1}, one={"operand": 4, "base": 1, "target": 1}),
    "pairing dual+fuse:lilui+fw:imm": pairing(
        26, 4, fused={"lilui": 2}, forward={"none": 1, "imm": 3},
        younger={"mov": 2, "ld": 1, "br": 1},
        alone={"system": 12, "end": 1}, one={"operand": 3}),
    "pairing dual+fuse:movop": pairing(
        26, 1, fused={"movop": 1}, forward={"none": 1}, younger={"mov": 1},
        alone={"system": 15, "end": 1}, one={"operand": 5, "base": 1, "target": 1}),
}

# pair_trap.s: 13 instructions.  The MOV ahead of the trapping LDW and
# the LDW itself issue alone ("break"); DI, the handler's four
# instructions, the MOV before BREAK and, in the plain stage, the LLI
# before DI count under "system".
#   dual:               LLI feeds LUI, LUI the STW's data, and X cannot
#                       issue the STW ahead of the second LLI.
#   +fw:imm:data+flex:  LLI and LUI pair, and the STW pairs from Y with the
#                       LLI behind it.
PAIR_TRAP = {
    "pairing dual": pairing(13, alone={"break": 2, "system": 7, "end": 1},
                            one={"slot-older": 1, "operand": 1, "data": 1}),
    "pairing dual+fw:imm:data+flex": pairing(
        13, 2, forward={"none": 1, "imm": 1}, younger={"mov": 2},
        alone={"break": 2, "system": 6, "end": 1}),
}

# pair_flags.s: 27 instructions, 10 of them DI or BREAK.  One stage a
# case, so that a case names the flag rule it pins:
#   dual:           P1 (both write) and P2 (the older alone writes) pair,
#                   as do P5 and P6, whose flags are an earlier cycle's.
#                   P3's BEQ and P4's ADC wait for the flags.  P8's ADD
#                   and CMP pair, leaving its BEQ alone.
#   fw:cond:       P3 pairs.
#   fw:carry:       P4 pairs.
#   yflags:         P2 no longer pairs: its MOV would have to commit the
#                   ADD's flags.
#   yflags+fw:cond:carry:  P4 pairs, its ADC writing the flags itself;
#                   P3 does not, since its BEQ writes none.
#   fuse:cmpbr:     P3's and P8's CMP and BEQ fuse.  P3's issues alone; P8's
#                   joins the ADD, both writing the flags.
PAIR_FLAGS = {
    "dual": pairing(27, 5, forward={"none": 5},
                    younger={"alu": 2, "mov": 1, "adc": 1, "br": 1},
                    alone={"system": 14, "end": 1}, one={"flags": 2}),
    "dual+fw:cond": pairing(27, 6, forward={"none": 5, "cond": 1},
                             younger={"alu": 2, "mov": 1, "adc": 1, "br": 2},
                             alone={"system": 13, "end": 1}, one={"flags": 1}),
    "dual+fw:carry": pairing(27, 6, forward={"none": 5, "carry": 1},
                             younger={"alu": 2, "mov": 1, "adc": 2, "br": 1},
                             alone={"system": 13, "end": 1}, one={"flags": 1}),
    "dual+yflags": pairing(27, 4, forward={"none": 4}, younger={"alu": 2, "adc": 1, "br": 1},
                           alone={"system": 15, "end": 1}, one={"flags": 3}),
    "dual+yflags+fw:cond:carry": pairing(
        27, 5, forward={"none": 4, "carry": 1}, younger={"alu": 2, "adc": 2, "br": 1},
        alone={"system": 14, "end": 1}, one={"flags": 2}),
    "dual+fuse:cmpbr": pairing(27, 5, fused={"cmpbr": 2}, forward={"none": 5},
                               younger={"alu": 1, "mov": 1, "adc": 1, "br": 2},
                               alone={"system": 13, "end": 1}, one={"flags": 1}),
}
PAIR_FLAGS_CASES = {
    "dual": "an older flag writer keeps a flag reader out",
    "dual+fw:cond": "fw:cond lets a branch join",
    "dual+fw:carry": "fw:carry lets an ADC join",
    "dual+yflags": "yflags keeps a pair the older op's flags would outlive apart",
    "dual+yflags+fw:cond:carry": "yflags outranks a forward",
    "dual+fuse:cmpbr": "a fused compare-and-branch writes the flags",
}


def fusing(insns, fused):
    """A single-issue stage's expected counts where only fusion is at
    stake: `fused` instructions by kind, and so the ops and issue cycles
    left."""
    ops = insns - sum(fused.values())
    return {"pair": {"issued": (insns, ops, ops, 0, ops, 0), "fused": fused}}


# pair_opbr.s: 30 instructions, 11 of them DI or BREAK.
#   single+fuse:opbr:        Q1, Q5's ADD, Q7 and Q6 fuse with their
#                            branches; a compare, a shift and an ADC do not.
#   single+fuse:cmpbr:       Q2 alone.
#   single+fuse:cmpbr:opbr:  both sets.
#   single+fuse:movop:opbr:  Q5's MOV takes its ADD first, and an instruction
#                            is in one fusion only, so its branch stays; Q7's
#                            MOV does not match, and its SUB fuses.
#   dual+fuse:opbr:          Q7's MOV and its fused op pair, three
#                            instructions in the cycle.  Q5's MOV feeds its
#                            fused op; Q2's and Q3's branches wait for the
#                            flags; Q4's ADC also has no slot ahead of its
#                            branch.
PAIR_OPBR = {
    "pairing single+fuse:opbr": fusing(30, {"opbr": 4}),
    "pairing single+fuse:cmpbr": fusing(30, {"cmpbr": 1}),
    "pairing single+fuse:cmpbr:opbr": fusing(30, {"cmpbr": 1, "opbr": 4}),
    "pairing single+fuse:movop:opbr": fusing(30, {"movop": 1, "opbr": 3}),
    "pairing dual+fuse:opbr": pairing(
        30, 1, fused={"opbr": 4}, forward={"none": 1}, younger={"br": 1},
        alone={"system": 19, "end": 1}, one={"flags": 2, "operand": 1},
        several=(1, {"slot-older": 1, "flags": 1})),
}

# pair_addr.s: 40 instructions, 15 of them DI or BREAK.
#   single+fuse:shadd:          A and J's SHL and ADD; B's scale is too far,
#                               C's ADD reads R5 twice, D's sum is in R6.
#   single+fuse:addmem:         E, G and J's ADD and load; F's load writes
#                               another register, H adds an immediate, and
#                               I's ADD and branch are opbr's.
#   +agu:3:                     the same: three inputs is the default.
#   +agu:2:                     E and J, whose accesses have no offset; G's
#                               store has one.
#   single+fuse:shadd:addmem:   J's SHL takes its ADD first, and an
#                               instruction is in one fusion only.
#   single+fuse:opbr:addmem:    I fuses as opbr beside addmem's three.
#   dual+fuse:shadd:addmem:     nothing pairs: B C D wait for an operand,
#                               F H and J's load for a base, I's branch for
#                               the flags.
PAIR_ADDR = {
    "pairing single+fuse:shadd": fusing(40, {"shadd": 2}),
    "pairing single+fuse:addmem": fusing(40, {"addmem": 3}),
    "pairing single+fuse:addmem+agu:3": fusing(40, {"addmem": 3}),
    "pairing single+fuse:addmem+agu:2": fusing(40, {"addmem": 2}),
    "pairing single+fuse:shadd:addmem": fusing(40, {"shadd": 2, "addmem": 2}),
    "pairing single+fuse:opbr:addmem": fusing(40, {"opbr": 1, "addmem": 3}),
    "pairing dual+fuse:shadd:addmem": pairing(
        40, fused={"shadd": 2, "addmem": 2}, alone={"system": 28, "end": 1},
        one={"operand": 3, "base": 3, "flags": 1}),
}

# pair_wb.s: 30 instructions, 11 of them DI or BREAK.
#   single+fuse:addwb:            A, and D's and E's ADD and load; G's load
#                                 overwrites its base.
#   single+fuse:incmem:           B, C and F.
#   single+fuse:addmem:addwb:     G as addmem beside addwb's three.
#   single+fuse:addwb:incmem+agu:2:  every access here has no offset, so all
#                                 six fuse with a two-input address adder.
#   dual+fuse:addwb:incmem+flex:  E's fused load and its compare pair, the
#                                 load in Y; D's copy and fused load need
#                                 three write ports; G's load waits for its
#                                 base.
PAIR_WB = {
    "pairing single+fuse:addwb": fusing(30, {"addwb": 3}),
    "pairing single+fuse:incmem": fusing(30, {"incmem": 3}),
    "pairing single+fuse:addmem:addwb": fusing(30, {"addmem": 1, "addwb": 3}),
    "pairing single+fuse:addwb:incmem+agu:2": fusing(30, {"addwb": 3, "incmem": 3}),
    "pairing dual+fuse:addwb:incmem+flex": pairing(
        30, 1, fused={"addwb": 3, "incmem": 3}, forward={"none": 1}, younger={"alu": 1},
        alone={"system": 19, "end": 1}, one={"ports": 1, "base": 1}),
}

# pair_limits.s: 31 instructions, 11 of them DI or BREAK.
#   single+fuse:addmem:          A B C D: three reads is the default.
#   +reads:3:                    the same.
#   +reads:2:                    B C D; A's store would read R5, R9 and R10.
#                                B reads R10 once, for the add and as data.
#   dual+fuse:cmpbr+flex:        E F G H I pair, E's fused branch, F's branch
#                                and G's load as the older op in Y; A B D
#                                wait for a base, C for its base and data.
#   +last:br:                    E and F take no younger op ("order"); their
#                                MOVs issue alone ahead of a DI.
#   +last:br:ld:st:              G's load neither.  H and I still pair, the
#                                branch and the load the younger op.
PAIR_LIMITS = {
    "pairing single+fuse:addmem": fusing(31, {"addmem": 4}),
    "pairing single+fuse:addmem+reads:3": fusing(31, {"addmem": 4}),
    "pairing single+fuse:addmem+reads:2": fusing(31, {"addmem": 3}),
    "pairing dual+fuse:cmpbr+flex": pairing(
        31, 5, fused={"cmpbr": 1}, forward={"none": 5}, younger={"mov": 3, "br": 1, "ld": 1},
        alone={"system": 15, "end": 1}, one={"base": 3},
        several=(1, {"base": 1, "data": 1})),
    "pairing dual+fuse:cmpbr+flex+last:br": pairing(
        31, 3, fused={"cmpbr": 1}, forward={"none": 3}, younger={"mov": 1, "br": 1, "ld": 1},
        alone={"system": 17, "end": 1}, one={"base": 3, "order": 2},
        several=(1, {"base": 1, "data": 1})),
    "pairing dual+fuse:cmpbr+flex+last:br:ld:st": pairing(
        31, 2, fused={"cmpbr": 1}, forward={"none": 2}, younger={"br": 1, "ld": 1},
        alone={"system": 18, "end": 1}, one={"base": 3, "order": 3},
        several=(1, {"base": 1, "data": 1})),
}

# pair_ext.s: 33 instructions, 12 of them DI or BREAK.
#   single+fuse:ext:        A B C E, and H's shifts; D's distances differ,
#                           F shifts two registers, G is an SHL and an ADD,
#                           and I's SHL takes its distance from a register.
#   single+fuse:movop:ext:  H's MOV takes its SHL first, leaving the SAR.
#   single+fuse:ext:shadd:  G fuses as shadd beside ext's five.
#   dual+fuse:ext:          F's two shifts pair; D, G and I wait for an
#                           operand, and so does H's fused op for its MOV.
PAIR_EXT = {
    "pairing single+fuse:ext": fusing(33, {"ext": 5}),
    "pairing single+fuse:movop:ext": fusing(33, {"movop": 1, "ext": 4}),
    "pairing single+fuse:ext:shadd": fusing(33, {"ext": 5, "shadd": 1}),
    "pairing dual+fuse:ext": pairing(
        33, 1, fused={"ext": 5}, forward={"none": 1}, younger={"sh": 1},
        alone={"system": 21, "end": 1}, one={"operand": 4}),
}

# pair_reach.s: 9 instructions, three LLI+LUI pairs: one aligned in a
# line, one straddling two lines, one in a line from an odd word.
PAIR_REACH = {
    "pairing single+fuse:lilui": fusing(9, {"lilui": 3}),
    "pairing single+fuse:lilui+reach:any": fusing(9, {"lilui": 3}),
    "pairing single+fuse:lilui+reach:line": fusing(9, {"lilui": 2}),
    "pairing single+fuse:lilui+reach:pair": fusing(9, {"lilui": 1}),
}


# pair_fetch.s: 12 instructions: six MOVs, a taken B, four MOVs and the
# BREAK.  With every instruction at hand, `dual` pairs the MOVs two by
# two, and the B ("taken") and the BREAK ("end") issue alone.  The third
# MOV of the B's target line reads the second, which keeps `single`'s
# second from it for two reasons and would keep `dual`'s, were those two
# ever older and younger.  A fetch stage's first cycle finds the queue
# empty and is idle.  Fetch runs a cycle ahead of issue, so where the
# stage does not say otherwise the two-wide ones pair as `dual` does.
#   single+fetch:1:   the queue never holds two instructions, so each
#                     issues with nothing behind it ("fetch").  The cycle
#                     after the B's fetch is a bubble, and the one after
#                     that idle.
#   single+fetch:2+queue:4:  fetch outruns issue, and twice finds the
#                     queue without room for two.
#   dual+fetch:2:     the B is fetched alone, the next fetch waits out the
#                     bubble, and issue is idle for a cycle behind the B.
#   +fetch:2l:        the target line's first two MOVs arrive together,
#                     from an odd word, and pair.  Its third is fetched
#                     alone and issues with nothing behind it; the MOV
#                     after it is left with the BREAK ("system").
#   +fetch:4l:        the B arrives with the two MOVs ahead of it, and the
#                     bubble passes while they issue.
#   +fetch:4l+queue:4:  a fetch waits for an empty queue, twice a cycle
#                     longer than without the limit; the target line's
#                     last MOV again issues alone.
#   +fetch:4+queue:4:  across the line, that MOV arrives with the one
#                     behind it, and they pair.
#   +bubble:0, +bubble:2:  the idle cycle behind the B goes, or is two.
PAIR_FETCH_AS_DUAL = dict(pairs=5, forward={"none": 5}, younger={"mov": 5},
                          alone={"taken": 1, "end": 1}, sizes={"one": 2, "two": 5})
PAIR_FETCH_LINE_END = dict(pairs=4, forward={"none": 4}, younger={"mov": 4},
                           alone={"taken": 1, "system": 1, "end": 1, "fetch": 1},
                           sizes={"one": 4, "two": 4})
PAIR_FETCH_SINGLE = dict(alone={"taken": 1, "system": 1, "end": 1}, one={"slot-older": 8},
                         several=(1, {"slot-older": 1, "operand": 1}))
PAIR_FETCH = {
    "pairing single": pairing(12, **PAIR_FETCH_SINGLE, sizes={"one": 12}),
    "pairing dual": pairing(12, **PAIR_FETCH_AS_DUAL),
    "pairing single+fetch:1": pairing(12, alone={"taken": 1, "end": 1, "fetch": 10}, idle=2,
                                      fetch={"fetches": 12, "bubbles": 1}, sizes={"one": 12}),
    "pairing single+fetch:2+queue:4": pairing(
        12, **PAIR_FETCH_SINGLE, idle=1, fetch={"fetches": 7, "bubbles": 1, "full": 2}),
    "pairing dual+fetch:2": pairing(12, **PAIR_FETCH_AS_DUAL, idle=2,
                                    fetch={"fetches": 7, "bubbles": 1}),
    "pairing dual+fetch:2l": pairing(12, **PAIR_FETCH_LINE_END, idle=2,
                                     fetch={"fetches": 7, "bubbles": 1}),
    "pairing dual+fetch:4l": pairing(12, **PAIR_FETCH_AS_DUAL, idle=1,
                                     fetch={"fetches": 4, "bubbles": 1}),
    "pairing dual+fetch:4l+queue:4": pairing(12, **PAIR_FETCH_LINE_END, idle=1,
                                             fetch={"fetches": 4, "bubbles": 1, "full": 2}),
    "pairing dual+fetch:4+queue:4": pairing(12, **PAIR_FETCH_AS_DUAL, idle=1,
                                            fetch={"fetches": 4, "bubbles": 1, "full": 2}),
    "pairing dual+fetch:2+bubble:0": pairing(12, **PAIR_FETCH_AS_DUAL, idle=1,
                                             fetch={"fetches": 7}),
    "pairing dual+fetch:2+bubble:2": pairing(12, **PAIR_FETCH_AS_DUAL, idle=3,
                                             fetch={"fetches": 7, "bubbles": 2}),
}

# pair_reach.s behind a fetch stage: two instructions fuse only when the
# queue holds both.
#   fetch:1:  it never does: nine singles, each LLI and LUI with nothing
#             behind it.
#   fetch:2:  all three pairs fuse, the straddling one too: its LUI
#             arrives while the DI ahead of its LLI issues.  The first
#             fused op issues with nothing behind it, the second ahead of
#             the third pair's LLI, which X cannot issue.
PAIR_FETCH_FUSE = {
    "pairing single+fuse:lilui+fetch:1": pairing(
        9, alone={"system": 2, "end": 1, "fetch": 6}, idle=1, fetch={"fetches": 9}),
    "pairing single+fuse:lilui+fetch:2": pairing(
        9, fused={"lilui": 3}, alone={"system": 3, "end": 1, "fetch": 1}, one={"slot-older": 1},
        idle=1, fetch={"fetches": 5}, sizes={"one": 3, "two": 3}),
}

# pair_trap.s behind a fetch stage: the trapping LDW ends its fetch and
# redirects it, as the handler's ERET does, so two bubbles; each op issues
# as it does with every instruction at hand.
#   fetch:2:   seven fetches.
#   fetch:4l:  the handler's first three instructions end their line, and
#              the ERET is the first word of the next.  It is fetched
#              alone: a fetch crosses no line, not even for the
#              instruction that would end it.  Six fetches, and issue
#              falls far enough behind that two find the queue full.
PAIR_FETCH_TRAP_ISSUE = dict(alone={"break": 2, "system": 7, "end": 1},
                             one={"slot-older": 1, "operand": 1, "data": 1}, idle=1)
PAIR_FETCH_TRAP = {
    "pairing dual+fetch:2": pairing(13, **PAIR_FETCH_TRAP_ISSUE,
                                    fetch={"fetches": 7, "bubbles": 2}),
    "pairing dual+fetch:4l": pairing(13, **PAIR_FETCH_TRAP_ISSUE,
                                     fetch={"fetches": 6, "bubbles": 2, "full": 2}),
}

# pair_irq.s: 12 instructions.  The MOV the interrupt is entered behind
# issues alone ("break"), though the handler's first instruction is no
# system one.  STW, EI, the handler's LUI ahead of its WRSPR, WRSPR and
# ERET count under "system"; the two MOVs after the return pair.
#   dual:        each LLI feeds its LUI, and the first LUI the STW's data.
#   fuse:lilui:  both constants fuse; the first feeds the STW's data and
#                the second issues ahead of the WRSPR.
#   fetch:2:     the interrupted MOV ends its fetch and redirects it, as
#                the ERET does: two bubbles.
PAIR_IRQ_ALONE = {"break": 1, "system": 5, "end": 1}
PAIR_IRQ = {
    "pairing dual": pairing(12, 1, forward={"none": 1}, younger={"mov": 1}, alone=PAIR_IRQ_ALONE,
                            one={"operand": 2, "data": 1}),
    "pairing dual+fuse:lilui": pairing(12, 1, fused={"lilui": 2}, forward={"none": 1},
                                       younger={"mov": 1}, alone=PAIR_IRQ_ALONE, one={"data": 1}),
    "pairing dual+fetch:2": pairing(12, 1, forward={"none": 1}, younger={"mov": 1},
                                    alone=PAIR_IRQ_ALONE, one={"operand": 2, "data": 1}, idle=1,
                                    fetch={"fetches": 7, "bubbles": 2}),
}


def pair_case(name, program, stages, plusargs=()):
    """A case running `program` through the stages of `stages`, a dict
    of "pairing <item>" labels."""
    items = ",".join(k[len("pairing "):] for k in stages)
    return Case(name, program, ["+pairmodel=" + items, *plusargs],
                windows=[("reset to exit", stages)])


def waits(insns, waited=None, late=0, cond_late=0, both=0):
    """A load-use pipeline's expected counts over one window of
    lu_waits.s: the cycles `waited`, by the need each is charged to; the
    ALU ops executed `late`; and of the two conditional branches, of
    which the first is mispredicted, those resolved late (`cond_late`)
    and the mispredicted ones among them (`both`)."""
    waited = waited or {}
    cond = {"late": cond_late, "mispredicted": 1, "both": both}
    return {"lu": {"instructions": (insns, sum(waited.values())), "waited": waited,
                   "alu ops": (10, late),
                   "conditional branches": (2, {k: v for k, v in cond.items() if v})}}


# lu_waits.s: 29 instructions, 10 of them ALU ops, with a load's wait of
# two slots unless the item says otherwise.  A reader directly behind a
# load waits the load's wait where it needs the value at EX:
#   ex:        A's ADD, D's CMP, E's first ADD, F's ADD and M's MUL wait
#              for an operand, B's load for its base and T's JMP for its
#              target.  C's store never waits for data.
#   ex+late:   the five ALU ops execute late in place of waiting, and with
#              them E's second ADD, whose operand is then late too.  D's
#              BNE resolves late and is mispredicted.  F's load waits
#              three slots for the base its late ADD writes; the MUL, the
#              base and the target wait as before.
#   late:      every ALU op but the two LLIs executes late, and both
#              branches resolve late.  G's load now waits three slots for
#              its base, as F's does.
#   wait:1:    each wait is one slot, and F's behind a late ADD two.
LU_WAITS = {
    "load-use ex": waits(29, {"operand": 10, "base": 2, "target": 2}),
    "load-use ex+late": waits(29, {"operand": 2, "base": 5, "target": 2},
                              late=5, cond_late=1, both=1),
    "load-use late": waits(29, {"operand": 2, "base": 8, "target": 2},
                           late=8, cond_late=2, both=1),
    "load-use ex+wait:1": waits(29, {"operand": 5, "base": 1, "target": 1}),
    "load-use ex+late+wait:1": waits(29, {"operand": 1, "base": 3, "target": 1},
                                     late=5, cond_late=1, both=1),
}


# ops_muldiv.s's window: 47 instructions.  The fourteen probes are three
# instructions each, their operands 19 LLIs and 9 MOVs, and a branch
# leaves the window; DZ's trap adds the handler's RDSPR, ADD, WRSPR and
# ERET.  DZ counts as an instruction but not as a divide: it produced
# nothing.
OPS_MULDIV_CLASSES = {"total executed": 47, "const materialize": 19, "MOV reg-reg": 9,
                      "MUL/DIV": 14, "branch": 1, "sys/spr": 3, "ALU immediate": 1}


def operands(steps_each, rd, rs, shorter):
    """+opstats' expected counts over the window of ops_muldiv.s: the
    instruction classes, and a census whose four multiplies are of
    `steps_each` steps and need `rd`, `rs` and `shorter` of them in all,
    by the operand walked.  The nine divides are the same on every core."""
    return {"ops": {"classes": OPS_MULDIV_CLASSES,
                    "multiplies": (4, steps_each, rd, rs, shorter),
                    "divides": (9, 32, 127, 90),
                    "divides by steps needed aligned": {"none": 2, "1-8": 3, "9-16": 2,
                                                        "17-32": 2}}}


# A multiply needs the steps that cover the significant bits of the
# operand walked, and a divide one for each quotient bit it cannot skip.
#   multiplies, bits of Rd / Rs / the shorter:
#       M1 5 / 13 / 5     M2 17 / 2 / 2     M3 2 / 9 / 2     M4 32 / 0 / 0
#     At one bit a step (penumbra1) those are the steps: 56, 24 and 9 in
#     all.  At four (penumbra2_5): 2+5+1+8, 4+1+3+0 and 2+1+1+0.
#   divides, past the dividend's leading zeros: its bits, 127 in all.
#   divides, aligned to the divisor: leading dividend bits fewer than
#     the divisor's cannot produce a quotient 1, and are skipped too.
#       D1 12 by 5: 8     D2 13 by 5: 9     D3 32 by 17: 16
#       D4 21 by 5: 17    D5 32 by 1: 32    D6 3 by 7: none
#       D7 4 by 4: 1      D8 10 by 4: 7     D9 none by 3: none
OPS_MULDIV = {"penumbra1": operands(32, 56, 24, 9), "penumbra2_5": operands(8, 16, 8, 4)}
OPS_MULDIV_ARGS = ["+opstats", "+model_start=0xFFFF0800", "+model_stop=0xFFFF0900"]


def tlb_misses(user=0, supervisor=0):
    """A shadow TLB's expected misses over one window, or with the label
    "real" the refills the program's own miss handler took."""
    return {"tlb": (user, supervisor)}


# tlb_basic.s: the same in every layout, and what the real TLB takes.
TLB_BASIC = tlb_misses(user=2, supervisor=6)
TLB_BASIC_LAYOUTS = ["penumbra1", "penumbra2_5", "32x2/lru", "64x2/hash", "16x4", "1x64/lru"]
# tlb_assoc.s, whose real TLB conflicts on every set it shares.
TLB_ASSOC_LAYOUTS = ["32x4/lru", "64x2/lru", "16x4/lru", "1x64/lru"]


class Case:
    """One ISS run.  `regs` maps register number to its expected final
    value; `windows` lists the model windows expected in the report, each
    as (how it opened and closed, {label: expected counts}), a cache
    model's labelled by its hierarchy with hierarchy(...) and a TLB
    model's by "tlb <shadow>" with tlb_misses(...); `error` instead
    expects the ISS to refuse the options with a message matching the
    pattern.  `peer` names a script in this directory that is given the
    ISS's command line and runs it, inside a network namespace of its
    own, as the other station on the ISS's Ethernet; such a case is
    skipped where no namespace can be made."""

    def __init__(self, name, program, plusargs, regs=None, windows=None, error=None,
                 peer=None):
        self.name, self.program, self.plusargs = name, program, plusargs
        self.regs, self.windows, self.error = regs or {}, windows, error
        self.peer = peer


CASES = [
    Case(f"+core={core}", "core_identity.s", [f"+core={core}"],
         regs={**presented, **CTRL_READBACK})
    for core, presented in CORE_IDENTITY.items()
] + [
    Case("default core", "core_identity.s", [],
         regs={**CORE_IDENTITY["penumbra1"], **CTRL_READBACK}),
    Case("unknown core", "core_identity.s", ["+core=penumbra9"],
         error=r"unknown \+core generation 'penumbra9'"),
    Case("+mmu=2:256x2", "mmu_v2.s", ["+mmu=2:256x2"], regs=MMU_V2),
    Case("MMU version 1 reserves 6-8", "mmu_inval_reserved.s", [],
         regs={2: mmu_config(1, 32, 2), 3: 1}),
    Case("+mmu=2 keeps the core's TLB", "mmu_inval_reserved.s", ["+mmu=2"],
         regs={2: mmu_config(2, 32, 2), 3: 0}),
    Case("+core=penumbra3 presents its MMU", "mmu_inval_reserved.s", ["+core=penumbra3"],
         regs={2: mmu_config(2, 256, 2), 3: 0}),
    Case("MMU version 1 past its bounds", "mmu_inval_reserved.s", ["+mmu=1:64x2"],
         error=r"MMU version 1 cannot report a 64x2 main TLB"),
    Case("MMU sets not a power of two", "mmu_inval_reserved.s", ["+mmu=2:96x2"],
         error=r"MMU version 2 cannot report a 96x2 main TLB"),
    Case("unknown MMU version", "mmu_inval_reserved.s", ["+mmu=3"],
         error=r"MMU version 3 cannot report"),
    Case("bad +mmu", "mmu_inval_reserved.s", ["+mmu=2:256"],
         error=r"\+mmu=2:256: expected VER or VER:SETSxWAYS"),
    Case("audio device ready", "audio_status.s", [],
         regs={2: AUDIO_CAP, 3: 0, 4: 0, 5: 0,
               6: AUDIO_DEPTH, 7: AUDIO_DEPTH, 8: 7}),
    Case("+audio_status=busy:2", "audio_status.s", ["+audio_status=busy:2"],
         regs={3: ST_BUSY, 4: ST_BUSY, 5: 0, 6: AUDIO_HELD, 7: AUDIO_DEPTH}),
    Case("+audio_status=busy", "audio_status.s", ["+audio_status=busy"],
         regs={3: ST_BUSY, 4: ST_BUSY, 5: ST_BUSY, 6: AUDIO_HELD, 7: AUDIO_HELD}),
    Case("+audio_status=fault", "audio_status.s", ["+audio_status=fault"],
         regs={3: ST_FAULT, 4: ST_FAULT, 5: ST_FAULT, 6: AUDIO_HELD, 7: AUDIO_HELD}),
    Case("+audio_status=fault:0x5A", "audio_status.s", ["+audio_status=fault:0x5A"],
         regs={3: ST_FAULT | (0x5A << 8), 5: ST_FAULT | (0x5A << 8),
               6: AUDIO_HELD, 7: AUDIO_HELD}),
    Case("bad audio status", "audio_status.s", ["+audio_status=busy:0"],
         error=r"bad \+audio_status= 'busy:0'"),
    Case("audio fault code out of range", "audio_status.s", ["+audio_status=fault:256"],
         error=r"bad \+audio_status= 'fault:256'"),
    # Seven frames to the station, each looped back.  The first six find
    # nothing released, so the ring takes four and two are dropped;
    # releasing two slots lets the seventh in.
    Case("ethernet loopback", "eth_loop.s", ETH_ARGS,
         regs={2: 5, 3: ETH_CAP,
               4: ETH_RX_SLOTS | (ETH_SLOT_SIZE << 16),
               5: ETH_TX_SLOTS | (ETH_SLOT_SIZE << 16),
               6: ETH_LINK, 7: ETH_PADDED_TO_STATION,
               8: ETH_RX_SLOTS, 9: 6 - ETH_RX_SLOTS, 10: ETH_RX_SLOTS + 1, 11: 7}),
    # RX_PROD after each frame: it steps only for a frame the checks let
    # through.  In order: the station (in), another station (out),
    # broadcast (in), a group address (out), the group with ALLMULTI
    # (in), the other station with PROMISC (in), the station with RX_EN
    # clear (out), 1519 bytes (out).  None is dropped for want of a slot.
    Case("ethernet receive filter", "eth_filter.s", ETH_ARGS,
         regs={2: 1, 3: 1, 4: 2, 5: 2, 6: 3, 7: 4, 8: 4, 9: 4, 10: 0}),
    # IRQ_STATUS, RX while the receive ring holds a frame and TX while
    # the transmit ring holds fewer frames than TX_WM.  R2-R3: an empty
    # ring is fewer than 1 but not fewer than 0.  R4-R5: two queued is
    # not fewer than 2, and once both are sent and back both bits hold.
    # R6: released, threshold 0.  R7: two queued is fewer than 3.
    # R8-R9: the handler saw RX alone, and releasing ended it.
    # R10-R11: the same two comparisons with the counts wrapped.
    Case("ethernet interrupt sources", "eth_irq.s", ETH_ARGS,
         regs={2: 0, 3: ETH_IRQ_TX, 4: 0, 5: ETH_IRQ_RX | ETH_IRQ_TX, 6: 0,
               7: ETH_IRQ_TX, 8: ETH_IRQ_RX, 9: 0, 10: 0, 11: ETH_IRQ_TX}),
    Case("ethernet slot count not a power of two", "eth_loop.s", ["+eth=loop,rx=3"],
         error=r"bad \+eth= 'loop,rx=3'"),
    Case("ethernet transmit ring past its aperture", "eth_loop.s", ["+eth=loop,tx=32"],
         error=r"bad \+eth= 'loop,tx=32'"),
    # One frame each way through a TAP device.  The peer checks the
    # guest's frame byte for byte; the guest's registers show the
    # peer's 18-byte answer arriving padded to 60.
    Case("ethernet tap", "eth_tap.s", ETH_TAP_ARGS, peer="eth_tap_peer.py",
         regs={2: 1, 3: ETH_PADDED_TO_STATION, 4: ETH_PEER_TAIL_AND_TYPE,
               5: ETH_PEER_PAYLOAD, 6: 0}),
    # A PHY that software manages.  Nothing answers at address 0; the
    # PHY is at 1.  It first settles on 1000 Mbit/s, which this device
    # does not carry, so a frame sent then is lost, as is one sent
    # while it negotiates again without gigabit.  It then settles on
    # 100, and frames pass while CTRL.SPEED says 100 and not while it
    # says 10.  All four frames leave the transmit ring whether or not
    # they arrive.
    Case("ethernet managed PHY", "eth_mdio.s", ["+eth=loop,mdio,rx=4,tx=2"],
         regs={2: ETH_CAP_MANAGED, 3: 0,
               4: mdio_read(0, 2, 0xFFFF), 5: mdio_read(1, 2, ETH_PHY_ID1),
               6: ETH_MDIO_BUSY, 7: 0, 8: 1, 9: 1,
               10: mdio_read(1, 1, ETH_PHY_STATUS_UP), 11: 4}),
    # Eight frames from the peer at once.  They arrive at the link's
    # pace, not the host's, so the guest first sees one frame however
    # many are waiting; then the ring holds four and four are dropped.
    Case("ethernet tap burst", "eth_tap_burst.s", [ETH_TAP_ARGS[0] + ",speed=10"],
         peer="eth_tap_peer.py",
         regs={2: 1, 3: ETH_RX_SLOTS, 4: ETH_PEER_BURST - ETH_RX_SLOTS}),
    Case("cachemodel data", "cache_data.s",
         ["+cachemodel=penumbra1,penumbra2,penumbra2_5,1k/1:1k/1:64k/4,4k/2:4k/2:64k/4"],
         windows=[("reset to exit", {
             "penumbra1": DATA_PENUMBRA1, "penumbra2": DATA_PENUMBRA2,
             "penumbra2_5": DATA_PENUMBRA2, "1k/1:1k/1:64k/4": DATA_PENUMBRA1,
             "4k/2:4k/2:64k/4": DATA_PENUMBRA2})]),
    Case("cachemodel follows +core", "cache_data.s", ["+core=penumbra2_5", "+cachemodel"],
         windows=[("reset to exit", {"penumbra2_5": DATA_PENUMBRA2})]),
    Case("cachemodel ifetch", "cache_ifetch.s", ["+cachemodel=penumbra1,penumbra2"],
         windows=[("reset to exit", {"penumbra1": IFETCH_PENUMBRA1,
                                     "penumbra2": IFETCH_PENUMBRA2})]),
    Case("cachemodel next-line prediction", "cache_nextline.s",
         ["+cachemodel=penumbra1,penumbra2_5"],
         windows=[("reset to exit", {"penumbra1": NEXTLINE, "penumbra2_5": NEXTLINE})]),
    Case("model window", "cache_window.s",
         ["+cachemodel", "+model_start=0xFFFF0800", "+model_stop=0xFFFF0900"],
         windows=[("start PC to stop PC", {"penumbra1": WINDOW_PENUMBRA1})]),
    Case("tlbmodel layouts", "tlb_basic.s", ["+tlbmodel=" + ",".join(TLB_BASIC_LAYOUTS)],
         windows=[("reset to exit", {f"tlb {label}": TLB_BASIC
                                     for label in ["real"] + TLB_BASIC_LAYOUTS})]),
    Case("tlbmodel follows +core", "tlb_basic.s", ["+core=penumbra2", "+tlbmodel"],
         windows=[("reset to exit", {"tlb real": TLB_BASIC, "tlb penumbra2": TLB_BASIC})]),
    Case("tlbmodel associativity", "tlb_assoc.s",
         ["+tlbmodel=32x2/lru," + ",".join(TLB_ASSOC_LAYOUTS)],
         windows=[("reset to exit", {
             "tlb real": tlb_misses(supervisor=15), "tlb 32x2/lru": tlb_misses(supervisor=13),
             **{f"tlb {label}": tlb_misses(supervisor=10) for label in TLB_ASSOC_LAYOUTS}})]),
    Case("both models, one window", "cache_window.s",
         ["+cachemodel", "+tlbmodel", "+model_start=0xFFFF0800", "+model_stop=0xFFFF0900"],
         windows=[("start PC to stop PC", {"penumbra1": WINDOW_PENUMBRA1,
                                           "tlb real": tlb_misses(), "tlb penumbra1": tlb_misses()})]),
    Case("tlbmodel bad sets", "tlb_basic.s", ["+tlbmodel=48x2"],
         error=r"sets must be a power of two"),
    Case("tlbmodel unknown option", "tlb_basic.s", ["+tlbmodel=64x2/fifo"],
         error=r"unknown option '/fifo'"),
    Case("cachemodel bad ways", "cache_data.s", ["+cachemodel=4k/3:4k/2:64k/4"],
         error=r"ways must be a power of two"),
    Case("cachemodel unknown item", "cache_data.s", ["+cachemodel=penumbra7"],
         error=r"neither a generation nor L1I:L1D:L2"),
    Case("sbmodel default buffers", "sb_window.s", SB_WINDOW_ARGS + ["+sbmodel"],
         windows=[("start PC to stop PC", SB_WINDOW_DEFAULT)]),
    Case("sbmodel listed buffers", "sb_window.s",
         SB_WINDOW_ARGS + ["+sbmodel=" + ",".join(k[3:] for k in SB_WINDOW_LISTED)],
         windows=[("start PC to stop PC", SB_WINDOW_LISTED)]),
    Case("sbmodel cpi", "sb_window.s",
         SB_WINDOW_ARGS + ["+sbmodel=8l/all/bus", "+sbmodel_cpi=2"],
         windows=[("start PC to stop PC", SB_WINDOW_CPI2)]),
    Case("sbmodel posted write-through", "sb_posted.s",
         ["+cachemodel=4k/2:1k/1:64k/4", "+model_start=0xFFFF0800", "+model_stop=0xFFFF0900",
          "+sbmodel=" + ",".join(k[3:] for k in SB_POSTED)],
         windows=[("start PC to stop PC", SB_POSTED)]),
    Case("bpmodel predictors", "bp_basic.s", ["+bpmodel=" + ",".join(k[3:] for k in BP_BASIC)],
         windows=[("reset to exit", BP_BASIC)]),
    Case("bpmodel return stack overflow", "bp_ras.s",
         ["+bpmodel=" + ",".join(k[3:] for k in BP_RAS)],
         windows=[("reset to exit", BP_RAS)]),
    Case("bpmodel return stack in recursion", "bp_recurse.s",
         ["+bpmodel=" + ",".join(k[3:] for k in BP_RECURSE)],
         windows=[("reset to exit", BP_RECURSE)]),
    Case("bpmodel unknown component", "bp_basic.s", ["+bpmodel=btb32+xyz"],
         error=r"unknown component 'xyz'"),
    Case("bpmodel size not a power of two", "bp_basic.s", ["+bpmodel=btb30"],
         error=r"unknown component 'btb30'"),
    pair_case("pairmodel slots, units and memory", "pair_slots.s", PAIR_SLOTS),
    Case("pairmodel default stages", "pair_slots.s", ["+pairmodel"],
         windows=[("reset to exit", PAIR_DEFAULT)]),
    pair_case("pairmodel forwards", "pair_deps.s", PAIR_DEPS),
    pair_case("pairmodel fusions", "pair_fuse.s", PAIR_FUSE),
    pair_case("pairmodel traps", "pair_trap.s", PAIR_TRAP),
    pair_case("pairmodel op-and-branch fusion", "pair_opbr.s", PAIR_OPBR),
    pair_case("pairmodel address fusions", "pair_addr.s", PAIR_ADDR),
    pair_case("pairmodel extension fusion", "pair_ext.s", PAIR_EXT),
    pair_case("pairmodel write-back fusions and write ports", "pair_wb.s", PAIR_WB),
    pair_case("pairmodel read limit and pair order", "pair_limits.s", PAIR_LIMITS),
    pair_case("pairmodel fuser reach", "pair_reach.s", PAIR_REACH),
    pair_case("pairmodel fetch stage", "pair_fetch.s", PAIR_FETCH),
    pair_case("pairmodel fusion behind a fetch stage", "pair_reach.s", PAIR_FETCH_FUSE),
    pair_case("pairmodel trap behind a fetch stage", "pair_trap.s", PAIR_FETCH_TRAP),
    pair_case("pairmodel interrupt", "pair_irq.s", PAIR_IRQ, ["+irq_at_pc=0xFFFF0014"]),
] + [
    pair_case(f"pairmodel flags: {rule}", "pair_flags.s", {f"pairing {item}": PAIR_FLAGS[item]})
    for item, rule in PAIR_FLAGS_CASES.items()
] + [
    Case("pairmodel item without a base", "pair_slots.s", ["+pairmodel=fw:cond"],
         error=r"an item starts with single or dual"),
    Case("pairmodel unknown forward", "pair_slots.s", ["+pairmodel=dual+fw:cond:xyz"],
         error=r"unknown modifier 'fw:cond:xyz'"),
    Case("pairmodel unknown class", "pair_slots.s", ["+pairmodel=dual+x:alu:fpu"],
         error=r"unknown modifier 'x:alu:fpu'"),
    Case("loadusemodel waits", "lu_waits.s",
         ["+loadusemodel=" + ",".join(k[len("load-use "):] for k in LU_WAITS)],
         windows=[("reset to exit", LU_WAITS)]),
    Case("opstats window and operands", "ops_muldiv.s", OPS_MULDIV_ARGS,
         windows=[("start PC to stop PC", {"opstats": OPS_MULDIV["penumbra1"]})]),
    Case("opstats multiply steps follow +core", "ops_muldiv.s",
         ["+core=penumbra2_5"] + OPS_MULDIV_ARGS,
         windows=[("start PC to stop PC", {"opstats": OPS_MULDIV["penumbra2_5"]})]),
    Case("loadusemodel unknown base", "lu_waits.s", ["+loadusemodel=mem"],
         error=r"an item starts with ex or late"),
    Case("loadusemodel second late", "lu_waits.s", ["+loadusemodel=late+late"],
         error=r"unknown modifier 'late'"),
    Case("loadusemodel unknown wait", "lu_waits.s", ["+loadusemodel=ex+wait:x"],
         error=r"unknown modifier 'wait:x'"),
    Case("pairmodel unknown reach", "pair_reach.s", ["+pairmodel=dual+reach:far"],
         error=r"unknown modifier 'reach:far'"),
    Case("pairmodel unknown agu", "pair_addr.s", ["+pairmodel=dual+agu:4"],
         error=r"unknown modifier 'agu:4'"),
    Case("pairmodel unknown reads", "pair_limits.s", ["+pairmodel=dual+reads:4"],
         error=r"unknown modifier 'reads:4'"),
    Case("pairmodel unknown last class", "pair_limits.s", ["+pairmodel=dual+last:br:fpu"],
         error=r"unknown modifier 'last:br:fpu'"),
    Case("pairmodel fetch of nothing", "pair_fetch.s", ["+pairmodel=dual+fetch:0"],
         error=r"unknown modifier 'fetch:0'"),
    Case("pairmodel queue with no fetch stage", "pair_fetch.s", ["+pairmodel=dual+queue:16"],
         error=r"bubble: and queue: shape a fetch stage"),
    Case("pairmodel queue smaller than a fetch", "pair_fetch.s",
         ["+pairmodel=dual+fetch:4+queue:2"],
         error=r"the queue holds less than one fetch delivers"),
    Case("sbmodel needs cachemodel", "sb_window.s", ["+sbmodel"],
         error=r"\+sbmodel needs \+cachemodel"),
    Case("sbmodel unknown policy", "sb_window.s", ["+cachemodel", "+sbmodel=8l/some/bus"],
         error=r"unknown policy 'some'"),
    Case("sbmodel bad drain", "sb_window.s", ["+cachemodel", "+sbmodel=8l/all/5:7:9"],
         error=r"neither a preset"),
    Case("sbmodel unknown option", "sb_window.s", ["+cachemodel", "+sbmodel=8l/all/bus/fifo"],
         error=r"unknown option '/fifo'"),
]


def final_registers(stderr):
    """The registers from the last 'SR=... R1=...' line the ISS prints."""
    lines = [l for l in stderr.splitlines() if l.startswith("SR=")]
    if not lines:
        return None
    return {int(n): int(v, 16) for n, v in re.findall(r"R(\d+)=([0-9a-fA-F]+)", lines[-1])}


def named_counts(text):
    """A report line's "name N, name N" list, without the zero counts."""
    return {name: int(n) for name, n in re.findall(r"([a-z-]+) (\d+)", text) if int(n)}


def parse_report(text):
    """The models' windows: [(span, {label: counts})]."""
    windows, counts = [], None
    for line in text.splitlines():
        m = re.match(r"== model window \d+: \d+ instructions, (.+) ==$", line)
        if m:
            windows.append((m.group(1), {}))
            continue
        m = re.match(r"-- cache (\S+): ", line)
        if m:
            counts = windows[-1][1].setdefault(m.group(1), {"fills": {}})
            continue
        if line.startswith("-- tlb: "):
            counts = None
            continue
        m = re.match(r"-- branch predictor (\S+)$", line)
        if m:
            counts = windows[-1][1].setdefault(f"bp {m.group(1)}", {"bp": {}})
            continue
        m = re.match(r"\s+transfers \(taken\): conditional (\d+) \((\d+)\), direct (\d+), calls (\d+),"
                     r" indirect calls (\d+), returns (\d+), indirect (\d+)", line)
        if m:
            counts["bp"]["transfers"] = tuple(int(v) for v in m.groups())
            continue
        m = re.match(r"\s+redirects: fetch (\d+), decode (\d+), execute (\d+) \(conditional (\d+),"
                     r" direct (\d+), calls (\d+), indirect calls (\d+), returns (\d+), indirect (\d+)\)",
                     line)
        if m:
            counts["bp"]["redirects"] = tuple(int(v) for v in m.groups())
            continue
        m = re.match(r"-- pairing (\S+)$", line)
        if m:
            counts = windows[-1][1].setdefault(f"pairing {m.group(1)}", {"pair": {}})
            continue
        m = re.match(r"\s+instructions (\d+), ops (\d+), issue cycles (\d+): (\d+) pairs,"
                     r" (\d+) singles, (\d+) idle", line)
        if m:
            counts["pair"]["issued"] = tuple(int(v) for v in m.groups())
            continue
        m = re.match(r"\s+singles, several causes: (\d+) \((.*)\)", line)
        if m:
            counts["pair"]["singles, several causes"] = (int(m.group(1)), named_counts(m.group(2)))
            continue
        m = re.match(r"\s+(cycles by instructions issued|fetch|fused|pairs by forward"
                     r"|pairs by younger|singles by class|singles, no candidate"
                     r"|singles, one cause): (.*)", line)
        if m:
            counts["pair"][m.group(1)] = named_counts(m.group(2))
            continue
        m = re.match(r"-- load-use (\S+)$", line)
        if m:
            counts = windows[-1][1].setdefault(f"load-use {m.group(1)}", {"lu": {}})
            continue
        m = re.match(r"\s+instructions (\d+), waited (\d+): (.*)", line)
        if m:
            counts["lu"]["instructions"] = (int(m.group(1)), int(m.group(2)))
            counts["lu"]["waited"] = named_counts(m.group(3))
            continue
        m = re.match(r"\s+alu ops (\d+), late (\d+)$", line)
        if m:
            counts["lu"]["alu ops"] = tuple(int(v) for v in m.groups())
            continue
        m = re.match(r"\s+conditional branches (\d+): (.*)", line)
        if m:
            counts["lu"]["conditional branches"] = (int(m.group(1)), named_counts(m.group(2)))
            continue
        if line == "-- instruction classes":
            counts = windows[-1][1].setdefault("opstats", {"ops": {"classes": {}}})
            continue
        if line == "-- MUL and DIV operands":
            continue
        if counts is not None and "ops" in counts:
            m = re.match(r"  (\S(?:.*?\S)?) {2,}(\d+)(?: |$)", line)
            if m:
                if int(m.group(2)):
                    counts["ops"]["classes"][m.group(1)] = int(m.group(2))
                continue
            m = re.match(r"\s+multiplies (\d+), of (\d+) steps each; needed walking Rd (\d+) \(\S+\),"
                         r" Rs (\d+) \(\S+\), the shorter (\d+) \(", line)
            if m:
                counts["ops"]["multiplies"] = tuple(int(v) for v in m.groups())
                continue
            m = re.match(r"\s+divides (\d+), of (\d+) steps each; needed past the dividend's leading"
                         r" zeros (\d+) \(\S+\), aligned to the divisor (\d+) \(", line)
            if m:
                counts["ops"]["divides"] = tuple(int(v) for v in m.groups())
                continue
            m = re.match(r"\s+(divides by steps needed aligned): (.*)", line)
            if m:
                counts["ops"][m.group(1)] = {name: int(n) for name, n
                                             in re.findall(r"(none|\d+-\d+) (\d+)", m.group(2))}
                continue
        m = re.match(r"-- store buffer (\S+): ", line)
        if m:
            counts = windows[-1][1].setdefault(f"sb {m.group(1)}", {"sb": {}})
            continue
        m = re.match(r"\s+stores: (\d+) buffered, (\d+) merged, (\d+) entries; (\d+) uncached", line)
        if m:
            counts["sb"]["stores"] = tuple(int(v) for v in m.groups())
            continue
        m = re.match(r"\s+entries drained by words: (\d+) (\d+) (\d+) (\d+)", line)
        if m:
            counts["sb"]["drained"] = tuple(int(v) for v in m.groups())
            continue
        m = re.match(r"\s+waits, cycles: buffer full (\d+), I fills (\d+), D fills (\d+),"
                     r" uncached (\d+)", line)
        if m:
            counts["sb"]["waits"] = tuple(int(v) for v in m.groups())
            continue
        m = re.match(r"\s+accesses that waited: buffer full (\d+), I fills (\d+) of (\d+),"
                     r" D fills (\d+) of (\d+), uncached (\d+) of (\d+)", line)
        if m:
            counts["sb"]["waited"] = tuple(int(v) for v in m.groups())
            continue
        m = re.match(r"\s+(\S+)\s+\d+x\d+/\S+\s+\d+\s+(\d+)\s+(\d+)\s", line)
        if m and counts is None:
            windows[-1][1][f"tlb {m.group(1)}"] = {"tlb": (int(m.group(2)), int(m.group(3)))}
            continue
        m = re.match(r"\s+(L1I|L1D|L2)\s+(\d+)\s+(\d+)\s+(\d+|-)\s+(\d+|-)", line)
        if m:
            counts[m.group(1)] = tuple(0 if v == "-" else int(v) for v in m.groups()[1:])
            continue
        m = re.match(r"\s+([ID]) (user|super)\s+(\d+)\s+(\d+)", line)
        if m:
            counts["fills"][(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)))
            continue
        m = re.match(r"\s+next-line\s+buffer serves (\d+) \(once\) and (\d+) \(continue\)"
                     r".* looks up (\d+), (\d+) missing L2, (\d+) unused", line)
        if m:
            counts["prediction"] = tuple(int(v) for v in m.groups())
    return windows


def compare_windows(expected, got):
    problems = []
    if [span for span, _ in got] != [span for span, _ in expected]:
        return [f"windows {[s for s, _ in got]}, expected {[s for s, _ in expected]}"]
    for (span, want_cfgs), (_, got_cfgs) in zip(expected, got):
        for label, want in want_cfgs.items():
            have = got_cfgs.get(label)
            if have is None:
                problems.append(f"{span}: no report for {label}")
                continue
            if "tlb" in want:
                if have.get("tlb") != want["tlb"]:
                    problems.append(f"{label} misses (user, supervisor) {have.get('tlb')}, "
                                    f"expected {want['tlb']}")
                continue
            if "bp" in want:
                for key, value in want["bp"].items():
                    if have["bp"].get(key) != value:
                        problems.append(f"{label} {key} {have['bp'].get(key)}, expected {value}")
                continue
            if "pair" in want:
                for key, value in want["pair"].items():
                    if have["pair"].get(key) != value:
                        problems.append(f"{label} {key} {have['pair'].get(key)}, expected {value}")
                continue
            if "lu" in want:
                for key, value in want["lu"].items():
                    if have["lu"].get(key) != value:
                        problems.append(f"{label} {key} {have['lu'].get(key)}, expected {value}")
                continue
            if "ops" in want:
                for key, value in want["ops"].items():
                    if have["ops"].get(key) != value:
                        problems.append(f"{label} {key} {have['ops'].get(key)}, expected {value}")
                continue
            if "sb" in want:
                for key, value in want["sb"].items():
                    if have["sb"].get(key) != value:
                        problems.append(f"{label} {key} {have['sb'].get(key)}, expected {value}")
                continue
            for level in ("L1I", "L1D", "L2"):
                if have.get(level) != want[level]:
                    problems.append(f"{label} {level} {have.get(level)}, expected {want[level]}")
            for key in set(want["fills"]) | set(have["fills"]):
                w, h = want["fills"].get(key, (0, 0)), have["fills"].get(key, (0, 0))
                if w != h:
                    problems.append(f"{label} fills {key[0]} {key[1]} {h}, expected {w}")
            if "prediction" in want and have.get("prediction") != want["prediction"]:
                problems.append(f"{label} next-line {have.get('prediction')}, "
                                f"expected {want['prediction']}")
    return problems


def run_case(case, iss, asm, outdir):
    src = HERE / case.program
    hexfile = outdir / (src.stem + ".hex")
    subprocess.run(asm + [str(src), "-o", str(hexfile)], check=True,
                   stdout=subprocess.DEVNULL)
    report = outdir / (re.sub(r"\W+", "_", case.name) + ".report")
    plusargs = case.plusargs + ([f"+model_report={report}"] if case.windows else [])
    command = [iss, str(hexfile), "+halt-on-break"] + plusargs
    if case.peer:
        if not can_make_namespace():
            return None
        command = NAMESPACE + [sys.executable, str(HERE / case.peer)] + command
    try:
        proc = subprocess.run(command,
                              stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return [f"no BREAK within {RUN_TIMEOUT} s"]
    if case.error:
        if proc.returncode != 0 and re.search(case.error, proc.stderr):
            return []
        return [f"expected refusal matching /{case.error}/, "
                f"got exit {proc.returncode}"]
    if case.windows is not None:
        if proc.returncode != 0 or not report.exists():
            return [f"no model report (exit {proc.returncode}): {proc.stderr.strip()[-200:]}"]
        return compare_windows(case.windows, parse_report(report.read_text()))
    regs = final_registers(proc.stderr)
    if regs is None:
        return [f"no register dump (exit {proc.returncode})"]
    return [f"R{r} = 0x{regs.get(r, 0):08X}, expected 0x{want:08X}"
            for r, want in sorted(case.regs.items()) if regs.get(r) != want]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--iss", required=True)
    ap.add_argument("--asm", required=True)
    ap.add_argument("--outdir", required=True, type=Path)
    args = ap.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)

    failed = skipped = 0
    for case in CASES:
        problems = run_case(case, args.iss, shlex.split(args.asm), args.outdir)
        if problems is None:
            print(f"  \033[33mSKIP\033[0m  {case.name} (no network namespace to be had)")
            skipped += 1
            continue
        verdict = "\033[31mFAIL\033[0m" if problems else "\033[32mPASS\033[0m"
        print(f"  {verdict}  {case.name}")
        for p in problems:
            print(f"          {p}")
        failed += bool(problems)
    ran = len(CASES) - skipped
    print(f"{ran - failed}/{ran} ISS option tests passed" +
          (f", {skipped} skipped" if skipped else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
