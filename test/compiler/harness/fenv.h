// test/compiler/harness/fenv.h
// Minimal fenv.h: rounding-mode selection is a no-op because the
// harness is soft-float with single (FE_TONEAREST) behavior.
// Any request for a non-default mode abort()s to avoid silent
// semantic divergence from what the test expects.
#ifndef FENV_H
#define FENV_H

#define FE_TONEAREST   0
#define FE_DOWNWARD    1
#define FE_UPWARD      2
#define FE_TOWARDZERO  3

void abort(void);

static inline int fesetround(int mode) {
    if (mode != FE_TONEAREST) abort();
    return 0;
}
static inline int fegetround(void) { return FE_TONEAREST; }

#endif
