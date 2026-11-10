#pragma once

// Mod scripts (.amod): database changes written down, so they can be shared
// and applied again from the editor or the command line (attribcli).
// doc/scripts.md describes the language. In short:
//
//   attribulina 1
//   game prostreet_pc
//   title "Quicker 911"
//   option power number "Torque scale" default 1.1 min 1 max 2
//
//   edit pvehicle/911gt3 {
//       MAX_RPM = 9000 was 8400
//       ENGINE.TORQUE[2] *= $power
//   }
//   collection pvehicle/911gt3_race under pvehicle/911gt3 in vault cars_vault {
//       TIRES.GRIP_SCALE = (1.1, 1.1, 1.1, 1.1)
//   }
//   delete pvehicle/old_car
//   include "parts/kits.amod"
//
// A script is a list of changes, not a program: every statement names what it
// changes, so applying it twice gives the same database (except += and the
// like), a reader sees what a mod does, and the editor can make one from the
// difference between two databases. Values are typed only when they are
// applied, against the field they go into.

#include "ClassDesign.h"
#include "Database.h"
#include "Values.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace attrib
{
	class GameplayModel;
}

namespace attrib::script
{

	// The newest format this build reads. 2 added append, remove by value,
	// resize by, indices from the end ([-1]) and conditions on the database
	// (exists, has, own, count, comparisons, contains). A script is printed with
	// the lowest format that reads it (FormatNeeded), so one that uses none of
	// those still opens in a build that reads 1.
	constexpr int kVersion = 2;
	constexpr const char* kExtension = ".amod";

	// 1-based line and column.
	struct Pos
	{
		int line = 0, col = 0;
	};

	// what() is "<source>:<line>:<col>: <message>".
	struct ScriptError : std::runtime_error
	{
		ScriptError(Pos p, const std::string& message, const std::string& source = {});
		// A mistake in an included script: pos is the include line, and what()
		// names the included file.
		ScriptError(Pos p, const std::string& message, const std::string& file, Pos filePos);
		Pos pos;
		std::string message;
		std::string file; // the included script's path ("" when it's this one)
		Pos filePos;
	};

	// A value as written. `a/b` is a reference for a RefSpec and a division for a
	// number; a word is a name, an enum value, true/false/null or a number.
	struct Value
	{
		enum class Kind : uint8_t
		{
			Number, // text: as written (decimal, 1.5e3, 0x1F)
			String, // text
			Word,   // text: a bare name
			Option, // $text
			Tuple,  // (a, b, ...): vectors, colours, a StringKey with its stored hash
			List,   // [a, b, ...]: arrays
			Struct, // { member = value, ... }: members not listed keep their values
			Bytes,  // bytes "0a 0b ...": the value's raw bytes
			Lua,    // lua "source": a handler compiled and packed when applied
			Binary, // items[0] op items[1]; op is + - * /
			Negate, // -items[0]
		} kind = Kind::Word;
		Pos pos;
		std::string text;
		char op = 0;
		std::vector<std::shared_ptr<Value>> items;
		std::vector<std::pair<std::string, std::shared_ptr<Value>>> members;
	};
	using ValuePtr = std::shared_ptr<Value>;

	ValuePtr MakeNumber(std::string text);
	ValuePtr MakeString(std::string text);
	ValuePtr MakeWord(std::string text);
	ValuePtr MakeList(std::vector<ValuePtr> items, Value::Kind kind = Value::Kind::List);

	// class/collection. Names are as written: a name or 0x.... An edit's
	// collection may hold * and ? wildcards.
	struct Target
	{
		std::string cls, name;
		bool root = false; // `under root`
		Pos pos;
		std::string text() const;
	};

	// FIELD, FIELD[2], FIELD.member[1].x
	struct FieldPath
	{
		struct Step
		{
			std::string member; // empty: an index
			int64_t index = -1;
		};
		std::string field; // a name or 0x...
		std::vector<Step> steps;
		Pos pos;
		std::string text() const;
	};

	// Configurator settings: asked for when the script is applied (the editor
	// shows a form, attribcli takes --set id=value) and used as $id in values and
	// in `if`. Provisions for now; nfsms compatibility will say what else it needs.
	struct OptionDef
	{
		enum class Type : uint8_t
		{
			Bool, Number, Integer, Choice, Text
		} type = Type::Bool;
		std::string id;
		std::string label;
		std::string help;
		ValuePtr defaultValue;
		std::optional<double> min, max;
		std::vector<std::string> choices;
		Pos pos;
	};
	const char* OptionTypeName(OptionDef::Type t);

