// test/compiler/harness/strings.h
#ifndef STRINGS_H
#define STRINGS_H

static inline int ffs(int v)          { return __builtin_ffs(v); }
static inline int ffsl(long v)        { return __builtin_ffsl(v); }
static inline int ffsll(long long v)  { return __builtin_ffsll(v); }

#endif
