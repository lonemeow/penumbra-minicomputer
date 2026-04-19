// test/compiler/harness/alloca.h
#ifndef ALLOCA_H
#define ALLOCA_H

// Function-like macro so textual replacements of `alloca(x)` become
// `__builtin_alloca(x)` — including inside the test's own `extern`
// declaration, which then harmlessly becomes an extern declaration of
// the builtin.  Must be inlined at the call site: a real function call
// can't extend the caller's frame.  Lowers to G_DYN_STACKALLOC.
#define alloca(size) __builtin_alloca(size)

#endif
