// while_float.cpp — C++ twin of bench/cases/m2b/while_float.pen
// Same algorithm as the M1 float_loop twin: x = x * 1.0000001 + 0.5 in
// double precision, 12,976,128 steps, seed 1.0, identical IEEE-754 op
// sequence, identical output bytes. The Pentest side is a real while loop.
// fmt_float is a line-for-line replica of runtime/cstar_rt.c
// cs_fmt_float (shortest round-trip rule, unchanged from M1).
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
            if (end && *end == '\0' && back == v) {
                if (!std::strpbrk(buf, ".eEn")) {
                    size_t len = std::strlen(buf);
                    if (len + 2 < cap) {
                        buf[len] = '.';
                        buf[len + 1] = '0';
                        buf[len + 2] = '\0';
                    }
                }
                return;
            }
        }
        std::snprintf(buf, cap, "%.17g", v);
    } else if (std::isnan(v)) {
        std::snprintf(buf, cap, "nan");
    } else {
        std::snprintf(buf, cap, v < 0 ? "-inf" : "inf");
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
    std::printf("%s\n", buf);
    return 0;
}
