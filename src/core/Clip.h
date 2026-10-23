#pragma once

// Collections on the clipboard. A copy holds each collection's own values
// with their nested data, keyed by class and field hashes and independent of
// the database it came from, so it pastes into the window it was copied in
// or into another window with another database of the same game.
//
// The clipboard text is a header line, '#' comment lines naming what was
// copied, and the payload in base64:
//
//   Attribulina collections 1
//   # pvehicle/911gt3 (and 2 under it)
//   QUNMUAEAAAA...
//
// Payload (little-endian): "ACLP", u32 version, u64 source window, u8 cut,
// u8 big-endian values, str origin, u32 class key, str class name, u32
// layout size, u32 count, then per collection u32 key, u64 export id, str
// name, i32 parent index, u32 original parent key, str vault, block layout,
// u32 values, and per value u32 field, u32 type hash, u32 type
// size, u8 flags (1 in layout, 2 array), u16 capacity, u16 encodedTypePad,
// u8 node flags, u8 entry flags, u32 count, u32 element size, block data.
// A block is u32 align, u32 size, bytes, u32 pointers, and per pointer u32
// offset, u8 kind (0 empty, 1 string + str, 2 block, 3 index of a block
// already written, so shared blocks stay shared). str is u32 size + bytes.

#include "Database.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace attrib
{

	struct ClipValue
	{
		uint32_t field = 0;
		uint32_t typeHash = 0;
		uint32_t typeSize = 0;
		bool inLayout = false;
		bool isArray = false;
		uint16_t capacity = 0;
		uint16_t encodedTypePad = 0;
		uint8_t nodeFlags = 0;
		uint8_t entryFlags = 0;
		uint32_t count = 1;
		uint32_t elemSize = 0;
		DataBlock data;
	};

	struct ClipCollection
	{
		uint32_t key = 0;
		uint64_t exportId = 0;  // the original's; kept when the key is, since it hashes the name
		std::string name;       // empty when the source's dictionary didn't know it
		int parent = -1;        // index in Clip::collections; -1 for a copied root
		uint32_t parentKey = 0; // the original's parent
		std::string vault;      // the vault the original was in
		DataBlock layout;
		std::vector<ClipValue> values;
	};

	struct Clip
	{
		uint64_t source = 0;    // the window that copied it: a cut only moves within it
		bool cut = false;
		Endian endian = Endian::Little; // of the values (the source vault's)
		std::string origin;     // where it came from, for messages (the database's folder)
		uint32_t classKey = 0;
		std::string className;
		uint32_t layoutSize = 0; // the class's, where it was copied: layouts are copied only between equal ones
		std::vector<ClipCollection> collections; // parents before their children
	};

	// Copies `roots` (all of one class) and, with `withChildren`, everything
	// under them. A root that is under another root is copied once, below it.
	Clip CopyCollections(const Database& db, const std::vector<const Collection*>& roots, bool withChildren);

	std::string EncodeClip(const Clip& clip);
	// nullopt for text that isn't a clip, or is damaged.
	std::optional<Clip> DecodeClip(std::string_view text);

	// A name for a copy of `name` that the class doesn't use yet: "name_copy",
	// then "name_copy2", ... A name that already ends in _copy or _copyN counts up
	// from there instead of growing another suffix.
	std::string CopyName(const Database& db, uint32_t classKey, const std::string& name);

	struct PasteResult
	{
		std::vector<Collection*> created; // in clip order: the roots first
		std::vector<std::string> warnings;
		std::vector<std::string> newNames; // names the dictionary didn't have (copy names, mostly)
	};

	// Creates the clip's collections in `cls` (the clip's class): the roots
	// under `parent` (null: root collections) in `vault`, the rest under the
	// copies of their parents. A collection keeps its name unless the class
	// already has it; then it gets CopyName(). Descendants go to the vault their
	// original was in when this database has a vault of that name and it isn't
	// the roots' vault; otherwise to `vault`. Values for fields the class lacks,
	// or has with another type or size, are left out with a warning. Throws when
	// the clip's values have the other byte order than `vault`.
	PasteResult PasteClip(Database& db, const Clip& clip, ClassDef& cls, Collection* parent, Vault& vault);

} // namespace attrib
