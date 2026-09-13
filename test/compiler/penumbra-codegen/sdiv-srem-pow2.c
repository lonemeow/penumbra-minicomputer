/*
 * Signed division and remainder by a constant power of two lower to
 * shifts, adds and subtracts rather than a divide.  Every such divisor --
 * 2^0 through 2^31 (INT_MIN) at 32 bits, 2^0 through 2^63 at 64 bits --
 * is checked on its own and as a quotient + remainder pair, against the
 * same operation on a divisor known only at run time, over inputs that
 * straddle each power of two and both ends of the range.
 */

extern void abort(void);

#define POW32(k) ((int)(1u << (k)))
#define POW64(k) ((long long)(1ull << (k)))

#define DEF32(k)                                                         \
  __attribute__((noinline)) static int rem32_##k(int x) {                \
    return x % POW32(k);                                                 \
  }                                                                      \
  __attribute__((noinline)) static int pair32_##k(int x, int *q) {       \
    *q = x / POW32(k);                                                   \
    return x % POW32(k);                                                 \
  }

#define DEF64(k)                                                         \
  __attribute__((noinline)) static long long rem64_##k(long long x) {    \
    return x % POW64(k);                                                 \
  }                                                                      \
  __attribute__((noinline)) static long long pair64_##k(long long x,     \
                                                        long long *q) {  \
    *q = x / POW64(k);                                                   \
    return x % POW64(k);                                                 \
  }

DEF32(0) DEF32(1) DEF32(2) DEF32(3) DEF32(4) DEF32(5) DEF32(6) DEF32(7)
DEF32(8) DEF32(9) DEF32(10) DEF32(11) DEF32(12) DEF32(13) DEF32(14)
DEF32(15) DEF32(16) DEF32(17) DEF32(18) DEF32(19) DEF32(20) DEF32(21)
DEF32(22) DEF32(23) DEF32(24) DEF32(25) DEF32(26) DEF32(27) DEF32(28)
DEF32(29) DEF32(30) DEF32(31)

DEF64(0) DEF64(1) DEF64(2) DEF64(3) DEF64(4) DEF64(5) DEF64(6) DEF64(7)
DEF64(8) DEF64(9) DEF64(10) DEF64(11) DEF64(12) DEF64(13) DEF64(14)
DEF64(15) DEF64(16) DEF64(17) DEF64(18) DEF64(19) DEF64(20) DEF64(21)
DEF64(22) DEF64(23) DEF64(24) DEF64(25) DEF64(26) DEF64(27) DEF64(28)
DEF64(29) DEF64(30) DEF64(31) DEF64(32) DEF64(33) DEF64(34) DEF64(35)
DEF64(36) DEF64(37) DEF64(38) DEF64(39) DEF64(40) DEF64(41) DEF64(42)
DEF64(43) DEF64(44) DEF64(45) DEF64(46) DEF64(47) DEF64(48) DEF64(49)
DEF64(50) DEF64(51) DEF64(52) DEF64(53) DEF64(54) DEF64(55) DEF64(56)
DEF64(57) DEF64(58) DEF64(59) DEF64(60) DEF64(61) DEF64(62) DEF64(63)

#define ENTRY32(k) { rem32_##k, pair32_##k }
#define ENTRY64(k) { rem64_##k, pair64_##k }

static const struct {
  int (*rem)(int);
  int (*pair)(int, int *);
} ops32[32] = {
  ENTRY32(0), ENTRY32(1), ENTRY32(2), ENTRY32(3), ENTRY32(4), ENTRY32(5),
  ENTRY32(6), ENTRY32(7), ENTRY32(8), ENTRY32(9), ENTRY32(10), ENTRY32(11),
  ENTRY32(12), ENTRY32(13), ENTRY32(14), ENTRY32(15), ENTRY32(16),
  ENTRY32(17), ENTRY32(18), ENTRY32(19), ENTRY32(20), ENTRY32(21),
  ENTRY32(22), ENTRY32(23), ENTRY32(24), ENTRY32(25), ENTRY32(26),
  ENTRY32(27), ENTRY32(28), ENTRY32(29), ENTRY32(30), ENTRY32(31)
};

