/*
 * Two-slot aggregate straddling the register/stack boundary
 * (doc/system/abi.md "Argument Passing"): slots are never skipped
 * for alignment, so with three leading scalars in R1-R3, an 8-byte
 * aggregate occupies slot 4 (R4) and slot 5 (the first stack word).
 * A trailing scalar in slot 6 confirms the assignment stays in step
 * past the straddle.
 */

extern void abort(void);

struct s8 { int a, b; };

volatile int X = 0x40;

static void check_straddle(int a, int b, int c, struct s8 s, int e) {
  if (a != X)       abort();
  if (b != X + 1)   abort();
  if (c != X + 2)   abort();
  if (s.a != X + 3) abort();
  if (s.b != X + 4) abort();
  if (e != X + 5)   abort();
}

void (*volatile pstraddle)(int, int, int, struct s8, int) = check_straddle;

int main(void) {
  int x = X;
  struct s8 s = { x + 3, x + 4 };

  pstraddle(x, x + 1, x + 2, s, x + 5);

  return 0;
}
