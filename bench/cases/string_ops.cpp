// string_ops.cpp — C++ twin of bench/cases/string_ops.pen
// Same algorithm: per chain, the K=125-position string chain consists of 25
// periods of 4 appends of the 16-byte literal "0123456789ABCDEF" (appends
// accumulate on the unwind) with a reset to the 8-byte seed "seed0000" after
// every period except the last, so the final string is 72 bytes; 6000 chains.
// Every append allocates la+lb+1, copies both halves and NUL-terminates: the
// exact allocation/copy pattern of the Pentest runtime's pt_concat, with the
// identical sequence of intermediate string lengths (8->24->40->56->72 per
// period; 100 concatenations per chain).  Difference: this twin frees the
// previous buffer; M1 has no GC and leaks every intermediate (~29 MB leaked
// by this program; documented finding).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char**) {
    static const char lit[] = "0123456789ABCDEF";
    static const char seed[] = "seed0000";
    const size_t litlen = sizeof(lit) - 1;   // 16
    const size_t seedlen = sizeof(seed) - 1; // 8
    const uint64_t chains = 6000ull + (argc > 1000 ? 1u : 0u);
    const uint64_t periods = 25;

    char* s = static_cast<char*>(std::malloc(seedlen + 1));
    std::memcpy(s, seed, seedlen + 1);
    size_t len = seedlen;

    for (uint64_t n = 0; n < chains; n++) {
        std::free(s); // reset before every chain (top-level re-assignment)
        s = static_cast<char*>(std::malloc(seedlen + 1));
        std::memcpy(s, seed, seedlen + 1);
        len = seedlen;
        for (uint64_t p = 0; p < periods; p++) {
            for (int j = 0; j < 4; j++) { // one period: 4 appends
                char* ns = static_cast<char*>(std::malloc(len + litlen + 1));
                std::memcpy(ns, s, len);
                std::memcpy(ns + len, lit, litlen);
                ns[len + litlen] = '\0';
                std::free(s);
                s = ns;
                len += litlen;
            }
            if (p + 1 < periods) { // chain reset frame (i%5==0, except top)
                std::free(s);
                s = static_cast<char*>(std::malloc(seedlen + 1));
                std::memcpy(s, seed, seedlen + 1);
                len = seedlen;
            }
        }
    }
    std::fwrite(s, 1, len, stdout);
    std::fputc('\n', stdout);
    std::free(s);
    return 0;
}
