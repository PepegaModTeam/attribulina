#pragma once

// Reverse lookup for hashed keys: StringHash32 (AttribSys keys) or bStringHash
// (type_bStringHash values). Names come from name lists (one per line:
// attrib_keys.txt, data/bin_keys.txt), from every string a loaded vault
// contains, and from names the user types in the editor.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace attrib
{

	class KeyNames
	{
	public:
		using HashFn = uint32_t(*)(std::string_view);
		explicit KeyNames(HashFn hash);
		// A copy builds its own 64-bit index (the index points into its names).
		KeyNames(const KeyNames& o) : hash_(o.hash_), names_(o.names_), fromData_(o.fromData_)
		{
		}
		KeyNames& operator=(const KeyNames& o)
		{
			if (this != &o)
			{
				hash_ = o.hash_;
				names_ = o.names_;
				fromData_ = o.fromData_;
				index64_.clear();
				indexed64_ = false;
			}
			return *this;
		}
		KeyNames(KeyNames&&) = default;
		KeyNames& operator=(KeyNames&&) = default;

		uint32_t hash(std::string_view name) const
		{
			return hash_(name);
		}
		// Adds `name` under its hash. First name for a hash wins.
		void add(std::string_view name);
		// A string the data holds (a vault's strings): it takes its hash from a
		// name added otherwise, and the first such string for a hash wins. Two
		// names can share a hash: the lists give Pro Street's
		// tracks/portland/portland_grip_1/checkpoint1 Carbon's
		// characters/major_crew_darius/alan, and the vault names it.
		void addFromData(std::string_view name);
		// Returns how many lines were read; throws if the file can't be opened.
		size_t loadFile(const std::string& path);

		const std::string* find(uint32_t hash) const;
		// The name, or "0x1234ABCD" when unknown.
		std::string format(uint32_t hash) const;
		size_t size() const
		{
			return names_.size();
		}
		// The name whose StringHash64 is `hash` (the early Most Wanted demos'
		// keys), from the same names. The index is built on first use.
		const std::string* find64(uint64_t hash) const;

	private:
		HashFn hash_;
		std::unordered_map<uint32_t, std::string> names_;
		std::unordered_set<uint32_t> fromData_; // hashes addFromData named
		mutable std::unordered_map<uint64_t, const std::string*> index64_;
		mutable bool indexed64_ = false;
	};

} // namespace attrib
