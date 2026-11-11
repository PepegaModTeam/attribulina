#pragma once

// Which files of a game folder hold its database, in the order they load.
// The editor's File > Open Folder and attribcli both use it.

#include <optional>
#include <string>
#include <vector>

namespace attrib
{

	struct FolderFile
	{
		std::string path;
		std::string skipped; // why it isn't loaded; empty: load it
		// Where it is saved once loaded, as one of World's event vault files:
		// OGVI's VPAK save of an event vault, in the folder above gc.vaults
		// (vaultlib::MoveToEventVaultFile). Empty: where it is.
		std::string saveAs;
	};

	// attributes.bin first (it defines the classes), then the other packs by
	// name. A .bin is skipped when its .lzc twin is there (the .lzc is the file
	// the game loads; the .bin holds the same vaults), and so is any .bin or
	// .lzc that isn't an attribute pack (GLOBALB.LZC is a compressed bundle).
	// World's event vaults follow, from the gc.vaults folder inside it (only
	// the files directly in it). A pack VaultLib wrote beside attributes.bin
	// that has the name of one of them is OGVI's save of that event vault: it
	// loads, with saveAs, and the gc.vaults copy is skipped. Throws when the
	// folder has no attributes.bin.
	std::vector<FolderFile> FolderPacks(const std::string& dir);

	// The attributes.bin that defines the classes of the pack at `pack`: the one
	// beside it, or for one of World's event vaults (gc.vaults\x.bin), the one
	// in the folder above.
	std::optional<std::string> DatabaseFileFor(const std::string& pack);

	// Where World's event vault file `file` ("x.bin") goes for the database
	// whose attributes.bin, or one of whose event vaults, is at `pack`: in the
	// gc.vaults folder beside attributes.bin (as it is spelled there, when it
	// is), or beside the event vault.
	std::string EventVaultPath(const std::string& pack, const std::string& file);

	// A VPAK, a compressed one, or one of World's event vaults.
	bool LooksLikePack(const std::string& path);

	// `name` in `dir`, matched case-insensitively.
	std::optional<std::string> FindFileNoCase(const std::string& dir, const std::string& name);

} // namespace attrib
