#pragma once

// Serializes the model back to vaults and VPAK packs. An unmodified Pro Street
// PC pack is reproduced byte for byte; doc/vault-format.md lists the rules.

#include "Database.h"

namespace attrib
{

	struct VaultBlobs
	{
		Bytes bin;
		Bytes vlt;
	};

	VaultBlobs WriteVault(const Database& db, const Vault& vault);

	// The VPAK bytes (uncompressed).
	Bytes WritePack(const Database& db, const LoadedPack& pack);
	// What goes on disk: the pack, LZHeader-wrapped if it was loaded that way.
	Bytes WritePackFile(const Database& db, const LoadedPack& pack);

	// The file saved along with `path`: the .bin of a .lzc, always, and the .lzc
	// of a .bin when that file exists (found case-insensitively). Pro Street reads
	// both. GManager::LoadVaultPackImage loads GAMEPLAY.LZC for the vault
	// directory and the vaults that stay resident (gpcore), and vaults streamed in
	// later (race bins, track layouts) are read from GAMEPLAY.BIN at the offsets
	// the .lzc lists. A .bin left behind crashes the game in Attrib::Vault::Vault
	// as soon as an edit moves a vault.
	std::optional<std::string> PackTwin(const std::string& path);

	// Writes the pack to `path`, and to its PackTwin() (uncompressed for a .bin,
	// LZHeader-wrapped for a .lzc), then clears its dirty flag. Each file goes
	// through a .tmp first.
	void SavePack(const Database& db, LoadedPack& pack, const std::string& path);

	// The mTableKeyShift the writer uses for a collection: the stored one while
	// the entry keys are as loaded, otherwise the shift with the shortest worst
	// probe in the game's table (rotl(key, shift) % entryCount, linear probing).
	// In the early Most Wanted demos (64-bit keys) it writes 0 for a changed
	// collection, as their tool did (the game grows a table whose probes run long).
	uint32_t ChooseTableKeyShift(const Collection& c);
	// Whether a collection's entry keys are as loaded.
	bool SameEntryKeys(const Collection& c);

} // namespace attrib
