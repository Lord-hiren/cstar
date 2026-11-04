/* scoreboard: collatz C baseline — identical workload to collatz.pen
 * (total stopping steps for every n in [1, 4M], same warm-up prefix).
 *
 * Same flags as the Pentest driver: -std=c11 -O3 -march=native, no
 * fast-math. Output contract: line 1 = checksum (total steps), line 2 =
 * kernel seconds on CLOCK_MONOTONIC.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void) {
  const long long n = 4000000;
  long long total = 0;
  long long warm = 0;

  /* warm-up: the same uncounted prefix as the .pen variant */
  for (long long i = 1; i <= 100000; i++) {
    long long k = i;
    while (k != 1) {
      if (k % 2 == 0) k = k / 2;
      else k = k * 3 + 1;
      warm += 1;
    }
  }
  (void)warm;

  double t0 = now();
  for (long long i = 1; i <= n; i++) {
    long long k = i;
    while (k != 1) {
      if (k % 2 == 0) k = k / 2;
      else k = k * 3 + 1;
      total += 1;
    }
  }
  double t1 = now();
  printf("%lld\n%.9f\n", total, t1 - t0);
  return 0;
}
