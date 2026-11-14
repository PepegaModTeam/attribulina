#include "LogFile.h"

#include <ctime>
#include <filesystem>

#ifdef _WIN32
#include <share.h>
#endif

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

		// Opened so that other programs can read the file but not write it.
		std::FILE* OpenForWriting(const fs::path& p, bool append)
		{
#ifdef _WIN32
			return _wfsopen(p.c_str(), append ? L"ab" : L"wb", _SH_DENYWR);
#else
			return std::fopen(p.c_str(), append ? "ab" : "wb");
#endif
		}

		// Another program is writing it: it can't be opened to write.
		bool InUse(const fs::path& p)
		{
			std::error_code ec;
			if (!fs::exists(p, ec)) return false;
			std::FILE* f = OpenForWriting(p, true);
			if (!f) return true;
			std::fclose(f);
			return false;
		}

		std::tm LocalTime()
		{
			const std::time_t now = std::time(nullptr);
			std::tm t{};
#ifdef _WIN32
			localtime_s(&t, &now);
#else
			localtime_r(&now, &t);
#endif
			return t;
		}
	} // namespace

	std::string LocalTimestamp(bool forFileName)
	{
		const std::tm t = LocalTime();
		char buf[32];
		std::strftime(buf, sizeof(buf), forFileName ? "%Y-%m-%d_%H-%M-%S" : "%Y-%m-%d %H:%M:%S", &t);
		return buf;
	}

	LogFile::~LogFile()
	{
		close();
	}

	void LogFile::close()
	{
		if (file_) std::fclose(file_);
		file_ = nullptr;
	}

	bool LogFile::open(const std::string& path, bool keepPrevious)
	{
		close();
		const fs::path want = FromUtf8(path);
		std::error_code ec;
		if (want.has_parent_path() && !fs::exists(want.parent_path(), ec)) fs::create_directories(want.parent_path(), ec);
		for (int n = 1; n <= 9; ++n)
		{
			fs::path p = want;
			if (n > 1) p.replace_filename(ToUtf8(want.stem()) + " (" + std::to_string(n) + ")" + ToUtf8(want.extension()));
			if (InUse(p)) continue;
			if (keepPrevious && fs::exists(p, ec))
			{
				fs::path previous = p;
				previous.replace_filename(ToUtf8(p.stem()) + ".previous" + ToUtf8(p.extension()));
				fs::rename(p, previous, ec); // a previous one that can't go is written over
			}
			file_ = OpenForWriting(p, false);
			if (!file_) return false;
			path_ = ToUtf8(p);
			return true;
		}
		return false;
	}

	void LogFile::write(std::string_view level, std::string_view text)
	{
		if (!file_) return;
		const std::tm t = LocalTime();
		char time[16];
		std::strftime(time, sizeof(time), "%H:%M:%S ", &t);
		std::string line = time;
		if (!level.empty())
		{
			line += level;
			line += ": ";
		}
		line += text;
		writeRaw(line);
	}

	void LogFile::writeRaw(std::string_view text)
	{
		if (!file_) return;
		std::fwrite(text.data(), 1, text.size(), file_);
		std::fputc('\n', file_);
		std::fflush(file_);
	}

} // namespace attrib
