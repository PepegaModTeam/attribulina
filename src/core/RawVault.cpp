#include "RawVault.h"

#include "Hash.h"

namespace attrib
{

	namespace
	{
		constexpr uint32_t kVers = FourCC("Vers");
		constexpr uint32_t kDepN = FourCC("DepN");
		constexpr uint32_t kStrN = FourCC("StrN");
		constexpr uint32_t kDatN = FourCC("DatN");
		constexpr uint32_t kExpN = FourCC("ExpN");
		constexpr uint32_t kPtrN = FourCC("PtrN");
		constexpr uint32_t kStrE = FourCC("StrE");
		constexpr uint32_t kEndC = FourCC("EndC");
		constexpr uint32_t kSign = FourCC("Sign");

		// From the first chunk's id: Vers, or DepN in Most Wanted's vaults (which have no Vers).
		Endian DetectEndian(ByteSpan vlt)
		{
			if (vlt.size() < 4)
				throw FormatError("vlt is too small");
			for (Endian e : {Endian::Little, Endian::Big})
				if (load32(vlt.data(), e) == kVers || load32(vlt.data(), e) == kDepN) return e;
			throw FormatError("vlt starts with neither a Vers nor a DepN chunk");
		}
	} // namespace

	const uint32_t* RawVault::vltPtr(uint32_t off) const
	{
		auto it = vltFixups.find(off);
		return it == vltFixups.end() ? nullptr : &it->second;
	}

	const uint32_t* RawVault::binPtr(uint32_t off) const
	{
		auto it = binFixups.find(off);
		return it == binFixups.end() ? nullptr : &it->second;
	}

