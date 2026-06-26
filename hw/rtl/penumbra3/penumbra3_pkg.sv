// penumbra3_pkg -- Penumbra/3 (gen3) CPU-internal constants.
//
// Holds constants private to the gen3 core: op-class / control-bundle
// encodings, scoreboard indices, pipeline bubble causes, and the like.
// These describe the gen3 microarchitecture and are NOT part of the ISA --
// the ISA / system contract lives in penumbra_pkg (hw/rtl/common), which
// every gen3 module imports alongside this package.
//
// gen3 is a full generation fork (its own core and its own on-chip memory
// hierarchy); see doc/internals/penumbra3/overview.md. This package grows
// as each gen3 module is added -- a constant lands here when a module needs
// it, never speculatively ahead of the design that uses it.
package penumbra3_pkg;

  // Microarchitecture constants (op-class, scoreboard indices, bubble
  // causes, ...) are added here as the Phase-0 probes and the Phase-1
  // skeleton introduce the modules that consume them. Intentionally empty
  // until then -- a placeholder constant would lint as unused under -Wall.

endpackage
