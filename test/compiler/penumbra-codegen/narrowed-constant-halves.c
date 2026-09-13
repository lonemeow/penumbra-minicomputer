/*
 * 64-bit and 128-bit operations whose narrowing leaves halves with constant
 * operands: a zero-extended operand's zero high half, or a small constant's.
 * Those halves fold away after legalization — `0 * C`, `x ^ 0`, `x + 0` —
 * and the results must not change.  Every operation is checked against a
 * reference built from 32-bit arithmetic on 16-bit limbs, which no 64-bit
 * narrowing touches, over edge values and a fixed pseudo-random sequence.
 * test-flags.txt enables __int128.
 */

extern void abort(void);

typedef unsigned long long u64;
typedef unsigned __int128 u128;

#define U64(hi, lo) ((u64)(hi) << 32 | (lo))

__attribute__((noinline)) static int is_zero(unsigned hi, unsigned lo) {
  return U64(hi, lo) == 0;
}

__attribute__((noinline)) static int is_five(unsigned hi, unsigned lo) {
  return U64(hi, lo) == 5;
}

__attribute__((noinline)) static int is_not_2_32(unsigned hi, unsigned lo) {
  return U64(hi, lo) != 0x100000000ull;
}

__attribute__((noinline)) static void add_5(unsigned hi, unsigned lo,
                                            unsigned out[2]) {
  u64 r = U64(hi, lo) + 5;
  out[0] = (unsigned)r;
  out[1] = (unsigned)(r >> 32);
}

__attribute__((noinline)) static void mul_3(unsigned hi, unsigned lo,
                                            unsigned out[2]) {
  u64 r = U64(hi, lo) * 3;
  out[0] = (unsigned)r;
  out[1] = (unsigned)(r >> 32);
}

__attribute__((noinline)) static void widening_mul_1000(unsigned x,
                                                        unsigned out[2]) {
  u64 r = (u64)x * 1000;
  out[0] = (unsigned)r;
  out[1] = (unsigned)(r >> 32);
}

/* NetBSD's bintime2timespec: nanoseconds from a binary fraction. */
__attribute__((noinline)) static unsigned frac_to_nsec(unsigned hi,
                                                       unsigned lo) {
  u64 frac = U64(hi, lo);
  return (unsigned)((1000000000ull * (unsigned)(frac >> 32)) >> 32);
}

__attribute__((noinline)) static void mulhi64(unsigned ahi, unsigned alo,
                                              unsigned bhi, unsigned blo,
                                              unsigned out[2]) {
  u64 r = (u64)((u128)U64(ahi, alo) * U64(bhi, blo) >> 64);
  out[0] = (unsigned)r;
  out[1] = (unsigned)(r >> 32);
}

/* Adds V at 16-bit limb K of R, propagating the carry. */
static void add_limb(unsigned r[8], unsigned k, unsigned v) {
  while (v != 0 && k < 8) {
    r[k] += v;
    v = r[k] >> 16;
    r[k] &= 0xffff;
    k++;
  }
}

/* The 128-bit product of A and B as four 32-bit words, low word first. */
static void ref_mul(unsigned ahi, unsigned alo, unsigned bhi, unsigned blo,
                    unsigned p[4]) {
  unsigned a[4], b[4], r[8] = {0};
  unsigned i, j;

  a[0] = alo & 0xffff; a[1] = alo >> 16; a[2] = ahi & 0xffff; a[3] = ahi >> 16;
  b[0] = blo & 0xffff; b[1] = blo >> 16; b[2] = bhi & 0xffff; b[3] = bhi >> 16;
  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++) {
      unsigned t = a[i] * b[j];
      add_limb(r, i + j, t & 0xffff);
      add_limb(r, i + j + 1, t >> 16);
    }
  for (i = 0; i < 4; i++)
    p[i] = r[2 * i] | r[2 * i + 1] << 16;
}

static void check(unsigned ahi, unsigned alo, unsigned bhi, unsigned blo) {
  unsigned out[2], p[4], lo;

  if (is_zero(ahi, alo) != (ahi == 0 && alo == 0) ||
      is_five(ahi, alo) != (ahi == 0 && alo == 5) ||
      is_not_2_32(ahi, alo) != !(ahi == 1 && alo == 0))
    abort();

  add_5(ahi, alo, out);
  lo = alo + 5;
  if (out[0] != lo || out[1] != ahi + (lo < alo))
    abort();

  mul_3(ahi, alo, out);
  ref_mul(ahi, alo, 0, 3, p);
  if (out[0] != p[0] || out[1] != p[1])
    abort();

  widening_mul_1000(alo, out);
  ref_mul(0, alo, 0, 1000, p);
  if (out[0] != p[0] || out[1] != p[1])
    abort();

  ref_mul(0, ahi, 0, 1000000000u, p);
  if (frac_to_nsec(ahi, alo) != p[1])
    abort();

  mulhi64(ahi, alo, bhi, blo, out);
  ref_mul(ahi, alo, bhi, blo, p);
  if (out[0] != p[2] || out[1] != p[3])
    abort();
}

#define NUM_RANDOM 256

int main(void) {
  static const unsigned edges[] = {
    0u, 1u, 2u, 3u, 5u, 0xffffu, 0x10000u, 0x7fffffffu, 0x80000000u,
    0xfffffffbu, 0xfffffffeu, 0xffffffffu
  };
  const unsigned n = sizeof(edges) / sizeof(edges[0]);
  unsigned i, j, seed = 0x12345678u, v[4];

  for (i = 0; i < n * n; i++)
    for (j = 0; j < n; j++)
      check(edges[i / n], edges[i % n], edges[j], edges[(i + j) % n]);

  for (i = 0; i < NUM_RANDOM; i++) {
    for (j = 0; j < 4; j++) {
      seed = seed * 1664525u + 1013904223u;
      v[j] = seed;
    }
    check(v[0], v[1], v[2], v[3]);
  }

  return 0;
}
