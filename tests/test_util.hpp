#pragma once

// Admission Fabric - minimal deterministic test harness.
//
// A tiny self-contained harness (no external dependency) so the repository
// builds and tests with only CMake + the standard library. Each test binary
// registers named cases; the process returns non-zero if any assertion failed.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace af_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> f) { registry().push_back({name, std::move(f)}); }
};

inline int& failure_count() { static int f = 0; return f; }
inline int& check_count() { static int c = 0; return c; }

inline void report_failure(const char* file, int line, const std::string& msg) {
    ++failure_count();
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
}

inline int run_all(const char* program) {
    std::printf("== %s: %zu registered test(s)\n", program, registry().size());
    for (auto& t : registry()) {
        std::printf("  [run] %s\n", t.name.c_str());
        t.fn();
    }
    std::printf("== %s: %d checks, %d failure(s)\n", program, check_count(), failure_count());
    return failure_count() == 0 ? 0 : 1;
}

} // namespace af_test

#define AF_TEST(name) \
    static void name(); \
    static ::af_test::Registrar af_reg_##name(#name, name); \
    static void name()

#define AF_CHECK(cond) \
    do { ::af_test::check_count()++; if (!(cond)) ::af_test::report_failure(__FILE__, __LINE__, #cond); } while (0)

#define AF_CHECK_MSG(cond, msg) \
    do { ::af_test::check_count()++; if (!(cond)) ::af_test::report_failure(__FILE__, __LINE__, std::string(#cond) + " -- " + (msg)); } while (0)

#define AF_TEST_MAIN(x) int main() { return ::af_test::run_all(x); }
