// g++ -std=c++17 -Isrc tests/run_tests.cpp src/core/*.cpp -o run_tests && ./run_tests
#include "core/selftest.h"
#include <cstdio>

int main() {
    return ox::runSelfTest([](const std::string& s) { std::puts(s.c_str()); }) == 0 ? 0 : 1;
}
