#include "MemoryFile.h"

#include "Hash.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
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

		constexpr size_t kHeaderSize = 16; // bTNode (8), Magic, NumFileEntries
		constexpr size_t kEntrySize = 20;  // Hash, Offset, FileSize, MemorySize, Data
		constexpr size_t kAlign = 16;

		std::optional<Endian> MagicOrder(const uint8_t* header)
		{
			if (load32(header + 8, Endian::Little) == kMemoryFileMagic) return Endian::Little;
			if (load32(header + 8, Endian::Big) == kMemoryFileMagic) return Endian::Big;
			return std::nullopt;
		}

		struct RawEntry
		{
			uint32_t hash, offset, fileSize, memorySize, data;
		};

		// A memory file's table, read without its copies.
		std::optional<std::vector<RawEntry>> ReadTable(const fs::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			uint8_t header[kHeaderSize];
			if (!in.read(reinterpret_cast<char*>(header), kHeaderSize)) return std::nullopt;
			const std::optional<Endian> e = MagicOrder(header);
			if (!e) return std::nullopt;
			const uint32_t count = load32(header + 12, *e);
			if (count > 0x100000) return std::nullopt;
			std::vector<uint8_t> table(size_t(count) * kEntrySize);
			if (!table.empty() && !in.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size()))) return std::nullopt;
			std::vector<RawEntry> out(count);
			for (uint32_t i = 0; i < count; ++i)
			{
				const uint8_t* p = table.data() + size_t(i) * kEntrySize;
				out[i] = { load32(p, *e), load32(p + 4, *e), load32(p + 8, *e), load32(p + 12, *e), load32(p + 16, *e) };
			}
			return out;
		}

		std::string Key(const fs::path& p)
		{
			std::error_code ec;
			fs::path a = fs::absolute(p, ec);
			if (ec) a = p;
			return ToUtf8(a.lexically_normal());
		}
	} // namespace

	MemoryFileEntry* MemoryFile::find(uint32_t hash)
	{
		for (MemoryFileEntry& e : entries)
			if (e.hash == hash) return &e;
		return nullptr;
	}

	const MemoryFileEntry* MemoryFile::find(uint32_t hash) const
	{
		return const_cast<MemoryFile*>(this)->find(hash);
	}

	std::optional<MemoryFile> ReadMemoryFile(ByteSpan data)
	{
		if (data.size() < kHeaderSize) return std::nullopt;
		const std::optional<Endian> e = MagicOrder(data.data());
		if (!e) return std::nullopt;
		ByteReader r(data, *e);
		MemoryFile out;
		out.endian = *e;
		out.node.assign(data.begin(), data.begin() + 8);
		const uint32_t count = r.u32(12);
		if (count > (data.size() - kHeaderSize) / kEntrySize)
			throw FormatError("memory file table of " + std::to_string(count) + " entries runs past the end of the file");
		out.entries.reserve(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			const size_t t = kHeaderSize + size_t(i) * kEntrySize;
			MemoryFileEntry entry;
			entry.hash = r.u32(t);
			const uint32_t offset = r.u32(t + 4);
			entry.fileSize = r.u32(t + 8);
			const uint32_t memorySize = r.u32(t + 12);
			entry.data = r.u32(t + 16);
			if (offset > data.size() || memorySize > data.size() - offset)
				throw FormatError("memory file entry " + std::to_string(i) + " runs past the end of the file");
			entry.bytes.assign(data.begin() + offset, data.begin() + offset + memorySize);
			out.entries.push_back(std::move(entry));
		}
		return out;
	}

	Bytes WriteMemoryFile(const MemoryFile& file)
	{
		ByteWriter w(file.endian);
		Bytes node = file.node;
		node.resize(8, 0);
		w.raw(node);
		w.u32(kMemoryFileMagic);
		w.u32(uint32_t(file.entries.size()));
		w.fill(file.entries.size() * kEntrySize);
		for (size_t i = 0; i < file.entries.size(); ++i)
		{
			const MemoryFileEntry& e = file.entries[i];
			// Every copy starts at a 16-byte boundary, the last one included; the
			// file ends where the last copy does.
			w.align(kAlign);
			const size_t t = kHeaderSize + i * kEntrySize;
			w.put32(t, e.hash);
			w.put32(t + 4, uint32_t(w.size()));
			w.put32(t + 8, e.fileSize);
			w.put32(t + 12, uint32_t(e.bytes.size()));
			w.put32(t + 16, e.data);
			w.raw(e.bytes);
		}
		return std::move(w.bytes());
	}

	uint32_t GameFileHash(std::string_view gamePath)
	{
		std::string s(gamePath);
		for (char& c : s)
		{
			if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
			else if (c == '/') c = '\\';
		}
		return BinHash(s);
	}

	std::optional<std::string> GamePath(const std::string& file, const std::string& memoryFile)
	{
		std::error_code ec;
		fs::path f = fs::absolute(FromUtf8(file), ec);
		if (ec) return std::nullopt;
		fs::path dir = fs::absolute(FromUtf8(memoryFile), ec).parent_path();
		if (ec) return std::nullopt;
		const fs::path rel = f.lexically_normal().lexically_relative(dir.lexically_normal());
		if (rel.empty()) return std::nullopt;
		std::vector<std::string> parts;
		for (const fs::path& part : rel) parts.push_back(ToUtf8(part));
		std::string out;
		size_t first = 0;
		if (parts[0] == "..")
		{
			// Outside Global: the game root is the folder above it.
			if (parts.size() < 2 || parts[1] == "..") return std::nullopt;
			first = 1;
		}
		else
		{
			out = "GLOBAL";
		}
		for (size_t i = first; i < parts.size(); ++i)
		{
			if (parts[i] == "." || parts[i].empty()) continue;
			if (parts[i] == "..") return std::nullopt;
			if (!out.empty()) out += '\\';
			out += parts[i];
		}
		for (char& c : out)
			if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
		return out;
	}

	std::vector<std::string> MemoryFilesIn(const std::string& dir)
	{
		std::vector<std::string> out;
		std::error_code ec;
		const fs::path d = FromUtf8(dir.empty() ? std::string(".") : dir);
		for (const auto& entry : fs::directory_iterator(d, ec))
		{
			if (!entry.is_regular_file(ec)) continue;
			const std::string name = Lower(ToUtf8(entry.path().filename()));
			if (!name.ends_with("memoryfile.bin")) continue;
			if (ReadTable(entry.path())) out.push_back(ToUtf8(entry.path()));
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	std::vector<std::string> MemoryFilesFor(const std::string& file)
	{
		std::vector<std::string> out;
		std::set<std::string> seen;
		std::error_code ec;
		fs::path dir = fs::absolute(FromUtf8(file), ec).parent_path();
		if (ec) return out;
		for (int up = 0; up < 2 && !dir.empty(); ++up)
		{
			for (const std::string& m : MemoryFilesIn(ToUtf8(dir)))
				if (seen.insert(Key(FromUtf8(m))).second) out.push_back(m);
			if (dir == dir.parent_path()) break;
			dir = dir.parent_path();
		}
		return out;
	}

	std::vector<MemoryFileUpdate> PlanMemoryFileUpdates(const std::vector<SavedFile>& saved,
		const std::vector<std::string>& removed)
	{
		// Every memory file beside one of the files, once, in the order found.
		std::vector<std::string> memoryFiles;
		std::set<std::string> seen;
		auto gather = [&](const std::string& file)
		{
			for (const std::string& m : MemoryFilesFor(file))
				if (seen.insert(Key(FromUtf8(m))).second) memoryFiles.push_back(m);
		};
		for (const SavedFile& s : saved) gather(s.path);
		for (const std::string& r : removed) gather(r);

		std::vector<MemoryFileUpdate> out;
		for (const std::string& m : memoryFiles)
		{
			const std::optional<std::vector<RawEntry>> table = ReadTable(FromUtf8(m));
			if (!table) continue;
			auto listed = [&](const std::string& file) -> std::optional<std::pair<std::string, uint32_t>>
			{
				const std::optional<std::string> gp = GamePath(file, m);
				if (!gp) return std::nullopt;
				const uint32_t h = GameFileHash(*gp);
				if (std::none_of(table->begin(), table->end(), [&](const RawEntry& e) { return e.hash == h; })) return std::nullopt;
				return std::make_pair(*gp, h);
			};
			// Read the whole file only when its table lists one of them.
			std::vector<std::pair<const SavedFile*, std::pair<std::string, uint32_t>>> hits;
			std::vector<std::pair<std::string, uint32_t>> gone;
			for (const SavedFile& s : saved)
				if (auto l = listed(s.path)) hits.push_back({ &s, *l });
			for (const std::string& r : removed)
				if (auto l = listed(r)) gone.push_back(*l);
			if (hits.empty() && gone.empty()) continue;

			std::optional<MemoryFile> file = ReadMemoryFile(ReadFile(m));
			if (!file) continue;
			MemoryFileUpdate u;
			u.path = m;
			for (const auto& [s, l] : hits)
			{
				for (MemoryFileEntry& e : file->entries)
				{
					if (e.hash != l.second) continue;
					if (e.whole() && e.bytes == s->bytes) continue;
					if (!e.whole()) u.madeWhole.push_back(l.first);
					e.bytes = s->bytes;
					e.fileSize = uint32_t(s->bytes.size());
					u.updated.push_back(l.first);
				}
			}
			for (const auto& l : gone)
			{
				const size_t before = file->entries.size();
				std::erase_if(file->entries, [&](const MemoryFileEntry& e) { return e.hash == l.second; });
				if (file->entries.size() != before) u.dropped.push_back(l.first);
			}
			if (u.updated.empty() && u.dropped.empty()) continue;
			u.bytes = WriteMemoryFile(*file);
			out.push_back(std::move(u));
		}
		return out;
	}

	void SaveMemoryFileUpdate(const MemoryFileUpdate& update)
	{
		const fs::path target = FromUtf8(update.path);
		fs::path tmp = target;
		tmp += ".tmp";
		WriteFile(ToUtf8(tmp), update.bytes);
		std::error_code ec;
		fs::rename(tmp, target, ec);
		if (ec)
		{
			fs::remove(target, ec);
			fs::rename(tmp, target, ec);
			if (ec) throw std::runtime_error("could not replace " + update.path + ": " + ec.message());
		}
	}

	std::vector<MemoryFileCopy> MemoryFileCopiesOf(const std::string& file)
	{
		std::vector<MemoryFileCopy> out;
		std::optional<Bytes> current;
		for (const std::string& m : MemoryFilesFor(file))
		{
			const std::optional<std::string> gp = GamePath(file, m);
			if (!gp) continue;
			const std::optional<std::vector<RawEntry>> table = ReadTable(FromUtf8(m));
			if (!table) continue;
			const uint32_t h = GameFileHash(*gp);
			for (const RawEntry& e : *table)
			{
				if (e.hash != h) continue;
				MemoryFileCopy c;
				c.memoryFile = m;
				c.gamePath = *gp;
				c.size = e.memorySize;
				c.whole = e.memorySize == e.fileSize;
				try
				{
					if (!current) current = ReadFile(file);
				}
				catch (const std::exception&)
				{
					current = Bytes();
				}
				if (c.whole && e.fileSize == current->size())
				{
					std::ifstream in(FromUtf8(m), std::ios::binary);
					Bytes copy(e.memorySize);
					in.seekg(std::streamoff(e.offset));
					c.same = copy.empty() || (in.read(reinterpret_cast<char*>(copy.data()), std::streamsize(copy.size())) && copy == *current);
				}
				out.push_back(std::move(c));
			}
		}
		return out;
	}

} // namespace attrib
