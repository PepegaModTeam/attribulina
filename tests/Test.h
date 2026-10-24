#pragma once

// Minimal test harness: TEST(name) { CHECK(...); } registers a test, and
// tests/test_main.cpp runs them (optionally filtered by a substring).

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

struct Failure {
    std::string message;
};

// Repo root, so tests can find attrib/databases (set by main from --root).
std::string& Root();
std::string DataPath(const std::string& relative);

} // namespace test

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                                   \
    static void TEST_CONCAT(test_fn_, __LINE__)();                                   \
    static test::Registrar TEST_CONCAT(test_reg_, __LINE__)(name, TEST_CONCAT(test_fn_, __LINE__)); \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::ostringstream os_;                                                  \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed";       \
            throw test::Failure{os_.str()};                                          \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto& a_ = (a);                                                        \
        const auto& b_ = (b);                                                        \
        if (!(a_ == b_)) {                                                           \
            std::ostringstream os_;                                                  \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b ") failed: " \
                << a_ << " != " << b_;                                               \
            throw test::Failure{os_.str()};                                          \
        }                                                                            \
    } while (0)
