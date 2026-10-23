#pragma once

// One vault as stored: a .vlt blob (load data, exports, pointer fixups) and a
// .bin blob (strings and the data the load data points at).
//
// vlt chunks, each { u32 id; u32 size (including this 8-byte header) }:
//   Vers  u64 version (none in Most Wanted's own vaults, which start with DepN)
//   DepN  u32 count; u32 ids[count]; u32 nameOffsets[count]; char names[]
//         Two entries, the vault's own "<name>.vlt" and "<name>.bin"; ids are
//         StringHash32 of those names.
//   StrN  8 bytes, zero in all retail data
//   DatN  the export payloads (DatabaseLoadData, ClassLoadData, CollectionLoadData)
//   ExpN  u32 count; ExportEntry { u32 id; u32 type; u32 size; u32 offset (from vlt start) }[count]
//         (Most Wanted's layout: { u32 id; u32 type; u32 nameOffset (0); u32 size; u32 offset })
//   PtrN  PtrRef { u32 fixupOffset; u16 ptrType; u16 index; u32 value }[] :
//           type 2: subsequent fixup offsets are in block `index` (0 = vlt, 1 = bin)
//           type 3: *(block + fixupOffset) = blocks[index] + value (index is 1: every
//                   pointer targets the bin)
//           type 1: the pointer at fixupOffset stays null (Most Wanted's layout only)
//           type 0: terminator / padding
// bin: a StrE chunk (NUL-terminated strings) followed by raw data.
//
// World ends the vlt with an EndC chunk (8 zero bytes) and puts another after
// the bin's StrE, before the data (RawVault::endC). Its event vaults
// (gc.vaults, Pack.h) have a Sign chunk before the vlt's EndC: { u8 header[8];
// u8 signature[128] per dependency }. The game decrypts each signature to a
// 16-byte hash and compares the first with a hash of the vlt up to Sign, the
// others with the dependency's data (nfsw.exe 0x6CA140, 0x6C7490); a vault
// without Sign counts as matching, as the VPAKs' vaults do. The writer keeps
// it while the bin and the vlt before it are as read, and drops it from an
// edited vault, as the community Attribulator writes them.
//
// The early Most Wanted demos (RawVault::keys64) key everything with
// StringHash64: DepN { u32 count; u32 0; u64 ids[]; u32 nameOffsets[]; names },
// ExpN { u32 count; u32 0; { u64 id; u64 type; u32 0; u32 size; u32 offset;
// u32 0 }[] }, PtrN entries { u32 fixupOffset; u16 type; u16 index; u64 value },
// and a pointer group with no entries has no selection entry either.

#include "Bytes.h"

#include <unordered_map>
#include <unordered_set>

namespace attrib
{

	struct ExportEntry
	{
		uint64_t id = 0;   // StringHash32, or StringHash64 in keys64 vaults
		uint32_t type = 0; // an ExportType (a keys64 vault's StringHash64 types are read as these)
		uint32_t size = 0;
		uint32_t offset = 0; // from the start of the vlt
	};

	struct Dependency
	{
		uint64_t id = 0; // StringHash32 of the name, or StringHash64 in keys64 vaults
		std::string name;
	};

	struct PtrRef
	{
		uint32_t fixupOffset = 0;
		uint16_t type = 0;
		uint16_t index = 0;
		uint32_t value = 0;
	};

	namespace ExportType
	{
		// StringHash32 of the load-data type names.
		constexpr uint32_t DatabaseLoadData = 0xCBBC628Fu;   // "Attrib::DatabaseLoadData"
		constexpr uint32_t ClassLoadData = 0x5E970CBCu;      // "Attrib::ClassLoadData"
		constexpr uint32_t CollectionLoadData = 0x8E112EB7u; // "Attrib::CollectionLoadData"
		// World's event vaults end with an export "VaultData" of this type: 4 bytes,
		// the vault's slot (GameCore::VaultSlot: 0 an event, -1 gpcore.bin, the
		// core). The editor keeps it as read.
		constexpr uint32_t VaultData = 0x1C54EE91u; // "VaultDataType"
		// StringHash64 of the same names, in keys64 vaults.
		constexpr uint64_t DatabaseLoadData64 = 0x0B38846845E9C175ull;
		constexpr uint64_t ClassLoadData64 = 0x2A7895AC4A876152ull;
		constexpr uint64_t CollectionLoadData64 = 0xAD303B8F42B3307Eull;
	} // namespace ExportType

