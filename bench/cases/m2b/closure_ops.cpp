// closure_ops.cpp — C++ twin of bench/cases/m2b/closure_ops.pen
// Stage-2B-only workload (M1 has no function values/closures): 300,000
// iterations of (closure creation + two calls through the function value,
// one with a default arg and one with a named arg). On the Pentest side
// each call goes through pt_call/funmeta with a captured cell read; the
// twin models the same algorithm with a noinline captured struct call —
// C++ closures are pure compile-time constructs, so the gap below is the
// measured cost of the dynamic function-value machinery.
#include <cstdint>
#include <cstdio>

struct Adder {
    int64_t n;
    __attribute__((noinline)) int64_t call(int64_t x, int64_t by) const {
        return x + n + by;
    }
};

int main(int argc, char**) {
    const uint64_t iters = 300000ull + (argc > 1000 ? 1u : 0u);
    int64_t total = 0;
    for (uint64_t i = 0; i < iters; i++) {
        Adder f{static_cast<int64_t>(i)};
        total = total + f.call(static_cast<int64_t>(i), 1) +
                f.call(static_cast<int64_t>(i), 2);
    }
    std::printf("%lld\n", static_cast<long long>(total));
    return 0;
}
