#pragma once

// A log written to a file line by line as it happens, so a crash still leaves
// it. The editor keeps attribulina.log in the user's folder (the last
// session's; the one before it as attribulina.previous.log), and the mod
// installer writes a timestamped log for each run.

#include <cstdio>
#include <string>
#include <string_view>

namespace attrib
{

	class LogFile
	{
	public:
		LogFile() = default;
		~LogFile();
		LogFile(const LogFile&) = delete;
		LogFile& operator=(const LogFile&) = delete;

		// Starts `path` afresh (UTF-8). With `keepPrevious`, a file already there
		// is renamed to <stem>.previous<ext> first. When another program is
		// writing the file (a second editor window), "<stem> (2)<ext>", "(3)" and
		// so on are tried in its place; that check works on Windows only, where
		// the file is opened so others can read it but not write it. False when
		// no file could be opened.
		bool open(const std::string& path, bool keepPrevious = false);
		void close();
		bool isOpen() const
		{
			return file_ != nullptr;
		}
		// The file being written, once open (it may be one of the numbered names).
		const std::string& path() const
		{
			return path_;
		}
		// One line with the time in front: "15:30:12 warning: text", or
		// "15:30:12 text" with no level. Written at once.
		void write(std::string_view level, std::string_view text);
		// A line as it is, with no time.
		void writeRaw(std::string_view text);

	private:
		std::FILE* file_ = nullptr;
		std::string path_;
	};

	// The local time: "2026-11-14 15:30:12", or for a file name "2026-11-14_15-30-12".
	std::string LocalTimestamp(bool forFileName = false);

} // namespace attrib
