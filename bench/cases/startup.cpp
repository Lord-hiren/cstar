// startup.cpp — C++ twin of bench/cases/startup.pen
// Same program: process start + runtime init + one print("0") + teardown.
#include <cstdio>

int main(int, char**) {
    std::printf("0\n");
    return 0;
}
