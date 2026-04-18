# Penumbra Autoconfig — Hardware Implementation

## The Config Chain

A single `cfg` signal is **daisy-chained** through all autoconfigurable devices:

```
                 cfg_in    cfg_out    cfg_in    cfg_out    cfg_in
Bus controller ───────> Device 0 ───────> Device 1 ───────> Device 2 ──> ...
```

- After `rst` pulse, all devices return to config state and **block** `cfg_out`.
- Only the **first unconfigured device** (the one with `cfg_in=1`) responds to config cycles at address `0xFE00_0000`.
- Once `CFG_BASE` is written, the device latches its address and enters enabled state.
- The device **passes** `cfg_out = cfg_in` only after the `CFG_EN` bus signal has been toggled (deasserted then reasserted). This prevents race conditions during the write cycle.

## Discrete 74xx Implementation

- Two flip-flops per device: `configured` and `cfg_seen_low` (both async-cleared by `rst`).
- `cfg_seen_low` is set when `CFG_EN` goes low while `configured` is high.
- `cfg_out = cfg_in & configured & cfg_seen_low`.
- Address decode: one 74HC85 comparator gated by `cfg_in` for the `0xFE00_0000` range.
