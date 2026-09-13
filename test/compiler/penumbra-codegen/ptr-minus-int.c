/*
 * Pointer minus integer, `p - n`, which reaches the backend as a pointer plus
 * a negated, scaled integer and is rewritten as a subtract.  Each result is
 * checked against the address computed with unsigned integer arithmetic,
 * which involves no pointer offset, for byte, word and non-power-of-two
 * element sizes, positive and negative n, a negation that is also used
 * elsewhere, and a backward walk that reads through the result.
 */

extern void abort(void);

struct rec { int a, b, c; };
struct big { char pad[40]; int tag; };
struct mbuf { char *data; int len; };

#define N 64

static char bytes[4 * N];
static int words[N];
static struct rec recs[N];
static struct big bigs[N];

__attribute__((noinline)) char *sub_bytes(char *p, int n) {
  return p - n;
}

__attribute__((noinline)) int *sub_words(int *p, int n) {
  return p - n;
}

__attribute__((noinline)) struct rec *sub_recs(struct rec *p, int n) {
  return p - n;
}

__attribute__((noinline)) struct big *sub_bigs(struct big *p, int n) {
  return p - n;
}

__attribute__((noinline)) void adjust(struct mbuf *m, int off) {
  m->data -= off;
  m->len += off;
}

__attribute__((noinline)) char *sub_keep(char *p, int n, int *out) {
  *out = -n;
  return p - n;
}

/* Sums every step-th word walking back from end, count times. */
__attribute__((noinline)) int walk_back(int *end, int count, int step) {
  int sum = 0;
  while (count-- > 0) {
    end -= step;
    sum += *end;
  }
  return sum;
}

static void check(int n) {
  unsigned long b = (unsigned long)&bytes[2 * N];
  unsigned long w = (unsigned long)&words[N / 2];
  unsigned long r = (unsigned long)&recs[N / 2];
  unsigned long g = (unsigned long)&bigs[N / 2];
  struct mbuf m;
  int negated;

  if ((unsigned long)sub_bytes((char *)b, n) != b - (unsigned long)n ||
      (unsigned long)sub_words((int *)w, n) != w - (unsigned long)n * 4 ||
      (unsigned long)sub_recs((struct rec *)r, n) !=
          r - (unsigned long)n * 12 ||
      (unsigned long)sub_bigs((struct big *)g, n) !=
          g - (unsigned long)n * 44)
    abort();

  m.data = (char *)b;
  m.len = 100;
  adjust(&m, n);
  if ((unsigned long)m.data != b - (unsigned long)n || m.len != 100 + n)
    abort();

  if ((unsigned long)sub_keep((char *)b, n, &negated) !=
          b - (unsigned long)n ||
      negated != -n)
    abort();
}

int main(void) {
  int i, step;

  for (i = 0; i < N; i++)
    words[i] = i * 7 + 1;

  for (i = -(N / 2) + 1; i < N / 2; i++)
    check(i);

  for (step = 1; step <= 4; step++) {
    int count = (N - 1) / step, expect = 0, k;
    for (k = 1; k <= count; k++)
      expect += (N - k * step) * 7 + 1;
    if (walk_back(words + N, count, step) != expect)
      abort();
  }

  return 0;
}