static const struct {
  long long (*rem)(long long);
  long long (*pair)(long long, long long *);
} ops64[64] = {
  ENTRY64(0), ENTRY64(1), ENTRY64(2), ENTRY64(3), ENTRY64(4), ENTRY64(5),
  ENTRY64(6), ENTRY64(7), ENTRY64(8), ENTRY64(9), ENTRY64(10), ENTRY64(11),
  ENTRY64(12), ENTRY64(13), ENTRY64(14), ENTRY64(15), ENTRY64(16),
  ENTRY64(17), ENTRY64(18), ENTRY64(19), ENTRY64(20), ENTRY64(21),
  ENTRY64(22), ENTRY64(23), ENTRY64(24), ENTRY64(25), ENTRY64(26),
  ENTRY64(27), ENTRY64(28), ENTRY64(29), ENTRY64(30), ENTRY64(31),
  ENTRY64(32), ENTRY64(33), ENTRY64(34), ENTRY64(35), ENTRY64(36),
  ENTRY64(37), ENTRY64(38), ENTRY64(39), ENTRY64(40), ENTRY64(41),
  ENTRY64(42), ENTRY64(43), ENTRY64(44), ENTRY64(45), ENTRY64(46),
  ENTRY64(47), ENTRY64(48), ENTRY64(49), ENTRY64(50), ENTRY64(51),
  ENTRY64(52), ENTRY64(53), ENTRY64(54), ENTRY64(55), ENTRY64(56),
  ENTRY64(57), ENTRY64(58), ENTRY64(59), ENTRY64(60), ENTRY64(61),
  ENTRY64(62), ENTRY64(63)
};

/* The reference divides by a value the compiler cannot see. */
volatile int divisor32;
volatile long long divisor64;

/* Each power of two j contributes 2^j - 1, 2^j, 2^j + 1 and their
 * negations (computed with wraparound), plus both ends of the range. */
#define MAX_INPUTS 400
static int in32[MAX_INPUTS];
static long long in64[MAX_INPUTS];

static int fill32(void) {
  int n = 0, j, s;
  in32[n++] = (int)0x80000000u;
  in32[n++] = (int)0x80000001u;
  in32[n++] = 0x7ffffffe;
  in32[n++] = 0x7fffffff;
  for (j = 0; j < 32; j++)
    for (s = -1; s <= 1; s++) {
      unsigned v = (1u << j) + (unsigned)s;
      in32[n++] = (int)v;
      in32[n++] = (int)(0u - v);
    }
  return n;
}

static int fill64(void) {
  int n = 0, j, s;
  in64[n++] = (long long)0x8000000000000000ull;
  in64[n++] = (long long)0x8000000000000001ull;
  in64[n++] = 0x7ffffffffffffffell;
  in64[n++] = 0x7fffffffffffffffll;
  for (j = 0; j < 64; j++)
    for (s = -1; s <= 1; s++) {
      unsigned long long v = (1ull << j) + (unsigned long long)(long long)s;
      in64[n++] = (long long)v;
      in64[n++] = (long long)(0ull - v);
    }
  return n;
}

int main(void) {
  int n32 = fill32(), n64 = fill64();
  int k, i;

  for (k = 0; k < 32; k++) {
    divisor32 = POW32(k);
    for (i = 0; i < n32; i++) {
      int x = in32[i], q;
      int ref_q = x / divisor32;
      int ref_r = x % divisor32;
      if (ops32[k].rem(x) != ref_r)
        abort();
      if (ops32[k].pair(x, &q) != ref_r || q != ref_q)
        abort();
    }
  }

  for (k = 0; k < 64; k++) {
    divisor64 = POW64(k);
    for (i = 0; i < n64; i++) {
      long long x = in64[i], q;
      long long ref_q = x / divisor64;
      long long ref_r = x % divisor64;
      if (ops64[k].rem(x) != ref_r)
        abort();
      if (ops64[k].pair(x, &q) != ref_r || q != ref_q)
        abort();
    }
  }

  return 0;
}
