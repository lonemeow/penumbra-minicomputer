// REQUIRES: penumbra-registered-target

// Every Penumbra generation runs the same binaries, so both flags select
// tuning only: -mcpu names the default tuning target, -mtune overrides it.

// RUN: %clang -### --target=penumbra-unknown-none -mcpu=penumbra2 -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CPU
// CPU: "-target-cpu" "penumbra2"

// RUN: %clang -### --target=penumbra-unknown-none -mtune=penumbra1 -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=TUNE
// TUNE: "-tune-cpu" "penumbra1"

// RUN: %clang -### --target=penumbra-unknown-none -mtune=penumbra2_5 -c %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=TUNE25
// TUNE25: "-tune-cpu" "penumbra2_5"

// RUN: %clang -### --target=penumbra-unknown-none -mcpu=penumbra1 \
// RUN:   -mtune=penumbra2 -c %s 2>&1 | FileCheck %s --check-prefix=BOTH
// BOTH-DAG: "-target-cpu" "penumbra1"
// BOTH-DAG: "-tune-cpu" "penumbra2"

// An unknown name is a diagnostic rather than a silently ignored argument,
// for either flag.
// RUN: not %clang --target=penumbra-unknown-none -mcpu=penumbra9 -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BAD-CPU
// BAD-CPU: error: unknown target CPU 'penumbra9'
// BAD-CPU: note: valid target CPU values are: generic, penumbra1, penumbra2, penumbra2_5

// RUN: not %clang --target=penumbra-unknown-none -mtune=penumbra9 -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BAD-TUNE
// BAD-TUNE: error: unknown target CPU 'penumbra9'
