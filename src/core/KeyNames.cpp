#include "KeyNames.h"

#include "Bytes.h"
#include "Hash.h"

#include <cstdio>

namespace attrib
{

	KeyNames::KeyNames(HashFn hash) : hash_(hash)
	{
	}

	void KeyNames::add(std::string_view name)
	{
		if (name.empty())
			return;
		auto [it, added] = names_.try_emplace(hash_(name), name);
		if (added && indexed64_) index64_.try_emplace(StringHash64(it->second), &it->second);
	}

	const std::string* KeyNames::find64(uint64_t hash) const
	{
		if (!indexed64_)
		{
			index64_.reserve(names_.size());
			for (const auto& [h, n] : names_) index64_.try_emplace(StringHash64(n), &n);
			indexed64_ = true;
		}
		auto it = index64_.find(hash);
		return it == index64_.end() ? nullptr : it->second;
	}

	size_t KeyNames::loadFile(const std::string& path)
	{
		const Bytes data = ReadFile(path);
		size_t lines = 0, start = 0;
		for (size_t i = 0; i <= data.size(); ++i)
		{
			if (i == data.size() || data[i] == '\n')
			{
				size_t end = i;
				if (end > start && data[end - 1] == '\r')
					--end;
				if (end > start)
				{
					add(std::string_view(reinterpret_cast<const char*>(data.data()) + start, end - start));
					++lines;
				}
				start = i + 1;
			}
		}
		return lines;
	}

	const std::string* KeyNames::find(uint32_t hash) const
	{
		auto it = names_.find(hash);
		return it == names_.end() ? nullptr : &it->second;
	}

	std::string KeyNames::format(uint32_t hash) const
	{
		if (const std::string* n = find(hash))
			return *n;
		char buf[16];
		std::snprintf(buf, sizeof(buf), "0x%08X", hash);
		return buf;
	}

} // namespace attrib
