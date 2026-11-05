// while_string.cpp — C++ twin of bench/cases/m2b/while_string.pen
// Same algorithm as the M1 string_ops twin: 750,000 frames = 600,000
// concatenations of a 16-byte literal + 150,000 period-5 seed resets
// (reset does not append) — the identical intermediate-length sequence
// (8 -> 24 -> 40 -> 56 -> 72, peak 72), so the before/after ratio
// isolates loop-vs-chain cost. The cstar runtime leaks every
// intermediate concatenation (no GC until 2C); the twin's std::string
// reallocations reuse freed buffers, so the RSS gap is the honest
// measured cost of the leak policy.
#include <cstdint>
#include <cstdio>
#include <string>

int main(int argc, char**) {
    const char lit[] = "0123456789ABCDEF";
    std::string s = "seed0000";
    const uint64_t iters = 750000ull + (argc > 1000 ? 1u : 0u);
    for (uint64_t i = 0; i < iters; i++) {
        if (i % 5 == 0) {
            s = "seed0000";
        } else {
            s += lit;
        }
    }
    std::printf("%zu %c %c\n", s.size(), s[0], s[s.size() - 1]);
    return 0;
}
