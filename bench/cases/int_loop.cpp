// int_loop.cpp — C++ twin of bench/cases/int_loop.pen
// Same algorithm: a = a * 1664525 + 1013904223, wrapping two's-complement,
// 12967936 steps (chain K=2048 x N=6336 on the Pentest side), same seed,
// same printed output.
// int64 wraparound is computed through uint64_t (defined behavior, identical
// to the Pentest runtime's pt_wrap_*).
#include <cstdint>
#include <cstdio>

int main(int argc, char**) {
    uint64_t a = 22695477u;
    // The trip count is kept opaque (argc) so the optimizer cannot fold the
    // loop; the Pentest side cannot fold anything either (external pt_* calls).
    const uint64_t iters = 12976128ull + (argc > 1000 ? 1u : 0u);
    for (uint64_t i = 0; i < iters; i++) {
        a = a * 1664525u + 1013904223u;
    }
    std::printf("%lld\n", static_cast<long long>(static_cast<int64_t>(a)));
    return 0;
}
