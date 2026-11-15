#pragma once

// What installing a mod changed in the database, kept so the mod can be
// uninstalled and checked against the others (the user's design,
// 2026-11-15). A trace is a mod script: the undo script that turns the
// database as the mod left it back into the database before it
// (script::Diff(after, before)), whose `was` values are the ones the mod set.
// It is kept beside the list of installed mods (ModList.h), in
// GLOBAL\attribulina_traces\, and the mod's entry names it
// (InstalledMod::trace). Applying it by hand undoes the mod too.
//
// Only the database is in it. Files a mod copies (nfsms file_copy) stay
// when it is uninstalled: putting files back could leave the game's folder
// broken.
//
// Uninstalling a mod nothing installed after it touches applies its trace.
// When a later mod changed some of the same things, the later mods are
// undone too, newest first, the mod's trace applied, and the later mods
// done again (their changes as the database held them), each traced again,
// so their traces set things back to how they were before the uninstalled
// mod. A later mod that builds on what the uninstalled one made (a value of
// a collection it made) can't be done again then, and nothing is
// uninstalled.

#include "Database.h"
#include "ModList.h"
#include "Script.h"

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace attrib
{

	inline constexpr const char* kTraceFolder = "attribulina_traces";

	// The trace of `mod`, which turned `before` into `after`: the undo
	// script, with a title and a comment naming the mod. `before` takes the
	// names `after` knows first (a collection the mod made is named, not 0x...).
	script::Script MakeTrace(Database& before, const Database& after, const InstalledMod& mod);

	// One thing a trace changes, compared by keys (a name the database didn't
	// know when one trace was made is the same key in another).
	struct ModPlace
	{
		enum class Kind : uint8_t
		{
			Value,      // a value of a collection: its field and the path below it
			Collection, // a collection made, deleted or renamed: all of it
			Parent,     // a collection moved under another
			InVault,    // a collection moved to another vault
			Class,      // a class made, changed or deleted: every collection of it too
			Vault,      // a vault made, deleted or renamed
		} kind = Kind::Value;
		uint32_t cls = 0, key = 0; // the collection's class and key (Class: cls)
		uint32_t field = 0;
		std::vector<script::FieldPath::Step> steps;
		bool incremental = false; // append, or remove by value: two of these never clash
		std::string vault;        // Vault, in lower case
		std::string text;         // for the user: "pvehicle/911gt3 ENGINE.TORQUE[2]"
	};
	std::vector<ModPlace> PlacesOf(const script::Script& trace);
	// "12 values of 3 collections, 1 collection made, deleted or renamed"
	std::string DescribePlaces(const std::vector<ModPlace>& places);
	// Do two places hold something in common? A value and one inside it do,
	// a collection and anything in it, a class and its collections.
	bool Clash(const ModPlace& a, const ModPlace& b);

	// An entry of the list with its trace read.
	struct TracedMod
	{
		InstalledMod mod;
		std::shared_ptr<const script::Script> trace; // null: none (problem says why)
		std::string problem;
		// One applied in the editor that isn't saved yet: its index in the
		// pending ModListUpdate::add. -1: on the list's file.
		int added = -1;
		bool pending() const
		{
			return added >= 0;
		}
	};
	// The list's mods in `folder` (the GLOBAL folder) with their traces read.
	// One that doesn't read is a TracedMod with the problem, never a throw.
	std::vector<TracedMod> ReadTraces(const std::string& folder, const ModList& list);
	// The path of a trace an entry names, from the list's folder.
	std::string TracePath(const std::string& folder, const std::string& trace);

	// The mods of `mods` that change some of what `places` change, with the
	// places they share (the first few as text, and how many).
	struct ModConflict
	{
		size_t index = 0; // in `mods`
		std::vector<std::string> places;
		size_t count = 0;
	};
	std::vector<ModConflict> FindConflicts(const std::vector<ModPlace>& places, const std::vector<TracedMod>& mods,
		size_t maxPlaces = 5);
	// "changes 3 things <other> changed: pvehicle/a X, pvehicle/b Y and 1 more"
	std::string ConflictText(const ModConflict& c, const std::string& other);

	// --------------------------------------------------------- uninstalling ---

	struct UninstallResult
	{
		bool ok = false;
		// Why not: the database may be half changed then, so the caller drops
		// it (the installer reads the files again; the editor tries a copy first).
		std::string error;
		std::vector<std::string> notes; // what to know: values changed since, mods without a trace
		std::vector<size_t> removed;    // indices into the mods given: the ones uninstalled
		// Mods installed after them that were undone and done again, by index,
		// with their new traces.
		std::vector<std::pair<size_t, std::shared_ptr<const script::Script>>> retraced;
		bool redone = false; // later mods were undone and done again
		std::set<LoadedPack*> touched;
		std::vector<std::string> removedFiles; // World event vault files a trace dropped
		std::vector<std::string> newNames;
		size_t valuesSet = 0, created = 0, deleted = 0;
		std::string summary() const; // "12 values set back, 1 collection deleted"
	};
	// Uninstalls the mods `which` (indices into `mods`, the list in order,
	// oldest first) from `db`. `beforeRemove` is the editor's, as for Apply.
	UninstallResult Uninstall(Database& db, const std::vector<TracedMod>& mods, std::vector<size_t> which,
		const std::function<void(Collection&)>& beforeRemove = {});

	// ------------------------------------------------- changing the list ---

	// What installing and uninstalling change in a list: done on the file as
	// it is when it is written, so another program's changes since stay.
	struct ModListUpdate
	{
		struct Added
		{
			InstalledMod mod;
			std::shared_ptr<const script::Script> trace; // null: none
		};
		std::vector<Added> add;          // at the end, in order
		std::vector<InstalledMod> remove; // entries of the file (SameEntry), their traces deleted
		// Entries of the file whose traces change (later mods done again).
		std::vector<std::pair<InstalledMod, std::shared_ptr<const script::Script>>> retrace;
		bool empty() const
		{
			return add.empty() && remove.empty() && retrace.empty();
		}
	};
	// `saved` (ReadTraces of the file) as `u` leaves it: removed entries out,
	// new traces in, the added ones last (TracedMod::added).
	std::vector<TracedMod> WithUpdate(std::vector<TracedMod> saved, const ModListUpdate& u);
	// Puts into `u` what uninstalling `r.removed` from `rows` (WithUpdate's)
	// changes: a pending mod goes from `add`, a saved one to `remove`, and the
	// traces redone follow.
	void FoldUninstall(ModListUpdate& u, const std::vector<TracedMod>& rows, const UninstallResult& r);
	// Writes the traces (new ones named by time and mod, in attribulina_traces),
	// then the list, then deletes the traces of the entries removed. A trace
	// that can't be written goes in `problems` (a new entry then names none);
	// the list itself throws as WriteModList does.
	ModList UpdateModList(const std::string& folder, const ModListUpdate& u, std::vector<std::string>* problems = nullptr);

} // namespace attrib
