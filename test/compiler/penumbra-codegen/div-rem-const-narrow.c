/*
 * Division and remainder of a char or short by a constant.  InstCombine
 * narrows these to i8 / i16 operations, so a core that takes the
 * magic-number rewrite produces an 8- or 16-bit high-half multiply, which
 * the legalizer widens.  Every input of each width is checked against the
 * same operation on a divisor known only at run time.
 */

extern void abort(void);

#define DEF(type, tag, d, name)                                               \
  __attribute__((noinline)) static int div_##tag##_##name(type x) {           \
    return x / (d);                                                           \
  }                                                                           \
  __attribute__((noinline)) static int rem_##tag##_##name(type x) {           \
    return x % (d);                                                           \
  }

DEF(unsigned char, u8, 3, 3)
DEF(unsigned char, u8, 7, 7)
DEF(unsigned char, u8, 10, 10)
DEF(unsigned char, u8, 100, 100)
DEF(signed char, s8, 3, 3)
DEF(signed char, s8, 7, 7)
DEF(signed char, s8, -3, minus_3)
DEF(signed char, s8, 100, 100)
DEF(unsigned short, u16, 3, 3)
DEF(unsigned short, u16, 10, 10)
DEF(unsigned short, u16, 641, 641)
DEF(unsigned short, u16, 10000, 10000)
DEF(short, s16, 3, 3)
DEF(short, s16, 7, 7)
DEF(short, s16, -10, minus_10)
DEF(short, s16, 1000, 1000)

volatile int divisor;

#define CHECK(type, tag, d, name, lo, hi)                                     \
  divisor = (d);                                                              \
  for (x = (lo); x <= (hi); x++) {                                            \
    if (div_##tag##_##name((type)x) != x / divisor)                           \
      abort();                                                                \
    if (rem_##tag##_##name((type)x) != x % divisor)                           \
      abort();                                                                \
  }

int main(void) {
  int x;

  CHECK(unsigned char, u8, 3, 3, 0, 255)
  CHECK(unsigned char, u8, 7, 7, 0, 255)
  CHECK(unsigned char, u8, 10, 10, 0, 255)
  CHECK(unsigned char, u8, 100, 100, 0, 255)
  CHECK(signed char, s8, 3, 3, -128, 127)
  CHECK(signed char, s8, 7, 7, -128, 127)
  CHECK(signed char, s8, -3, minus_3, -128, 127)
  CHECK(signed char, s8, 100, 100, -128, 127)
  CHECK(unsigned short, u16, 3, 3, 0, 65535)
  CHECK(unsigned short, u16, 10, 10, 0, 65535)
  CHECK(unsigned short, u16, 641, 641, 0, 65535)
  CHECK(unsigned short, u16, 10000, 10000, 0, 65535)
  CHECK(short, s16, 3, 3, -32768, 32767)
  CHECK(short, s16, 7, 7, -32768, 32767)
  CHECK(short, s16, -10, minus_10, -32768, 32767)
  CHECK(short, s16, 1000, 1000, -32768, 32767)

  return 0;
}
