// call_chain.cpp — C++ twin of bench/cases/call_chain.pen
// Same per-frame algorithm, 2400256 frames (chain K=1024 x N=2344 on the
// Pentest side):
//   b = a*3+17; c = b % 1000003; d = c < 1000000007; e = d == True;
//   f = b != c; g = e != f; h = a - b; a = h*5 + c
// plus a final comparison z = a < 1000000007.  Signed int64 semantics with
// wrapping computed via uint64_t (identical to Pentest: % and < see signed
// values, wraparound via two's complement).  Note: M1 has no branching; the
// comparisons are straight-line expressions and the per-frame bools cannot
// influence ints (no bool->int), so on the C++ side the optimizer legally
// deletes the dead bool chain while Pentest must execute it — that asymmetry
// is part of what this case measures.
#include <cstdint>
#include <cstdio>

int main(int argc, char**) {
    int64_t a = 22695477;
    const uint64_t frames = 2400256ull + (argc > 1000 ? 1u : 0u);
    for (uint64_t i = 0; i < frames; i++) {
        uint64_t b = static_cast<uint64_t>(a) * 3u + 17u;      // wraps
        int64_t bs = static_cast<int64_t>(b);
        int64_t c = bs % 1000003;                               // signed %
        bool d = c < 1000000007;                                // signed <
        bool e = (d == true);
        bool f = (bs != c);
        bool g = (e != f);
        (void)g;
        int64_t h = static_cast<int64_t>(static_cast<uint64_t>(a) - b); // wraps
        a = static_cast<int64_t>(static_cast<uint64_t>(h) * 5u +
                                 static_cast<uint64_t>(c));            // wraps
    }
    bool z = (a < 1000000007);
    std::printf("%lld %s\n", static_cast<long long>(a), z ? "True" : "False");
    return 0;
}
