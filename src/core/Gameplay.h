#pragma once

// The `gameplay` class: Pro Street's activities, races, tracks, characters,
// hubs and state graphs, 5154 collections over 286 vaults. It has its own
// organisation on top of AttribSys:
//
// - Templates. A collection's AttribSys parent is its template ("circuit",
//   "track_layout", "character"), and templates inherit from each other up to
//   "baseelement". Template collections have short names without a '/'.
// - Folders. Element names are paths ("tracks/autobahn_gd/autobahn_gd_grip_1"),
//   and each element lists the elements under it in its `Children`
//   (GCollectionKey[]). Most path prefixes are themselves elements.
// - Vaults. `gameplayvault` (Text) names the vault that holds an element's
//   children; the game loads it by that name. In retail every listed child
//   is in its owner's gameplayvault vault. Elements without children often
//   name a vault that doesn't exist.
// - References. GCollectionKey fields hold the StringHash32 of a collection
//   name, almost always a gameplay element (SunAndSky points at `sunandsky`).
//   A few Text fields hold element paths instead (TrackRecordHolder).
// - Layout fields every element sets: Name and CollectionName (its own path),
//   Template (true on templates), Position, Rotation.
//
// State graphs: `state` collections ("stategraph_basicrace_racing", templateref
// = the activity template) and `messagehandler` collections
// ("basicrace/stategraph_basicrace_racing_handler_notifyescaped") with stateref,
// handler_owner, message_id (StringHash32("M" + Hermes message class)),
// bytecode (compiled Lua, see lua/Lua.h) and transitionlist (target states).

