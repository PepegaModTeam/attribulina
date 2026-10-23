#pragma once

// VPAK: the container every attribute database file uses (attributes.bin,
// gameplay.bin, cars_vault.bin, fe_attrib.bin, ...). gameplay.lzc is the same
// pack wrapped in an LZHeader block.
//
//   AttribVaultPackHeader (0x10, little-endian on every platform):
//     char mMagic[4] "VPAK"; u32 mNumEntries; u32 mStringBlockOffset; u32 mStringBlockSize
//   AttribVaultPackEntry[mNumEntries] (0x14 each):
//     u32 mVaultNameOffset (into the string block); u32 mBinSize; u32 mVltSize;
//     u32 mBinOffset; u32 mVltOffset (both from the start of the pack)
//
// World's event vaults (GLOBAL\gc.vaults\<name>.bin, one vault each) have a
// header of their own instead, little-endian (PackFormat::VaultFile):
//   char name[0x2C] (the file's name, zero-padded); u32 binOffset (0x40);
//   u32 binSize; u32 vltOffset; u32 vltSize; u32 fileSize
// The bin, the vlt and the file's end are on 0x40 boundaries, zero-padded.

#include "Bytes.h"
#include "Compression.h"

#include <optional>

namespace attrib
{

	struct PackVault
	{
		std::string name;
		Bytes bin;
		Bytes vlt;
		uint32_t binOffset = 0; // where it was in the pack (diagnostics)
		uint32_t vltOffset = 0;
	};

	enum class PackFormat
	{
		Vpak, VaultFile
	};

	struct PackFile
	{
		PackFormat format = PackFormat::Vpak;
		std::vector<PackVault> vaults;
		std::optional<LzKind> compression; // set when the file was LZHeader-wrapped
		size_t fileSize = 0;
		// What the string block and each bin and vlt start on: 0x40, or 16 in
		// Most Wanted's PS2 beta (which pads its end to 16 too).
		uint32_t align = 0x40;
	};

	// Reads a pack from file bytes; unwraps an LZHeader block first if present.
	PackFile ReadPack(ByteSpan fileData);

	// The bytes start like a pack: VPAK, or World's event vault header.
	bool IsPackData(ByteSpan data);

	// World's event vault header (PackFormat::VaultFile), when `data` has one.
	struct VaultFileHeader
	{
		std::string name;
		uint32_t binOffset = 0, binSize = 0, vltOffset = 0, vltSize = 0, fileSize = 0;
	};
	std::optional<VaultFileHeader> ParseVaultFileHeader(ByteSpan data);

} // namespace attrib