	RawVault ParseRawVault(std::string name, Bytes bin, Bytes vlt)
	{
		RawVault v;
		v.name = std::move(name);
		v.endian = DetectEndian(vlt);
		v.vlt = std::move(vlt);
		v.bin = std::move(bin);
		const ByteReader r = v.vltReader();

		// vlt chunks
		for (size_t off = 0; off + 8 <= v.vlt.size();)
		{
			RawChunk c{ r.u32(off), uint32_t(off), r.u32(off + 4) };
			if (c.size < 8)
				break; // zero padding after the last chunk
			r.require(off, c.size);
			v.vltChunks.push_back(c);
			off += c.size;
		}

		bool haveExp = false, havePtr = false, haveDep = false;
		for (const RawChunk& c : v.vltChunks)
		{
			if (c.id == kDepN) haveDep = true;
			if (c.id == kStrN && !haveDep) v.strNFirst = true;
			const uint32_t p = c.offset + 8, n = c.size - 8;
			if (c.id == kVers)
			{
				v.hasVers = true;
				v.version = n >= 8 ? r.u64(p) : 0;
			}
			else if (c.id == kDepN)
			{
				const uint32_t count = r.u32(p);
				// The early Most Wanted demos: a u32 0, then u64 ids, which hash
				// the names with StringHash64.
				if (count && n >= 8 + uint64_t(count) * 12 && r.u32(p + 4) == 0)
				{
					const uint32_t names = p + 8 + count * 12;
					if (names < c.offset + c.size && StringHash64(r.cstr(names + r.u32(p + 8 + count * 8))) == r.u64(p + 8)) v.keys64 = true;
				}
				const uint32_t idSize = v.keys64 ? 8 : 4, ids = p + (v.keys64 ? 8 : 4);
				const uint32_t names = ids + count * (idSize + 4);
				for (uint32_t i = 0; i < count; ++i)
				{
					Dependency d;
					d.id = v.keys64 ? r.u64(ids + i * 8) : r.u32(ids + i * 4);
					d.name = r.cstr(names + r.u32(ids + count * idSize + i * 4));
					v.dependencies.push_back(std::move(d));
				}
			}
			else if (c.id == kStrN)
			{
				auto s = r.slice(p, n);
				v.strN.assign(s.begin(), s.end());
			}
			else if (c.id == kEndC)
			{
				v.endC = true;
			}
			else if (c.id == kSign)
			{
				auto s = r.slice(p, n);
				v.sign.assign(s.begin(), s.end());
			}
			else if (c.id == kDatN)
			{
				v.datOffset = p;
				v.datSize = n;
			}
			else if (c.id == kExpN && v.keys64)
			{
				haveExp = true;
				const uint32_t count = r.u32(p);
				if (n < 8 + uint64_t(count) * 32) throw FormatError("the export table doesn't fit its chunk");
				for (uint32_t i = 0; i < count; ++i)
				{
					const uint32_t e = p + 8 + i * 32;
					ExportEntry x;
					x.id = r.u64(e);
					const uint64_t type = r.u64(e + 8);
					if (type == ExportType::DatabaseLoadData64) x.type = ExportType::DatabaseLoadData;
					else if (type == ExportType::ClassLoadData64) x.type = ExportType::ClassLoadData;
					else if (type == ExportType::CollectionLoadData64) x.type = ExportType::CollectionLoadData;
					else throw FormatError("unknown export type 0x" + ByteReader::Hex(size_t(type)));
					x.size = r.u32(e + 20);
					x.offset = r.u32(e + 24);
					if (uint64_t(x.offset) + x.size > v.vlt.size()) throw FormatError("an export lies past the vault's data");
					v.exports.push_back(x);
				}
				v.mostWantedLayout = true;
			}
			else if (c.id == kExpN)
			{
				haveExp = true;
				const uint32_t count = r.u32(p);
				// 16-byte entries, or Most Wanted's 20 with a name offset before
				// the size. Every export lies in DatN, which tells them apart, and
				// no export is empty: read as 16-byte entries, a 20-byte table's
				// first size is its name offset, 0. A Most Wanted vault holding
				// one collection whose export is larger than DatN's offset fit
				// the 16-byte reading otherwise (a mod's pursuit vaults, 100
				// bytes at 0x58).
				auto read = [&](uint32_t size)
				{
					v.exports.clear();
					if (n < 4 + uint64_t(count) * size) return false;
					for (uint32_t i = 0; i < count; ++i)
					{
						const uint32_t e = p + 4 + i * size;
						ExportEntry x{ r.u32(e), r.u32(e + 4), r.u32(e + size - 8), r.u32(e + size - 4) };
						if (x.size == 0) return false;
						if (size == 20 && r.u32(e + 8) != 0) return false; // a named export: never seen
						if (v.datSize ? x.offset < v.datOffset || uint64_t(x.offset) + x.size > uint64_t(v.datOffset) + v.datSize
							: uint64_t(x.offset) + x.size > v.vlt.size())
							return false;
						v.exports.push_back(x);
					}
					return true;
				};
				if (!read(16))
				{
					if (!read(20)) throw FormatError("the export table doesn't fit the vault's data");
					v.mostWantedLayout = true;
				}
			}
			else if (c.id == kPtrN)
			{
				havePtr = true;
				int block = -1;
				const uint32_t size = v.keys64 ? 16 : 12;
				for (uint32_t e = p; e + size <= p + n; e += size)
				{
					PtrRef ref{ r.u32(e), r.u16(e + 4), r.u16(e + 6), r.u32(e + 8) };
					if (v.keys64)
					{
						const uint64_t value = r.u64(e + 8);
						if (value >> 32) throw FormatError("a pointer fixup's value doesn't fit 32 bits");
						ref.value = uint32_t(value);
					}
					v.pointers.push_back(ref);
					if (ref.type == 2)
					{
						block = ref.index;
					}
					else if (ref.type == 3)
					{
						if (ref.index != 1)
							throw FormatError("pointer fixup targets block " + std::to_string(ref.index) +
											  "; only the bin (1) is supported");
						if (block == 0)
							v.vltFixups[ref.fixupOffset] = ref.value;
						else if (block == 1)
							v.binFixups[ref.fixupOffset] = ref.value;
						else
							throw FormatError("pointer fixup before any block selection");
					}
					else if (ref.type == 1)
					{
						if (block == 0) v.vltNulls.insert(ref.fixupOffset);
						else if (block == 1) v.binNulls.insert(ref.fixupOffset);
						else throw FormatError("pointer fixup before any block selection");
					}
					else if (ref.type != 0)
					{
						throw FormatError("unknown pointer fixup type " + std::to_string(ref.type));
					}
				}
			}
		}
		if (!haveExp || !havePtr || v.datSize == 0)
			throw FormatError("vault " + v.name + " is missing DatN, ExpN or PtrN");

		// bin: StrE chunk first
		const ByteReader br = v.binReader();
		if (v.bin.size() >= 8 && br.u32(0) == kStrE)
		{
			const uint32_t size = br.u32(4);
			br.require(0, size);
			v.strEOffset = 8;
			v.strESize = size - 8;
			// World: an EndC chunk before the data.
			if (v.endC && v.bin.size() >= size + 8u && br.u32(size) == kEndC) v.binEndCSize = br.u32(size + 4);
		}
		for (auto& [loc, target] : v.vltFixups)
		{
			r.require(loc, 4);
			if (target > v.bin.size())
				throw FormatError("pointer at vlt 0x" + ByteReader::Hex(loc) + " targets past the bin");
		}
		for (auto& [loc, target] : v.binFixups)
		{
			br.require(loc, 4);
			if (target > v.bin.size())
				throw FormatError("pointer at bin 0x" + ByteReader::Hex(loc) + " targets past the bin");
		}
		return v;
	}

} // namespace attrib
