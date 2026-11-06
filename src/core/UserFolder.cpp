#include "UserFolder.h"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib") // SHGetKnownFolderPath
#pragma comment(lib, "ole32.lib")   // CoTaskMemFree
#endif
#endif

namespace attrib
{

	namespace
	{
		namespace fs = std::filesystem;

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}

		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}
	} // namespace

	UserFolder ChooseUserFolder(const std::string& exeDir, const std::string& perUserDir)
	{
		std::error_code ec;
		if (fs::is_regular_file(FromUtf8(exeDir) / kSettingsFileName, ec) || perUserDir.empty()) return { exeDir, true };
		const fs::path dir = FromUtf8(perUserDir);
		fs::create_directories(dir, ec);
		if (!fs::is_directory(dir, ec)) return { exeDir, true };
		return { perUserDir, false };
	}

	std::string PerUserFolder()
	{
#ifdef _WIN32
		PWSTR roaming = nullptr;
		std::string out;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming)
			out = ToUtf8(fs::path(roaming) / L"Attribulina");
		CoTaskMemFree(roaming);
		return out;
#elif defined(__APPLE__)
		const char* home = std::getenv("HOME");
		return home && *home ? ToUtf8(FromUtf8(home) / "Library" / "Application Support" / "Attribulina") : std::string();
#else
		if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg == '/') return ToUtf8(FromUtf8(xdg) / "attribulina");
		const char* home = std::getenv("HOME");
		return home && *home ? ToUtf8(FromUtf8(home) / ".config" / "attribulina") : std::string();
#endif
	}

} // namespace attrib
