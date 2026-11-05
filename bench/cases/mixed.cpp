// mixed.cpp — C++ twin of bench/cases/mixed.pen
// Same algorithm: 4000 repetitions of (125 LCG steps a = a*1664525+1013904223
// on an int64 accumulator) followed by (accumulator reset + the string_ops
// period-5 chain: 25 periods of 4 literal appends on the unwind, reset to the
// 8-byte seed after every period except the last, 100 concatenations with the
// cs_concat allocation pattern, final string 72 bytes).  Final line:
// "<int> <72-byte string>".  As in string_ops, the C++ twin frees previous
// buffers while M1 leaks them (~12 MB leaked by this program).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char**) {
    static const char lit[] = "0123456789ABCDEF";
    static const char seed[] = "seed0000";
    const size_t litlen = sizeof(lit) - 1;   // 16
    const size_t seedlen = sizeof(seed) - 1; // 8
    const uint64_t reps = 4000ull + (argc > 1000 ? 1u : 0u);
    const uint64_t kdepth = 125;
    const uint64_t periods = 25;

    uint64_t a = 22695477u;
    char* s = static_cast<char*>(std::malloc(seedlen + 1));
    std::memcpy(s, seed, seedlen + 1);
    size_t len = seedlen;

    for (uint64_t r = 0; r < reps; r++) {
        for (uint64_t k = 0; k < kdepth; k++) {
            a = a * 1664525u + 1013904223u;
        }
        std::free(s);
        s = static_cast<char*>(std::malloc(seedlen + 1));
        std::memcpy(s, seed, seedlen + 1);
        len = seedlen;
        for (uint64_t p = 0; p < periods; p++) {
            for (int j = 0; j < 4; j++) {
                char* ns = static_cast<char*>(std::malloc(len + litlen + 1));
                std::memcpy(ns, s, len);
                std::memcpy(ns + len, lit, litlen);
                ns[len + litlen] = '\0';
                std::free(s);
                s = ns;
                len += litlen;
            }
            if (p + 1 < periods) {
                std::free(s);
                s = static_cast<char*>(std::malloc(seedlen + 1));
                std::memcpy(s, seed, seedlen + 1);
                len = seedlen;
            }
        }
    }
    std::printf("%lld ", static_cast<long long>(static_cast<int64_t>(a)));
    std::fwrite(s, 1, len, stdout);
    std::fputc('\n', stdout);
    std::free(s);
    return 0;
}
