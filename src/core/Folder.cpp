#include "Folder.h"

#include "Bytes.h"
#include "Compression.h"
#include "Pack.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace attrib
{

	namespace fs = std::filesystem;

	namespace
	{
		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}
		std::string Lower(std::string s)
		{
			for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}
	} // namespace

	namespace
	{
		constexpr const char* kEventVaults = "gc.vaults";

		std::optional<std::string> FindDirNoCase(const std::string& dir, const std::string& name)
		{
			std::error_code ec;
			for (const auto& e : fs::directory_iterator(FromUtf8(dir), ec))
				if (e.is_directory(ec) && Lower(ToUtf8(e.path().filename())) == Lower(name)) return ToUtf8(e.path());
			return std::nullopt;
		}
	} // namespace

	std::optional<std::string> FindFileNoCase(const std::string& dir, const std::string& name)
	{
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(FromUtf8(dir), ec))
			if (e.is_regular_file(ec) && Lower(ToUtf8(e.path().filename())) == Lower(name)) return ToUtf8(e.path());
		return std::nullopt;
	}

	bool LooksLikePack(const std::string& path)
	{
		try
		{
			const Bytes b = ReadFile(path);
			if (b.size() < 4) return false;
			if (IsPackData(b)) return true;
			if (!ParseLzHeader(b)) return false;
			const Bytes u = Decompress(b);
			return u.size() >= 4 && std::memcmp(u.data(), "VPAK", 4) == 0;
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	std::vector<FolderFile> FolderPacks(const std::string& dir)
	{
		const std::optional<std::string> attrs = FindFileNoCase(dir, "attributes.bin");
		if (!attrs) throw std::runtime_error("no attributes.bin in " + dir);
		std::vector<FolderFile> out{ {*attrs, {}} };
		std::vector<fs::path> files;
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(FromUtf8(dir), ec))
		{
			if (!e.is_regular_file(ec)) continue;
			const std::string ext = Lower(ToUtf8(e.path().extension()));
			if ((ext == ".bin" || ext == ".lzc") && ToUtf8(e.path()) != *attrs) files.push_back(e.path());
		}
		std::sort(files.begin(), files.end());
		for (const fs::path& f : files)
		{
			if (Lower(ToUtf8(f.extension())) == ".bin")
			{
				fs::path lzc = f;
				lzc.replace_extension(".lzc");
				if (FindFileNoCase(dir, ToUtf8(lzc.filename())))
				{
					out.push_back({ ToUtf8(f), ToUtf8(lzc.filename()) + " holds the same vaults" });
					continue;
				}
			}
			if (!LooksLikePack(ToUtf8(f)))
			{
				out.push_back({ ToUtf8(f), "not an attribute pack" });
				continue;
			}
			out.push_back({ ToUtf8(f), {} });
		}
		// World's event vaults, in GLOBAL\gc.vaults (the folders under it hold
		// other files, such as a mod's choices of gpcore.bin).
		if (const std::optional<std::string> gc = FindDirNoCase(dir, kEventVaults))
		{
			std::vector<fs::path> vaults;
			for (const auto& e : fs::directory_iterator(FromUtf8(*gc), ec))
				if (e.is_regular_file(ec) && Lower(ToUtf8(e.path().extension())) == ".bin") vaults.push_back(e.path());
			std::sort(vaults.begin(), vaults.end());
			for (const fs::path& f : vaults) out.push_back({ ToUtf8(f), LooksLikePack(ToUtf8(f)) ? std::string() : "not an attribute pack" });
		}
		return out;
	}

	std::optional<std::string> DatabaseFileFor(const std::string& pack)
	{
		const fs::path dir = FromUtf8(pack).parent_path();
		const std::string d = ToUtf8(dir.empty() ? fs::path(".") : dir);
		if (auto attrs = FindFileNoCase(d, "attributes.bin")) return attrs;
		if (Lower(ToUtf8(dir.filename())) == kEventVaults && dir.has_parent_path())
			return FindFileNoCase(ToUtf8(dir.parent_path()), "attributes.bin");
		return std::nullopt;
	}

} // namespace attrib
