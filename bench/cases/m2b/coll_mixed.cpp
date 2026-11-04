// coll_mixed.cpp — C++ twin of bench/cases/m2b/coll_mixed.pen
// Stage-2B-only workload (M1's mixed twin had no collections): 202,752
// reps of (LCG step + list append + map write + text append with a
// period-80 reset). All four value types stay live through the dynamic
// runtime. The twin models list -> vector, map -> vector (insertion order
// equals ascending i here, so keys() iteration matches), text -> std::string.
// total == msum by construction (the same value lands in both).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char**) {
    const uint64_t n = 202752ull + (argc > 1000 ? 1u : 0u);
    uint64_t a = 22695477u;
    std::vector<uint64_t> xs;
    std::vector<uint64_t> mv;
    xs.reserve(static_cast<size_t>(n));
    mv.reserve(static_cast<size_t>(n));
    std::string s = "seed0000";
    for (uint64_t i = 0; i < n; i++) {
        a = a * 1664525u + 1013904223u;
        xs.push_back(a);
        mv.push_back(a);
        if (i % 80 == 0) s = "seed0000";
        s += "0123456789ABCDEF";
    }
    uint64_t total = 0;
    for (size_t i = 0; i < xs.size(); i++) total = total + xs[i];
    uint64_t msum = 0;
    for (size_t i = 0; i < mv.size(); i++) msum = msum + mv[i];
    std::printf("%lld %zu %zu %lld %lld %zu\n",
                static_cast<long long>(static_cast<int64_t>(a)), xs.size(),
                mv.size(), static_cast<long long>(static_cast<int64_t>(total)),
                static_cast<long long>(static_cast<int64_t>(msum)), s.size());
    return 0;
}
