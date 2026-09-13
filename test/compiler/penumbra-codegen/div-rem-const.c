/*
 * Division and remainder by a constant that is not a power of two.  A core
 * whose multiply is sufficiently cheaper than its divide lowers these to a
 * high-half multiply plus shift and add fixups (the default tuning), another
 * keeps the divide; either way the result must match the divide.  Each
 * divisor is checked as a quotient, a remainder and a quotient + remainder
 * pair, against the same operation on a divisor known only at run time, over
 * inputs at both ends of the range, around multiples of the divisor, and from
 * a fixed pseudo-random sequence.
 */

extern void abort(void);

#define UNSIGNED_DIVISORS(X)                                                  \
  X(3, 3u) X(5, 5u) X(6, 6u) X(7, 7u) X(9, 9u) X(10, 10u) X(11, 11u)          \
  X(12, 12u) X(13, 13u) X(25, 25u) X(60, 60u) X(100, 100u) X(641, 641u)       \
  X(1000, 1000u) X(86400, 86400u) X(big_prime, 1000000007u)                   \
  X(int_max, 0x7fffffffu) X(just_over_half, 0x80000001u)                      \
  X(uint_max_less_1, 0xfffffffeu) X(uint_max, 0xffffffffu)

#define SIGNED_DIVISORS(X)                                                    \
  X(3, 3) X(5, 5) X(6, 6) X(7, 7) X(10, 10) X(11, 11) X(12, 12) X(25, 25)     \
  X(100, 100) X(641, 641) X(1000, 1000) X(big_prime, 1000000007)              \
  X(int_max, 0x7fffffff) X(minus_3, -3) X(minus_7, -7) X(minus_10, -10)       \
  X(minus_1000, -1000) X(minus_int_max, -0x7fffffff)

#define DEF_UNSIGNED(name, d)                                                 \
  __attribute__((noinline)) static unsigned udiv_##name(unsigned x) {         \
    return x / (d);                                                           \
  }                                                                           \
  __attribute__((noinline)) static unsigned urem_##name(unsigned x) {         \
    return x % (d);                                                           \
  }                                                                           \
  __attribute__((noinline)) static unsigned upair_##name(unsigned x,          \
                                                         unsigned *q) {       \
    *q = x / (d);                                                             \
    return x % (d);                                                           \
  }

#define DEF_SIGNED(name, d)                                                   \
  __attribute__((noinline)) static int sdiv_##name(int x) { return x / (d); } \
  __attribute__((noinline)) static int srem_##name(int x) { return x % (d); } \
  __attribute__((noinline)) static int spair_##name(int x, int *q) {          \
    *q = x / (d);                                                             \
    return x % (d);                                                           \
  }

UNSIGNED_DIVISORS(DEF_UNSIGNED)
SIGNED_DIVISORS(DEF_SIGNED)

#define ENTRY_UNSIGNED(name, d) { d, udiv_##name, urem_##name, upair_##name },
#define ENTRY_SIGNED(name, d) { d, sdiv_##name, srem_##name, spair_##name },

static const struct {
  unsigned d;
  unsigned (*div)(unsigned);
  unsigned (*rem)(unsigned);
  unsigned (*pair)(unsigned, unsigned *);
} unsigned_ops[] = { UNSIGNED_DIVISORS(ENTRY_UNSIGNED) };

static const struct {
  int d;
  int (*div)(int);
  int (*rem)(int);
  int (*pair)(int, int *);
} signed_ops[] = { SIGNED_DIVISORS(ENTRY_SIGNED) };

/* The reference divides by a value the compiler cannot see. */
volatile unsigned unsigned_divisor;
volatile int signed_divisor;

#define NUM_RANDOM 256
#define MAX_INPUTS (NUM_RANDOM + 32)
static unsigned inputs[MAX_INPUTS];

/* Bit patterns shared by both signednesses: the ends of both ranges, the
 * values around each multiple boundary the divisor makes near those ends,
 * and a fixed linear congruential sequence. */
static int fill_inputs(unsigned d) {
  unsigned seed = 0x12345678u, near_top = (0xffffffffu / d) * d;
  unsigned near_int_max = (0x7fffffffu / d) * d;
  int n = 0, i;

  inputs[n++] = 0u;
  inputs[n++] = 1u;
  inputs[n++] = 0x7fffffffu;
  inputs[n++] = 0x80000000u;
  inputs[n++] = 0x80000001u;
  inputs[n++] = 0xfffffffeu;
  inputs[n++] = 0xffffffffu;
  inputs[n++] = d - 1u;
  inputs[n++] = d;
  inputs[n++] = d + 1u;
  inputs[n++] = 2u * d - 1u;
  inputs[n++] = 2u * d;
  inputs[n++] = near_top - 1u;
  inputs[n++] = near_top;
  inputs[n++] = near_top + 1u;
  inputs[n++] = near_int_max - 1u;
  inputs[n++] = near_int_max;
  inputs[n++] = near_int_max + 1u;
  inputs[n++] = 0u - d;
  inputs[n++] = 0u - d + 1u;
  inputs[n++] = 0u - d - 1u;
  inputs[n++] = 0u - near_int_max;
  inputs[n++] = 0u - near_int_max - 1u;
  for (i = 0; i < NUM_RANDOM; i++) {
    seed = seed * 1664525u + 1013904223u;
    inputs[n++] = seed;
  }
  return n;
}

int main(void) {
  unsigned k;
  int i, n;

  for (k = 0; k < sizeof(unsigned_ops) / sizeof(unsigned_ops[0]); k++) {
    unsigned_divisor = unsigned_ops[k].d;
    n = fill_inputs(unsigned_ops[k].d);
    for (i = 0; i < n; i++) {
      unsigned x = inputs[i], q;
      unsigned ref_q = x / unsigned_divisor;
      unsigned ref_r = x % unsigned_divisor;
      if (unsigned_ops[k].div(x) != ref_q)
        abort();
      if (unsigned_ops[k].rem(x) != ref_r)
        abort();
      if (unsigned_ops[k].pair(x, &q) != ref_r || q != ref_q)
        abort();
    }
  }

  for (k = 0; k < sizeof(signed_ops) / sizeof(signed_ops[0]); k++) {
    int d = signed_ops[k].d;
    signed_divisor = d;
    n = fill_inputs(d < 0 ? 0u - (unsigned)d : (unsigned)d);
    for (i = 0; i < n; i++) {
      int x = (int)inputs[i], q;
      int ref_q = x / signed_divisor;
      int ref_r = x % signed_divisor;
      if (signed_ops[k].div(x) != ref_q)
        abort();
      if (signed_ops[k].rem(x) != ref_r)
        abort();
      if (signed_ops[k].pair(x, &q) != ref_r || q != ref_q)
        abort();
    }
  }

  return 0;
}
