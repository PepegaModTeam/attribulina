// attribcli's entry point: arguments in UTF-8 (wmain on Windows), then Cli.cpp.

#include "Cli.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string ExeDir(const char* argv0) {
    namespace fs = std::filesystem;
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n && n < std::size(buf)) {
        const std::u8string u = fs::path(std::wstring(buf, n)).parent_path().u8string();
        return std::string(u.begin(), u.end());
    }
#else
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return self.parent_path().string();
#endif
    return argv0 ? fs::path(argv0).parent_path().string() : std::string();
}

int Main(const std::vector<std::string>& args, const char* argv0) {
    std::ios::sync_with_stdio(false);
    cli::Env env;
    env.exeDir = ExeDir(argv0);
    env.in = &std::cin;
    const int code = cli::Run(args, std::cout, std::cerr, env);
    std::cout.flush();
    return code;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    // UTF-8 output, so names and paths print as they are.
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::u8string u = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(u.begin(), u.end());
    }
    return Main(args, nullptr);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return Main(args, argv[0]);
}
#endif
