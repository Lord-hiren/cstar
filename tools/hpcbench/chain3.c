/* scoreboard: chain3 C baselines — identical workload to chain3.pen and
 * chain3_dyn.pen (z = (2.0 * x + 1.0) * 3.0 + 0.5 over 8M doubles).
 *
 *   ./chain3 multi   one loop per operation with real temporary arrays
 *                    (what an unfused compilation emits)
 *   ./chain3 fused   one loop, the whole expression (the hand-written
 *                    single-pass form; also what `gcc -O3` itself
 *                    vectorizes this function to)
 *
 * Same flags as the Pentest driver: -std=c11 -O3 -march=native, no
 * fast-math. Output contract: line 1 = checksum (sum(z)), line 2 =
 * kernel seconds on CLOCK_MONOTONIC.
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
  const int reps = 30;
  const int fused = argc < 2 || strcmp(argv[1], "fused") == 0;
  double* x = (double*)malloc(n * sizeof(double));
  double* z = (double*)malloc(n * sizeof(double));
  double* t1 = (double*)malloc(n * sizeof(double));
  double* t2 = (double*)malloc(n * sizeof(double));
  double ck, t0, t1s;
  size_t i;
  int r;
  if (!x || !z || !t1 || !t2) { fprintf(stderr, "alloc failed\n"); return 1; }
  for (i = 0; i < n; i++) { x[i] = 1.5; z[i] = 0.0; }

  /* warm-up pass: identical work, faults pages outside the timed section */
  if (fused) {
    for (i = 0; i < n; i++) z[i] = (2.0 * x[i] + 1.0) * 3.0 + 0.5;
  } else {
    for (i = 0; i < n; i++) t1[i] = 2.0 * x[i] + 1.0;
    for (i = 0; i < n; i++) t2[i] = t1[i] * 3.0;
    for (i = 0; i < n; i++) z[i] = t2[i] + 0.5;
  }

  t0 = now();
  if (fused) {
    for (r = 0; r < reps; r++)
      for (i = 0; i < n; i++) z[i] = (2.0 * x[i] + 1.0) * 3.0 + 0.5;
  } else {
    for (r = 0; r < reps; r++) {
      for (i = 0; i < n; i++) t1[i] = 2.0 * x[i] + 1.0;
      for (i = 0; i < n; i++) t2[i] = t1[i] * 3.0;
      for (i = 0; i < n; i++) z[i] = t2[i] + 0.5;
    }
  }
  t1s = now();

  ck = 0.0;
  for (i = 0; i < n; i++) ck += z[i];
  printf("%.1f\n%.9f\n", ck, t1s - t0);
  free(x); free(z); free(t1); free(t2);
  return 0;
}
