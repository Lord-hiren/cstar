// map_ops.cpp — C++ twin of bench/cases/m2b/map_ops.pen
// Stage-2B-only workload (M1 has no maps): 150,000 inserts m[i] = i*2+7,
// 150,000 lookups + 150,000 get-with-default misses (absent key -> 0),
// one keys() sum pass. The Pentest map preserves insertion order
// (keys()/iteration); the sum is order-independent, so an unordered_map
// twin is byte-identical. Insert/lookup are hash-based on both sides.
#include <cstdint>
#include <cstdio>
#include <unordered_map>

int main(int argc, char**) {
    std::unordered_map<int64_t, int64_t> m;
    const uint64_t n = 150000ull + (argc > 1000 ? 1u : 0u);
    m.reserve(static_cast<size_t>(n) * 2);
    for (uint64_t i = 0; i < n; i++) {
        m[static_cast<int64_t>(i)] = static_cast<int64_t>(i) * 2 + 7;
    }
    int64_t total = 0;
    for (uint64_t i = 0; i < n; i++) {
        auto it = m.find(static_cast<int64_t>(i) + static_cast<int64_t>(n));
        total = total + m[static_cast<int64_t>(i)] + (it == m.end() ? 0 : it->second);
    }
    int64_t ksum = 0;
    for (const auto& kv : m) {
        ksum = ksum + kv.first;
    }
    std::printf("%zu %lld %lld %lld %lld\n", m.size(),
                static_cast<long long>(m[0]),
                static_cast<long long>(m[static_cast<int64_t>(n) - 1]),
                static_cast<long long>(total),
                static_cast<long long>(ksum));
    return 0;
}