#include "Database.h"
#include "lua/Lua.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace attrib
{

	namespace gameplay
	{
		// Field names.
		inline constexpr const char* kChildren = "Children";
		inline constexpr const char* kGameplayVault = "gameplayvault";
		inline constexpr const char* kName = "Name";
		inline constexpr const char* kCollectionName = "CollectionName";
		inline constexpr const char* kTemplate = "Template";
	} // namespace gameplay

	// Analysis of a loaded gameplay class, for browsing and for the editor's
	// pickers. Rebuild it after structural changes; it holds raw pointers.
	class GameplayModel
	{
	public:
		// Empty (valid() false) when the database has no gameplay class.
		explicit GameplayModel(const Database& db);

		bool valid() const
		{
			return cls_ != nullptr;
		}
		ClassDef* cls() const
		{
			return cls_;
		}

		// The folder tree by name. Nodes without a collection are path prefixes
		// no element has.
		struct Node
		{
			std::string segment; // last path part
			std::string path;    // full path
			Collection* collection = nullptr;
			std::vector<Node*> children; // sorted by segment
			Node* parent = nullptr;
			size_t elementCount = 0; // collections in this subtree, this one included
		};
		const Node& root() const
		{
			return *root_;
		}
		const Node* node(const std::string& path) const;
		const Node* nodeOf(const Collection* c) const;

		// Template collections (no '/' in the name, or Template set), sorted.
		const std::vector<Collection*>& templates() const
		{
			return templates_;
		}
		bool isTemplate(const Collection* c) const;

		// The element that lists `c` in its Children, else the element at its
		// path's parent, else null.
		Collection* owner(const Collection* c) const;
		// Elements listed in `c`'s Children that exist, in list order.
		std::vector<Collection*> listedChildren(const Collection* c) const;
		// The vault named by `c`'s gameplayvault (own or inherited); empty if none.
		std::string childrenVaultName(const Collection* c) const;

		// A reference field: where its values point, learned from the data.
		struct RefField
		{
			bool isText = false;          // a Text holding a path (else GCollectionKey)
			uint32_t targetClass = 0;     // class most values resolve in
			std::vector<Collection*> templates; // templates of the targets, most used first
		};
		// For GCollectionKey fields of the gameplay class and for Text fields
		// whose values are (nearly) all element paths.
		const RefField* refField(uint32_t fieldKey) const;

		// The collection a GCollectionKey value names: in the field's target
		// class first, then gameplay, then any class.
		Collection* resolveKey(uint32_t key, const RefField* field) const;

		// Candidates for a reference picker: collections of the field's target
		// class whose template chain includes one of its usual templates (all of
		// the class when there's no data to go by), sorted by name.
		std::vector<Collection*> candidates(const RefField& field, bool all) const;
		bool derivesFrom(const Collection* c, const Collection* templ) const;

		// State graph pieces. A template's own states are those whose
		// templateref names it. An activity runs its own and its templates'
		// (GActivity::CollectionIsStateForActivity follows the activity's
		// parents): Carbon's career_game runs career's "initial".
		std::vector<Collection*> statesOf(const Collection* activityTemplate) const;
		std::vector<Collection*> allStatesOf(const Collection* activityTemplate) const; // its own first
		Collection* stateTemplate(const Collection* state) const;                     // what its templateref names
		// What ChangeState calls a state: its Name (GActivity::GetStateByName),
		// else its collection name without "stategraph_<template>_".
		std::string stateName(const Collection* state) const;
		// Every handler whose stateref is the state, whatever activity owns it.
		std::vector<Collection*> handlersOf(const Collection* state) const;
		// The ones that run while `activity` runs the state: their handler_owner
		// is the activity, one of its templates, or an element with its
		// CollectionName (GActivity::CollectionIsHandlerForState). Pro Street's
		// lapknockout runs basicrace's states, and one handler of theirs is
		// lapknockout's own.
		std::vector<Collection*> handlersOf(const Collection* state, const Collection* activity) const;
		bool handlerCountsFor(const Collection* handler, const Collection* activity) const;
		Collection* handlerOwner(const Collection* handler) const; // what its handler_owner names
		// What the State Graph Builder shows a graph for, by name: templates
		// that run states (their own or their templates', as the race modes
		// under basicrace run its), and elements that own handlers (a race with
		// a script of its own).
		std::vector<Collection*> graphActivities() const;

	private:
		const Database& db_;
		ClassDef* cls_ = nullptr;
		std::unique_ptr<Node> root_;
		std::vector<std::unique_ptr<Node>> nodes_;
		std::unordered_map<std::string, Node*> byPath_;
		std::unordered_map<const Collection*, Node*> byCollection_;
		std::unordered_map<const Collection*, Collection*> owners_;
		std::vector<Collection*> templates_;
		std::unordered_set<const Collection*> templateSet_;
		std::map<uint32_t, RefField> refFields_;
		std::unordered_map<const Collection*, std::vector<Collection*>> states_, handlers_;
		std::unordered_map<const Collection*, Collection*> stateTemplates_;
		std::unordered_map<const Collection*, Collection*> handlerOwners_;
		std::unordered_set<const Collection*> ownsHandlers_;
	};

	// --- edits -------------------------------------------------------------------
	// They keep the gameplay conventions: Name/CollectionName = the path, the
	// element listed in its owner's Children and stored in the owner's
	// gameplayvault vault (created in the owner's pack when missing). They throw
	// std::runtime_error with a readable message.

	struct NewGameplayElement
	{
		std::string path;              // full name, e.g. "prostreet_career/bins/race_bin_an_2_1/gr_3"
		Collection* templ = nullptr;   // the AttribSys parent
		Collection* owner = nullptr;   // the folder element that lists it (may be null)
		std::string childrenVault;     // its gameplayvault ("" leaves it unset)
	};
	// Where a new element under `owner` is stored: the owner's gameplayvault
	// vault (an empty string when the owner names none) and whether it exists yet.
	std::string StorageVaultName(const GameplayModel& m, const Collection* owner);

	Collection& CreateGameplayElement(Database& db, const GameplayModel& m, const NewGameplayElement& e);

	// Copies `src` and, with `withChildren`, everything listed under it (its
	// Children, recursively) to `newPath`. Descendants' paths and the subtree's
	// own gameplayvault vaults are renamed by replacing src's last path part with
	// the new one (new vaults are created). References inside the subtree
	// (GCollectionKey values and path Texts) are re-pointed at the copies. Copied
	// message handlers are recompiled with their new function name (h_ + hash of
	// the new path); one that can't be keeps its old Lua and gets a line in
	// `warnings`.
	Collection& DuplicateGameplayElement(Database& db, const GameplayModel& m, Collection& src, const std::string& newPath,
										 bool withChildren, std::vector<std::string>* warnings = nullptr);

	// Recompiles a handler's Lua with its function renamed to match the handler's
	// (current) name: decompile, rename `oldFunction`, compile. Returns false with
	// `error` set when that fails; the bytecode is then unchanged.
	bool RenameHandlerFunction(Database& db, Collection& handler, const std::string& oldFunction, std::string* error);

	// The function a handler named `handlerName` defines, by the database's game:
	// h_<hash> (lua::HandlerFunctionName), or in Most Wanted the name itself.
	std::string HandlerFunction(const Database& db, std::string_view handlerName);

	// Deletes `c` and everything listed under it, takes it out of its owner's
	// Children, and removes vaults left empty that the subtree owned. Throws if
	// some collection in the subtree is the template of another collection.
	// Returns the deleted collections (pointers are dangling afterwards).
	std::vector<Collection*> DeleteGameplayElement(Database& db, const GameplayModel& m, Collection& c);

	// Every collection under `c` by Children lists, depth first, `c` first.
	std::vector<Collection*> GameplaySubtree(const GameplayModel& m, Collection& c);

	// What a folder of the gameplay view holds, for a script that makes it: the
	// elements under its path (the folder's own first) and everything their
	// Children lists lead to. The two mostly agree, but a race can list
	// checkpoints kept under its track's path, and some elements no owner lists.
	// In path order, each element followed by what it lists.
	std::vector<Collection*> GameplayFolderContents(const GameplayModel& m, const GameplayModel::Node& folder);

	// --- messages and handlers ---------------------------------------------------

	// A Hermes message a handler can answer. message_id = StringHash32(name); its
	// Lua sees the fields as message.<field>. The games share message names, so
	// every list names message_ids in every game; a game's fields can differ.
	struct MessageDef
	{
		std::string name;                                        // "MNotifyEscaped"
		std::vector<std::pair<std::string, std::string>> fields; // {"Reason", "int"}
		Game game = Game::Unknown; // whose header it is from; Unknown for inferred.txt's
	};
	// data/messages/<game>_<platform>.txt (scripts/gen_messages.py) and
	// inferred.txt (names found from handler names, scripts/infer_messages.py):
	// one message per line, "<class> <field>:<type> ...", '#' comments. The file
	// name's first word sets `game`. Throws if unreadable.
	std::vector<MessageDef> LoadMessages(const std::string& path);
	// One entry per name, by name: `game`'s own, else the one with the most fields.
	std::vector<const MessageDef*> MessagesFor(const std::vector<MessageDef>& all, Game game);

	// Gives `c` its own value of a single GCollectionKey / Text field.
	void SetGameplayKey(Database& db, Collection& c, const char* field, uint32_t key);
	void SetGameplayText(Database& db, Collection& c, const char* field, const std::string& text);

	// The compiled Lua chunk in a handler's bytecode blob (unpacked from its
	// LZHeader block); empty when it has none.
	Bytes HandlerChunk(const Collection& handler);
	// The Lua version a database's handlers are compiled with: 5.1 for
	// current-gen Undercover (PC, 360, PS3) and World, 5.0 for the others,
	// Undercover's Wii and PS2 version included (Carbon's engine). A chunk that
	// is there already is read and recompiled by its own header's version
	// (VersionOfChunk), so the two never mix.
	lua::Version LuaVersionOf(const Database& db);
	lua::Version VersionOfChunk(const Database& db, ByteSpan chunk); // its header's, else LuaVersionOf
	// Stores a compiled chunk as the handler's bytecode, packed the way the game's
	// LZCompress does (Compression.h, CompressBest).
	void SetHandlerChunk(Database& db, Collection& handler, ByteSpan chunk);
	// Sets the handler's transitionlist (GCollectionKey[]) to `states`; an empty
	// list removes the handler's own value.
	void SetHandlerTransitions(Database& db, Collection& handler, const std::vector<uint32_t>& states);
	// The state names a script switches to: the string arguments of its
	// ChangeState( "name" ) calls, in order, without repeats.
	std::vector<std::string> ChangeStateTargets(std::string_view source);

} // namespace attrib
