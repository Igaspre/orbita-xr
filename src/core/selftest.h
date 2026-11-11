#pragma once
#include <functional>
#include <string>

namespace ox {
// Platform independent tests. Used by tests/run_tests.cpp and by `OrbitaXR.exe --selftest`.
// Returns the number of failed checks.
int runSelfTest(const std::function<void(const std::string&)>& print);
}
