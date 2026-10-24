#include "Test.h"

#include <chrono>
#include <cstring>
#include <exception>

namespace test {

std::string& Root() {
    static std::string root = ".";
    return root;
}

std::string DataPath(const std::string& relative) { return Root() + "/" + relative; }

} // namespace test

// Usage: attribtests [--root <repo root>] [name filter]
int main(int argc, char** argv) {
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--root") == 0 && i + 1 < argc)
            test::Root() = argv[++i];
        else
            filter = argv[i];
    }

    int passed = 0, failed = 0;
    for (const auto& c : test::Registry()) {
        if (filter && !std::strstr(c.name, filter))
            continue;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  ok    %s (%lld ms)\n", c.name, (long long)ms);
            ++passed;
        } catch (const test::Failure& f) {
            std::printf("  FAIL  %s\n        %s\n", c.name, f.message.c_str());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("  FAIL  %s\n        exception: %s\n", c.name, e.what());
            ++failed;
        }
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
