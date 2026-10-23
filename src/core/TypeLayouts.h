#pragma once

// Where pointers sit inside attribute value types, and what they point at.
// The writer needs the exact size of every block a pointer targets, so this
// table has to be complete: the reader warns about any pointer it doesn't
// explain. Verified against all 288 Pro Street PC vaults (every bin fixup is
// accounted for, and the files rebuild byte for byte). ATTRIB_POINTER_TRACE=1
// makes the reader print the type and offset of each pointer it can't explain.

#include "Bytes.h"

#include <string>
#include <string_view>
#include <vector>

namespace attrib
{

	enum class CountSource : uint8_t
	{
		U32, // u32 at countOffset in the element
		U16, // u16 at countOffset
		U8,  // u8 at countOffset
		One, // always one element
		// The sum of the second u32 of each 8-byte element of the block the
		// pointer at countOffset leads to (an index table's ISets { index, count }).
		PairSum,
	};

	struct NestedField
	{
		uint32_t offset;       // of the pointer in the element
		std::string elemType;
		uint32_t elemSize;
		uint32_t align;        // of the pointed-to block in the bin
		CountSource count;
		uint32_t countOffset;
		// The block holds an LZHeader block (compiled Lua) and runs to its end
		// when that is past the count. Retail Carbon's gameplay.bin has blobs
		// whose mSize is short of their data (244 of its 312); the game
		// decompresses by the header.
		bool toLzEnd = false;
	};

	struct PointerLayout
	{
		std::vector<uint32_t> strings;   // char* fields pointing into StrE
		std::vector<NestedField> nested; // pointers to further blocks
		// Pointers the files never fill in (RefSpec's Collection*, set when the
		// game loads) or that can't be sized. Only their places matter: Most
		// Wanted's layout lists each one as null.
		std::vector<uint32_t> unfollowed = {};
	};

	// The verified hand-written layout for a type of this size, or nullptr. Types
	// it doesn't list, or lists at another size (another platform's layout), get
	// theirs from the type schema (DerivePointerLayout).
	const PointerLayout* PointerLayoutFor(std::string_view typeName, uint32_t size);

	class TypeSchema;
	struct StructDef;
	// String pointers (cstr members) and counted pointer arrays (ptr members with
	// count=) of a schema struct, through nested structs and inline arrays.
	// Pointers without a count can't be sized and are left out.
	PointerLayout DerivePointerLayout(const StructDef& st, const TypeSchema& schema);

	// The count a NestedField reads from its element; PairSum needs the other
	// block, so the reader works it out (0 here).
	uint32_t NestedElementCount(const NestedField& f, const uint8_t* elem, Endian e);

	// Types fields use that no type table lists, which the reader knows by name.
	// Current-gen Undercover's aud_moment, speech, traffic_engine and
	// traffic_horn have a static IndexTable_CollectionName of this type (16
	// bytes: { u32 mCount; u32* mKeys; ISet* mIndices; u32* mVals }, the 360
	// debug build's PDB); doc/vault-format.md has what the tables hold.
	inline constexpr const char* kIndexTableType =
		"Attrib::Query::Static_Inorder_N_to_1<Attrib::Query::Typespace<Attrib::Key,Attrib::Key,EA::Reflection::UInt32> >";
	struct UnlistedType
	{
		const char* name;
		uint32_t size;
	};
	inline constexpr UnlistedType kUnlistedTypes[] = { {kIndexTableType, 16} };

} // namespace attrib
