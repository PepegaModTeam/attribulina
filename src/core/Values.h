#pragma once

// The type of one value inside an attribute (a whole element, a struct member,
// or an element a pointer member leads to), and small helpers to read and
// write values. Shared by the editor's inspector, Search and scripts.

#include "Database.h"

#include <string>
#include <vector>

namespace attrib
{

	struct ValueType
	{
		TypeKind kind = TypeKind::Unknown;
		uint32_t size = 0;
		const StructDef* st = nullptr;     // kind Struct
		const EnumDef* en = nullptr;       // kind Enum
		const MemberDef* member = nullptr; // when the value is a struct member
		std::string name;                  // field or member name
		std::string hint;                  // colour, key, binkey, hex
		std::string typeName;              // for the Type column
	};

	// The type of an attribute's elements (`fieldName` drives name-based choices
	// such as colours). A field the type schema gives an enum (FieldDef::fieldEnum)
	// is that enum.
	ValueType TypeOfAttribute(const Attribute& a, const std::string& fieldName);
	// The same for one value of `type`; `size` stands in for a type the table
	// doesn't list.
	ValueType TypeOfType(const TypeInfo* type, uint32_t size, const std::string& fieldName);
	// A class's static value of field `f`.
	ValueType TypeOfStatic(const FieldDef& f, const std::string& fieldName);
	// The type of one element of a struct member.
	ValueType TypeOfMember(const MemberDef& m);
	// The type of a nested array element, by its type name ("float", a struct).
	ValueType TypeOfNamed(const Database& db, const std::string& typeName);

	// Attrib::StringKey: { u32 StringHash32(text); char* text } (8 bytes), or Most
	// Wanted's { u64 StringHash64(text); u32 StringHash32(text); char* text } (16
	// bytes; Carbon's demos). An empty text is stored with hashes 0.
	struct StringKeyLayout
	{
		uint32_t hash32; // offset of the StringHash32
		uint32_t text;   // offset of the char*
		bool hash64;     // a StringHash64 at 0
	};
	inline StringKeyLayout StringKeyAt(uint32_t size)
	{
		return size == 16 ? StringKeyLayout{ 8, 12, true } : StringKeyLayout{ 0, 4, false };
	}

	// Attrib::RefSpec: { u32 class; u32 collection; Collection* } (12 bytes), or
	// the early Most Wanted demos' { u64 class; u64 collection; Collection*; u32 0 }
	// (24), whose keys are StringHash64.
	struct RefSpecLayout
	{
		uint32_t cls, col; // offsets of the two keys
		uint32_t keySize;  // 4, or 8
	};
	inline RefSpecLayout RefSpecAt(uint32_t size)
	{
		return size == 24 ? RefSpecLayout{ 0, 8, 8 } : RefSpecLayout{ 0, 4, 4 };
	}

	// The class a ClassRefSpec value's collection is of: StringHash32 of what
	// follows "ClassRefSpec_" in its type's name (the type's template argument in
	// the 360 debug build's PDB). 0 if the name has none.
	uint32_t ClassRefSpecClass(const std::string& typeName);

	// A key in a value, `size` bytes: a u32, or an early Most Wanted demo's u64
	// StringHash64, as the model's 32-bit key (Database::keyFrom64).
	uint32_t LoadKey(const Database& db, const uint8_t* p, uint32_t size, Endian e);
	// False (nothing written) when a u64 is wanted and the key has no name to hash.
	bool StoreKey(const Database& db, uint8_t* p, uint32_t size, uint32_t key, Endian e);

	// Structs without a dedicated editor expand into one row per member.
	bool ExpandsIntoMembers(const ValueType& vt);
	bool IsSignedKind(TypeKind k);

	// Reads/writes an integer member or bitfield.
	int64_t ReadInteger(const DataBlock& b, uint32_t offset, const ValueType& vt, Endian e);
	void WriteInteger(DataBlock& b, uint32_t offset, const ValueType& vt, int64_t v, Endian e);

	// Human-readable type name: "float", "reference", "GRace::Type".
	std::string TypeLabel(const TypeInfo* type);
	// Types no header describes whose 4-byte values read better in hex
	// (PathEventEnum): their values get the hint "hex", which the inspector and
	// scripts show as 0x1234ABCD.
	bool ShownInHex(const std::string& typeName);

	// "0a 1b 2c"; ParseHexBytes also takes commas and no separators.
	std::string HexBytes(const uint8_t* p, size_t n);
	bool ParseHexBytes(const std::string& s, std::vector<uint8_t>& out);
	// "name" -> its hash (StringHash32, or bStringHash with binHash), "0x1234ABCD"
	// -> that value, "" -> 0.
	bool ParseKey(const std::string& text, uint32_t& out, bool binHash);

	// The string slot at `off`, made if missing (a nested block there is dropped).
	PtrSlot& EnsureStringSlot(DataBlock& b, uint32_t off);
	// Removes the slot at `off`: a null pointer.
	void DropSlot(DataBlock& b, uint32_t off);

} // namespace attrib
