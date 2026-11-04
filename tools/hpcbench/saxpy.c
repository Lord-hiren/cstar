/* scoreboard: saxpy C baselines — identical workload to saxpy.pen.
 *
 *   ./saxpy multi   one loop per operation with a real temporary array
 *                   (the form an unfused compilation emits, and a common
 *                   clarity-first C style)
 *   ./saxpy fused   one loop, the whole expression (the form a C
 *                   programmer writes by hand for this kernel)
 *
 * Same flags as the Pentest driver: -std=c11 -O3 -march=native, no
 * fast-math. Output contract: line 1 = checksum (sum(y)), line 2 =
 * kernel seconds on CLOCK_MONOTONIC — the same clock Pentest's clock()
 * reads.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char** argv) {
  const size_t n = 8388608;
  const int reps = 40;
  const int fused = argc < 2 || strcmp(argv[1], "fused") == 0;
  double* x = (double*)malloc(n * sizeof(double));
  double* y = (double*)malloc(n * sizeof(double));
  double* t = (double*)malloc(n * sizeof(double));
  double ck, t0, t1;
  size_t i;
  int r;
  if (!x || !y || !t) { fprintf(stderr, "alloc failed\n"); return 1; }
  for (i = 0; i < n; i++) { x[i] = 1.5; y[i] = 0.25; }

  /* warm-up pass: identical work, faults pages outside the timed section */
  if (fused) {
    for (i = 0; i < n; i++) y[i] = 2.5 * x[i] + y[i];
  } else {
    for (i = 0; i < n; i++) t[i] = 2.5 * x[i];
    for (i = 0; i < n; i++) y[i] = t[i] + y[i];
  }

  t0 = now();
  if (fused) {
    for (r = 0; r < reps; r++)
      for (i = 0; i < n; i++) y[i] = 2.5 * x[i] + y[i];
  } else {
    for (r = 0; r < reps; r++) {
      for (i = 0; i < n; i++) t[i] = 2.5 * x[i];
      for (i = 0; i < n; i++) y[i] = t[i] + y[i];
    }
  }
  t1 = now();

  ck = 0.0;
  for (i = 0; i < n; i++) ck += y[i];
  printf("%.1f\n%.9f\n", ck, t1 - t0);
  free(x); free(y); free(t);
  return 0;
}
