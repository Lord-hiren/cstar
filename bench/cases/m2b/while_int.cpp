// while_int.cpp — C++ twin of bench/cases/m2b/while_int.pen
// Same algorithm as the M1 int_loop twin: a = a * 1664525 + 1013904223,
// wrapping two's-complement, 12,976,128 steps, same seed, same printed
// output. The cstar side is now a REAL while loop (Stage 2B) instead of
// the M1 K x N call chain, so this pair measures the loop-construct gain.
// int64 wraparound is computed through uint64_t (defined behavior,
// identical to the cstar runtime's cs_wrap_*).
#include <cstdint>
#include <cstdio>

int main(int argc, char**) {
    uint64_t a = 22695477u;
    // Trip count kept opaque (argc) so the optimizer cannot fold the loop.
    const uint64_t iters = 12976128ull + (argc > 1000 ? 1u : 0u);
    for (uint64_t i = 0; i < iters; i++) {
        a = a * 1664525u + 1013904223u;
    }
    std::printf("%lld\n", static_cast<long long>(static_cast<int64_t>(a)));
    return 0;
}
