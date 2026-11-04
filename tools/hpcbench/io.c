/* scoreboard: io C baselines — identical workload to io.pen / io_join.pen
 * (write 600k formatted records per rep, close, reopen binary, read
 * whole; 4 reps; identical record bytes "i v\n" with v = (i*75)%1000003).
 *
 *   ./io stdio   fprintf one record per line + whole-file fread (the
 *                idiomatic formatted I/O form)
 *   ./io raw     preformat every record into one buffer, one fwrite +
 *                whole-file fread (the hand-optimized form)
 *
 * Same flags as the Pentest driver: -std=c11 -O3 -march=native, no
 * fast-math. Output contract: line 1 = checksum (file byte-sum + length
 * term), line 2 = kernel seconds on CLOCK_MONOTONIC.
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

static const char *kPath = "hpcbench_io.dat";

/* one full write pass over the file, in the requested form */
static void write_records(FILE *f, long long n, int raw, char *buf) {
  if (raw) {
    size_t off = 0;
    for (long long i = 0; i < n; i++)
      off += (size_t)sprintf(buf + off, "%lld %lld\n", i, (i * 75) % 1000003);
    fwrite(buf, 1, off, f);
  } else {
    for (long long i = 0; i < n; i++)
      fprintf(f, "%lld %lld\n", i, (i * 75) % 1000003);
  }
}

int main(int argc, char **argv) {
  const long long n = 600000;
  const int reps = 4;
  int raw;
  size_t cap = (size_t)n * 24 + 4096;
  char *buf = malloc(cap);
  if (!buf || argc < 2 || (strcmp(argv[1], "stdio") != 0 &&
                           strcmp(argv[1], "raw") != 0)) {
    fprintf(stderr, "usage: io stdio|raw\n");
    return 2;
  }
  raw = strcmp(argv[1], "raw") == 0;

  /* warm-up: one uncounted write + read cycle */
  {
    FILE *f = fopen(kPath, "w");
    write_records(f, n, raw, buf);
    fclose(f);
    f = fopen(kPath, "rb");
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    (void)got;
  }

  double t0 = now();
  for (int r = 0; r < reps; r++) {
    FILE *f = fopen(kPath, "w");
    write_records(f, n, raw, buf);
    fclose(f);
    f = fopen(kPath, "rb");
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    (void)got;
  }
  double t1 = now();

  /* checksum: byte-sum of the file the timed section left behind, plus
   * a length term so truncation can't hide behind a byte-sum match */
  {
    FILE *f = fopen(kPath, "rb");
    size_t len = fread(buf, 1, cap, f);
    fclose(f);
    long long total = 0;
    for (size_t i = 0; i < len; i++) total += (unsigned char)buf[i];
    printf("%lld\n%.9f\n", total + (long long)len * 1000000000LL, t1 - t0);
  }
  free(buf);
  return 0;
}
