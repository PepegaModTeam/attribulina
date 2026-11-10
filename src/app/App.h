#pragma once

// The editor: menus, the dock layout and every panel. Platform independent;
// a Host (src/platform/...) owns the window, the renderer and native dialogs.

#include "CodeEditor.h"
#include "Host.h"
#include "ValueEditors.h"

#include "Search.h"

#include "core/ClassDesign.h"
#include "core/Clip.h"
#include "core/Edit.h"
#include "core/GenHeader.h"
#include "core/Database.h"
#include "core/Gameplay.h"
#include "core/Nfsms.h"
#include "core/Script.h"

#include "imgui.h"

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace app
{

	enum class LogLevel
	{
		Info, Warning, Error
	};

	struct LogLine
	{
		LogLevel level;
		std::string text;
	};

	// Inspector view options (View menu; tests set them directly).
	struct ViewOptions
	{
		bool showInherited = true;
		bool showUnset = false;
		bool expandArrays = false;
		// The gameplay class as folders and templates, with references resolved
		// (View > Gameplay View).
		bool gameplayView = true;
	};

	// True on the frame Enter activates the item `id` (keyboard navigation).
	// Space activates too but expands tree nodes; Enter opens collections.
	bool NavEnterOn(ImGuiID id);
	// An ImGui id for a class or collection key. Trees use keys, not pointers, so
	// their nodes stay open when an undo rebuilds the database.
	inline const void* KeyId(uint32_t key)
	{
		return reinterpret_cast<const void*>(uintptr_t(key));
	}
	// Enter in a confirmation prompt, called right after its Cancel button: true
	// unless the keyboard is on Cancel (or the prompt only just opened).
	bool ConfirmKey();
	// Enter in a prompt whose safe answer is Cancel (unsaved changes), called
	// right after its Cancel button: true unless the keyboard was moved to
	// another button, which ImGui then presses.
	bool CancelKey();

	class App
	{
	public:
		explicit App(Host& host);
		~App();
		App(const App&) = delete;
		App& operator=(const App&) = delete;

		// Once, after ImGui::CreateContext() and before the first frame: style,
		// fonts and the settings handler. dpiScale 1.0 = 96 DPI. An empty iniPath
		// disables the .ini (tests).
		void setup(float dpiScale, const std::string& iniPath);
		// One frame of UI, between ImGui::NewFrame() and ImGui::Render().
		void frame();
		// The window's close button. Returns true when the host may close now;
		// otherwise the unsaved-changes prompt is shown and quits by itself.
		bool requestClose();

		// Commands (menus call these; so do tests).
		// Opens a pack file or a folder. When it belongs to another database than
		// the open one, nothing is opened: the "Another Database" prompt asks to
		// close the open one first (or open it in a new window), and this
		// returns false. So does a new database whose files don't say which game
		// and platform they are from (or any, with detection off): the "Game and
		// Platform" prompt asks first.
		bool open(const std::string& path);
		void requestOpenFolderDialog();
		void requestOpenFileDialog();
		// Why `path` can't join the open database: another folder, another
		// database file, or vaults that are open already. Nothing when it can.
		std::optional<std::string> conflictWith(const std::string& path) const;
		// A new database's game and platform come from its files (core/Game.h).
		// With detection on (the default), the "Game and Platform" prompt asks
		// only when the files don't say (Most Wanted's have no Vers value); off,
		// it asks every time, with what the files say chosen. DetectGame= in the
		// .ini.
		bool detectTarget() const
		{
			return detectTarget_;
		}
		void setDetectTarget(bool on);
		// Edit > Allow Arrays Past Their Limit. Off (the default), an entry array
		// stops at the count its definition allows (GEAR_RATIO's 9); on, it may
		// hold more, with a warning. Layout arrays keep their space either way,
		// and scripts may always go past, with a warning. ArraysPastLimit= in the .ini.
		bool arraysPastLimit() const
		{
			return arraysPastLimit_;
		}
		void setArraysPastLimit(bool on)
		{
			arraysPastLimit_ = on;
		}
		// Opens a new database as this game and platform, without asking.
		bool openAs(const std::string& path, const attrib::Target& target);
		// The folder the open database (attributes.bin) was loaded from.
		std::string databaseFolder() const;
		// File > Save All writes every open pack (the default), so a file changed
		// or deleted outside the editor is written again. With File > Save Only
		// Changed Packs on, it writes the packs with unsaved changes and is greyed
		// out while there are none. SaveOnlyChanged= in the .ini.
		bool saveAll();
		bool saveOnlyChanged() const
		{
			return saveOnlyChanged_;
		}
		void setSaveOnlyChanged(bool on)
		{
			saveOnlyChanged_ = on;
		}
		// The colour scheme (View > Theme; Theme= in the .ini, Dark by default).
		Theme theme() const
		{
			return theme_;
		}
		void setTheme(Theme t);
		bool savePackAs(attrib::LoadedPack& pack, const std::string& path);
		// Files of packs the database no longer has (World event vault files
		// whose vault was deleted): Save All removes them.
		std::vector<std::string> removedFiles() const;
		bool removeFile(const std::string& path);
		// How a compressed pack (gameplay.lzc) is saved: JDLZ as retail files are,
		// HUFF, or RAWW (stored). It starts as the file is; a change is an undo step.
		void setPackCompression(attrib::LoadedPack& pack, attrib::LzKind kind);
		// The compression of the pack's file as last opened or saved (none: not known).
		std::optional<attrib::LzKind> savedCompression(const attrib::LoadedPack& pack) const;
		// After a value of `c` changed: its pack needs saving (and the recorder notes it).
		void markDirty(attrib::Collection* c);
		void closeAll();
		// File > Load Key Names: adds the file's names to the open database and
		// to user_keys.txt (those it lacks that the program's attrib_keys.txt
		// doesn't have), so later sessions know them too. The program's list
		// stays as it is.
		bool loadKeyNames(const std::string& path);
		// Adds a name the user typed to the dictionary and to user_keys.txt in
		// the user's folder (Host::userFolder), so it is known again next time.
		void rememberName(const std::string& name);
		const std::string& userKeysPath() const
		{
			return userKeysPath_;
		}
		// After setup(); an empty path keeps typed names for this run only (tests).
		void setUserKeysPath(std::string path)
		{
			userKeysPath_ = std::move(path);
		}
		const std::vector<std::string>& recentFiles() const
		{
			return settings_.recent;
		}
		// The host's window position, kept in the .ini as text the host writes and reads.
		const std::string& windowPlacement() const
		{
			return settings_.windowPlacement;
		}
		void setWindowPlacement(std::string text);
		// Which panels are open, as the .ini keeps it: "Explorer,Collection,...".
		std::string openPanels() const;
		void setOpenPanels(const std::string& list);
		// Shows `c` in the current tab (opening a first tab if there is none).
		void select(attrib::Collection* c, bool reveal = false);
		// Opens `c` in a new tab after the current one; `activate` switches to it
		// (middle-click opens in the background, like a browser).
		void openInNewTab(attrib::Collection* c, bool activate = false);
		void closeTab(size_t index);
		// Deletes a collection (it must have no children); tabs showing it move
		// to its parent.
		bool removeCollection(attrib::Collection* c);
		// Deletes a collection and everything under it.
		bool removeTree(attrib::Collection* c);

		// The collection clipboard. The text goes on the system clipboard, so
		// another window can paste it. A cut is a move: pasting it in this window
		// puts the collection under the paste target; another window pastes a copy.
		bool copyCollection(attrib::Collection* c, bool withChildren, bool cut);
		// Pastes next to `target` (its parent's child), as its child (asChild),
		// or with no target as a root collection of `cls`. False with a log line
		// saying why when nothing was pasted.
		bool paste(attrib::Collection* target, attrib::ClassDef* cls, bool asChild);
		// A copy of `c` next to it, without going through the clipboard.
		attrib::Collection* duplicate(attrib::Collection* c);
		// A copy of `src` (and everything under it) under `parent`, in its vault.
		attrib::Collection* copyHere(attrib::Collection* src, attrib::Collection* parent, bool withChildren);
		// After a paste of `clip`: each copy's CollectionName takes its name, and
		// a copied gameplay handler's Lua function follows (Most Wanted finds it
		// by CollectionName, the later games by the name's hash).
		void copiesTakeTheirNames(const attrib::Clip& clip, const attrib::PasteResult& r);
		// Puts `c` under `newParent` (null: a root collection).
		bool moveCollection(attrib::Collection* c, attrib::Collection* newParent);
		bool isCut(const attrib::Collection* c) const;
		// Identifies this window in clipboard text.
		uint64_t instanceId() const
		{
			return instanceId_;
		}

		// One inspector tab: a collection and its own Back/Forward history.
		struct Tab
		{
			int id = 0;
			attrib::Collection* collection = nullptr;
			std::vector<std::pair<uint32_t, uint32_t>> history; // class, collection
			int historyPos = -1;
		};
		const std::vector<Tab>& tabs() const
		{
			return tabs_;
		}
		size_t activeTab() const
		{
			return activeTab_;
		}

		attrib::Database* database()
		{
			return db_.get();
		}
		attrib::Collection* selection() const
		{
			return selected_;
		}
		const std::deque<LogLine>& log() const
		{
			return log_;
		}
		bool hasUnsavedChanges() const;
		ViewOptions& viewOptions()
		{
			return view_;
		}
		DisplayPrefs& displayPrefs()
		{
			return prefs_;
		}
		// The open database's favourites. favorites.txt beside the .ini keeps a
		// list per database folder (where its attributes.bin is), re-read
		// before each change and written at once, as the recent files are.
		struct Favorite
		{
			uint32_t cls = 0, key = 0;
			std::string name; // "class/collection" as it was added, for one no longer there
		};
		const std::vector<Favorite>& favorites() const
		{
			return favorites_;
		}
		bool isFavorite(const attrib::Collection* c) const;
		void toggleFavorite(const attrib::Collection* c);
		void removeFavorite(uint32_t cls, uint32_t key);
		// The gameplay model (null without a gameplay class); rebuilt with the caches.
		const attrib::GameplayModel* gameplay() const
		{
			return gameplay_.get();
		}
		// Shows the State Graph Builder on the state graph `c` belongs to (an
		// activity template, one of its states or a message handler).
		void openStateGraphBuilder(attrib::Collection* c);
		// State Graph Builder state, for tests.
		attrib::Collection* sequenceActivity() const
		{
			return sbActivity_;
		}
		attrib::Collection* sequenceState() const
		{
			return sbState_;
		}
		attrib::Collection* sequenceHandler() const
		{
			return sbHandler_;
		}
		struct GraphBox
		{
			attrib::Collection* state = nullptr;
			ImVec2 min, max; // screen position
		};
		const std::vector<GraphBox>& sequenceGraph() const
		{
			return sbBoxes_;
		} // as last drawn
		std::string& sequenceSource()
		{
			return sbSource_;
		}
		// Compiles the edited source into the selected handler; false with
		// sequenceStatus() saying why.
		bool compileHandler();
		const std::string& sequenceStatus() const
		{
			return sbStatus_;
		}
		const std::vector<attrib::MessageDef>& messages() const
		{
			return messages_;
		}

		// The Lua functions the State Graph Builder lists and colours, for the open
		// database's game: the user's own (user_lua_<game>.txt in the user's folder)
		// first, then the game's list (data/lua/<game>_<platform>.txt: this
		// platform's, else the PC's, else any of the game's). A user entry
		// replaces a listed one of the same name.
		struct LuaEntry
		{
			std::string kind, name, signature, description;
			bool user = false; // from the user's file
		};
		// A line of a list: "<kind> <name> <signature>  # description". Nothing for
		// blank and comment lines; with `error`, a line that isn't one says why there.
		static std::optional<LuaEntry> ParseLuaLine(std::string line, std::string* error = nullptr);
		const std::vector<LuaEntry>& luaFunctions() const
		{
			return luaApi_;
		}
		const std::string& luaFunctionList() const
		{
			return luaList_;
		} // "prostreet_pc", "" for none
		// Where user_lua_<game>.txt goes: the user's folder (Host::userFolder)
		// unless set (tests point it elsewhere).
		void setUserLuaFolder(const std::string& dir)
		{
			userLuaDir_ = dir;
		}
		std::string userLuaPath(attrib::Game game) const;
		// The window that edits the user's functions for the open game.
		void openUserLuaFunctions();
		std::string& userLuaText()
		{
			return userLuaText_;
		}
		const std::map<int, std::string>& userLuaErrors() const
		{
			return userLuaErrors_;
		}
		bool saveUserLuaFunctions(); // writes the window's text, then lists it

		// What the Edit menu and the Explorer's shortcuts act on: the Explorer
		// item with the keyboard focus (a collection, or a class node), or the
		// collection in front when the inspector was used last.
		struct EditTarget
		{
			attrib::Collection* collection = nullptr;
			attrib::ClassDef* cls = nullptr;
			// Gameplay elements and templates in the gameplay view follow the
			// gameplay commands (Duplicate, Delete Element, ...).
			enum class Kind
			{
				Plain, Element, Template
			} kind = Kind::Plain;
			bool empty() const
			{
				return !collection && !cls;
			}
		};
		EditTarget editTarget() const;

		// Mod scripts (AppScript.cpp).
		const std::string& scriptText() const
		{
			return scriptText_;
		}
		// Puts `text` in the Script panel, as saved to `path` (none: not saved yet).
		void setScriptText(std::string text, std::string path = {});
		// Adds statements to the Script panel's script; edits of a collection it
		// edits already are merged into that block. False when its text doesn't read.
		bool addToScript(const std::vector<attrib::script::Statement>& statements, bool show = true);
		void addCollectionsToScript(const std::vector<attrib::Collection*>& roots, bool withChildren,
									const attrib::script::ExportSettings& settings = {});
		// `create vault` (not for the database's own vault), and with
		// `withCollections` every collection in it.
		void addVaultToScript(const attrib::Vault& vault, bool withCollections);
		// Gameplay elements (GameplayFolderContents) with the vaults they are in
		// and their owners' Children lists (script::ExportGameplay).
		void addGameplayToScript(const std::vector<attrib::Collection*>& elements);
		// Reads `text` and the scripts it includes (relative paths from `folder`)
		// and opens the Apply Script prompt (its options, then Apply).
		bool requestApplyScript(const std::string& text, const std::string& source, const std::string& folder = {});
		// Applies `s`: on a copy of the database first, so a script that stops
		// changes nothing. With dryRun, only the copy.
		attrib::script::ApplyReport applyScript(const attrib::script::Script& s, const attrib::script::ApplySettings& settings,
												bool dryRun);
		// Adds what changed since the files were saved (or since the .bak
		// originals) to the Script panel. False when nothing did.
		bool exportChanges(bool originals, bool recordWas);
		// The Script panel's console line: runs one statement (or a few) on the open database.
		bool runConsole(const std::string& line);

		// nfsms (AppNfsms.cpp): its own window, for scripts from NFS-VltEd and Attribulator.
		const std::string& nfsmsText() const
		{
			return nfsmsText_;
		}
		void setNfsmsText(std::string text, std::string path = {});
		// Runs lines first..last of the nfsms window (0, 0: all). Running all of
		// a script with a setup opens the setup prompt first.
		bool runNfsms(int firstLine, int lastLine);
		// The nfsms setup's choices, for the next run (tests set them).
		attrib::nfsms::Choices& nfsmsChoices()
		{
			return nfsmsChoices_;
		}
		void setNfsmsStrict(bool on)
		{
			nfsmsStrict_ = on;
		}
		// A script the window's script includes, in a tab of its own (opened from
		// a message, or by itself when a run fails in it). Runs read the tab's
		// text, saved or not. Null when it isn't open.
		std::string* nfsmsIncludedText(const std::string& path);
		bool openNfsmsIncluded(const std::string& path, int line = 0, bool focus = false);
		// The window's script as a mod script, into the Script panel (nfsms::ToModScript).
		bool convertNfsms();
		// The tab in front: "" for the window's script, else an included file's path.
		const std::string& nfsmsActiveTab() const
		{
			return nfsmsActive_;
		}

		// Classes (AppClass.cpp). The Class Designer makes a class, changes one
		// made for a mod, or shows a class of the game (which stays as it is: its
		// compiled code reads it) to copy, and writes its Attrib::Gen header.
		struct ClassDesigner
		{
			bool open = false;
			bool focus = false;      // bring the window to front on the next frame
			bool headerTab = false;  // with its header tab in front
			bool fieldsTab = false;  // with its fields in front
			attrib::ClassDesign draft;
			uint32_t editing = 0;    // the class it changes; 0: a new one
			bool readOnly = false;   // a class of the game
			attrib::GenHeaderOptions header;
			std::string typeFilter;
		};
		// `cls` in the designer (null: a new class); `header` brings its header to front.
		void openClassDesigner(const attrib::ClassDef* cls, bool header = false);
		ClassDesigner& classDesigner()
		{
			return designer_;
		}
		// Makes the designer's class, or changes the class it edits. False, with
		// a log line, when the design has a problem.
		bool applyClassDesign();
		// Deletes a class and every collection of it (the menus ask first).
		bool removeClass(attrib::ClassDef* cls);
		// Puts a class of the game back as its retail files define it (ClassList.h).
		bool restoreGameClass(uint32_t classKey);
		// The header the designer shows: its class laid out, or "" with a design problem.
		std::string designerHeader();

		// Recording changes made in the editor as script statements: into the
		// Script panel (amod) and/or the nfsms window. Recording starts from a
		// copy of the database taken when it is switched on.
		void setRecording(bool amod, bool nfsms);
		// Turns the changes since the last call into statements (frame() calls it).
		void flushRecording();

		// Undo (Edit menu, Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z): the database after
		// each of the last kUndoSteps actions, kept as the packs' bytes. An action
		// ends when no widget is active any more (a drag, typing), so one drag is
		// one step. Undo rebuilds the database from the bytes, as Blender's
		// undo reloads its file in memory.
		static constexpr int kUndoSteps = 20;
		bool undo();
		bool redo();
		bool goToUndo(int index); // a state in undoLabels()
		// The states, oldest first: the opening, then each action's name.
		std::vector<std::string> undoLabels() const;
		int undoIndex() const
		{
			return undoPos_;
		}
		// Names the action in progress (the first name given wins).
		void nameAction(const std::string& label);
		// Ends the action in progress (frame() calls it; force: even mid-drag).
		void commitUndo(bool force = false);
		// A name typed into a StringKey (EditEnv::typingName): its box, the text
		// it held when typing started and the text now, remembered once another
		// item has the keyboard.
		struct TypedName
		{
			ImGuiID box = 0;
			std::string original, text;
		} typedName_;
		void commitTypedName();

	private:
		struct Settings
		{
			// KeysFile= from an older version: the list File > Load Key Names
			// chose, which then replaced attrib_keys.txt. setup moves its names
			// to user_keys.txt, and it isn't written again.
			std::string oldKeysFile;
			std::vector<std::string> recent;
			// Favorite= lines from an older version, which kept one list for
			// every database; moved to favorites.txt (moveOldFavorites).
			std::vector<std::pair<uint32_t, uint32_t>> oldFavorites; // class, collection
			std::string windowPlacement;
		};

		// Row decorations for a top-level field.
		struct RowExtras
		{
			const attrib::FieldDef* def = nullptr;
			const attrib::Collection* inheritedFrom = nullptr; // set when the value is inherited
			attrib::Attribute* array = nullptr;                // set for an array element row
			uint32_t element = 0;
		};
		// Structural edits found while drawing, applied after the table.
		struct PendingEdit
		{
			enum class Kind
			{
				None, Override, Set, Revert, InsertElement, RemoveElement
			} kind = Kind::None;
			const attrib::FieldDef* def = nullptr;
			uint32_t field = 0;
			uint32_t element = 0;
		};

		void log(LogLevel level, std::string text);
		// After collections, classes or the database itself change: rebuilds the
		// caches right away, so panels drawn later in the same frame never walk
		// pointers into a database that was just replaced.
		void structureChanged()
		{
			++modelVersion_;
			recordStructure_ = true;
			noteEdit(nullptr, nullptr); // for undo; which pack isn't known: every edited one is kept
			rebuildCaches();
		}
		void ensureDatabase();
		bool load(const std::string& path); // into the open database (or a new one)
		std::string nfsmsUnavailable() const; // why nfsms can't run on this database, or ""
		bool openFile(const std::string& path);
		bool openFolder(const std::string& path);
		void checkPackTwin(attrib::LoadedPack& pk);
		std::string recentPath() const;
		void loadRecent();
		void saveRecent();
		void addRecent(const std::string& path);
		void clearRecent();
		std::string favoritesPath() const;
		void useFavoritesOf(const std::string& folder);
		void moveOldFavorites();
		// Applies a change to the open database's list as favorites.txt holds it now.
		void changeFavorites(const std::function<void(std::vector<Favorite>&)>& change);
		void updateTitle();
		std::string title_; // as last given to the host
		void rebuildCaches();

		void drawMenuBar();
		void drawStatusBar();
		void drawDockSpace();
		void drawExplorer();
		void drawClassTree();
		void drawCollectionNode(attrib::Collection* c);
		void drawVaultList();
		void drawFilteredList();
		// treeNode: the collection's node in a tree (0 for a list row), and whether it is open.
		void classContextMenu(attrib::ClassDef* cls, ImGuiID treeNode, bool open);
		void collectionContextMenu(attrib::Collection* c, ImGuiID treeNode = 0, bool open = false);
		// Expand All / Collapse All. A node's open state lives in its window's
		// storage under the id its ID stack gives it, so a request is applied
		// where the node is drawn next frame (takeTreeToggle), and the walks below
		// set every node under it from there without drawing them.
		enum class Tree
		{
			None, Classes, Gameplay, Files
		};
		struct TreeToggle
		{
			ImGuiID id = 0;    // the node, 0 for none
			bool open = false; // what it and everything below it become
		};
		void treeToggleItem(ImGuiID node, bool open); // the node menu's item: Collapse All when open, else Expand All
		void treeBackgroundMenu(Tree t);              // a tree's own menu (right-click below its nodes)
		void noteTreeFocus(ImGuiID node, bool open);  // after a tree node: Shift+A acts on it while it has the keyboard
		bool takeTreeToggle(ImGuiID node, bool& open);
		bool takeTreeToggleAll(Tree t, bool& open);
		void setClassOpen(attrib::ClassDef* cls, bool open);
		void setCollectionOpen(attrib::Collection* c, bool open);
		TreeToggle treeToggle_, treeFocus_;
		// Another database starts with every tree closed: each tree clears its
		// open nodes the next time it is drawn (resetTree, after its BeginChild).
		unsigned treesToReset_ = 0;
		void resetTree(Tree t);
		Tree treeToggleAll_ = Tree::None;
		bool treeToggleAllOpen_ = false;
		int treeToggleFrame_ = -1; // the frame the request was made in; it lapses after the next
		void drawInspector();
		void drawTabBar();
		void drawNavigation(attrib::Collection* c);
		void drawTemplateBreadcrumb(attrib::Collection* c);
		void drawCollectionHeader(attrib::Collection* c);
		// Click handling shared by every collection link: middle-click opens a
		// background tab. Call right after the item.
		void linkClicks(attrib::Collection* c);
		// Drops pointers to a collection about to be deleted; tabs showing it
		// move to `fallback` (its parent when null) or close.
		// With parentFallback false, a null fallback closes those tabs.
		void forgetCollection(attrib::Collection* c, attrib::Collection* fallback = nullptr, bool parentFallback = true);
		void drawFieldRow(const attrib::FieldDef& f);
		EditEnv editEnv(attrib::Collection* c);
		// One value as table rows: its row, then member/element rows when expanded.
		bool drawValueRows(EditEnv& env, const std::string& label, attrib::DataBlock& b, uint32_t off, const ValueType& vt,
						   bool editable, const RowExtras* extras = nullptr);
		bool drawMemberRows(EditEnv& env, const attrib::StructDef& st, attrib::DataBlock& b, uint32_t off, bool editable);
		bool drawPointerRows(EditEnv& env, const attrib::MemberDef& m, const attrib::StructDef& parent, attrib::DataBlock& b,
							 uint32_t structOff, bool editable);
		bool drawArrayRows(EditEnv& env, const std::string& label, attrib::Attribute& a, const ValueType& vt, bool editable,
						   const RowExtras& extras);
		void rowDecorations(const RowExtras* extras, const std::string& label);
		void applyPendingEdit();
		void stepHistory(int delta);
		void jumpHistory(int pos); // to that entry of the active tab's history
		void drawHistoryMenu(int dir); // the entries behind Back (-1) or Forward (+1)
		void drawClassPanel();
		void drawLog();
		void drawModals();

		// Gameplay view (AppGameplay.cpp).
		bool isGameplay(const attrib::Collection* c) const;
		void drawGameplayTab();
		void drawGameplayFolder(const attrib::GameplayModel::Node* n);
		void drawGameplayTemplate(attrib::Collection* t, const std::unordered_map<const attrib::Collection*, std::vector<attrib::Collection*>>& kids);
		void gameplayContextMenu(attrib::Collection* c, const attrib::GameplayModel::Node* folder, ImGuiID treeNode, bool open);
		void drawGameplayHeader(attrib::Collection* c);
		void drawGameplayBreadcrumb(attrib::Collection* c);
		void drawGameplayModals();
		void requestGameplayNew(attrib::Collection* owner, attrib::Collection* templ);

		// State Graph Builder (StateGraphBuilder.cpp).
		void drawStateGraphBuilder();
		void chooseLuaFunctions(); // luaApi_ for the open game (frame() calls it when the game changes)
		void drawUserLuaWindow();
		void drawStateGraph(const std::vector<attrib::Collection*>& states);
		void drawHandlerList();
		void drawCodeEditor();
		void loadHandlerSource();
		void drawSequenceModals();
		std::string stateShortName(const attrib::Collection* state) const;
		attrib::Collection* stateNamed(const std::string& shortName) const;

		// Edit commands (AppEdit.cpp).
		EditTarget targetFor(attrib::Collection* c, attrib::ClassDef* cls) const;
		void drawEditMenu();
		void drawGoMenu();
		// The edit commands for `t`, for the Edit menu and the context menus.
		void drawEditItems(const EditTarget& t);
		void globalShortcuts();
		void switchTab(int delta);
		// The Explorer's Ctrl+C, Delete, F2, ... on the focused item. Only while
		// the Explorer has the keyboard and no text field is being edited.
		void explorerShortcuts();
		void commandCopy(const EditTarget& t, bool withChildren);
		void commandCut(const EditTarget& t);
		void commandPaste(const EditTarget& t, bool asChild);
		void commandDuplicate(const EditTarget& t);
		void commandDelete(const EditTarget& t);
		void commandRename(const EditTarget& t);
		void commandNew(const EditTarget& t);
		void commandFindReferences(const EditTarget& t);
		// The clipboard's collections, decoded once per clipboard text.
		const attrib::Clip* clipboardClip();
		bool canPaste(const EditTarget& t, bool asChild);
		// Enter or a click in a list: opens `c`, in a background tab with Ctrl.
		void openItem(attrib::Collection* c);
		// A search hit's field: shown highlighted for a moment (call before the row's tree node).
		void markHighlightedRow(bool expands);
		// Drag and drop in the class tree.
		void dragSource(attrib::Collection* c);
		void dropTarget(attrib::Collection* newParent, attrib::ClassDef* cls);
		void drawDropMenu();
		void drawEditModals();
		void requestGameplayDuplicate(attrib::Collection* c);
		// Vault commands (AppEdit.cpp), on the Files tree's menus: a new vault in
		// a pack, renaming and deleting one, and World's event vault files.
		void drawPackItems(attrib::LoadedPack& pack);
		void drawVaultItems(attrib::Vault& vault);
		void drawNewEventVaultItem();
		bool canMakeEventVaults() const;
		// Deletes `vault` with its collections and everything under them; one of
		// World's event vaults takes its file with it (Save All removes it).
		void deleteVault(attrib::Vault& vault);
		void drawVaultModals();

		// Script panel (AppScript.cpp).
		void drawScriptPanel();
		void drawScriptModals();
		void checkScript();
		void markScriptLines();
		void drawConsole();
	public:
		int consoleHistoryStep(ImGuiInputTextCallbackData* data);
	private:
		bool saveScript(bool as);
		bool loadScriptFile(const std::string& path, std::string& text);
		std::string scriptName() const;
		std::string scriptFolder() const; // where the panel script's includes start ("" before it's saved)

		// nfsms window (AppNfsms.cpp).
		void drawNfsmsPanel();
		void drawNfsmsModals();
		bool runNfsmsScript(const attrib::nfsms::Script& s, int firstLine, int lastLine);
		void appendNfsms(const std::vector<std::string>& lines);
		void followNfsmsLine();
		// update_field lines that set a field (or one element of it) to its value
		// here, in NFS-VltEd's words (the field menu's nfsms items).
		std::vector<std::string> nfsmsLinesFor(attrib::Collection& c, uint32_t field, int element = -1);
		void nfsmsMenuItems(attrib::Collection& c, uint32_t field, int element);
		CodeEditor& nfsmsEditorOf(const std::string& file);
		std::string& nfsmsTextOf(const std::string& file);
		std::string nfsmsFolderOf(const std::string& file) const;
		void markNfsmsLines();
		void drawNfsmsEditor(const std::string& file);
		std::string gameFolder() const;
		void releaseSetupImages();
		// While a script runs: its changes are the script's, not the user's.
		void beginScriptRun();
		void endScriptRun();

		// Search panel (AppSearch.cpp).
		void drawSearch();
		void openSearch();
		void findReferences(uint32_t classKey, uint32_t key);
		void runSearch();

		// Deferred actions so menus/context menus can open modals.
		enum class Pending
		{
			None, NewChild, Rename, Delete, ConfirmClose, ConfirmCloseAll, ConfirmReplace, About,
			GameplayNew, GameplayDuplicate, GameplayDelete, NewState, NewHandler, MoveToVault,
			ApplyScript, ExportChanges, ExportFields, NfsmsSetup, ChooseTarget, DeleteClass,
			NewVault, RenameVault, DeleteVault, NewEventVault
		};
		void request(Pending p, attrib::Collection* target = nullptr);
		void requestNew(attrib::ClassDef* cls, attrib::Collection* parent);

		Host& host_;
		std::unique_ptr<attrib::Database> db_;
		// The active tab's collection (selection in the Explorer, target of the
		// inspector and the menus).
		attrib::Collection* selected_ = nullptr;
		std::vector<Tab> tabs_;
		size_t activeTab_ = 0;
		int nextTabId_ = 1;
		int focusTabId_ = 0; // a tab to bring to front on the next frame
		std::deque<LogLine> log_;
		uint64_t logVersion_ = 0, logShown_ = ~0ull; // log_ changes; the version logText_ is of
		size_t logDropped_ = 0;                     // bytes of logText_ the cap dropped since it was built
		std::string logText_;                       // log_ as one text, for the Log panel
		std::vector<uint8_t> logLevels_;            // per line: 0 info, 1 warning, 2 error
		LogView logView_;
		Settings settings_;
		std::vector<Favorite> favorites_;
		std::string favoritesFor_; // the database folder favorites_ belongs to; empty: none open
		DisplayPrefs prefs_;
		std::vector<std::shared_ptr<const attrib::TypeSchema>> schemas_;
		bool walkingHistory_ = false;
		PendingEdit pendingEdit_;
		std::string iniPath_;
		std::vector<bool> recentThere_; // per recent file, checked when the File menu opens
		std::string userKeysPath_;
		std::unordered_set<std::string> userNames_;
		std::string keysFile_; // the program's list: attrib_keys.txt beside it or in data/
		// Adds names to the open database's dictionaries, and to user_keys.txt
		// those it doesn't hold and keysFile_ doesn't have. Returns how many went
		// to the file.
		size_t keepUserNames(const std::vector<std::string>& names);
		// Appends lines to user_keys.txt, after a newline if its last line has none.
		void appendUserKeys(const std::string& lines);
		float dpiScale_ = 1.0f;

		// Edit commands.
		uint64_t instanceId_ = 0;
		std::unordered_set<uint64_t> cut_; // class << 32 | key of the cut collections and everything under them
		std::string cutText_;              // the clipboard text of the cut
		double cutCheckAt_ = 0;
		std::string clipText_;             // clipboardClip() cache
		std::optional<attrib::Clip> clip_;
		EditTarget explorerFocus_;         // this frame's focused Explorer item
		EditTarget explorerTarget_;        // the last one while the Explorer had the keyboard
		enum class Panel
		{
			None, Explorer, Inspector, Other
		} lastPanel_ = Panel::None;
		attrib::Collection* dropSource_ = nullptr;
		attrib::Collection* dropParent_ = nullptr;
		attrib::ClassDef* dropClass_ = nullptr;
		bool dropOpen_ = false;
		bool focusExplorerFilter_ = false, focusFieldFilter_ = false;
		std::string conflictReason_;
		// Game and platform.
		bool detectTarget_ = true;
		bool arraysPastLimit_ = false;
		bool saveOnlyChanged_ = false;
		attrib::Target lastTarget_;          // chosen last this run
		attrib::Detection targetDetection_;  // what the prompt's files say
		attrib::Target targetChoice_;        // what the prompt has chosen
		std::vector<std::string> queuedOpens_; // files dropped while it asks

		// Search panel.
		bool showSearch_ = false, focusSearch_ = false, bringSearch_ = false;
		SearchQuery searchQuery_;
		std::string searchText_, searchClassFilter_;
		SearchResult searchResult_;
		double searchMs_ = 0;
		bool searchRan_ = false;
		// A field to bring into view in the inspector (a search hit).
		uint32_t highlightField_ = 0;
		double highlightUntil_ = 0;
		bool scrollToField_ = false;
		bool highlightRow_ = false;

		// Script panel.
		bool showScript_ = false, bringScript_ = false;
		bool showNfsms_ = false;
		std::string scriptText_, scriptSaved_, scriptPath_;
		std::string scriptStatus_;
		bool scriptStatusError_ = false;
		std::vector<attrib::script::Message> scriptMessages_;
		std::unique_ptr<attrib::script::Script> pendingScript_; // the Apply Script prompt's
		std::map<std::string, std::string> scriptOptions_;
		std::string scriptApplyError_;
		bool scriptStrict_ = false;
		bool exportOriginals_ = false, exportWas_ = true;
		std::set<uint32_t> exportFields_;
		bool exportExact_ = false, exportEditOnly_ = false;
		std::string exportFilter_;
		CodeEditor scriptEditor_{ CodeLanguage::Amod };
		std::string consoleInput_, consoleStatus_;
		bool consoleError_ = false, focusConsole_ = false;
		std::vector<std::string> consoleHistory_;
		int consoleHistoryPos_ = -1;
		CodeEditor luaEditor_{ CodeLanguage::Lua };
		// The games' Lua APIs (data/lua/*.txt, doc/lua-api.md), by file name
		// ("prostreet_pc"), and the open game's with the user's own (luaFunctions()).
		std::map<std::string, std::vector<LuaEntry>> luaLists_;
		std::vector<LuaEntry> luaApi_;
		std::string luaList_, luaChosenFor_ = "?"; // the list luaApi_ uses; the target id it was chosen for
		std::string userLuaDir_;
		// The user's functions window: the game it edits, its text and what is saved.
		bool showUserLua_ = false;
		attrib::Game userLuaGame_ = attrib::Game::Unknown;
		std::string userLuaText_, userLuaSaved_, userLuaChecked_;
		std::map<int, std::string> userLuaErrors_;
		CodeEditor userLuaEditor_{ CodeLanguage::Plain };
		bool sbShowFunctions_ = false;
		std::string sbFunctionFilter_;

		// nfsms window.
		bool bringNfsms_ = false;
		std::string nfsmsText_, nfsmsSaved_, nfsmsPath_, nfsmsStatus_;
		bool nfsmsStatusError_ = false;
		std::vector<attrib::script::Message> nfsmsMessages_;
		CodeEditor nfsmsEditor_{ CodeLanguage::Nfsms };
		bool nfsmsFollow_ = false;
		// Strict: tried on a copy first and applied only when no line fails.
		// Off (NFS-VltEd's way): line by line on the open database.
		bool nfsmsStrict_ = false;
		std::unique_ptr<attrib::nfsms::Script> pendingNfsms_;
		attrib::nfsms::Choices nfsmsChoices_;
		std::string nfsmsChoicesFor_; // the script nfsmsChoices_ were made for
		std::vector<std::shared_ptr<const attrib::nfsms::PropertyNames>> nfsmsNames_; // data/nfsms
		struct NfsmsFile
		{
			std::string path, name; // the file, and as the including line wrote it (the tab's label)
			std::string text, saved;
			std::unique_ptr<CodeEditor> editor; // its callbacks hold a pointer to it: keep it in place
		};
		std::vector<std::unique_ptr<NfsmsFile>> nfsmsFiles_;
		std::string nfsmsActive_;    // the tab in front ("" the window's script)
		std::string nfsmsSelect_;    // a tab to bring to front next frame ("\n": the window's script)
		std::string nfsmsRunFile_;   // the script the last run's line numbers count in ("" the window's)
		std::string nfsmsClosing_;   // an included tab with unsaved changes the user is closing
		bool nfsmsConvertAsk_ = false; // To Mod Script would replace unsaved text in the Script panel
		struct SetupImage
		{
			std::string path;
			ImTextureData* tex = nullptr;
			int w = 0, h = 0;
		};
		std::vector<SetupImage> setupImages_;
		std::vector<ImTextureData*> retiredTextures_;
		ImVec2 setupSize_;           // the setup prompt's size last frame
		bool setupResized_ = false;  // it changed: centre it again

		// Elements taken off the end of an array by shrinking it in the inspector,
		// last first, so that growing it again brings them back rather than
		// zeros. Kept only while the array still has the length the shrink left
		// (anything else that changes it drops them), and until the database is
		// rebuilt (undo, opening). Keyed by the array: class, collection and field,
		// or the block and offset of a pointer to one.
		struct ArrayTail
		{
			uint32_t countAfter = 0;
			std::vector<attrib::ElementData> removed;
		};
		std::map<std::string, ArrayTail> arrayTails_;

		// Undo history.
		struct UndoState
		{
			std::string label;                                      // the action that led here
			std::vector<std::string> paths;                         // its packs (World's event vault files come and go)
			std::vector<std::shared_ptr<const attrib::Bytes>> packs; // per path; unchanged ones shared
			std::vector<std::optional<attrib::LzKind>> compression; // per pack: how saving compresses it
			std::set<uint64_t> touched;                             // class << 32 | key of what it edited
			bool script = false;                                    // a script run (not recorded)
			std::pair<uint32_t, uint32_t> selBefore{}, selAfter{};  // the collection being edited, before and after
		};
		std::deque<UndoState> undo_;
		int undoPos_ = -1;
		// What each pack's file holds (null: unknown), by path, and how it is
		// compressed. A pack with no entry has no file yet; an entry no pack has
		// is a file saving removes (its event vault was deleted).
		std::map<std::string, std::shared_ptr<const attrib::Bytes>> savedPacks_;
		std::map<std::string, std::optional<attrib::LzKind>> savedCompression_;
		std::set<const attrib::LoadedPack*> undoPacks_;                // edited since the last step
		std::set<uint64_t> undoTouched_;
		bool undoChanged_ = false, undoAll_ = false, undoScript_ = false;
		ImGuiID undoActiveId_ = 0; // the widget active when the action began
		std::pair<uint32_t, uint32_t> undoSelBefore_{};
		std::string undoLabel_;
		void noteEdit(const attrib::LoadedPack* pack, const attrib::Collection* c);
		void resetUndo(); // after loading: the packs as they are become the first state
		void restoreUndo(int pos);
		void drawUndoItems();

		// Recording changes.
		bool recordAmod_ = false, recordNfsms_ = false;
		std::unique_ptr<attrib::Database> recordBase_;
		std::set<uint64_t> recordDirty_;
		bool recordStructure_ = false;
		int recordSuppress_ = 0;

		// Caches rebuilt when modelVersion_ changes.
		uint64_t modelVersion_ = 0, cacheVersion_ = ~0ull;
		// A walk over the caches (the Explorer's trees and lists) notes
		// cacheVersion_ as it starts. A command run from inside it (a context
		// menu, a drop) can rebuild them, freeing the lists it is walking, so it
		// stops when this says so.
		bool cachesRebuiltSince(uint64_t version) const
		{
			return cacheVersion_ != version;
		}
		CollectionIndex index_;
		std::vector<attrib::ClassDef*> sortedClasses_;
		std::unordered_map<const attrib::ClassDef*, std::vector<attrib::Collection*>> sortedRoots_;
		std::unordered_map<const attrib::Collection*, std::vector<attrib::Collection*>> sortedChildren_;
		std::unordered_map<const attrib::ClassDef*, std::vector<const attrib::FieldDef*>> sortedFields_;
		std::unique_ptr<attrib::GameplayModel> gameplay_;
		// car_paint_swatch/swatch's colours (Paint.h), read again with the caches
		// and when a car_paint_swatch collection changes.
		attrib::PaintSwatches paintSwatches_;

		// UI state.
		std::string explorerFilter_, fieldFilter_;
		ViewOptions view_;
		bool showExplorer_ = true, showInspector_ = true, showClass_ = false, showLog_ = true, showDemo_ = false;
		bool showSequence_ = false;
		std::string panelsSeen_; // openPanels() as last saved, to notice a change
		bool resetLayout_ = false;
		Theme theme_ = Theme::Dark;
		std::unordered_set<const void*> reveal_; // tree nodes to open for a navigation
		int explorerTabRequest_ = 0;              // 1 Classes, 2 Gameplay: bring to front once
		int revealIn_ = 0;                        // the tree reveal_ is for (1 Classes, 2 Gameplay)
		bool scrollToSelection_ = false;
		Pending pending_ = Pending::None;
		attrib::Collection* pendingTarget_ = nullptr;
		attrib::ClassDef* pendingClass_ = nullptr;
		attrib::Vault* pendingVault_ = nullptr;
		attrib::LoadedPack* pendingPack_ = nullptr; // New Vault's pack
		std::string pendingPath_;
		std::string nameInput_;
		// State Graph Builder.
		std::vector<attrib::MessageDef> messages_;
		ImFont* codeFont_ = nullptr;
		attrib::Collection* sbActivity_ = nullptr;
		attrib::Collection* sbState_ = nullptr;
		std::vector<GraphBox> sbBoxes_;
		attrib::Collection* sbHandler_ = nullptr;
		const attrib::Collection* sbSourceFor_ = nullptr; // the handler sbSource_ was loaded from
		std::string sbSource_, sbLoaded_;                  // edited text, and as decompiled
		std::string sbDisassembly_;
		std::vector<std::string> sbWarnings_;
		std::string sbStatus_;
		bool sbStatusError_ = false;
		bool sbShowDisassembly_ = false;
		ImVec2 sbPan_{ 0, 0 };
		std::string sbFilter_;
		const attrib::MessageDef* sbMessage_ = nullptr;
		// Classes.
		ClassDesigner designer_;
		std::vector<std::shared_ptr<const attrib::ClassList>> classLists_; // data/classes
		void drawClassDesigner();
		void drawClassItems(attrib::ClassDef* cls);
		void drawNewClassItems();
		void drawDeleteClassPrompt();
		void forgetClass(attrib::ClassDef* cls);
		// Gameplay modals.
		attrib::Collection* pendingTemplate_ = nullptr;
		std::string vaultInput_;
		bool withChildren_ = true;
		std::string templateFilter_;
	};

} // namespace app
