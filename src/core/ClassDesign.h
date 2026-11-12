#pragma once

// Classes as the user designs them, and the edits that add, change and remove
// classes in a database.
//
// A design lists fields: a name, a type from the database's type table, and
// where the value is kept:
//   layout  in every collection's layout block, at a fixed offset (the game's
//           compiled Attrib::Gen::<class>::_LayoutStruct reads it there); never
//           inherited
//   entry   an optional value a collection sets or inherits from its parent
//   static  one value for the whole class, in the class's static block
// Arrays: a layout array has a fixed number of slots (an inline Attrib::Array
// header and that many elements), an entry array grows as needed up to its
// definition's count.
//
// LayOutClass turns a design into the definitions the vault stores, by the
// rules retail data follows (doc/vault-format.md): layout and static fields in
// the design's order at their natural alignment, every field bound, sizes and
// counts derived. A retail class's design (DesignOf) holds the stored values
// those rules don't give, so laying it out again gives the class as stored.

#include "Database.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace attrib
{

	enum class FieldStorage : uint8_t
	{
		Layout, Entry, Static
	};
	const char* FieldStorageName(FieldStorage s); // "layout", "entry", "static"

	// Entry array counts: 0 means the definition sets no limit (Edit.h ElementLimit).
	constexpr uint16_t kNoElementLimit = 0;

	struct FieldDesign
	{
		std::string name; // "" when only the key is known
		uint32_t key = 0; // StringHash32(name)
		uint32_t typeHash = 0;
		FieldStorage storage = FieldStorage::Entry;
		bool array = false;
		// A layout array's slots; an entry array's most elements (mMaxCount, 0 for
		// no limit). Not used otherwise.
		uint16_t count = 0;
		// Searchable layout fields are the ones the class's layout count counts
		// (DefFlag::NotSearchable when false).
		bool searchable = true;
		// Stored values the rules don't give (a few retail classes have them);
		// unset, LayOutClass derives them.
		std::optional<uint16_t> offset;
		std::optional<uint8_t> alignment; // log2
		std::optional<uint8_t> flags;
		std::optional<uint16_t> maxCount; // a non-array's (1 when derived)
		// The element size, for a type the table doesn't list (else the type's).
		std::optional<uint16_t> size;

		bool operator==(const FieldDesign&) const = default;
	};

	struct ClassDesign
	{
		std::string name; // "" when only the key is known
		uint32_t key = 0;
		// Layout fields in layout order, static fields in static block order;
		// entries anywhere.
		std::vector<FieldDesign> fields;
		// Stored values the rules don't give; unset, LayOutClass derives them.
		std::optional<uint32_t> collectionReserve, layoutSize, staticSize;
		std::optional<uint16_t> layoutCount, layoutKeyShift;

		const FieldDesign* field(uint32_t fieldKey) const;
		bool operator==(const ClassDesign&) const = default;
	};

	// The design of a class as the database holds it: layout fields by offset,
	// then static fields by offset, then entries in definition order, with the
	// stored values the rules don't give and its collection reserve.
	ClassDesign DesignOf(const Database& db, const ClassDef& cls);

	// What is wrong with a design, one line each (empty: nothing). `editing` is
	// the class it replaces (its key may stay taken). Static fields are for the
	// game's own classes only: the game copies static values into a table
	// compiled into it, and a class it doesn't know there gets none (Undercover
	// crashes loading it).
	std::vector<std::string> DesignProblems(const Database& db, const ClassDesign& design, const ClassDef* editing = nullptr);
	// What may go wrong in game with a design that has no problems, one line
	// each: a layout larger than the game's default data area (what an Instance
	// without a collection reads; DefaultDataAreaSize).
	std::vector<std::string> DesignWarnings(const Database& db, const ClassDesign& design);
	// The size of the zeroed buffer the game's Attrib::DefaultDataArea returns,
	// compiled into each game (2960 in Undercover, 1368 in Pro Street, 2048 in
	// Most Wanted and Carbon, at most 2752 in World); 0 when not known.
	uint32_t DefaultDataAreaSize(const Target& target);

	// The class a design describes: key, field definitions (type pointers into
	// `db`), layout size, count and key shift, static size and collection
	// reserve. Its vault, collections, static data and writer hints are left
	// alone. Throws std::runtime_error when DesignProblems has something.
	void LayOutClass(const Database& db, const ClassDesign& design, ClassDef& out);

	// The log2 alignment fields of `type` get: the schema's, else by its kind
	// (16 for Vector4, Quaternion, Matrix and FloatColour, 8 for 64-bit
	// numbers, 1 for Colour, else the size's, up to 4). Every retail definition
	// has it. Layouts go by the types alone, so a retail class restored into a
	// database whose other classes are gone comes back as it was.
	uint8_t NaturalAlignment(const TypeInfo& type);
	// Whether fields of `type` carry DefFlag::HasHandler: in Undercover (PC,
	// 360, PS3) and World, a type the game gives a constructor or destructor
	// (a table compiled into it): RefSpec, ClassRefSpec, Curve, the structs
	// holding them, and a few listed by name, as every retail definition has it.
	// Nitro marks the same but for Curve.
	bool TypeHasHandler(const Database& db, const TypeInfo& type);
	// The type a design names: a type of the table by its full name, or by its
	// short label ("float", "RefSpec") when only one type has it. Null if none.
	const TypeInfo* FindTypeByName(const Database& db, std::string_view name);

	// A collection-table reserve for a class with `collections` collections, as
	// the retail tool sized them (max(1, 1.4 x collections)).
	uint32_t CollectionReserveFor(size_t collections);

	class ClassEditor
	{
	public:
		explicit ClassEditor(Database& db) : db_(db)
		{
		}

		// A new class laid out from `design`, its definition exported from
		// `vault`, which must be the database's (attributes.bin's "db", where
		// every retail class is: the game loads it first, and skips a
		// collection whose class isn't loaded yet). Throws std::runtime_error
		// when the design has a problem or the class exists. The database's
		// class count and default data size (DatabaseLoadData; the game reads
		// neither as a limit) grow to cover it.
		ClassDef& create(Vault& vault, const ClassDesign& design);

		// Lays `cls` out again from `design` (its key stays; rename() renames).
		// Only a class of the user's own: a class of the game (IsGameClass,
		// ClassList.h) throws, since the game's compiled code reads its layout.
		// Collections follow: a field that keeps its key and type keeps each
		// collection's value wherever it is kept now (a field moved into the
		// layout takes the value the collection had or inherited, one moved out
		// of it becomes the collection's own), a layout array keeps the elements
		// that fit, and values of fields that went or changed type are dropped.
		// Static values follow the same way. False when nothing changed.
		bool redesign(ClassDef& cls, const ClassDesign& design);

		// A new name: the class key and its collections' export ids change.
		// References to its collections from other classes keep the old class key.
		// Throws when the name is taken, and for a class of the game.
		void rename(ClassDef& cls, const std::string& newName);

		// Removes the class and every collection of it, in every vault.
		// `beforeRemove` sees each collection first (the editor lets go of it).
		void remove(ClassDef& cls, const std::function<void(Collection&)>& beforeRemove = {});

		// The packs the edits changed (marked dirty).
		const std::set<LoadedPack*>& touched() const
		{
			return touched_;
		}

	private:
		void touch(Vault* v);
		void growDatabaseValues(const ClassDef& cls);

		Database& db_;
		std::set<LoadedPack*> touched_;
	};

} // namespace attrib