	// `if` conditions over options.
	struct Condition
	{
		enum class Kind : uint8_t
		{
			Option, Not, And, Or,
			// A comparison of the left side with `value`: $option, or (format 2,
			// inside a collection's block) the value at `path` or with `count`
			// the length of the array there.
			Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
			Contains,         // path contains value: an element of the array there is it
			ExistsClass,      // exists class word
			ExistsVault,      // exists vault word
			ExistsField,      // exists field word name: class word has the field name
			ExistsCollection, // exists target (wildcards: any match)
			Has,              // has path: the collection has a value there, own or inherited
			Own,              // own name: the collection's own value of the field
		} kind = Kind::Option;
		std::string option; // the left side of a comparison when it is $option
		FieldPath path;     // or this path in the collection (hasPath)
		bool hasPath = false, count = false;
		std::string word, name;
		Target target;
		ValuePtr value;
		std::vector<Condition> items;
	};

	struct Script;

	struct Statement
	{
		enum class Kind : uint8_t
		{
			Comment,     // comment (a whole line)
			Header,      // attribulina <number>
			Meta,        // <word> <text>: game, title, author, version, description, url
			Option,      // option
			If,          // if cond { body } else { elseBody }
			Edit,        // edit target { body }: the collection must exist
			Create,      // create target [from from] [under under] [in vault vault] { body }: it must not
			Collection,  // collection target [under under] [in vault vault] [exact] { body }: made if missing
			Delete,      // delete target [with children]
			Rename,      // rename target to word
			Move,        // move target under under
			MoveToVault, // move target to vault vault
			CreateVault, // create vault vault in file word
			DeleteVault, // delete vault vault
			RenameVault, // rename vault vault to word
			Set,         // path op value [was was]
			Revert,      // revert path: inherit again
			Resize,      // resize path count
			Insert,      // insert path[index] [= value]
			Remove,      // remove path[index], or (format 2) remove path = value: each element that is value
			Append,      // append path [= value] [if missing] (format 2): at the end; a list adds each item
			Echo,        // echo [warning|error] values...: word is the level ("" for plain), value a List
			Include,     // include "path": text is the path as written
			// class word [in vault vault] [reserve n ...] { fields }: made if missing,
			// else laid out again to this design (a class of the game only when it
			// is this already). settings: the stored values the rules don't give.
			Class,
			// layout|entry|static path.field text[[count]] [unsearchable] [at n ...]
			// [= value]: one field of a class (word: where it is kept; text: its
			// type; value: a static field's value). Or `bytes "..."` (word "bytes"):
			// the class's static block as stored, when it has bytes no static
			// field covers (World's gameplay).
			ClassField,
			DeleteClass, // delete class word: the class and every collection of it
		} kind = Kind::Comment;
		Pos pos;
		std::string comment; // Comment's text, or a comment after the statement
		std::string word;
		std::string text;
		int number = 0;
		Target target, from, under;
		bool hasFrom = false, hasUnder = false, withChildren = false, exact = false;
		// Append: `if missing`, only values the array doesn't hold yet. Resize:
		// `by` (format 2), count changes by `number` instead of becoming `count`.
		bool ifMissing = false, by = false;
		std::string vault;
		FieldPath path;
		char op = '='; // '=', or '+', '-', '*', '/' for +=, ...
		ValuePtr value, was;
		uint32_t count = 0;
		OptionDef option;
		Condition cond;
		std::vector<Statement> body, elseBody;
		// Include: the script it names, once LoadIncludes has read it.
		std::shared_ptr<const Script> included;
		// Class, ClassField: stored values the layout rules don't give, as
		// written (`reserve 12`, `at 4`, `flags 0x2E`), in order.
		std::vector<std::pair<std::string, int64_t>> settings;
		bool isArray = false, unsearchable = false; // ClassField
	};

	struct Script
	{
		std::vector<Statement> statements;
		std::string source; // file name, for messages
		// The folder of the script's file, where relative include paths start
		// ("" for a script that isn't a file: those can include full paths only).
		std::string folder;

		// `game`, `title`, ... ("" when not set).
		std::string meta(const std::string& key) const;
		void setMeta(const std::string& key, const std::string& value);
		// Top-level options, in order, then those of the scripts it includes
		// (read with LoadIncludes) where the include is.
		std::vector<const OptionDef*> options() const;
	};