	struct RawChunk
	{
		uint32_t id = 0;
		uint32_t offset = 0; // of the chunk header
		uint32_t size = 0;   // including the header
	};

	struct RawVault
	{
		std::string name;
		Endian endian = Endian::Little;
		Bytes vlt, bin;

		std::vector<RawChunk> vltChunks;
		bool hasVers = false; // Most Wanted's vaults start with DepN
		bool strNFirst = false; // StrN before DepN (current-gen Undercover; the writer keeps the order)
		bool endC = false;      // EndC chunks after the bin's StrE and at the end of the vlt (World)
		Bytes sign;             // a Sign chunk's payload, as read (World's event vaults; empty: none)
		uint64_t version = 0;
		std::vector<Dependency> dependencies;
		Bytes strN;                   // StrN payload, preserved
		uint32_t datOffset = 0;       // DatN payload start in vlt
		uint32_t datSize = 0;
		std::vector<ExportEntry> exports;
		std::vector<PtrRef> pointers; // PtrN in file order
		// Most Wanted's vault layout, which two Carbon demos (PS2, a late 360 one)
		// use too: 20-byte export entries, its ClassLoadData (0x24 bytes, the key
		// shift and count as u32s, the static data at the end), every pointer
		// listed in PtrN even when null (type 1), and 0xEFFECADD in the vlt's
		// pointer slots. doc/vault-format.md has the rules.
		bool mostWantedLayout = false;
		// Most Wanted's own vaults (its layout, no Vers chunk) differ from the
		// demos' in two records: ClassLoadData has no static data (0x1c bytes),
		// and CollectionLoadData's type count is a u32 with the types unpadded.
		bool mostWantedOwn() const
		{
			return mostWantedLayout && !hasVers;
		}
		// The early Most Wanted demos: Most Wanted's own layout with StringHash64
		// keys (the chunks above, and wider keys in the load data). The model
		// keeps 32-bit keys (Database::keyFrom64).
		bool keys64 = false;
		uint32_t strEOffset = 0;      // StrE payload start in bin
		uint32_t strESize = 0;        // payload size
		uint32_t binEndCSize = 0;     // the EndC chunk after StrE (World), header included

		// Resolved pointer fixups: location -> bin offset of the target, per block.
		std::unordered_map<uint32_t, uint32_t> vltFixups;
		std::unordered_map<uint32_t, uint32_t> binFixups;
		// Pointers listed as null (type 1), by location.
		std::unordered_set<uint32_t> vltNulls, binNulls;

		ByteReader vltReader() const
		{
			return ByteReader(vlt, endian);
		}
		ByteReader binReader() const
		{
			return ByteReader(bin, endian);
		}

		bool hasVltPtr(uint32_t off) const
		{
			return vltFixups.count(off) != 0;
		}
		bool hasBinPtr(uint32_t off) const
		{
			return binFixups.count(off) != 0;
		}
		// Target of the pointer stored at `off` in the vlt/bin, if there is one.
		const uint32_t* vltPtr(uint32_t off) const;
		const uint32_t* binPtr(uint32_t off) const;

		bool inStrE(uint32_t binOffset) const
		{
			return binOffset >= strEOffset && binOffset < strEOffset + strESize;
		}
		uint32_t dataAreaStart() const
		{
			return strEOffset + strESize + binEndCSize;
		}
	};

	RawVault ParseRawVault(std::string name, Bytes bin, Bytes vlt);

} // namespace attrib
