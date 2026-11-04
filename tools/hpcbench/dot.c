/* scoreboard: dot C baseline — identical workload to dot.pen
 * (s = dot(x, y) over 8M doubles, repeated).
 *
 * A dot product is inherently a single pass, so there is no multi-pass
 * form here: this is the straightforward hand-written C reduction.
 *
 * Same flags as the Pentest driver: -std=c11 -O3 -march=native, no
 * fast-math. Output contract: line 1 = checksum (s), line 2 = kernel
 * seconds on CLOCK_MONOTONIC.
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

int main(int argc, char** argv) {
  const size_t n = 8388608;
  const int reps = 50;
  double* x = (double*)malloc(n * sizeof(double));
  double* y = (double*)malloc(n * sizeof(double));
  double s = 0.0, t0, t1;
  size_t i;
  int r;
  (void)argc; (void)argv;
  if (!x || !y) { fprintf(stderr, "alloc failed\n"); return 1; }
  for (i = 0; i < n; i++) { x[i] = 1.5; y[i] = 150.25; }

  /* warm-up pass: identical value, primes caches outside the timed section */
  { double acc = 0.0; for (i = 0; i < n; i++) acc += x[i] * y[i]; s = acc; }

  t0 = now();
  for (r = 0; r < reps; r++) {
    double acc = 0.0;
    for (i = 0; i < n; i++) acc += x[i] * y[i];
    s = acc;
  }
  t1 = now();

  printf("%.1f\n%.9f\n", s, t1 - t0);
  free(x); free(y);
  return 0;
}
