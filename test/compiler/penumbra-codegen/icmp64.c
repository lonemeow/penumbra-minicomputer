/*
 * 64-bit compares: CMP on the low words and CMPC on the high words, or a
 * compare of the high words alone when a constant's low word allows.  Every
 * predicate is checked as a branch, a value and a select, against variable
 * operands and against constants chosen for each path: a zero low word, a low
 * word that fits an immediate or does not, a zero or all-ones high word, and
 * the extremes where stepping `>` to `>=` would wrap.  The reference compares
 * 32-bit words, which no 64-bit lowering touches, over edge values and a
 * fixed pseudo-random sequence.
 */

extern void abort(void);

typedef unsigned long long u64;
typedef long long i64;

#define NOINLINE __attribute__((noinline))
#define U64(hi, lo) ((u64)(hi) << 32 | (lo))

enum { P_slt, P_sle, P_sgt, P_sge, P_ult, P_ule, P_ugt, P_uge, P_eq, P_ne };

/* A volatile store cannot be speculated, so the branch forms keep a branch. */
static volatile int sink;

#define PREDS(X, ...)                                                        \
  X(slt, <, i64, __VA_ARGS__) X(sle, <=, i64, __VA_ARGS__)                   \
  X(sgt, >, i64, __VA_ARGS__) X(sge, >=, i64, __VA_ARGS__)                   \
  X(ult, <, u64, __VA_ARGS__) X(ule, <=, u64, __VA_ARGS__)                   \
  X(ugt, >, u64, __VA_ARGS__) X(uge, >=, u64, __VA_ARGS__)                   \
  X(eq, ==, u64, __VA_ARGS__) X(ne, !=, u64, __VA_ARGS__)

/* Variable operands: branch, value and select. */
#define VAR_FNS(p, op, T, unused)                                           \
  NOINLINE static int br_##p(u64 a, u64 b) {                                \
    sink = 0;                                                               \
    if ((T)a op (T)b)                                                       \
      sink = 1;                                                             \
    return sink;                                                            \
  }                                                                         \
  NOINLINE static int val_##p(u64 a, u64 b) { return (T)a op (T)b; }        \
  NOINLINE static u64 sel_##p(u64 a, u64 b) { return (T)a op (T)b ? a : b; }
PREDS(VAR_FNS, 0)

/* Constant right-hand sides, by id. */
#define CONSTANTS(X)                                                         \
  X(0, 0x0ull) X(1, 0x1ull) X(2, 0x3e8ull) X(3, 0x12345678ull)               \
  X(4, 0xffffffffull) X(5, 0x100000000ull) X(6, 0x1000003e8ull)              \
  X(7, 0xffffffffffffffffull) X(8, 0xfffffffffffffc18ull)                    \
  X(9, 0xffffffff00000000ull) X(10, 0x7fffffffffffffffull)                   \
  X(11, 0x8000000000000000ull) X(12, 0x7fffffff00000000ull)

#define CONST_FNS(p, op, T, id, k)                                          \
  NOINLINE static int brk_##p##_##id(u64 a) {                               \
    sink = 0;                                                               \
    if ((T)a op (T)(k))                                                     \
      sink = 1;                                                             \
    return sink;                                                            \
  }                                                                         \
  NOINLINE static int valk_##p##_##id(u64 a) { return (T)a op (T)(k); }
#define CONST_ROW_FNS(id, k) PREDS(CONST_FNS, id, k)
CONSTANTS(CONST_ROW_FNS)

struct var_case {
  int pred;
  int (*br)(u64, u64);
  int (*val)(u64, u64);
  u64 (*sel)(u64, u64);
};
#define VAR_ENTRY(p, op, T, unused) {P_##p, br_##p, val_##p, sel_##p},
static const struct var_case var_cases[] = {PREDS(VAR_ENTRY, 0)};

struct const_case {
  int pred;
  u64 k;
  int (*br)(u64);
  int (*val)(u64);
};
#define CONST_ENTRY(p, op, T, id, k)                                        \
  {P_##p, k, brk_##p##_##id, valk_##p##_##id},
#define CONST_ROW(id, k) PREDS(CONST_ENTRY, id, k)
static const struct const_case const_cases[] = {CONSTANTS(CONST_ROW)};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* pred(a, b), from 32-bit compares of the words. */
static int ref(int pred, unsigned ahi, unsigned alo, unsigned bhi,
               unsigned blo) {
  int ult = ahi < bhi || (ahi == bhi && alo < blo);
  int ugt = ahi > bhi || (ahi == bhi && alo > blo);
  int slt = (int)ahi < (int)bhi || (ahi == bhi && alo < blo);
  int sgt = (int)ahi > (int)bhi || (ahi == bhi && alo > blo);
  int eq = ahi == bhi && alo == blo;

  switch (pred) {
  case P_slt: return slt;
  case P_sle: return !sgt;
  case P_sgt: return sgt;
  case P_sge: return !slt;
  case P_ult: return ult;
  case P_ule: return !ugt;
  case P_ugt: return ugt;
  case P_uge: return !ult;
  case P_eq:  return eq;
  default:    return !eq;
  }
}

static void check_var(unsigned ahi, unsigned alo, unsigned bhi, unsigned blo) {
  u64 a = U64(ahi, alo), b = U64(bhi, blo), r;
  unsigned i;
  int want;

  for (i = 0; i < COUNT(var_cases); i++) {
    want = ref(var_cases[i].pred, ahi, alo, bhi, blo);
    if (var_cases[i].br(a, b) != want || var_cases[i].val(a, b) != want)
      abort();
    r = var_cases[i].sel(a, b);
    if ((unsigned)(r >> 32) != (want ? ahi : bhi) ||
        (unsigned)r != (want ? alo : blo))
      abort();
  }
}

static void check_const(unsigned ahi, unsigned alo) {
  u64 a = U64(ahi, alo);
  unsigned i, khi, klo;
  int want;

  for (i = 0; i < COUNT(const_cases); i++) {
    khi = (unsigned)(const_cases[i].k >> 32);
    klo = (unsigned)const_cases[i].k;
    want = ref(const_cases[i].pred, ahi, alo, khi, klo);
    if (const_cases[i].br(a) != want || const_cases[i].val(a) != want)
      abort();
  }
}

#define NUM_RANDOM 256

int main(void) {
  static const unsigned edges[] = {
    0u, 1u, 0x3e8u, 0xffffu, 0x12345678u, 0x7fffffffu, 0x80000000u,
    0xfffffc18u, 0xffffffffu
  };
  const unsigned n = COUNT(edges);
  unsigned i, j, seed = 0x12345678u, v[4];

  for (i = 0; i < n * n; i++) {
    check_const(edges[i / n], edges[i % n]);
    for (j = 0; j < n * n; j++)
      check_var(edges[i / n], edges[i % n], edges[j / n], edges[j % n]);
  }

  /* Each constant's neighbours, where a stepped constant would go wrong. */
  for (i = 0; i < COUNT(const_cases); i++) {
    u64 k = const_cases[i].k;
    check_const((unsigned)((k - 1) >> 32), (unsigned)(k - 1));
    check_const((unsigned)(k >> 32), (unsigned)k);
    check_const((unsigned)((k + 1) >> 32), (unsigned)(k + 1));
  }

  for (i = 0; i < NUM_RANDOM; i++) {
    for (j = 0; j < 4; j++) {
      seed = seed * 1664525u + 1013904223u;
      v[j] = seed;
    }
    check_var(v[0], v[1], v[2], v[3]);
    check_var(v[0], v[1], v[0], v[3]);
    check_const(v[0], v[1]);
  }

  return 0;
}
