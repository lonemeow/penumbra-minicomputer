/*
 * Aggregate arguments: value correctness across the ABI size classes
 * (doc/system/abi.md "Argument Passing"): 1-4 bytes = one slot,
 * 5-8 bytes = two slots, > 8 bytes = by reference to a caller-owned
 * copy.  Also covers slot positioning: aggregates between scalars and
 * aggregates passed entirely on the stack.
 *
 * All calls go through volatile function pointers so every call
 * boundary survives inlining and IPO; expected values are re-read
 * from a volatile global inside each callee so the checks cannot
 * constant-fold.
 */

extern void abort(void);

struct s1 { signed char a; };
struct s2 { short a; };
struct s3 { signed char a, b, c; };
struct s4 { int a; };
struct s6 { short a, b, c; };
struct s8 { int a, b; };
struct s12 { int a, b, c; };
struct s16 { int a, b, c, d; };

volatile int X = 0x100;

static void f1(struct s1 s) { if (s.a != (signed char)(X + 1)) abort(); }
static void f2(struct s2 s) { if (s.a != (short)(X + 2)) abort(); }
static void f3(struct s3 s) {
  if (s.a != (signed char)(X + 3)) abort();
  if (s.b != (signed char)(X + 4)) abort();
  if (s.c != (signed char)(X + 5)) abort();
}
static void f4(struct s4 s) { if (s.a != X + 6) abort(); }
static void f6(struct s6 s) {
  if (s.a != (short)(X + 7)) abort();
  if (s.b != (short)(X + 8)) abort();
  if (s.c != (short)(X + 9)) abort();
}
static void f8(struct s8 s) {
  if (s.a != X + 10) abort();
  if (s.b != X + 11) abort();
}
static void f12(struct s12 s) {
  if (s.a != X + 12) abort();
  if (s.b != X + 13) abort();
  if (s.c != X + 14) abort();
}
static void f16(struct s16 s) {
  if (s.a != X + 15) abort();
  if (s.b != X + 16) abort();
  if (s.c != X + 17) abort();
  if (s.d != X + 18) abort();
}

/* aggregate between scalars: scalars must land in the right slots */
static void fmix(int a, struct s8 s, int b) {
  if (a != X + 20) abort();
  if (s.a != X + 21) abort();
  if (s.b != X + 22) abort();
  if (b != X + 23) abort();
}

/* aggregate entirely in stack slots */
static void fstack(int a, int b, int c, int d, struct s8 s, int e) {
  if (a != X + 30) abort();
  if (b != X + 31) abort();
  if (c != X + 32) abort();
  if (d != X + 33) abort();
  if (s.a != X + 34) abort();
  if (s.b != X + 35) abort();
  if (e != X + 36) abort();
}

/* by-reference aggregate (> 8 bytes) in a stack slot position */
static void fstack_ref(int a, int b, int c, int d, struct s16 s) {
  if (a != X + 40) abort();
  if (b != X + 41) abort();
  if (c != X + 42) abort();
  if (d != X + 43) abort();
  if (s.a != X + 44) abort();
  if (s.d != X + 47) abort();
}

void (*volatile p1)(struct s1) = f1;
void (*volatile p2)(struct s2) = f2;
void (*volatile p3)(struct s3) = f3;
void (*volatile p4)(struct s4) = f4;
void (*volatile p6)(struct s6) = f6;
void (*volatile p8)(struct s8) = f8;
void (*volatile p12)(struct s12) = f12;
void (*volatile p16)(struct s16) = f16;
void (*volatile pmix)(int, struct s8, int) = fmix;
void (*volatile pstack)(int, int, int, int, struct s8, int) = fstack;
void (*volatile pstack_ref)(int, int, int, int, struct s16) = fstack_ref;

int main(void) {
  int x = X;

  struct s1 a1 = { (signed char)(x + 1) };
  p1(a1);
  struct s2 a2 = { (short)(x + 2) };
  p2(a2);
  struct s3 a3 = { (signed char)(x + 3), (signed char)(x + 4),
                   (signed char)(x + 5) };
  p3(a3);
  struct s4 a4 = { x + 6 };
  p4(a4);
  struct s6 a6 = { (short)(x + 7), (short)(x + 8), (short)(x + 9) };
  p6(a6);
  struct s8 a8 = { x + 10, x + 11 };
  p8(a8);
  struct s12 a12 = { x + 12, x + 13, x + 14 };
  p12(a12);
  struct s16 a16 = { x + 15, x + 16, x + 17, x + 18 };
  p16(a16);

  struct s8 m = { x + 21, x + 22 };
  pmix(x + 20, m, x + 23);

  struct s8 st = { x + 34, x + 35 };
  pstack(x + 30, x + 31, x + 32, x + 33, st, x + 36);

  struct s16 sr = { x + 44, x + 45, x + 46, x + 47 };
  pstack_ref(x + 40, x + 41, x + 42, x + 43, sr);

  return 0;
}
