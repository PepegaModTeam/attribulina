#pragma once

// The game's memory files: Global\GlobalMemoryFile.bin, InGameMemoryFile.bin,
// PermanentMemoryFile.bin, CarHeadersMemoryFile.bin and the like, each holding
// copies of other files that the game loads in one read. bFile looks every
// file it opens up in them first (FindMemoryFileEntry, by a hash of its path),
// takes the size the entry gives and reads from the copy, and never opens the
// loose file. So a pack saved while a memory file holds an older copy of it is
// a change the game never sees: every PC game's GlobalMemoryFile.bin holds
// GLOBAL\ATTRIBUTES.BIN, and Undercover's also CARS_VAULT.BIN and GAMEPLAY.LZC.
// doc/memory-files.md has the format and the code it was read from.

#include "Bytes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace attrib
{

	constexpr uint32_t kMemoryFileMagic = 0x53219999; // MEMORY_FILE_MAGIC

	struct MemoryFileEntry
	{
		uint32_t hash = 0;     // GameFileHash of the path the game opens the file by
		uint32_t fileSize = 0; // the file's size, as bFile gives it
		uint32_t data = 0;     // the Data pointer as stored: AddMemoryFile sets it again
		Bytes bytes;           // the file's first MemorySize bytes (usually all of it)

		bool whole() const
		{
			return bytes.size() == fileSize;
		}
	};

	struct MemoryFile
	{
		// The game reads the header and the table little-endian on the 360 too
		// (AddMemoryFile's lwbrx); a big-endian file is read and written as one.
		Endian endian = Endian::Little;
		Bytes node = Bytes(8, 0); // bTNode's links, zero in the files
		std::vector<MemoryFileEntry> entries;

		MemoryFileEntry* find(uint32_t hash);
		const MemoryFileEntry* find(uint32_t hash) const;
	};

	// nullopt when `data` isn't a memory file (no magic at +8). Throws
	// FormatError for one whose table or copies run past its end.
	std::optional<MemoryFile> ReadMemoryFile(ByteSpan data);
	// The header, the table, then each copy at the next 16-byte boundary in
	// table order, zero-padded, as every retail memory file is laid out.
	Bytes WriteMemoryFile(const MemoryFile& file);

	// bFileGetFilenameHash: bStringHash of the path with a-z upper-cased and
	// '/' as '\' ("GLOBAL\ATTRIBUTES.BIN" is 0x9775379A).
	uint32_t GameFileHash(std::string_view gamePath);

	// The path the game opens `file` by, as a memory file at `memoryFile` sees
	// it. The game loads memory files from its Global folder, whatever the
	// folder is called here, so a file beside it is "GLOBAL\ATTRIBUTES.BIN",
	// one in a folder under it "GLOBAL\GC.VAULTS\X.BIN" and one in a folder
	// beside it "FRONTEND\X.BIN". nullopt for a file anywhere else.
	std::optional<std::string> GamePath(const std::string& file, const std::string& memoryFile);

	// The memory files in `dir`: files named *MemoryFile.bin in any case that
	// open with the magic.
	std::vector<std::string> MemoryFilesIn(const std::string& dir);
	// Those a file at `file` may have a copy in: in its folder and the one
	// above (the Global folder of a file in gc.vaults).
	std::vector<std::string> MemoryFilesFor(const std::string& file);

	struct SavedFile
	{
		std::string path;
		Bytes bytes;
	};

	struct MemoryFileUpdate
	{
		std::string path; // the memory file
		Bytes bytes;      // what it becomes
		std::vector<std::string> updated;   // game paths whose copy now holds the saved bytes
		std::vector<std::string> madeWhole; // of those, copies that held only the file's start
		std::vector<std::string> dropped;   // copies of files that were removed
	};

	// What the memory files beside `saved` and `removed` files become. A copy
	// of a saved file takes its bytes whole (FileSize and MemorySize both the
	// new size); a copy of a removed file is dropped, so the game reads neither
	// stale. Every other copy keeps its bytes, sizes and order, moved to the
	// next 16-byte boundary after the one before it. Memory files that hold no
	// copy that changes are left out, so saving again writes nothing.
	std::vector<MemoryFileUpdate> PlanMemoryFileUpdates(const std::vector<SavedFile>& saved,
		const std::vector<std::string>& removed = {});

	// Writes `update` through a .tmp (ReplaceFile, Writer.h).
	void SaveMemoryFileUpdate(const MemoryFileUpdate& update);

	// Where the memory files beside `file` hold a copy of it, for the Log on
	// open: whether the copy is whole and the same as the file on disk (which
	// is read only when one does).
	struct MemoryFileCopy
	{
		std::string memoryFile;
		std::string gamePath;
		uint32_t size = 0; // MemorySize: how much of the file it holds
		bool same = false;
		bool whole = false;
	};
	std::vector<MemoryFileCopy> MemoryFileCopiesOf(const std::string& file);

} // namespace attrib
