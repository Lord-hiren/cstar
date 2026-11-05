// set_ops.cpp — C++ twin of bench/cases/m2b/set_ops.pen
// Stage-2B-only workload (M1 has no sets): 120,000 iterations adding both
// i and i>>1 (so half the adds are duplicates), 120,000 membership tests,
// then drain via pop() summing the popped values. The cstar set pops in
// insertion order; the sum and count are order-independent, so a std::set
// (sorted) twin is byte-identical. Membership/add are O(1) hash on the
// cstar side vs O(log n) here — a deliberate, documented divergence that
// only understates the twin.
#include <cstdint>
#include <cstdio>
#include <set>

int main(int argc, char**) {
    std::set<int64_t> s;
    const uint64_t n = 120000ull + (argc > 1000 ? 1u : 0u);
    for (uint64_t i = 0; i < n; i++) {
        s.insert(static_cast<int64_t>(i));
        s.insert(static_cast<int64_t>(i >> 1));
    }
    uint64_t hits = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (s.find(static_cast<int64_t>(i)) != s.end()) hits++;
    }
    int64_t popped = 0;
    uint64_t drain = 0;
    while (!s.empty()) {
        popped = popped + *s.begin();
        s.erase(s.begin());
        drain++;
    }
    std::printf("%llu %llu %lld\n",
                static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(drain),
                static_cast<long long>(popped));
    return 0;
}
