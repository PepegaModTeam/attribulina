#pragma once

// Installing a mod into a game: what attribcli's apply and the mod installer
// (src/installer) share. A game keeps its database in its GLOBAL folder.
// Every pack there loads (FolderPacks), the mod's script runs on them, and
// the packs it changed are saved, each file's first original kept as .bak,
// with the copies the game's memory files hold of them (core/MemoryFile.h).
// doc/installer.md has the installer's steps.

#include "Database.h"
#include "Game.h"
#include "MemoryFile.h"
#include "ModList.h"
#include "Nfsms.h"
#include "Script.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace attrib::install
{

	// The program's data folder (data/ beside it): type schemas, class lists,
	// key names, message names and NFS-VltEd's words for each game.
	struct DataFiles
	{
		std::vector<std::shared_ptr<const TypeSchema>> schemas;
		std::vector<std::shared_ptr<const ClassList>> classLists;
		std::vector<std::shared_ptr<const nfsms::PropertyNames>> nfsmsWords;
		std::string keysFile;    // attrib_keys.txt, read into both dictionaries
		std::string binKeysFile; // bin_keys.txt, bStringHash names
		std::vector<std::string> messageNames;
		// A missing folder, and each file that didn't read (what is left still works).
		std::vector<std::string> problems;

		static DataFiles Load(const std::string& dataDir);
		// An empty database with all of it, and the names in `userKeys`
		// (user_keys.txt) when that file is there.
		std::unique_ptr<Database> newDatabase(const std::string& userKeys = {}) const;
	};

	// A game's folder and where its database is.
	struct GameFolder
	{
		std::string game;     // the game's folder: nfsms file_copy writes here
		std::string global;   // its GLOBAL folder
		std::string database; // attributes.bin in it, as the file is named
	};
	// The game `chosen` names: its folder (holding GLOBAL, in any case, with
	// attributes.bin), that GLOBAL folder, or the attributes.bin in it.
	// Nothing otherwise, with the reason in `why`.
	std::optional<GameFolder> FindGameFolder(const std::string& chosen, std::string* why = nullptr);

	// A mod: a mod script (.amod) with the scripts it includes, or an nfsms
	// script (.nfsms), whose included scripts are read as it runs.
	struct Mod
	{
		std::string path;
		bool isNfsms = false;
		script::Script amod; // a mod script's
		nfsms::Script nfsms; // an nfsms script's
		// An nfsms script's # amod block (nfsms::Script::modInfo): its game,
		// title, author, version, description and url lines.
		script::Script info;
		// From a mod script's title, author, version and description lines, or
		// an nfsms script's # amod block. Without a title there, the title is
		// the file's name (`titled` false); an nfsms script without a
		// description there has its ui_text lines as one.
		std::string title, author, version, description;
		bool titled = false;
		std::string sha256; // the script file's (Sha256Hex), which names a mod without a title
		// What to know about the file that doesn't stop it: a # amod block that
		// doesn't read, with its file and line.
		std::vector<std::string> warnings;
		bool hasOptions() const; // a mod script's options, an nfsms script's setup
		// A mod script's line (game, title, ...), or the # amod block's.
		std::string meta(const std::string& key) const;
		ModIdentity identity() const;
	};
	// Reads `path` as an nfsms script when its extension is .nfsms, else as a
	// mod script. Throws std::runtime_error for a file that can't be read and
	// script::ScriptError for a mod script (or one it includes) that doesn't read.
	Mod ReadMod(const std::string& path);
	// An nfsms script's # amod block as a mod script: only its header lines
	// (game, title, author, version, description, url) count. What doesn't
	// read goes to `warnings` ("<file>:<line>: ..."), and the block is empty then.
	script::Script ModInfoOf(const nfsms::Script& s, std::vector<std::string>* warnings = nullptr);

	// What the mod list (ModList.h) keeps of a script file, before it is
	// installed: its title, author and version (`meta`: the mod script, or an
	// nfsms script's ModInfoOf), its path and sha256 (of `fileText`, the file
	// as read), and whether it is nfsms.
	InstalledMod DescribeMod(const std::string& path, std::string_view fileText, bool nfsms, const script::Script& meta);
	ModIdentity IdentityOf(const InstalledMod& m);
	// The rest of the entry for installing a mod into `db`: the game, the time
	// now, the program (`by`) and the options chosen (a mod script's values by
	// id, or an nfsms setup's choices). The result and files are the caller's.
	void CompleteRecord(InstalledMod& m, const Database& db, const std::string& by, const std::map<std::string, std::string>& options,
		const nfsms::Choices* choices = nullptr);

	// Why `mod` can't be installed into `db`: its game line names another game
	// (a mod script's `game`, an nfsms script's), or it is nfsms and NFS-VltEd
	// never read the game. Empty when it can.
	std::string ModProblem(const Mod& mod, const Database& db);
	// What the user should know first: a mod script written for the game on
	// another platform (its values still go in by their fields' types), or a
	// game line the editor doesn't know. Empty otherwise.
	std::string ModWarning(const Mod& mod, const Database& db);

	// Loads every pack of a folder (FolderPacks), or one pack after its
	// folder's attributes.bin, as attribcli does. OGVI's saves of World event
	// vaults go back to gc.vaults as the game's event vault files: their own
	// files are added to `movedFrom`, which saving removes. Throws on a pack
	// that doesn't load.
	void LoadPacks(Database& db, const std::string& path, std::vector<std::string>& movedFrom);

	struct ModSettings
	{
		std::map<std::string, std::string> options; // a mod script's option values by id; the rest take their defaults
		nfsms::Choices choices;                     // an nfsms setup's
		std::string gameFolder;                     // where nfsms file_copy and folder_create write
		bool doFiles = true;                        // off: they are checked and skipped
		// nfsms read the way NFS-VltEd 4.6 reads it (nfsms::RunSettings::vltEd).
		// The installer always runs it so: nfsms mods are made for NFS-VltEd.
		bool vltEd = true;
		bool strict = false; // warnings stop the script
	};
	// Runs the mod on `db`: a mod script stops at its first error, nfsms goes
	// on after a line that fails, as NFS-VltEd does.
	script::ApplyReport RunMod(Database& db, const Mod& mod, const ModSettings& settings,
		const std::vector<std::shared_ptr<const nfsms::PropertyNames>>& words);

	// A message of a run as attribcli prints it: "<script>:<line>:<column>:
	// warning: <text>", or an included script's file and line in front. What a
	// script printed (echo) is its text alone.
	std::string MessageLine(const std::string& source, const script::Message& m);

	// What saving a database writes and removes.
	struct Changes
	{
		std::vector<LoadedPack*> packs;   // with unsaved changes
		std::vector<std::string> removed; // files of packs the database no longer has
		bool empty() const
		{
			return packs.empty() && removed.empty();
		}
	};
	// The dirty packs of `db`, and `removed` without the files a pack has
	// again (a World event vault made again after its vault was deleted),
	// sorted, each once.
	Changes ChangesOf(const Database& db, std::vector<std::string> removed);

	struct SaveSettings
	{
		bool backup = true;      // each file's first original kept as <file>.bak
		bool memoryFiles = true; // the memory files beside a pack update their copies of it
		std::string outDir;      // written there instead; the game's files stay as they are
	};
	struct SaveNote
	{
		enum class Kind
		{
			Saved,          // "saved <pack>[ and <twin>]"
			Backup,         // "kept the original as <file>.bak"
			Removed,        // "removed <file>[, kept as <file>.bak]"
			MemoryFile,     // "updated <memory file>: its copy of <game path>"
			MemoryFileLeft, // with memoryFiles off: a copy that stays as it was
		} kind;
		std::string text;
		std::vector<std::string> files; // Saved, Removed, MemoryFile: the files written or removed
	};
	struct SaveResult
	{
		std::vector<std::string> written; // packs and their twins
		std::vector<std::string> removed;
		std::vector<std::string> memoryFiles;
	};
	// Writes the changed packs (each with a .lzc's twin) through a .tmp,
	// removes the removed files (renamed to .bak when there is none yet), then
	// gives every memory file holding a copy of a written file its bytes
	// (PlanMemoryFileUpdates). Throws std::runtime_error on a file it can't
	// write or remove; the files before it are written by then.
	SaveResult SaveChanges(const Database& db, const Changes& changes, const SaveSettings& settings = {},
		const std::function<void(const SaveNote&)>& note = {});

	// Adds to user_keys.txt the names it lacks: the names scripts gave new
	// collections and keys, which the packs keep only as hashes. The editor
	// reads the file, so it shows them.
	void RememberNames(const std::string& userKeys, const std::vector<std::string>& names);

} // namespace attrib::install
