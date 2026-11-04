// float_loop.cpp — C++ twin of bench/cases/float_loop.pen
// Same algorithm: x = x * 1.0000001 + 0.5 in double precision, 12967936
// steps (chain K=2048 x N=6336 on the Pentest side), seed 1.0, identical
// IEEE-754 op sequence, identical output bytes.
// The float formatting below is a line-for-line replica of the Pentest M1
// runtime's pt_fmt_float (docs/contracts/m1-semantics.md §4 shortest round-trip rule),
// so printed text matches byte-for-byte.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void fmt_float(double v, char* buf, size_t cap) {
    if (std::isfinite(v)) {
        static const int precisions[3] = {15, 16, 17};
        size_t i;
        for (i = 0; i < sizeof(precisions) / sizeof(precisions[0]); i++) {
            char* end = NULL;
            double back;
            std::snprintf(buf, cap, "%.*g", precisions[i], v);
            back = std::strtod(buf, &end);
            if (end != buf && *end == '\0' && back == v) break; /* round-trips */
        }
    } else {
        std::snprintf(buf, cap, "%.17g", v);
    }
    if (std::strpbrk(buf, ".en") != NULL) return;
    {
        size_t len = std::strlen(buf);
        if (len + 3 <= cap) {
            buf[len] = '.';
            buf[len + 1] = '0';
            buf[len + 2] = '\0';
        }
    }
}

int main(int argc, char**) {
    double x = 1.0;
    const uint64_t iters = 12976128ull + (argc > 1000 ? 1u : 0u);
    for (uint64_t i = 0; i < iters; i++) {
        x = x * 1.0000001 + 0.5;
    }
    char buf[64];
    fmt_float(x, buf, sizeof(buf));
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    return 0;
}
