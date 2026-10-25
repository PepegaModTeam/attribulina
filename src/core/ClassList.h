#pragma once

// The classes each game's retail files define: data/classes/<game>_<platform>
// [_<build>].txt, one per game folder of attrib/databases, each a mod script
// of `class` statements (WriteClassList; `ATTRIB_WRITE_CLASS_LISTS=1
// attribtests classes:` writes them again). The editor and attribcli load them
// into every database (Database::addClassList). They do two things:
//   - Tell the game's classes from the user's. The game's compiled code reads
//     its classes' layouts and finds them by name, so those can be deleted and
//     restored but not redefined or renamed (ClassEditor refuses).
//   - Restore a retail class that was deleted, as the retail files define it,
//     static values included. Its collections aren't kept here.

#include "Script.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace attrib
{

	struct ClassList
	{
		std::string source;           // its file, for messages
		std::optional<Target> target; // its `game` line
		std::string title;            // what it was made from
		// The Vers value of the files it was made from (its `version` line; 0 for
		// Most Wanted's, which have none), and whether they key with StringHash64
		// (Most Wanted's early demos, `version "64-bit keys"`): of a target's
		// lists, one made from files like the database's fits.
		uint64_t vers = 0;
		bool keys64 = false;
		script::Script script;

		// Throws script::ScriptError.
		static std::shared_ptr<ClassList> Parse(std::string_view text, const std::string& source);
		static std::shared_ptr<ClassList> Load(const std::string& path);

		// The `class` statement of a class, or null.
		const script::Statement* find(uint32_t classKey) const;
		// Every class's key, in the list's order.
		const std::vector<uint32_t>& keys() const
		{
			return keys_;
		}

	private:
		std::vector<uint32_t> keys_;
		std::unordered_map<uint32_t, size_t> index_; // into script.statements
	};

	// The key a `class` statement names: StringHash32 of its name, or the 0x...
	// it gives.
	uint32_t ClassStatementKey(const script::Statement& s);

	// A class the game's retail files define, on any platform of the database's
	// game (any game's, when the database doesn't know its game).
	bool IsGameClass(const Database& db, uint32_t classKey);
	// The list of the database's game and platform that fits it best (its Vers
	// value, then most classes in common), or null when it has none.
	const ClassList* RetailClassesFor(const Database& db);
	// The classes RetailClassesFor lists that the database lacks, in its order.
	std::vector<const script::Statement*> MissingRetailClasses(const Database& db);
	// Puts a retail class back as RetailClassesFor has it. The report says what
	// went wrong; it fails when there is no list or the class is in it no more.
	script::ApplyReport RestoreRetailClass(Database& db, uint32_t classKey);

	// A class list of every class in `db`, in definition order, with the `game`
	// line, `title` and the database vault's Vers value as `version`.
	std::string WriteClassList(const Database& db, const std::string& title);

} // namespace attrib
