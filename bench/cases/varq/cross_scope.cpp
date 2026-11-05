// cross_scope.cpp — C twin of bench/cases/varq/cross_scope.pen
//
// Same storage classes the cstar side lowers `variable.` access to
// (docs/reference/cross-scope-access.md §5): a plain module global, two program-lifetime
// statics standing in for the pinned bindings of counter()/feeder(), and
// plain functions reading/writing exactly those statics. Same seeds, same
// arithmetic (all operands positive, so `%` matches cstar exactly), same
// output — the pair isolates the codegen quality of cross-scope access.
#include <cstdint>
#include <cstdio>

static int64_t g_total = 0;      // variable.global (module storage)
static int64_t pin_base = 5;     // counter()'s pinned binding (variable.parent)
static int64_t pin_feeder_s = 0; // feeder()'s pinned binding (variable.feeder)

static int64_t tick(void) {
    int64_t total = 0;
    for (int64_t j = 0; j < 40000; j++) {
        total += pin_base;                 // variable.parent base
    }
    return total;
}

static int64_t feeder(int64_t seed) {
    pin_feeder_s = seed;
    for (int64_t i = 0; i < 40000; i++) {
        pin_feeder_s = (pin_feeder_s * 214013 + 2531011) % 1073741823;
        g_total += pin_feeder_s % 16;      // variable.global g_total += ...
    }
    return pin_feeder_s;
}

static void drive(int64_t n) {
    for (int64_t k = 0; k < n; k++) {
        pin_feeder_s = (pin_feeder_s * 48271) % 2147483647;
        g_total += pin_feeder_s % 4;
    }
}

int main(void) {
    int64_t t = tick();
    int64_t x = feeder(7);
    drive(40000);
    std::printf("%lld\n%lld\n", static_cast<long long>(g_total),
                static_cast<long long>(x + pin_feeder_s + t));
    return 0;
}