#pragma once

// nfsms (NFS ModScript), for backwards compatibility: the line-based scripts of
// NFS-VltEd 4.6 (by nfsu360), as the community Attribulator also reads them.
// doc/nfsms.md describes the commands and how they map onto this editor.
// Attribulina's own format is the mod script (Script.h); nfsms runs through
// it where it can, so values are checked the same way.
//
//   version 4.6
//   game PS
//   update_field pvehicle 911gt3 ENGINE[0] Torque 300
//   add_node pvehicle 911gt3 911gt3_race
//   ui_control checkbox kits true "Body kits"
//   ui_option kits
//   update_field ...            # only when the box is ticked
//   ui_option kits              # any ui_option line ends the block

#include "Script.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace attrib::nfsms
{

	constexpr const char* kExtension = ".nfsms";

	struct Command
	{
		std::string name;              // update_field, add_node, ...
		std::vector<std::string> args; // after the name; quotes removed
		std::vector<uint32_t> at;      // where each starts in the line (0-based; a quoted one at its quote)
		std::string comment;           // after '#'
		int line = 0;                  // 1-based
		std::string error;             // why the line can't run (unknown command, bad setup line); reported when run
	};

	// A setup control (ui_control): the user's choice decides which ui_option
	// blocks run.
	struct Control
	{
		enum class Kind : uint8_t
		{
			Checkbox, Radio
		} kind = Kind::Checkbox;
		std::string group; // radio buttons
		std::string name;
		std::string label;
		bool checked = false; // a checkbox's default; radio buttons start unchosen, as in NFS-VltEd
		int line = 0;
	};

	// One entry of the setup prompt, in the order NFS-VltEd's installer shows
	// them: texts and checkboxes where they are written, and the radio buttons of
	// a group together where its first one is. NFS-VltEd gives each group a panel
	// there, so a ui_text written between two radio buttons shows after the group.
	struct SetupItem
	{
		enum class Kind : uint8_t
		{
			Text, Checkbox, Radios
		} kind = Kind::Text;
		std::string text;          // Text; empty for a blank line (`ui_text#`)
		std::vector<int> controls; // indexes into Script::controls
	};

	struct Script
	{
		std::string source; // file name, for messages
		std::string path;   // the file, when it is one (an included script's)
		std::string folder; // where `script`, `ui_image` and `file_copy` paths start
		std::vector<Command> commands;
		// The lines as written (a byte order mark and line ends dropped): OGVI reads
		// them its own way (RunSettings::vltEd off).
		std::vector<std::string> lines;
		// The setup: the text and pictures shown, and the controls.
		std::vector<std::string> texts;
		std::vector<std::string> images; // paths as written (relative to `folder`)
		std::vector<Control> controls;
		std::vector<SetupItem> setup; // all of the above in the prompt's order (pictures aside)
		// NFS-VltEd shows its installer before any script; here a script without
		// ui_ lines just runs.
		bool hasSetup() const
		{
			return !setup.empty() || !images.empty();
		}
		bool hasDescription() const;  // a ui_text that isn't blank

		// The mod's title, author, version and game as mod script statements
		// (Script.h) in comment lines, which NFS-VltEd skips (doc/nfsms.md):
		//   # amod begin
		//   # title "Shifted"
		//   # version "1.2"
		//   # amod end
		// The block's lines without their #, each at its line of the file and
		// the others empty, so a mod script parser's line numbers are the file's.
		// Empty without a block; install::ReadMod reads it.
		std::string modInfo;
		int modInfoLine = 0;      // the begin line
		std::string modInfoError; // "line 9: ..." for a block without its end, or a command in it
	};

	// The commands NFS-VltEd 4.6 knows, and Attribulator's additions.
	const std::vector<std::string>& CommandNames();

	// Never fails: a line with an unknown command or a malformed setup line
	// keeps its error, reported when the script runs (NFS-VltEd logs such lines
	// and goes on).
	Script Parse(std::string_view text, const std::string& source = "script.nfsms", const std::string& folder = {});
	// Splits one line the way NFS-VltEd does: a known command at the start of the
	// line (no space needed after it: "ui_text#", "script\"x.nfsms\""), then
	// "quoted" or space-separated parameters, up to a # outside quotes. A line
	// that starts with no known command is a comment, as NFS-VltEd reads it:
	// empty name.
	Command ParseLine(std::string_view line, int lineNumber = 0);
	// The length of the known command a line starts with (after its leading
	// spaces), or 0.
	size_t CommandAt(std::string_view line);
	// A file a script names (`script`, `ui_image`, `file_copy`) under `folder`.
	// Scripts are written on Windows, so a \ separates folders, and a name that
	// isn't there as written finds the one that differs only in case.
	std::string ScriptFile(const std::string& folder, const std::string& written);

	// NFS-VltEd's own names for the parts of a game's values, the words after the
	// field (TORQUE MinY, Parts[8] KitNumber), read from its type classes:
	// data/nfsms/<game>_pc.txt, one table for every platform of the game. Each
	// word has the offset of the bytes it sets in NFS-VltEd's (the PC's) layout,
	// and, for a type our schema has as a struct, the path of our members there.
	// The table also gives VaultLib's names where they aren't our members'.
	struct PropertyNames
	{
		struct Step
		{
			std::string member;
			int64_t index = -1; // an element of an inline array member
		};
		struct Property
		{
			std::string word;
			uint32_t offset = 0;
			// Our members leading to it (Reaction.mClassKey, ValueMin[1]): found
			// by name, they hold on every platform, where offsets don't (the PS2's
			// 4-byte bools move the members after them). Empty: by the offset.
			std::vector<Step> path;
			uint32_t rest = 0; // bytes into the path's last member (+N in the table)
			bool list = false;
			std::vector<std::string> elements; // a list's element words (Value; Group Name Value)
			// What NFS-VltEd itself reads and writes at the offset (the table's
			// :type): i32, u32/key, u8, f32, i32>f32, f32list, ... Empty in older
			// tables. NFS-VltEd compatible runs write a value with this width.
			std::string vltType;
			std::string pathText() const;      // as the table writes it
		};
		struct Type
		{
			bool single = false; // one property: NFS-VltEd ignores the word after the field
			std::vector<Property> properties;
			// As written, then (anyCase) without case.
			const Property* find(std::string_view word, bool anyCase = true) const;
			const Property* at(uint32_t offset) const;
			const Property* atPath(std::string_view path) const; // without case
			bool hasPaths() const;
		};
		std::string game; // the nfsms game code (PS)
		std::map<std::string, Type> types;
		const Type* find(const std::string& typeName) const; // also with the prefix a type label leaves off
#if 0 // RETIRED (2026-11-16): the extended nfsms reading, put aside.
		// VaultLib's names (the community Attribulator's) for parts our members
		// name otherwise: by type, the name (a reference's part after a '.':
		// ReferencedRow.ClassKey; looked up in any case) to our members. The
		// extended reading took them after our member names and before
		// NFS-VltEd's words. The tables' vaultlib blocks held them; the tables
		// now give OGVI's model in ogvi blocks (below), which OGVI mode reads.
		std::map<std::string, std::map<std::string, std::vector<Step>>> vaultLib;
		const std::vector<Step>* vaultLibPath(const std::string& typeName, std::string_view name) const;
		// A VaultLib name whose parts follow as the next word (ReferencedRow ClassKey).
		bool vaultLibHead(const std::string& typeName, std::string_view name) const;
#endif

		// OGVI's reading (RunSettings::vltEd off): the class VaultLib registers for
		// each type the game's fields have (the tables' ogvi blocks,
		// scripts/nfsms/ogvi.py). Attribulator's update_field takes the words after
		// the field as its C# property names and converts the value by the C# type.
		struct OgviProperty
		{
			std::string name;    // as reflection reaches it: Level, Reaction.ClassKey, ValueMin[1]
			int64_t offset = -1; // in the PC's layout of the type; -1: Read doesn't fill it
			std::string type;    // C#: uint, float, string, enum/<name>, obj/<class>, float[2]
			std::vector<Step> path; // our member there, when one is
			uint32_t rest = 0;
			bool readOnly = false; // no public setter: Attribulator refuses it
		};
		struct OgviType
		{
			std::string kind; // prim/<C# type>, enum/<name>, refspec, string, matrix, colour, struct
			std::string cls;  // the C# class, for messages
			std::vector<OgviProperty> properties;
			const OgviProperty* find(std::string_view name) const; // exactly as written
		};
		struct OgviEnum
		{
			std::string base; // the C# underlying type: int, uint, byte, ...
			std::vector<std::pair<std::string, int64_t>> values;
		};
		std::map<std::string, OgviType> ogvi;
		std::map<std::string, OgviEnum> ogviEnums;
		const OgviType* ogviType(const std::string& typeName) const;
		// Throws std::runtime_error on a line it can't read.
		static PropertyNames Parse(std::string_view text, const std::string& source = "names");
		static std::shared_ptr<const PropertyNames> Load(const std::string& path);
	};

	// What the setup controls are set to.
	struct Choices
	{
		std::map<std::string, bool> checkboxes;
		std::map<std::string, std::string> radios; // group -> chosen name
	};
	// The setup's first choices: each checkbox as written; radio buttons unchosen,
	// as NFS-VltEd shows them, or (`ogvi`) the first of each group chosen, as
	// OGVI's import window does.
	Choices DefaultChoices(const Script& script, bool ogvi = false);

	struct RunSettings
	{
		Choices choices;
		// Warnings are errors, and the first error stops the script. Otherwise a
		// line that fails is reported and the next one runs, as in NFS-VltEd.
		bool strict = false;
		// Only the commands on these lines (1-based, inclusive; 0 = all). Setup
		// blocks before them still decide whether they run.
		int firstLine = 0, lastLine = 0;
		// file_copy and folder_create copy into this folder (the game's), and
		// only when doFiles is set; otherwise they are checked and skipped.
		std::string targetFolder;
		bool doFiles = false;
		// Reads an included script (`script <file>`); the default reads the file.
		std::function<std::string(const std::string& path)> readFile;
		std::function<void(Collection&)> beforeRemove;
		// NFS-VltEd's words for the game's values (and VaultLib's names); without
		// them only our schema's member names are known.
		std::shared_ptr<const PropertyNames> names;
		// On: NFS-VltEd mode, the script read and run as NFS-VltEd 4.6 does
		// (doc/nfsms.md, "NFS-VltEd mode"). Off: OGVI mode, as OGVI (the community
		// Attribulator's editor) imports it: its setup, the community Attribulator's
		// commands and VaultLib's names and value types (doc/nfsms.md, "OGVI
		// mode"). Neither takes Attribulina's own names: those are for mod scripts.
		// Attribulina's extended reading (our schema's member names in any case,
		// then VaultLib's names, then NFS-VltEd's words, with Attribulator's forms)
		// was the default before 2026-11-16. It is retired: its code is in
		// "#if 0 // RETIRED" blocks in Nfsms.cpp and here, kept to restore it.
		bool vltEd = false;
	};

	// Runs the script line by line. A line that fails is reported (an Error
	// message at its line) and skipped; `ok` is false only when strict stopped
	// it. The report's messages carry the nfsms line numbers.
	script::ApplyReport Run(Database& db, const Script& script, const RunSettings& settings = {});

	// The script as a mod script (.amod): the setup's checkboxes and radio groups
	// become options, ui_option blocks `if` blocks on them, the ui_text lines the
	// description, and included scripts are written in place. It runs on
	// `scratch` (a copy of the database: values read by the field's type, and
	// later lines find what earlier ones made), every block included. A line it
	// can't convert comes out as a comment and an error in `report`; what a mod
	// script can't do (file_copy) as a comment.
	script::Script ToModScript(Database& scratch, const Script& script, const RunSettings& settings = {},
							   script::ApplyReport* report = nullptr);

	// The nfsms game code for a database's game, on any platform: PS, C, MW, UC,
	// W (empty when unknown, and for Nitro, which NFS-VltEd never read).
	std::string GameCode(const Database& db);
	// Why this database can't run or record nfsms (a game NFS-VltEd has no code
	// for), or "" when it can. NFS-VltEd edits the PC's files; the words lead to
	// our schema's members by name, so scripts run on every platform of a game.
	// Run and ToModScript refuse with it; the editor checks it before recording.
	std::string Incompatibility(const Database& db);

	// nfsms lines for mod script statements (the editor records changes this
	// way). Without `vltEd`, in OGVI mode's form, as OGVI reads it back:
	// VaultLib's names for the parts the statements change, each value read
	// from `db` as its C# type takes it (`names` holds the model; without it,
	// our member names). What nfsms can't say comes out as a # comment.
	// With `vltEd`, in the form NFS-VltEd 4.6 reads: its words for the parts
	// (MaxG) where `names` has them, its value widths, no Attribulator
	// commands, as RunSettings::vltEd reads them. `before`, when
	// given, is the database the statements change: a value they give a
	// collection that only inherited it gets add_field first, which NFS-VltEd
	// needs (the change recorder passes its copy).
	std::vector<std::string> FromStatements(const Database& db, const std::vector<script::Statement>& statements,
											const PropertyNames* names = nullptr, bool vltEd = false, const Database* before = nullptr);
	// The names among `tables` for this database's game, or null.
	std::shared_ptr<const PropertyNames> NamesFor(const Database& db, const std::vector<std::shared_ptr<const PropertyNames>>& tables);

	// The collection a line names (class and collection) and its field, for
	// following the cursor; nullptr when it names none.
	Collection* CollectionOfLine(const Database& db, const Command& c, uint32_t* field = nullptr);

	// Where the word at `column` (0-based, in the line) of `c` leads, for
	// showing it in the editor (F12): a node's collection, the value the field
	// and the words after it set (as far as the word under the caret, when that
	// leads somewhere), the file a `script` line runs, or the ui_control a
	// ui_option line tests. On the command or the class: the collection and
	// value the line changes. `script` gives the folder and the controls.
	struct LineTarget
	{
		Collection* collection = nullptr;
		script::FieldPath path;
		bool hasPath = false;
		std::string file;    // a `script` line's script
		int controlLine = 0; // a ui_option line's ui_control
		std::string why;     // why it leads nowhere
	};
	LineTarget TargetOfLine(Database& db, const Script& script, const Command& c, size_t column, const RunSettings& settings = {});

	// Where update_field's field token (ENGINE[0]) and the words after it lead in
	// `c`: the path of the value set, and in `part` "class" or "collection" for a
	// reference's part, "data" for NFS-VltEd's raw bytes. Throws
	// std::runtime_error with the line's message when they lead nowhere.
	script::FieldPath WordsLeadTo(Database& db, Collection& c, const std::string& field, const std::vector<std::string>& words,
								  const RunSettings& settings = {}, std::string* part = nullptr);

} // namespace attrib::nfsms
