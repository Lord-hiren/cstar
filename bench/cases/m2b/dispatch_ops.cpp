// dispatch_ops.cpp — C++ twin of bench/cases/m2b/dispatch_ops.pen
// Stage-2B-only workload (M1 has no indirect calls): 300,000 calls of a
// 4-parameter function (a, b = 5, *rest, **kw) through a function value
// held in a map, each with one positional + two named args (b binds the
// defaulted param, extra lands in **kwargs). The Pentest side rebinds
// positionals/names and builds the kwarg map per call (cs_call); the twin
// models the same call shape through a function pointer fetched from a
// one-entry "map" — the measured gap is the binding machinery.
#include <cstdint>
#include <cstdio>

__attribute__((noinline)) static int64_t f_impl(int64_t a, int64_t b,
                                                int64_t extra) {
    // rest is empty in every call; kw holds exactly {extra: 3}.
    return a + b + extra;
}

int main(int argc, char**) {
    int64_t (*const fp)(int64_t, int64_t, int64_t) = f_impl;
    const uint64_t iters = 300000ull + (argc > 1000 ? 1u : 0u);
    int64_t total = 0;
    for (uint64_t i = 0; i < iters; i++) {
        // m["f"](i, b = 2, extra = 3)
        total = total + fp(static_cast<int64_t>(i), 2, 3);
    }
    std::printf("%lld\n", static_cast<long long>(total));
    return 0;
}
