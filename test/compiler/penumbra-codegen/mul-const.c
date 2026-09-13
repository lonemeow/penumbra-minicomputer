/*
 * Multiplication by a constant.  A core lowers some of these to shifted
 * copies of the multiplicand combined by adds and subtracts, choosing by its
 * own multiply and ALU costs; either way the product must match the hardware
 * multiply.  Each constant is checked at 32 bits, and a set of 64-bit
 * constants whose partial products the legalizer splits into 32-bit
 * multiplies, against the same product with a multiplier known only at run
 * time, over inputs at both ends of the range and from a fixed pseudo-random
 * sequence.  mul-const-penumbra1.c runs the same checks tuned for a core that
 * accepts longer decompositions.
 */

extern void abort(void);

#define CONSTANTS_32(X)                                                       \
  X(0, 0u) X(1, 1u) X(2, 2u) X(3, 3u) X(5, 5u) X(6, 6u) X(7, 7u) X(9, 9u)     \
  X(10, 10u) X(12, 12u) X(15, 15u) X(24, 24u) X(80, 80u) X(100, 100u)         \
  X(320, 320u) X(641, 641u) X(1000, 1000u) X(1365, 1365u)                     \
  X(splat, 0x01010101u) X(nibble_splat, 0x0f0f0f0fu) X(int_max, 0x7fffffffu)  \
  X(int_min, 0x80000000u) X(int_min_plus_1, 0x80000001u)                      \
  X(minus_1, 0xffffffffu) X(minus_3, 0xfffffffdu) X(minus_7, 0xfffffff9u)     \
  X(minus_100, 0xffffff9cu) X(alternating, 0x55555555u)                       \
  X(alternating_high, 0xaaaaaaabu) X(mixed, 0x12345678u)

#define CONSTANTS_64(X)                                                       \
  X(10, 10ull) X(1000, 1000ull) X(two_32_plus_8, 0x100000008ull)              \
  X(two_32_plus_1, 0x100000001ull) X(minus_3, 0xfffffffffffffffdull)          \
  X(splat, 0x0101010101010101ull)

#define DEF_32(name, c)                                                       \
  __attribute__((noinline)) static unsigned mul32_##name(unsigned x) {        \
    return x * (c);                                                           \
  }
#define DEF_64(name, c)                                                       \
  __attribute__((noinline)) static unsigned long long mul64_##name(           \
      unsigned long long x) {                                                 \
    return x * (c);                                                           \
  }

CONSTANTS_32(DEF_32)
CONSTANTS_64(DEF_64)

#define ENTRY_32(name, c) { c, mul32_##name },
#define ENTRY_64(name, c) { c, mul64_##name },

static const struct {
  unsigned c;
  unsigned (*mul)(unsigned);
} ops32[] = { CONSTANTS_32(ENTRY_32) };

static const struct {
  unsigned long long c;
  unsigned long long (*mul)(unsigned long long);
} ops64[] = { CONSTANTS_64(ENTRY_64) };

/* The reference multiplies by a value the compiler cannot see. */
volatile unsigned multiplier32;
volatile unsigned long long multiplier64;

#define NUM_RANDOM 64

int main(void) {
  static const unsigned edges32[] = {
    0u, 1u, 2u, 3u, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xfffffffeu,
    0xffffffffu, 0x0000ffffu, 0x00010000u, 0x55555555u, 0xaaaaaaaau
  };
  unsigned k, i, seed;

  for (k = 0; k < sizeof(ops32) / sizeof(ops32[0]); k++) {
    multiplier32 = ops32[k].c;
    for (i = 0; i < sizeof(edges32) / sizeof(edges32[0]); i++)
      if (ops32[k].mul(edges32[i]) != edges32[i] * multiplier32)
        abort();
    seed = 0x12345678u;
    for (i = 0; i < NUM_RANDOM; i++) {
      seed = seed * 1664525u + 1013904223u;
      if (ops32[k].mul(seed) != seed * multiplier32)
        abort();
    }
  }

  for (k = 0; k < sizeof(ops64) / sizeof(ops64[0]); k++) {
    unsigned long long x = 0x0123456789abcdefull;
    multiplier64 = ops64[k].c;
    for (i = 0; i < NUM_RANDOM; i++) {
      x = x * 6364136223846793005ull + 1442695040888963407ull;
      if (ops64[k].mul(x) != x * multiplier64)
        abort();
    }
    if (ops64[k].mul(0xffffffffffffffffull) !=
        0xffffffffffffffffull * multiplier64)
      abort();
  }

  return 0;
}
