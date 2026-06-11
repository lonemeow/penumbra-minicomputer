/*
 * A const (.rodata) aggregate passed by value must survive a callee
 * that writes to its parameter (doc/system/abi.md "Argument
 * Passing": by-value semantics).  On hardware a violation scribbles
 * on a read-only mapping; the ISS has no memory protection, so this
 * test verifies by value instead: the constant is re-read through a
 * volatile pointer and passed to a second, checking callee.
 */

extern void abort(void);

struct s8 { int a, b; };

static const struct s8 G = { 0x1234, 0x5678 };

volatile int seen;
static void consume_impl(void *p) { seen = *(volatile int *)p; }
void (*volatile consume)(void *) = consume_impl;

static void scribble(struct s8 s) {
  s.a = -1;
  s.b = -1;
  consume(&s);
}
static void check(struct s8 s) {
  if (s.a != 0x1234) abort();
  if (s.b != 0x5678) abort();
}

void (*volatile pscribble)(struct s8) = scribble;
void (*volatile pcheck)(struct s8) = check;

int main(void) {
  pscribble(G);

  /* the constant must be intact, both read directly ... */
  const volatile struct s8 *vg = &G;
  if (vg->a != 0x1234) abort();
  if (vg->b != 0x5678) abort();

  /* ... and when passed again */
  pcheck(G);

  return 0;
}