	// Throws ScriptError on the first mistake. Included scripts aren't read.
	Script Parse(std::string_view text, const std::string& source = "script", const std::string& folder = {});
	// Reads the scripts that `include` statements name into Statement::included,
	// and the ones those include. A relative path starts from the folder of the
	// script that has the include. Throws ScriptError for a file that is missing
	// or doesn't read, and for a script that includes itself. Apply calls it when
	// the caller hasn't; call it first to show the included scripts' options.
	void LoadIncludes(Script& script);
	// Has `script` include statements LoadIncludes hasn't read?
	bool HasUnreadIncludes(const Script& script);
	// Script text; Parse(Print(s)) gives s back (comments included).
	std::string Print(const Script& script);
	// The lowest format version that reads these statements: 2 when one uses
	// what format 2 added (kVersion's comment), else 1. Included scripts aren't
	// counted; each says its own.
	int FormatNeeded(const std::vector<Statement>& statements);
	std::string PrintValue(const Value& v);
	std::string Quote(std::string_view text);
	// A number in the fewest digits that read back the same (as a float with
	// asFloat), without an exponent unless it is very large or small.
	std::string NumberText(double d, bool asFloat = false);
	// Can `s` be written without quotes and read back as the same word?
	bool IsBareWord(std::string_view s);
	// The game and platform scripts are written for: the database's target id
	// ("prostreet_ps2", Game.h), or "" without one.
	std::string GameId(const Database& db);

	// --------------------------------------------------------------- applying ---

	struct Message
	{
		// Echo: text a script printed with `echo`.
		enum class Level : uint8_t
		{
			Info, Echo, Warning, Error
		} level = Level::Info;
		Pos pos;
		std::string text;
		// In a file another one includes (`include`, nfsms `script`): its path
		// and line. pos is then the including line in the script that ran.
		std::string file;
		int fileLine = 0;

		Message() = default;
		Message(Level l, Pos p, std::string t, std::string f = {}, int fl = 0)
			: level(l), pos(p), text(std::move(t)), file(std::move(f)), fileLine(fl)
		{
		}
		// The error `e` reports.
		static Message Error(const ScriptError& e);
	};

	struct ApplySettings
	{
		// Option values by id, as text ("true", "1.2", "race"); others take their default.
		std::map<std::string, std::string> options;
		// `was` mismatches and another game are errors instead of warnings.
		bool strict = false;
		// Warn about references to collections that aren't there once it ran.
		// Off when the caller checks later (nfsms, after the whole script: a
		// reference's class may come on the next line).
		bool checkReferences = true;
		// A few statements typed in (attribcli -e): no "attribulina 1" line expected.
		bool fragment = false;
		// Called before a collection is deleted (the editor lets go of it).
		std::function<void(Collection&)> beforeRemove;
	};

	struct ApplyReport
	{
		bool ok = false;
		std::vector<Message> messages; // warnings, and the error that stopped it
		size_t valuesSet = 0, created = 0, deleted = 0, moved = 0, renamed = 0, reverted = 0;
		size_t classesCreated = 0, classesChanged = 0, classesDeleted = 0;
		std::set<LoadedPack*> touched;     // packs that changed (now marked dirty)
		// Packs it dropped (World event vault files whose vault was deleted): saving
		// the database removes their files.
		std::vector<std::string> removedFiles;
		std::vector<std::string> newNames; // names the dictionary didn't have
		std::string summary() const;       // "3 values set, 1 collection created"
		size_t warnings() const;
		size_t errors() const; // nfsms goes on after a failing line; mod scripts stop at the first
	};

	// Applies the statements in order and stops at the first error, leaving what
	// it changed until then (apply to a CloneDatabase() copy first to try it).
	ApplyReport Apply(Database& db, const Script& script, const ApplySettings& settings = {});

	// The option values `settings` gives, checked against the script's options:
	// unknown ids, wrong types and values out of range throw ScriptError.
	std::map<std::string, ValuePtr> ResolveOptions(const Script& script, const ApplySettings& settings);

	// The type of the value at `path` in `c` (its own value or the inherited
	// one), for turning untyped text (nfsms) into a value of the right kind. With
	// `current`, also its bytes. Throws ScriptError when the path doesn't lead to
	// a value; an array gives kind Unknown and size 0.
	ValueType TypeAt(Database& db, Collection& c, const FieldPath& path, Bytes* current = nullptr);
	// Where that value is: the block holding it (the own value's or the
	// inherited one's) and its offset there, or null when `c` has none. Throws
	// as TypeAt does.
	const DataBlock* BlockAt(Database& db, Collection& c, const FieldPath& path, uint32_t* offset);

