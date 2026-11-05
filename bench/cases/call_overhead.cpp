// call_overhead.cpp — C++ twin of bench/cases/call_overhead.pen
// Same algorithm: 12967936 pass-through calls that return their argument
// unchanged (chain K=2048 x N=6336 on the Pentest side); result equals the
// seed.  The C++ chain is a single noinline passthrough called in a loop
// (same call count as the .pen K-deep chain repeated N times); the empty asm
// barrier is a zero-cost optimizer fence (the Pentest side is fenced by its
// external cs_guard calls) that stops the optimizer from hoisting/DCE-ing
// the pure call.
#include <cstdint>
#include <cstdio>

__attribute__((noinline)) static uint64_t passthrough(uint64_t x) {
    asm volatile("" ::: "memory");
    return x;
}

int main(int argc, char**) {
    uint64_t a = 22695477u;
    const uint64_t iters = 12976128ull + (argc > 1000 ? 1u : 0u);
    for (uint64_t i = 0; i < iters; i++) {
        a = passthrough(a);
    }
    std::printf("%lld\n", static_cast<long long>(static_cast<int64_t>(a)));
    return 0;
}
