// text_ops.cpp — C++ twin of bench/cases/m2b/text_ops.pen
// Stage-2B-only workload (M1 has no text methods): 200,000 passes of
// upper + split(" ") + "|".join + replace("a","@") + find("@") +
// starts_with over a 37-code-point ASCII text. All methods dispatch at
// runtime on the cstar side. upper/lower are ASCII-only on the cstar
// side (documented 2B limitation) and the text is pure ASCII, so
// std::toupper is byte-identical. replace is 1-char-for-1-char, so
// len(rep) == len(base) == 37.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static void ascii_upper_inplace(std::string& s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
}

int main(int argc, char**) {
    const std::string base = "Alpha,Beta,Gamma Deltax Epsilon,Filth";
    const uint64_t iters = 200000ull + (argc > 1000 ? 1u : 0u);
    int64_t acc = 0;
    for (uint64_t i = 0; i < iters; i++) {
        std::string u = base;
        ascii_upper_inplace(u);
        // split(" ")
        std::vector<std::string> parts;
        size_t start = 0;
        for (;;) {
            size_t sp = u.find(' ', start);
            if (sp == std::string::npos) {
                parts.push_back(u.substr(start));
                break;
            }
            parts.push_back(u.substr(start, sp - start));
            start = sp + 1;
        }
        // "|".join(parts)
        std::string joined;
        for (size_t k = 0; k < parts.size(); k++) {
            if (k) joined += '|';
            joined += parts[k];
        }
        // base.replace("a", "@") — non-overlapping, 1-char for 1-char
        std::string rep;
        for (char c : base) {
            rep += (c == 'a' ? '@' : c);
        }
        // rep.find("@")
        int64_t f = static_cast<int64_t>(rep.find('@'));
        if (base.rfind("Alpha", 0) == 0) {  // starts_with
            acc = acc + static_cast<int64_t>(joined.size()) + f +
                  static_cast<int64_t>(rep.size());
        }
    }
    std::printf("%lld %zu %lld\n", static_cast<long long>(acc), base.size(),
                static_cast<long long>(base.find("Gamma")));
    return 0;
}
