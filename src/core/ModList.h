#pragma once

// The mods installed in a game: attribulina_mods.txt, beside the database in
// its GLOBAL folder. The list is kept there and not in the packs, so the
// game's files don't grow by anything an unpatched game would have to read.
// The mod installer adds each mod it installs, the editor each mod script or
// nfsms script it applies from a file once the changes are saved, and
// attribcli each it applies; each has a switch that turns this off. Nothing
// needs the file: a game without one has no mods listed. doc/installer.md
// has the format.
//
//   attribulina mods 1
//
//   mod "Shifted"
//       version "1.2"
//       author "someone"
//       script "D:\Mods\Shifted\Shifted.nfsms"
//       nfsms
//       sha256 3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b
//       game prostreet_pc
//       installed "2026-11-14 15:30:12"
//       by "Attribulina Mod Installer 0.9.3"
//       result "installed"
//       option "Loud" "on"
//       file "GLOBAL\ATTRIBUTES.BIN"
//   end
//
// A text in quotes doubles a quote inside it (""), so Windows paths keep
// their backslashes as they are.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace attrib
{

	inline constexpr const char* kModListFile = "attribulina_mods.txt";
	inline constexpr int kModListVersion = 1;

	// One installation of a mod.
	struct InstalledMod
	{
		std::string title;      // the mod's own; "" when it names none (the script's sha256 tells it apart then)
		std::string author, version;
		std::string script;     // the script file it was installed from
		std::string sha256;     // that file's (Sha256Hex)
		bool nfsms = false;     // an nfsms script; else a mod script
		std::string game;       // the game and platform it went into (Target::id(): prostreet_pc)
		std::string installed;  // local time, "2026-11-14 15:30:12"
		std::string by;         // the program and its version
		std::string result;     // "installed", "installed, 2 lines failed", "saving failed halfway"
		// A mod script's option values, an nfsms setup's choices (a checkbox's
		// "on"/"off", a radio group's button), as installed.
		std::vector<std::pair<std::string, std::string>> options;
		// The files it changed: from the folder above the list's (the game's),
		// with \ between folders, as the game names them: GLOBAL\ATTRIBUTES.BIN.
		std::vector<std::string> files;
		// Lines this version doesn't know (a later one's), kept as they are.
		std::vector<std::string> unknown;

		std::string name() const; // the title, else the script's file name
	};

	struct ModList
	{
		std::vector<InstalledMod> mods; // in the order they were installed
		int version = kModListVersion;  // the file's `attribulina mods` line

		// Throws std::runtime_error ("<source>:<line>: ...") on a line it can't read.
		static ModList Parse(std::string_view text, const std::string& source = kModListFile);
		std::string print() const;
	};

	// The list of the database in `folder` (the GLOBAL folder): its path.
	std::string ModListPath(const std::string& folder);
	// The list there, or nothing when there is no file. Throws
	// std::runtime_error for a file that can't be read or doesn't parse.
	std::optional<ModList> ReadModList(const std::string& folder);
	// Writes it through a .tmp, which then replaces the file. Throws
	// std::runtime_error when it can't.
	void WriteModList(const std::string& folder, const ModList& list);
	// Adds `mods` to the list in `folder` (made when there is none) and writes
	// it. Throws as ReadModList and WriteModList do; a file that doesn't
	// parse is left as it is.
	ModList AddToModList(const std::string& folder, const std::vector<InstalledMod>& mods);

	// `path` as InstalledMod::files has it: from `gameFolder`, with \ between
	// folders, or whole when it isn't in there.
	std::string ModListFileName(const std::string& path, const std::string& gameFolder);

	// Which mod a script is: the title it gives itself and its version, else
	// its file's sha256.
	struct ModIdentity
	{
		std::string title, version, sha256;
		std::string script; // its path, for a name when there is no title
	};
	struct ModMatch
	{
		const InstalledMod* mod = nullptr;
		bool sameVersion = false; // the same file, or the same title and version
	};
	// The entries of `list` that are this mod, oldest first: the same title, in
	// any case and without the spaces at its ends, or the same script file
	// (sha256). A mod without a title matches only its own file.
	std::vector<ModMatch> FindInstalled(const ModList& list, const ModIdentity& mod);
	// What to tell the user before installing `mod` over the latest of
	// `matches` (empty when there is none): installing a mod again, or another
	// version over it, runs its script over changes made already, which can fail.
	std::string InstalledNote(const std::vector<ModMatch>& matches, const ModIdentity& mod);

} // namespace attrib