	// A copy of the database (packs written out and read back), for trying a
	// script before applying it for real.
	std::unique_ptr<Database> CloneDatabase(const Database& db);

	// ------------------------------------------------------- what it names ---

	// What the caret is on in a script, for showing it in the editor (F12): the
	// collection a statement names (its target, `from`, `under`, `exists C/N`),
	// or a value of the collection whose block the caret is in (a path, or an
	// element or member of a list or struct written as the value, with the
	// steps that lead there). `target.cls` is empty when the caret's line names
	// nothing. A value written as class/name (a reference) also gives `named`,
	// the collection it may stand for. The text is read with Parse; when that
	// fails, the top-level statement around the caret is read alone, so a
	// mistake elsewhere doesn't get in the way.
	struct ScriptPlace
	{
		Target target;
		FieldPath path;
		bool hasPath = false;
		std::optional<Target> named;
	};
	ScriptPlace PlaceAt(std::string_view text, Pos caret);
	// The collections a target names (every match of a wildcard), none when its
	// class isn't there.
	std::vector<Collection*> TargetCollections(const Database& db, const Target& t);
	// The key a name stands for as scripts write it: 0x... is the key itself,
	// a quoted "0x..." a name.
	uint32_t NameKey(const std::string& written);

	// ------------------------------------------------------------- generating ---

	struct DiffSettings
	{
		bool recordWas = true; // `was <old>` on changed values, checked when applied
		// Only collections of these classes (empty: all).
		std::set<uint32_t> classes;
		// Compare the values of collections both have; false: only what was made,
		// deleted, renamed or moved (the editor's change recorder).
		bool values = true;
	};

	// The statements that turn `base` into `current`: new collections (with all
	// their values), edited values, moves, renames, deletes, new vaults.
	Script Diff(const Database& base, const Database& current, const DiffSettings& settings = {});

	struct ExportSettings
	{
		// Only these fields (empty: every own value).
		std::set<uint32_t> fields;
		// `collection ... exact`: applying it also reverts own values the export
		// doesn't list. Only meaningful with every field.
		bool exact = false;
		// `edit` blocks (the collections must exist) instead of `collection`.
		bool editOnly = false;
		// Begin with `create vault` for each vault the collections are in (the
		// database's own excepted), so applying makes the ones missing.
		bool vaults = false;
	};

	// `create vault` for `vault`, in its pack's file (one of World's event vaults
	// is a file of its own, which applying makes when it isn't there).
	Statement CreateVaultStatement(const Vault& vault);

	// An `edit` of `now` with what changed since `before` (the same collection in
	// another database); its body is empty when nothing did.
	Statement DiffValues(const Database& base, const Collection& before, const Database& cur, const Collection& now,
						 bool recordWas = true);

	// Statements that make `collections` (in this order; parents first) look the
	// way they do in `db`: parent, vault and own values.
	std::vector<Statement> Export(const Database& db, const std::vector<const Collection*>& collections,
								  const ExportSettings& settings = {});

	// Statements that make gameplay elements (GameplayFolderContents, Gameplay.h)
	// as `db` holds them, so the game finds them: `create vault` for the vaults
	// they are in, the elements, then for each owner outside them that lists one
	// in its `Children`, that whole list.
	std::vector<Statement> ExportGameplay(const Database& db, const GameplayModel& m, const std::vector<const Collection*>& elements);

	// `path = value` for a collection's own (or inherited) value at `path`.
	Statement ExportValue(const Database& db, const Collection& c, uint32_t field);

	// A `class` statement that makes `cls` as `db` holds it: its fields, with
	// the stored values the layout rules don't give, its vault, and (with
	// `values`) its static values.
	Statement ClassStatement(const Database& db, const ClassDef& cls, bool values = true);

	// The design a `class` statement describes, its types looked up in `db`'s
	// type table. Throws ScriptError for a type the table doesn't have and for
	// settings that aren't numbers in range.
	ClassDesign DesignFromStatement(const Database& db, const Statement& s);

	// A new script with the header, the game and `statements`.
	Script NewScript(const Database& db, std::vector<Statement> statements = {});

	// Adds `edit` statements to `script`, merging them into an existing top-level
	// edit of the same collection (a later Set of the same path replaces the
	// earlier one, keeping its `was`). A `create vault` the script has already
	// isn't added again. The editor's "Add to Script" uses it.
	void Merge(Script& script, const std::vector<Statement>& statements);

} // namespace attrib::script
