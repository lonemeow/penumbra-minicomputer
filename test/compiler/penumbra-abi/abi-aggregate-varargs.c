/*
 * Aggregates passed through `...` and retrieved with va_arg
 * (doc/system/abi.md "Argument Passing", variadic functions):
 * anonymous aggregates use the same slot assignment as named ones.
 * Exercises one aggregate per size class plus a trailing scalar to
 * confirm the walk stays in step.
 */

extern void abort(void);

#include <stdarg.h>

struct s4 { int a; };
struct s8 { int a, b; };
struct s12 { int a, b, c; };

volatile int X = 0x300;

static void vcheck(int n, ...) {
  va_list ap;
  va_start(ap, n);

  if (va_arg(ap, int) != X + 1) abort();

  struct s4 a = va_arg(ap, struct s4);
  if (a.a != X + 2) abort();

  struct s8 b = va_arg(ap, struct s8);
  if (b.a != X + 3) abort();
  if (b.b != X + 4) abort();

  struct s12 c = va_arg(ap, struct s12);
  if (c.a != X + 5) abort();
  if (c.b != X + 6) abort();
  if (c.c != X + 7) abort();

  if (va_arg(ap, int) != X + 8) abort();

  va_end(ap);
}

void (*volatile pv)(int, ...) = vcheck;

int main(void) {
  int x = X;
  struct s4 a = { x + 2 };
  struct s8 b = { x + 3, x + 4 };
  struct s12 c = { x + 5, x + 6, x + 7 };

  pv(5, x + 1, a, b, c, x + 8);

  return 0;
}
