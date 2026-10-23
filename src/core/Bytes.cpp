#include "Bytes.h"

#include <cstdio>
#include <memory>

namespace attrib
{

	std::string FourCCToString(uint32_t id)
	{
		std::string s(4, '?');
		for (int i = 0; i < 4; ++i)
		{
			char c = char((id >> (24 - 8 * i)) & 0xFF);
			s[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
		}
		return s;
	}

	std::string ByteReader::cstr(size_t off) const
	{
		require(off, 1);
		const uint8_t* begin = data_.data() + off;
		const void* end = std::memchr(begin, 0, data_.size() - off);
		if (!end)
			throw FormatError("unterminated string at 0x" + Hex(off));
		return std::string(reinterpret_cast<const char*>(begin), static_cast<const uint8_t*>(end) - begin);
	}

	std::string ByteReader::Hex(size_t v)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%zX", v);
		return buf;
	}

	namespace
	{
		struct FileCloser
		{
			void operator()(FILE* f) const
			{
				if (f) std::fclose(f);
			}
		};
		using FilePtr = std::unique_ptr<FILE, FileCloser>;

		FilePtr OpenFile(const std::string& path, const char* mode)
		{
#ifdef _WIN32
			// Paths are UTF-8 throughout the tool; convert for the wide CRT API.
			std::wstring wpath, wmode;
			for (size_t i = 0; i < path.size();)
			{
				uint32_t cp = uint8_t(path[i]);
				int extra = cp < 0x80 ? 0 : cp < 0xE0 ? 1 : cp < 0xF0 ? 2 : 3;
				cp &= extra == 0 ? 0x7F : extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07;
				for (int k = 1; k <= extra && i + k < path.size(); ++k)
					cp = (cp << 6) | (uint8_t(path[i + k]) & 0x3F);
				i += 1 + extra;
				if (cp >= 0x10000)
				{
					cp -= 0x10000;
					wpath.push_back(wchar_t(0xD800 + (cp >> 10)));
					wpath.push_back(wchar_t(0xDC00 + (cp & 0x3FF)));
				}
				else
				{
					wpath.push_back(wchar_t(cp));
				}
			}
			for (const char* m = mode; *m; ++m) wmode.push_back(wchar_t(*m));
			return FilePtr(_wfopen(wpath.c_str(), wmode.c_str()));
#else
			return FilePtr(std::fopen(path.c_str(), mode));
#endif
		}
	} // namespace

	Bytes ReadFile(const std::string& path)
	{
		FilePtr f = OpenFile(path, "rb");
		if (!f)
			throw std::runtime_error("cannot open " + path);
		Bytes data;
		uint8_t buf[1 << 16];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f.get())) > 0)
			data.insert(data.end(), buf, buf + n);
		if (std::ferror(f.get()))
			throw std::runtime_error("error reading " + path);
		return data;
	}

	void WriteFile(const std::string& path, ByteSpan data)
	{
		FilePtr f = OpenFile(path, "wb");
		if (!f)
			throw std::runtime_error("cannot create " + path);
		if (std::fwrite(data.data(), 1, data.size(), f.get()) != data.size())
			throw std::runtime_error("error writing " + path);
	}

} // namespace attrib
