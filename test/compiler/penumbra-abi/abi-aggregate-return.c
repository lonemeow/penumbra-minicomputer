/*
 * Aggregate return values across the ABI size classes
 * (doc/system/abi.md "Return Values"): <= 4 bytes in R1, 5-8 bytes
 * in R1:R2, > 8 bytes via the hidden result pointer.  Builders are
 * reached through volatile function pointers and construct their
 * results from a volatile global, so neither the calls nor the
 * checks can be folded away.
 */

extern void abort(void);

struct s1 { signed char a; };
struct s4 { int a; };
struct s6 { short a, b, c; };
struct s8 { int a, b; };
struct s12 { int a, b, c; };
struct s16 { int a, b, c, d; };

volatile int X = 0x200;

static struct s1 make1(void) {
  struct s1 s = { (signed char)(X + 1) };
  return s;
}
static struct s4 make4(void) {
  struct s4 s = { X + 2 };
  return s;
}
static struct s6 make6(void) {
  struct s6 s = { (short)(X + 3), (short)(X + 4), (short)(X + 5) };
  return s;
}
static struct s8 make8(void) {
  struct s8 s = { X + 6, X + 7 };
  return s;
}
static struct s12 make12(void) {
  struct s12 s = { X + 8, X + 9, X + 10 };
  return s;
}
static struct s16 make16(void) {
  struct s16 s = { X + 11, X + 12, X + 13, X + 14 };
  return s;
}

struct s1 (*volatile pm1)(void) = make1;
struct s4 (*volatile pm4)(void) = make4;
struct s6 (*volatile pm6)(void) = make6;
struct s8 (*volatile pm8)(void) = make8;
struct s12 (*volatile pm12)(void) = make12;
struct s16 (*volatile pm16)(void) = make16;

int main(void) {
  int x = X;

  struct s1 r1 = pm1();
  if (r1.a != (signed char)(x + 1)) abort();

  struct s4 r4 = pm4();
  if (r4.a != x + 2) abort();

  struct s6 r6 = pm6();
  if (r6.a != (short)(x + 3)) abort();
  if (r6.b != (short)(x + 4)) abort();
  if (r6.c != (short)(x + 5)) abort();

  struct s8 r8 = pm8();
  if (r8.a != x + 6) abort();
  if (r8.b != x + 7) abort();

  struct s12 r12 = pm12();
  if (r12.a != x + 8) abort();
  if (r12.b != x + 9) abort();
  if (r12.c != x + 10) abort();

  struct s16 r16 = pm16();
  if (r16.a != x + 11) abort();
  if (r16.b != x + 12) abort();
  if (r16.c != x + 13) abort();
  if (r16.d != x + 14) abort();

  return 0;
}
