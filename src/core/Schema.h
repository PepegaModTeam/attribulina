#pragma once

// Type schema: the layouts of the game's own attribute types (structs, enums)
// so they can be edited field by field. Generated per game from its debug
// headers by scripts/gen_schema.py into data/schema/<game>_<platform>.txt.
//
// File format (line based, '#' comments):
//   schema <game> "<title>" <platform>...
//   enum <Name> <size>
//     <Identifier> <value>
//   end
//   struct <Name> <size> <align> [hint=<word>]
//     <member> <type> <offset> [n=<count>] [bits=<offset>:<size>] [ptr=<target>] [count=<member>] [hint=<word>]
//   end
//   field <class> <field> <Enum>
// <game> and the platforms are ids from Game.h (prostreet; pc, x360, ps2): the
// game whose types it lays out, and every platform that lays them out this
// way. A header without the game (older files) takes it from the file name.
// Member types: bool int8 uint8 int16 uint16 int32 uint32 int64 uint64 float
// double char, bool32 (a bool in 4 bytes, as the PS2 and GameCube compilers
// make it), cstr (char* into the string table), ptr (any other pointer, with
// ptr= and optionally count=), or a struct/enum defined in the same file.
// A `field` line names the enum a class's field holds when the type table
// gives it a plain integer type: light_flares_cg's flare_texture is an
// EA::Reflection::UInt32 the game copies into eLightFlareParams::TextureID,
// an eLightFlareTextures. The enum is defined in the same file.

#include "Bytes.h"
#include "Game.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace attrib
{

	enum class MemberPrim : uint8_t
	{
		None, // a struct or enum
		Bool, Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Float, Double, Char,
		Bool32, // a bool in 4 bytes
		CStr,   // char* into the vault's string table
		Ptr,  // any other pointer
	};

	uint32_t PrimSize(MemberPrim p);
	const char* PrimName(MemberPrim p);

	struct EnumDef
	{
		std::string name;
		uint32_t size = 4;
		std::vector<std::pair<std::string, int64_t>> values; // header order; aliases share values

		// First name with this value, or nullptr.
		const std::string* nameOf(int64_t v) const;
	};

	struct StructDef;

	struct MemberDef
	{
		std::string name;
		std::string typeName;
		uint32_t offset = 0;
		uint32_t count = 1;      // inline array length
		uint32_t elemSize = 0;   // size of one element
		MemberPrim prim = MemberPrim::None;
		const StructDef* st = nullptr;
		const EnumDef* en = nullptr;
		uint8_t bitOffset = 0, bitSize = 0; // bitSize 0: not a bitfield (PC bit order)
		std::string ptrTarget;   // for Ptr
		std::string ptrCount;    // sibling member holding the element count
		std::string hint;        // colour, key, binkey

		bool isBitfield() const
		{
			return bitSize != 0;
		}
	};

	struct StructDef
	{
		std::string name;
		uint32_t size = 0;
		uint32_t align = 4;
		std::string hint;
		std::vector<MemberDef> members;

		const MemberDef* member(std::string_view n) const;
	};

	class TypeSchema
	{
	public:
		std::string title;
		Game game = Game::Unknown;
		std::vector<Platform> platforms; // whose types it lays out
		std::string source;    // file it came from
		uint32_t boolSize = 0; // of its bool members: 1, or 4 (bool32); 0 without any

		// Throws FormatError("<source>:<line>: ...").
		static std::shared_ptr<TypeSchema> Parse(std::string_view text, const std::string& source);
		static std::shared_ptr<TypeSchema> Load(const std::string& path);

		const StructDef* findStruct(std::string_view name) const;
		const EnumDef* findEnum(std::string_view name) const;
		// The enum a `field` line gives the class's field (both keys are
		// StringHash32 of the names), or nullptr.
		const EnumDef* fieldEnum(uint32_t classKey, uint32_t fieldKey) const;
		size_t fieldEnumCount() const
		{
			return fieldEnums_.size();
		}
		size_t structCount() const
		{
			return structs_.size();
		}
		size_t enumCount() const
		{
			return enums_.size();
		}

	private:
		std::vector<std::unique_ptr<StructDef>> structs_;
		std::vector<std::unique_ptr<EnumDef>> enums_;
		std::unordered_map<std::string, const StructDef*> structByName_;
		std::unordered_map<std::string, const EnumDef*> enumByName_;
		std::unordered_map<uint64_t, const EnumDef*> fieldEnums_; // class key << 32 | field key
	};

} // namespace attrib
