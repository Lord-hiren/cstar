// list_ops.cpp — C++ twin of bench/cases/m2b/list_ops.pen
// Stage-2B-only workload (M1 has no lists): 200,000 appends of i*3+1,
// one full sum pass (for-over-list), then len/index reads. The Pentest
// list holds tagged cs_value cells (16-byte value + cell overhead); the
// twin uses vector<int64_t>. Mutating methods return None on the Pentest
// side (no return-value dispatch modeled here).
#include <cstdint>
#include <cstdio>
#include <vector>

int main(int argc, char**) {
    std::vector<int64_t> xs;
    const uint64_t n = 200000ull + (argc > 1000 ? 1u : 0u);
    xs.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; i++) {
        xs.push_back(static_cast<int64_t>(i) * 3 + 1);
    }
    int64_t total = 0;
    for (size_t i = 0; i < xs.size(); i++) {
        total = total + xs[i];
    }
    std::printf("%zu %lld %lld %lld %lld\n", xs.size(),
                static_cast<long long>(xs.front()),
                static_cast<long long>(xs[n - 1]),
                static_cast<long long>(xs[xs.size() - 1]),
                static_cast<long long>(total));
    return 0;
}
