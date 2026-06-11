/*
 * By-value semantics: writes a callee makes to its aggregate
 * parameter must never be visible in the caller's argument object
 * (doc/system/abi.md "Argument Passing").
 *
 * Each callee scribbles on its parameter and keeps the write live by
 * handing the parameter's address to an opaque consumer, so the
 * store cannot be dead-store-eliminated.  The caller re-reads its
 * original object through a volatile pointer afterwards, so the
 * check observes actual memory rather than a forwarded SSA value
 * (LLVM is entitled to assume by-value arguments are unmodified and
 * forward the pre-call store otherwise).
 */

extern void abort(void);

struct s4 { int a; };
struct s8 { int a, b; };
struct s16 { int a, b, c, d; };

volatile int seen;
static void consume_impl(void *p) { seen = *(volatile int *)p; }
void (*volatile consume)(void *) = consume_impl;

static void clobber4(struct s4 s) {
  s.a = 0x5a5a5a5a;
  consume(&s);
}
static void clobber8(struct s8 s) {
  s.a = 0x5a5a5a5a;
  s.b = 0x5a5a5a5a;
  consume(&s);
}
static void clobber16(struct s16 s) {
  s.a = 0x5a5a5a5a;
  s.d = 0x5a5a5a5a;
  consume(&s);
}
/* same, with the aggregate in stack-slot position */
static void clobber8_stack(int a, int b, int c, int d, struct s8 s) {
  s.a = 0x5a5a5a5a;
  s.b = 0x5a5a5a5a;
  consume(&s);
}

void (*volatile p4)(struct s4) = clobber4;
void (*volatile p8)(struct s8) = clobber8;
void (*volatile p16)(struct s16) = clobber16;
void (*volatile p8s)(int, int, int, int, struct s8) = clobber8_stack;

volatile int X = 0x11;

int main(void) {
  int x = X;

  struct s4 a = { x };
  p4(a);
  if (*(volatile int *)&a.a != x) abort();

  struct s8 b = { x + 1, x + 2 };
  p8(b);
  if (*(volatile int *)&b.a != x + 1) abort();
  if (*(volatile int *)&b.b != x + 2) abort();

  struct s16 c = { x + 3, x + 4, x + 5, x + 6 };
  p16(c);
  if (*(volatile int *)&c.a != x + 3) abort();
  if (*(volatile int *)&c.d != x + 6) abort();

  struct s8 d = { x + 7, x + 8 };
  p8s(1, 2, 3, 4, d);
  if (*(volatile int *)&d.a != x + 7) abort();
  if (*(volatile int *)&d.b != x + 8) abort();

  return 0;
}
