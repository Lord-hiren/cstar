/* scoreboard: ptrlist C baseline — identical workload to ptrlist.pen
 * (1M-node linked list, same Fisher-Yates/Lehmer-LCG shuffled chain,
 * same warm-up pass, 6 timed passes summing node values).
 *
 * Same flags as the Pentest driver: -std=c11 -O3 -march=native, no
 * fast-math. Output contract: line 1 = checksum (sum of node values x
 * reps), line 2 = kernel seconds on CLOCK_MONOTONIC.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

struct node {
  long long val;
  struct node *nxt;
};

int main(void) {
  const long long n = 1000000;
  const int reps = 6;
  struct node **nodes = malloc((size_t)n * sizeof(*nodes));
  long long *a = malloc((size_t)n * sizeof(long long));
  struct node *head;
  long long s = 0, warm = 0, x = 12345, t;
  int r;

  for (long long i = 0; i < n; i++) {
    nodes[i] = malloc(sizeof(struct node));
    nodes[i]->val = i;
    nodes[i]->nxt = NULL;
  }
  for (long long i = 0; i < n; i++) a[i] = i;
  for (long long i = n - 1; i > 0; i--) {
    x = (x * 48271) % 2147483647;
    long long j = x % (i + 1);
    t = a[i];
    a[i] = a[j];
    a[j] = t;
  }
  for (long long k = 0; k < n - 1; k++) nodes[a[k]]->nxt = nodes[a[k + 1]];
  nodes[a[n - 1]]->nxt = NULL;
  head = nodes[a[0]];

  /* warm-up: one uncounted pass over the chain */
  for (struct node *cur = head; cur; cur = cur->nxt) warm += cur->val;
  (void)warm;

  double t0 = now();
  for (r = 0; r < reps; r++)
    for (struct node *cur = head; cur; cur = cur->nxt) s += cur->val;
  double t1 = now();
  printf("%lld\n%.9f\n", s, t1 - t0);

  free(nodes);
  free(a);
  return 0;
}
