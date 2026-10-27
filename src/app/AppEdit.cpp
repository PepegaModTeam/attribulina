// Editing collections the way files are edited: the collection clipboard
// (core/Clip.h, also between windows), moves, deletes with everything under a
// collection, drag and drop in the class tree, the Edit and Go menus and their
// shortcuts, vaults made, renamed and deleted from the Files tree, and the
// check that keeps one database per window.

#include "App.h"

#include "core/Edit.h"
#include "core/Hash.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace app
{

	using namespace attrib;
	namespace fs = std::filesystem;

	namespace
	{

		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);
		const char* kPayload = "ATTRIB_COLLECTION";

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}

		bool SamePath(const fs::path& a, const fs::path& b)
		{
			std::error_code ec;
			return fs::equivalent(a, b, ec) || a.lexically_normal() == b.lexically_normal();
		}

		uint64_t Id(const Collection* c)
		{
			return (uint64_t(c->classKey) << 32) | c->key;
		}

		void Subtree(Collection* c, std::vector<Collection*>& out)
		{
			out.push_back(c);
			for (Collection* k : c->children) Subtree(k, out);
		}

		bool IsUnder(const Collection* c, const Collection* root)
		{
			for (const Collection* p = c; p; p = p->parent)
				if (p == root) return true;
			return false;
		}

		std::string Label(const KeyNames& names, const Collection* c)
		{
			return names.format(c->classKey) + "/" + names.format(c->key);
		}

		std::string Under(size_t n)
		{
			return n ? " and the " + std::to_string(n) + " collection" + (n == 1 ? "" : "s") + " under it" : std::string();
		}

		// Clipboard text compared the way another program may have stored it.
		std::string WithoutCR(std::string s)
		{
			s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
			return s;
		}

	} // namespace

	// ------------------------------------------------- one database per window ---

	std::string App::databaseFolder() const
	{
		if (!db_) return {};
		const LoadedPack* base = nullptr;
		for (const auto& p : db_->packs())
		{
			for (const auto& v : p->vaults)
				if (v->isDatabase) base = p.get();
			if (base) break;
		}
		if (!base && !db_->packs().empty()) base = db_->packs().front().get();
		return base ? ToUtf8(FromUtf8(base->path).parent_path()) : std::string();
	}

	std::optional<std::string> App::conflictWith(const std::string& path) const
	{
		if (!db_ || db_->packs().empty()) return std::nullopt;
		const fs::path p = FromUtf8(path);
		const std::string folder = databaseFolder();
		std::error_code ec;
		if (fs::is_directory(p, ec))
		{
			if (folder.empty() || SamePath(p, FromUtf8(folder))) return std::nullopt;
			return path + " is another database than the one open from " + folder + ".";
		}
		for (const auto& pk : db_->packs())
			if (SamePath(FromUtf8(pk->path), p)) return std::nullopt; // open already
		PackSummary s;
		try
		{
			s = DescribePack(ReadFile(path));
		}
		catch (const std::exception&)
		{
			return std::nullopt; // opening it reports what's wrong
		}
		const std::string file = ToUtf8(p.filename());
		if (s.hasDatabase && db_->hasSchema())
			return file + " is a database of its own, and the one from " + folder + " is open.";
		std::vector<std::string> clash;
		std::string from;
		for (const std::string& name : s.vaults)
			for (const auto& pk : db_->packs())
				for (const auto& v : pk->vaults)
					if (v->name == name)
					{
						if (clash.size() < 4) clash.push_back(name);
						if (from.empty()) from = pk->path;
					}
		if (clash.empty()) return std::nullopt;
		std::string list;
		for (const std::string& c : clash) list += (list.empty() ? "" : ", ") + c;
		return file + " holds vaults that are open already from " + from + " (" + list + (clash.size() == 4 ? ", ..." : "") +
			"). Its collections would hide theirs.";
	}

	// ----------------------------------------------------------- commands ---

	bool App::isCut(const Collection* c) const
	{
		return !cut_.empty() && cut_.count(Id(c));
	}

	bool App::removeTree(Collection* c)
	{
		if (!db_ || !c) return false;
		std::vector<Collection*> all;
		Subtree(c, all);
		const std::string label = Label(db_->names(), c);
		nameAction("Delete " + db_->names().format(c->key) + Under(all.size() - 1));
		// Tabs on anything in it move to the collection above it.
		Collection* fallback = c->parent;
		for (Collection* x : all)
		{
			markDirty(x);
			forgetCollection(x, fallback, false);
		}
		CollectionEditor ed(*db_);
		for (auto it = all.rbegin(); it != all.rend(); ++it) ed.remove(**it);
		structureChanged();
		log(LogLevel::Info, "Deleted " + label + Under(all.size() - 1));
		return true;
	}

	bool App::copyCollection(Collection* c, bool withChildren, bool cut)
	{
		if (!db_ || !c) return false;
		// A cut carries everything under it: another window pastes it as a copy.
		Clip clip = CopyCollections(*db_, { c }, withChildren || cut);
		clip.source = instanceId_;
		clip.cut = cut;
		clip.origin = databaseFolder();
		const std::string text = EncodeClip(clip);
		ImGui::SetClipboardText(text.c_str());
		cut_.clear();
		cutText_.clear();
		if (cut)
		{
			std::vector<Collection*> all;
			Subtree(c, all);
			for (Collection* x : all) cut_.insert(Id(x));
			cutText_ = WithoutCR(text);
			cutCheckAt_ = ImGui::GetTime() + 1.0;
		}
		log(LogLevel::Info, std::string(cut ? "Cut " : "Copied ") + Label(db_->names(), c) + Under(clip.collections.size() - 1) +
			(cut ? ". Paste it where it should go." : ""));
		return true;
	}

	const Clip* App::clipboardClip()
	{
		const char* text = ImGui::GetClipboardText();
		if (!text || !*text)
		{
			clipText_.clear();
			clip_.reset();
			return nullptr;
		}
		if (clipText_ != text)
		{
			clipText_ = text;
			clip_ = DecodeClip(clipText_);
		}
		return clip_ ? &*clip_ : nullptr;
	}

	bool App::moveCollection(Collection* c, Collection* newParent)
	{
		if (!db_ || !c) return false;
		const KeyNames& names = db_->names();
		if (newParent == c->parent && (newParent || !c->parentKey))
		{
			log(LogLevel::Info, Label(names, c) + " is there already.");
			return false;
		}
		if (!CollectionEditor(*db_).move(*c, newParent))
		{
			log(LogLevel::Warning, "Can't move " + Label(names, c) + (newParent ? " under " + names.format(newParent->key) : "") +
				": a collection can't go under itself or under another class's collection.");
			return false;
		}
		nameAction("Move " + names.format(c->key));
		markDirty(c);
		structureChanged();
		log(LogLevel::Info, "Moved " + Label(names, c) + (newParent ? " under " + names.format(newParent->key) : " to the root of its class") +
			". What it inherits comes from there now.");
		return true;
	}

	bool App::paste(Collection* target, ClassDef* cls, bool asChild)
	{
		if (!db_) return false;
		const Clip* clip = clipboardClip();
		if (!clip || clip->collections.empty())
		{
			log(LogLevel::Warning, "The clipboard holds no collections. Copy one in the Explorer first (Ctrl+C).");
			return false;
		}
		ClassDef* into = target ? target->cls : cls;
		if (!into) return false;
		const KeyNames& names = db_->names();
		if (into->key != clip->classKey)
		{
			log(LogLevel::Warning, "The clipboard holds " + clip->className + " collections. Paste them on a " + clip->className +
				" collection, or on the class itself.");
			return false;
		}
		Collection* parent = asChild ? target : target ? target->parent : nullptr;
		nameAction(clip->cut && clip->source == instanceId_ ? "Paste (move)" : "Paste");

		// A cut in this window moves the collection.
		if (clip->cut && clip->source == instanceId_)
		{
			const Clip cutClip = *clip;
			std::vector<Collection*> moved;
			for (const ClipCollection& cc : cutClip.collections)
			{
				if (cc.parent >= 0) continue;
				Collection* c = db_->findCollection(cutClip.classKey, cc.key);
				if (!c) continue;
				if (parent && IsUnder(parent, c))
				{
					log(LogLevel::Warning, "Can't move " + Label(names, c) + " under itself.");
					continue;
				}
				if (moveCollection(c, parent)) moved.push_back(c);
			}
			cut_.clear();
			cutText_.clear();
			ImGui::SetClipboardText("");
			if (!moved.empty()) select(moved.front(), true);
			return !moved.empty();
		}

		Vault* vault = target ? target->vault : (selected_ && selected_->cls == into) ? selected_->vault : into->vault;
		if (!vault) return false;
		try
		{
			const PasteResult r = PasteClip(*db_, *clip, *into, parent, *vault);
			for (const std::string& w : r.warnings) log(LogLevel::Warning, w);
			for (const std::string& n : r.newNames) rememberName(n);
			copiesTakeTheirNames(*clip, r);
			for (Collection* n : r.created) markDirty(n);
			structureChanged();
			if (r.created.empty()) return false;
			select(r.created.front(), true);
			std::string what = "Pasted " + Label(names, r.created.front()) + Under(r.created.size() - 1) + " in vault " + vault->name;
			if (!clip->origin.empty() && !SamePath(FromUtf8(clip->origin), FromUtf8(databaseFolder())))
				what += ", copied from " + clip->origin;
			log(LogLevel::Info, what);
			if (clip->cut) log(LogLevel::Info, "It was cut in another window: this is a copy, and the original stays there.");
			return true;
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, std::string("Paste: ") + e.what());
			return false;
		}
	}

	void App::copiesTakeTheirNames(const Clip& clip, const PasteResult& r)
	{
		const KeyNames& names = db_->names();
		for (size_t i = 0; i < r.created.size() && i < clip.collections.size(); ++i)
		{
			Collection& n = *r.created[i];
			SetCollectionNameField(*db_, n);
			const std::string& was = clip.collections[i].name;
			if (was.empty() || !names.find(n.key) || names.format(n.key) == was || !isGameplay(&n) || !n.find(StringHash32("bytecode")))
				continue;
			std::string error;
			if (!RenameHandlerFunction(*db_, n, HandlerFunction(*db_, was), &error))
				log(LogLevel::Warning, names.format(n.key) + ": its Lua still defines " + HandlerFunction(*db_, was) +
					", as it couldn't be compiled under the new name (" + error + ")");
		}
	}

	Collection* App::copyHere(Collection* src, Collection* parent, bool withChildren)
	{
		if (!db_ || !src) return nullptr;
		Clip clip = CopyCollections(*db_, { src }, withChildren);
		clip.source = instanceId_;
		Vault* vault = parent ? parent->vault : src->vault;
		nameAction("Copy " + db_->names().format(src->key));
		try
		{
			const PasteResult r = PasteClip(*db_, clip, *src->cls, parent, *vault);
			for (const std::string& w : r.warnings) log(LogLevel::Warning, w);
			for (const std::string& n : r.newNames) rememberName(n);
			copiesTakeTheirNames(clip, r);
			for (Collection* n : r.created) markDirty(n);
			structureChanged();
			if (r.created.empty()) return nullptr;
			select(r.created.front(), true);
			log(LogLevel::Info, "Copied " + Label(db_->names(), src) + " to " + db_->names().format(r.created.front()->key) +
				Under(r.created.size() - 1));
			return r.created.front();
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, std::string("Copy: ") + e.what());
			return nullptr;
		}
	}

	Collection* App::duplicate(Collection* c)
	{
		if (!c || !db_) return nullptr;
		nameAction("Duplicate " + db_->names().format(c->key));
		return copyHere(c, c->parent, false);
	}

	// ------------------------------------------------------------- targets ---

	App::EditTarget App::targetFor(Collection* c, ClassDef* cls) const
	{
		EditTarget t{ c, c ? c->cls : cls };
		if (c && view_.gameplayView && isGameplay(c))
			t.kind = gameplay_->isTemplate(c) ? EditTarget::Kind::Template : EditTarget::Kind::Element;
		return t;
	}

	App::EditTarget App::editTarget() const
	{
		if (lastPanel_ == Panel::Explorer && !explorerTarget_.empty()) return explorerTarget_;
		return targetFor(selected_, nullptr);
	}

	void App::openItem(Collection* c)
	{
		if (ImGui::GetIO().KeyCtrl) openInNewTab(c, false);
		else select(c);
	}

	void App::commandCopy(const EditTarget& t, bool withChildren)
	{
		if (t.collection) copyCollection(t.collection, withChildren && t.kind == EditTarget::Kind::Plain, false);
	}

	void App::commandCut(const EditTarget& t)
	{
		if (!t.collection) return;
		if (t.kind != EditTarget::Kind::Plain)
		{
			log(LogLevel::Warning, "Gameplay elements can't be moved yet. Duplicate (Ctrl+D) copies one to a new path.");
			return;
		}
		copyCollection(t.collection, true, true);
	}

	void App::commandPaste(const EditTarget& t, bool asChild)
	{
		if (t.kind == EditTarget::Kind::Plain)
		{
			paste(t.collection, t.cls, asChild);
			return;
		}
		// The gameplay view pastes an element copied here as a duplicate, which
		// keeps the gameplay conventions (paths, Children, vaults).
		const Clip* clip = clipboardClip();
		Collection* src = clip && !clip->collections.empty() ? db_->findCollection(clip->classKey, clip->collections.front().key) : nullptr;
		if (clip && clip->source == instanceId_ && src && isGameplay(src) && !gameplay_->isTemplate(src)) requestGameplayDuplicate(src);
		else if (clip)
			log(LogLevel::Warning, "The gameplay view pastes elements copied in this window, as a duplicate. To bring one from "
				"another window, paste it in the Classes tab (a plain copy, without the gameplay conventions).");
		else
			log(LogLevel::Warning, "The clipboard holds no collections. Copy one in the Explorer first (Ctrl+C).");
	}

	void App::commandDuplicate(const EditTarget& t)
	{
		if (!t.collection) return;
		if (t.kind == EditTarget::Kind::Plain) duplicate(t.collection);
		else if (t.kind == EditTarget::Kind::Element) requestGameplayDuplicate(t.collection);
	}

	void App::commandDelete(const EditTarget& t)
	{
		if (!t.collection) return;
		if (t.kind == EditTarget::Kind::Plain) request(Pending::Delete, t.collection);
		else if (t.kind == EditTarget::Kind::Element) request(Pending::GameplayDelete, t.collection);
	}

	void App::commandRename(const EditTarget& t)
	{
		if (!t.collection) return;
		if (t.kind == EditTarget::Kind::Plain) request(Pending::Rename, t.collection);
		else log(LogLevel::Warning, "Gameplay elements can't be renamed yet. Duplicate (Ctrl+D) copies one to a new path.");
	}

	void App::commandNew(const EditTarget& t)
	{
		switch (t.kind)
		{
			case EditTarget::Kind::Plain:
				if (t.cls) requestNew(t.cls, t.collection);
				break;
			case EditTarget::Kind::Element: requestGameplayNew(t.collection, nullptr); break;
			case EditTarget::Kind::Template: requestGameplayNew(nullptr, t.collection); break;
		}
	}

	void App::commandFindReferences(const EditTarget& t)
	{
		if (t.collection) findReferences(t.collection->classKey, t.collection->key);
		else if (t.cls) findReferences(t.cls->key, 0);
	}

	bool App::canPaste(const EditTarget& t, bool asChild)
	{
		if (!db_ || t.empty()) return false;
		const Clip* clip = clipboardClip();
		if (!clip || clip->collections.empty()) return false;
		if (t.kind != EditTarget::Kind::Plain)
		{
			Collection* src = db_->findCollection(clip->classKey, clip->collections.front().key);
			return clip->source == instanceId_ && src && isGameplay(src) && !gameplay_->isTemplate(src);
		}
		const ClassDef* into = t.collection ? t.collection->cls : t.cls;
		return into && into->key == clip->classKey && (!asChild || t.collection);
	}

	// --------------------------------------------------------------- menus ---

	void App::drawEditItems(const EditTarget& t)
	{
		using Kind = EditTarget::Kind;
		Collection* c = t.collection;
		const bool plain = t.kind == Kind::Plain;
		if (c)
		{
			if (ImGui::MenuItem("Cut", "Ctrl+X", false, plain)) commandCut(t);
			if (!plain && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Gameplay elements can't be moved yet. Duplicate copies one to a new path.");
			if (ImGui::MenuItem("Copy", "Ctrl+C")) commandCopy(t, false);
			if (plain && ImGui::MenuItem("Copy with Children", "Ctrl+Shift+C", false, !c->children.empty())) commandCopy(t, true);
		}
		if (ImGui::MenuItem(plain ? (c ? "Paste" : "Paste as Root") : "Paste as Duplicate...", "Ctrl+V", false, canPaste(t, false)))
			commandPaste(t, false);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(plain ? (c ? "Next to this collection, with a free name" : "As a collection with no parent")
							  : "A duplicate of the element copied in this window");
		if (plain && c && ImGui::MenuItem("Paste as Child", "Ctrl+Shift+V", false, canPaste(t, true))) commandPaste(t, true);
		if (c)
		{
			if (ImGui::MenuItem(plain ? "Duplicate" : "Duplicate...", "Ctrl+D", false, t.kind != Kind::Template)) commandDuplicate(t);
			if (ImGui::MenuItem("Delete...", "Del", false, t.kind != Kind::Template)) commandDelete(t);
			if (ImGui::MenuItem("Rename...", "F2", false, plain)) commandRename(t);
		}
		ImGui::Separator();
		const char* newLabel = t.kind == Kind::Element ? "New Element Here..."
			: t.kind == Kind::Template ? "New Element from Template..."
			: c ? "New Child Collection..."
			: "New Collection...";
		if (ImGui::MenuItem(newLabel, "Ctrl+N", false, !t.empty())) commandNew(t);
		if (plain && c && ImGui::MenuItem("Move to Vault..."))
		{
			request(Pending::MoveToVault, c);
			pendingVault_ = c->vault;
		}
		if (!t.empty() && db_ && ImGui::BeginMenu("Add to Script"))
		{
			if (c)
			{
				if (ImGui::MenuItem("This Collection")) addCollectionsToScript({ c }, false);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its parent, vault and own values, as a script that makes it or updates it");
				if (t.kind == Kind::Element && gameplay_)
				{
					// What the folder holds: elements under its path and in Children
					// lists, not the elements made from it as a template. A race with
					// nothing under it still needs its vault and its owner's list.
					const GameplayModel::Node* n = gameplay_->nodeOf(c);
					if (ImGui::MenuItem("With Everything Under It"))
						addGameplayToScript(n ? GameplayFolderContents(*gameplay_, *n) : GameplaySubtree(*gameplay_, *c));
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("This element, the ones under its path and those their Children lists name, with\n"
										  "the vaults they are in and its owner's Children list, so the game finds them");
				}
				else if (ImGui::MenuItem("With Everything Under It", nullptr, false, !c->children.empty()))
					addCollectionsToScript({ c }, true);
			}
			else
			{
				if (ImGui::MenuItem("Every Collection"))
				{
					addCollectionsToScript(std::vector<Collection*>(t.cls->collections.begin(), t.cls->collections.end()), false);
				}
				if (ImGui::MenuItem("Chosen Fields..."))
				{
					pendingClass_ = t.cls;
					exportFields_.clear();
					request(Pending::ExportFields);
				}
				if (ImGui::MenuItem("Class Definition")) addToScript({ script::ClassStatement(*db_, *t.cls) });
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("A class statement that makes the class, or changes it to this");
			}
			ImGui::EndMenu();
		}
		if (!c && t.cls && plain)
		{
			ImGui::Separator();
			drawClassItems(t.cls);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Find References", "Shift+F12", false, !t.empty())) commandFindReferences(t);
		if (!t.empty() && ImGui::MenuItem("Copy Name")) ImGui::SetClipboardText(db_->names().format(c ? c->key : t.cls->key).c_str());
		if (!t.empty() && ImGui::MenuItem("Copy Key"))
		{
			char b[16];
			std::snprintf(b, sizeof(b), "0x%08X", c ? c->key : t.cls->key);
			ImGui::SetClipboardText(b);
		}
	}

	void App::drawEditMenu()
	{
		if (!ImGui::BeginMenu("Edit")) return;
		drawUndoItems();
		ImGui::Separator();
		const EditTarget t = db_ ? editTarget() : EditTarget{};
		// What the commands act on.
		if (t.collection) ImGui::TextDisabled("%s", Label(db_->names(), t.collection).c_str());
		else if (t.cls) ImGui::TextDisabled("class %s", db_->names().format(t.cls->key).c_str());
		else ImGui::TextDisabled("Nothing selected");
		ImGui::Separator();
		drawEditItems(t);
		ImGui::Separator();
		drawNewClassItems();
		ImGui::Separator();
		if (ImGui::MenuItem("Find in Database...", "Ctrl+Shift+F", false, db_ != nullptr)) openSearch();
		if (ImGui::MenuItem("Filter Explorer", "Ctrl+F"))
		{
			showExplorer_ = true;
			focusExplorerFilter_ = true;
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Allow Arrays Past Their Limit", nullptr, &arraysPastLimit_)) ImGui::MarkIniSettingsDirty();
		ImGui::SetItemTooltip("Lets an array hold more elements than its field's definition allows (GEAR_RATIO's 9), with a "
							  "warning.\nGame code made for that count may ignore the rest or misbehave. An array in the "
							  "layout keeps its fixed space.\nScripts may always go past the limit, with a warning.");
		ImGui::EndMenu();
	}

	void App::switchTab(int delta)
	{
		if (tabs_.size() < 2) return;
		const int n = int(tabs_.size());
		activeTab_ = size_t((int(activeTab_) + delta % n + n) % n);
		focusTabId_ = tabs_[activeTab_].id;
		selected_ = tabs_[activeTab_].collection;
	}

	void App::drawGoMenu()
	{
		if (!ImGui::BeginMenu("Go")) return;
		const Tab* tab = tabs_.empty() ? nullptr : &tabs_[activeTab_];
		if (ImGui::MenuItem("Back", "Alt+Left", false, tab && tab->historyPos > 0)) stepHistory(-1);
		if (ImGui::MenuItem("Forward", "Alt+Right", false, tab && tab->historyPos + 1 < int(tab->history.size()))) stepHistory(+1);
		ImGui::Separator();
		if (ImGui::MenuItem("New Tab", "Ctrl+T", false, selected_ != nullptr)) openInNewTab(selected_, true);
		if (ImGui::MenuItem("Close Tab", "Ctrl+W", false, tab != nullptr)) closeTab(activeTab_);
		if (ImGui::MenuItem("Next Tab", "Ctrl+PgDn", false, tabs_.size() > 1)) switchTab(+1);
		if (ImGui::MenuItem("Previous Tab", "Ctrl+PgUp", false, tabs_.size() > 1)) switchTab(-1);
		ImGui::Separator();
		if (ImGui::MenuItem("Show in Explorer", nullptr, false, selected_ != nullptr)) select(selected_, true);
		ImGui::EndMenu();
	}

	// ----------------------------------------------------------- shortcuts ---

	void App::globalShortcuts()
	{
		const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global))
			saveAll();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O, global))
			requestOpenFileDialog();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global))
			requestOpenFolderDialog();

		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, global)) openSearch();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, global))
		{
			if (lastPanel_ == Panel::Inspector)
			{
				focusFieldFilter_ = true;
			}
			else
			{
				showExplorer_ = true;
				focusExplorerFilter_ = true;
			}
		}
		if (host_.canOpenNewWindow() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N, global)) host_.openNewWindow("");
		if (!db_) return;
		// Undo and redo, unless a text field has the keyboard (it undoes its own text).
		if (!ImGui::GetIO().WantTextInput)
		{
			const ImGuiInputFlags repeat = global | ImGuiInputFlags_Repeat;
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, repeat)) undo();
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, repeat) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, repeat))
				redo();
		}
		// Browser keys: Alt+arrows and the mouse's back/forward buttons.
		if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_LeftArrow, global) || ImGui::IsMouseClicked(3)) stepHistory(-1);
		if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_RightArrow, global) || ImGui::IsMouseClicked(4)) stepHistory(+1);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_T, global) && selected_) openInNewTab(selected_, true);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, global) && !tabs_.empty()) closeTab(activeTab_);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageDown, global)) switchTab(+1);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageUp, global)) switchTab(-1);
	}

	void App::explorerShortcuts()
	{
		const EditTarget t = explorerFocus_;
		auto pressed = [](ImGuiKeyChord chord)
		{
			return ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused);
		};
		// Text fields keep their own keys (and the filtered list, with no tree, has no node to toggle).
		if (!db_ || ImGui::GetIO().WantTextInput || pending_ != Pending::None) return;
		if (treeFocus_.id && pressed(ImGuiMod_Shift | ImGuiKey_A))
		{
			treeToggle_ = treeFocus_;
			treeToggleFrame_ = ImGui::GetFrameCount();
		}
		if (t.empty()) return;
		if (pressed(ImGuiMod_Ctrl | ImGuiKey_C)) commandCopy(t, false);
		if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) commandCopy(t, true);
		if (pressed(ImGuiMod_Ctrl | ImGuiKey_X)) commandCut(t);
		if (pressed(ImGuiMod_Ctrl | ImGuiKey_V)) commandPaste(t, false);
		if (pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V)) commandPaste(t, true);
		if (pressed(ImGuiMod_Ctrl | ImGuiKey_D)) commandDuplicate(t);
		if (pressed(ImGuiKey_Delete)) commandDelete(t);
		if (pressed(ImGuiKey_F2)) commandRename(t);
		if (pressed(ImGuiMod_Ctrl | ImGuiKey_N)) commandNew(t);
		if (pressed(ImGuiMod_Shift | ImGuiKey_F12)) commandFindReferences(t);
	}

	// ------------------------------------------------------- drag and drop ---

	void App::dragSource(Collection* c)
	{
		if (!ImGui::BeginDragDropSource()) return;
		const uint64_t id = Id(c);
		ImGui::SetDragDropPayload(kPayload, &id, sizeof(id));
		ImGui::TextUnformatted(Label(db_->names(), c).c_str());
		ImGui::TextDisabled("Drop it on a collection of its class, or on the class");
		ImGui::EndDragDropSource();
	}

	void App::dropTarget(Collection* newParent, ClassDef* cls)
	{
		if (!ImGui::BeginDragDropTarget()) return;
		if (const ImGuiPayload* p = ImGui::GetDragDropPayload(); p && p->IsDataType(kPayload) && p->DataSize == sizeof(uint64_t))
		{
			uint64_t id;
			std::memcpy(&id, p->Data, sizeof(id));
			Collection* src = db_->findCollection(uint32_t(id >> 32), uint32_t(id));
			// Only where it may go: its own class, not under itself.
			if (src && src->cls == cls && !(newParent && IsUnder(newParent, src)) && ImGui::AcceptDragDropPayload(kPayload))
			{
				dropSource_ = src;
				dropParent_ = newParent;
				dropClass_ = cls;
				dropOpen_ = true;
			}
		}
		ImGui::EndDragDropTarget();
	}

	// A drop asks what to do, so a slip of the mouse moves nothing.
	void App::drawDropMenu()
	{
		if (dropOpen_)
		{
			ImGui::OpenPopup("##drop");
			dropOpen_ = false;
		}
		if (!ImGui::BeginPopup("##drop")) return;
		Collection* src = dropSource_;
		if (!db_ || !src || !dropClass_)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		const KeyNames& names = db_->names();
		Collection* parent = dropParent_;
		ImGui::TextDisabled("%s", Label(names, src).c_str());
		ImGui::Separator();
		const std::string where = parent ? "under " + names.format(parent->key) : "to the root of " + names.format(dropClass_->key);
		const bool there = parent == src->parent && (parent || !src->parentKey);
		if (ImGui::MenuItem(("Move " + where).c_str(), nullptr, false, !there) && moveCollection(src, parent)) select(src, true);
		if (ImGui::MenuItem(("Copy " + where).c_str())) copyHere(src, parent, false);
		if (!src->children.empty() && ImGui::MenuItem(("Copy with Children " + where).c_str())) copyHere(src, parent, true);
		ImGui::Separator();
		ImGui::MenuItem("Cancel");
		ImGui::EndPopup();
	}

	// -------------------------------------------------------------- modals ---

	void App::drawEditModals()
	{
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		auto begin = [&](const char* name)
		{
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		};
		auto done = [&]()
		{
			pending_ = Pending::None;
			pendingTarget_ = nullptr;
			ImGui::CloseCurrentPopup();
		};

		if (begin("Another Database"))
		{
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
			ImGui::TextUnformatted(conflictReason_.c_str());
			ImGui::Spacing();
			ImGui::TextDisabled("A window edits one database at a time. To work on both, or to copy collections from one to "
								"the other, open it in a new window.");
			ImGui::PopTextWrapPos();
			const bool dirty = hasUnsavedChanges();
			if (dirty)
			{
				ImGui::Spacing();
				ImGui::TextColored(kWarn, "Unsaved changes in:");
				for (const auto& p : db_->packs())
					if (p->dirty) ImGui::BulletText("%s", p->path.c_str());
				for (const std::string& f : removedFiles()) ImGui::BulletText("%s (deleted)", f.c_str());
			}
			ImGui::Spacing();
			const std::string path = pendingPath_;
			auto replace = [&]()
			{
				closeAll();
				open(path);
			};
			if (dirty)
			{
				if (ImGui::Button("Save and Open"))
				{
					const bool ok = saveAll();
					done();
					if (ok) replace();
				}
				ImGui::SameLine();
				if (ImGui::Button("Discard and Open"))
				{
					done();
					replace();
				}
			}
			else
			{
				if (ImGui::Button("Open in this window"))
				{
					done();
					replace();
				}
				ImGui::SetItemDefaultFocus();
			}
			if (host_.canOpenNewWindow())
			{
				ImGui::SameLine();
				if (ImGui::Button("Open in new window"))
				{
					if (host_.openNewWindow(path)) log(LogLevel::Info, "Opened " + path + " in a new window");
					else log(LogLevel::Error, "Could not start another window for " + path);
					done();
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			ImGui::EndPopup();
		}

		// Which game and platform a new database's files are from: asked when
		// they don't say (or detection is off), with the best guess chosen.
		if (begin("Game and Platform"))
		{
			const Detection& d = targetDetection_;
			ImGui::TextUnformatted("Which game and platform are these files from?");
			ImGui::TextDisabled("%s", pendingPath_.c_str());
			ImGui::Spacing();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
			if (d.certain) ImGui::Text("The files say %s (%s).", d.target.name().c_str(), d.reason.c_str());
			else ImGui::Text("The files don't say for certain (%s). The best guess is %s.", d.reason.c_str(), d.target.name().c_str());
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			const char* mark = d.certain ? "  (detected)" : "  (best guess)";
			if (ImGui::BeginTable("##target", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV))
			{
				ImGui::TableSetupColumn("Game");
				ImGui::TableSetupColumn("Platform");
				ImGui::TableHeadersRow();
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				for (const GameInfo& g : Games())
				{
					std::string label = g.name;
					if (!SupportedAnywhere(g.game)) label += "  (not supported yet)";
					if (g.game == d.target.game) label += mark;
					if (ImGui::RadioButton((label + "###game_" + g.id).c_str(), targetChoice_.game == g.game) && targetChoice_.game != g.game)
					{
						targetChoice_.game = g.game;
						// Keep the platform when the game came out on it; else the
						// first it did that fits the files.
						if (!ReleasedOn(g.game, targetChoice_.platform))
						{
							Platform pick = g.platforms.front();
							for (Platform p : g.platforms)
								if (Contradiction({ g.game, p }, d).empty())
								{
									pick = p;
									break;
								}
							targetChoice_.platform = pick;
						}
					}
				}
				ImGui::TableNextColumn();
				for (const PlatformInfo& p : Platforms())
				{
					std::string label = p.name;
					if (SupportedAnywhere(targetChoice_.game) && ReleasedOn(targetChoice_.game, p.platform) &&
						!SupportedOn(targetChoice_.game, p.platform))
						label += "  (not supported yet)";
					if (p.platform == d.target.platform && targetChoice_.game == d.target.game) label += mark;
					ImGui::BeginDisabled(!ReleasedOn(targetChoice_.game, p.platform));
					if (ImGui::RadioButton((label + "###platform_" + p.id).c_str(), targetChoice_.platform == p.platform))
						targetChoice_.platform = p.platform;
					ImGui::EndDisabled();
				}
				ImGui::EndTable();
			}
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
			if (!SupportedOn(targetChoice_.game, targetChoice_.platform))
				ImGui::TextColored(kWarn, "Attribulina doesn't edit %s yet. Its files open with their structs and enums as raw bytes, "
								   "and saving them may lose data.",
								   targetChoice_.name().c_str());
			if (std::string c = Contradiction(targetChoice_, d); !c.empty())
			{
				c[0] = char(std::toupper(static_cast<unsigned char>(c[0])));
				ImGui::TextColored(kWarn, "%s.", c.c_str());
			}
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			bool detect = detectTarget_;
			if (ImGui::Checkbox("Ask only when the files don't say", &detect)) setDetectTarget(detect);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("On: the game and platform come from the files' Vers value, and this shows only when there isn't one\n"
								  "Attribulina knows (Most Wanted's files). Off: it shows each time a database is opened.\n"
								  "Also File > Detect Game and Platform.");
			ImGui::Spacing();
			bool go = ImGui::Button("Open");
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				log(LogLevel::Info, "Didn't open " + pendingPath_);
				for (const std::string& q : queuedOpens_) log(LogLevel::Info, "Didn't open " + q);
				queuedOpens_.clear();
				done();
			}
			else if (go || ConfirmKey())
			{
				const std::string path = pendingPath_;
				const Target t = targetChoice_;
				if (const std::string c = Contradiction(t, targetDetection_); !c.empty())
					log(LogLevel::Warning, "Opening " + path + " as " + t.name() + ", though " + c);
				done();
				const std::vector<std::string> more = std::move(queuedOpens_);
				queuedOpens_.clear();
				openAs(path, t);
				for (const std::string& q : more) open(q);
			}
			ImGui::EndPopup();
		}

		if (begin("Move to Vault"))
		{
			Collection* c = pendingTarget_;
			if (!c || !db_)
			{
				done();
			}
			else
			{
				const KeyNames& names = db_->names();
				ImGui::Text("Move %s to another vault", Label(names, c).c_str());
				ImGui::TextDisabled("The vault decides which file it is saved in. Its children stay where they are.");
				auto vaultLabel = [](const Vault* v)
				{
					return ToUtf8(FromUtf8(v->pack->path).filename()) + ": " + v->name;
				};
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
				if (ImGui::BeginCombo("vault", pendingVault_ ? vaultLabel(pendingVault_).c_str() : "(choose)", ImGuiComboFlags_HeightLarge))
				{
					for (const auto& pk : db_->packs())
						for (const auto& v : pk->vaults)
						{
							if (v->raw.endian != c->vault->raw.endian) continue;
							if (ImGui::Selectable(vaultLabel(v.get()).c_str(), v.get() == pendingVault_)) pendingVault_ = v.get();
						}
					ImGui::EndCombo();
				}
				const bool ok = pendingVault_ && pendingVault_ != c->vault;
				ImGui::BeginDisabled(!ok);
				if (ImGui::Button("Move"))
				{
					try
					{
						const std::string from = c->vault->name;
						nameAction("Move " + names.format(c->key) + " to vault " + pendingVault_->name);
						markDirty(c); // the file it leaves
						CollectionEditor(*db_).moveToVault(*c, *pendingVault_);
						markDirty(c);
						structureChanged();
						log(LogLevel::Info, "Moved " + Label(names, c) + " from vault " + from + " to " + c->vault->name);
					}
					catch (const std::exception& e)
					{
						log(LogLevel::Error, e.what());
					}
					done();
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			}
			ImGui::EndPopup();
		}
	}


	// ----------------------------------------------------------------- vaults ---

	namespace
	{
		std::string FileOf(const LoadedPack& pk)
		{
			return ToUtf8(FromUtf8(pk.path).filename());
		}

		// "tracks/autobahn_gd, tracks/x and 3 more"
		std::string SomeNames(const KeyNames& names, const std::vector<Collection*>& cs, size_t shown = 3)
		{
			std::string out;
			for (size_t i = 0; i < cs.size() && i < shown; ++i) out += (i ? ", " : "") + names.format(cs[i]->key);
			if (cs.size() > shown) out += " and " + std::to_string(cs.size() - shown) + " more";
			return out;
		}
	} // namespace

	bool App::canMakeEventVaults() const
	{
		return db_ && db_->target().game == Game::World && EventVaultModel(*db_) && !NewEventVaultPath(*db_, "x").empty();
	}

	void App::drawNewEventVaultItem()
	{
		if (ImGui::MenuItem("New Event Vault File...", nullptr, false, canMakeEventVaults()))
		{
			request(Pending::NewEventVault);
			pendingPack_ = nullptr;
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("A file of its own in gc.vaults holding one vault, as each of World's events is");
	}

	void App::drawPackItems(LoadedPack& pack)
	{
		const bool eventFile = pack.format == PackFormat::VaultFile;
		if (ImGui::MenuItem("New Vault...", nullptr, false, !eventFile))
		{
			request(Pending::NewVault);
			pendingPack_ = &pack;
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(eventFile ? "An event vault file holds one vault. New Event Vault File makes another."
				: "An empty vault in this file. Collections go in with Move to Vault, or by choosing\n"
				"it when you make one.");
		if (eventFile && canMakeEventVaults()) drawNewEventVaultItem();
	}

	void App::drawVaultItems(Vault& vault)
	{
		const bool eventFile = vault.pack && vault.pack->format == PackFormat::VaultFile;
		const bool renamable = !vault.isDatabase && !eventFile;
		if (ImGui::MenuItem("Rename Vault...", nullptr, false, renamable))
		{
			request(Pending::RenameVault);
			pendingVault_ = &vault;
			nameInput_ = vault.name;
		}
		if (!renamable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(vault.isDatabase ? "The database's vault keeps its name."
				: "An event vault is named after its file. Make a new event vault file and move the\n"
				"collections into it.");
		const bool deletable = !vault.isDatabase && vault.classes.empty();
		if (ImGui::MenuItem("Delete Vault...", nullptr, false, deletable))
		{
			request(Pending::DeleteVault);
			pendingVault_ = &vault;
		}
		if (!deletable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("It holds the database's classes.");
		ImGui::Separator();
		if (ImGui::BeginMenu("Add to Script"))
		{
			if (ImGui::MenuItem("This Vault", nullptr, false, !vault.isDatabase)) addVaultToScript(vault, false);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(vault.isDatabase ? "The database's vault is always there."
					: eventFile ? "A create vault statement: applying it makes this event vault file when it's missing"
					: "A create vault statement: applying it makes the vault in this file when it's missing");
			if (ImGui::MenuItem("With Its Collections", nullptr, false, !vault.collections.empty())) addVaultToScript(vault, true);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("The vault, then each collection in it with its parent and own values");
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Copy Name")) ImGui::SetClipboardText(vault.name.c_str());
	}

	void App::deleteVault(Vault& vault)
	{
		if (!db_ || !vault.pack) return;
		LoadedPack* pack = vault.pack;
		const std::string name = vault.name, file = FileOf(*pack);
		const bool eventFile = pack->format == PackFormat::VaultFile;
		nameAction("Delete vault " + name);
		const std::vector<Collection*> all = VaultContents(vault);
		for (Collection* x : all)
		{
			markDirty(x);
			forgetCollection(x, nullptr, false);
		}
		CollectionEditor ed(*db_);
		for (Collection* x : all) ed.remove(*x); // each child before its parent
		noteEdit(pack, nullptr);
		if (pendingVault_ == &vault) pendingVault_ = nullptr;
		if (eventFile)
		{
			if (pendingPack_ == pack) pendingPack_ = nullptr;
			db_->removePack(*pack);
		}
		else
		{
			RemoveVault(*pack, vault);
			pack->dirty = true;
		}
		structureChanged();
		updateTitle();
		std::string what = "Deleted vault " + name;
		if (!all.empty()) what += " and " + std::to_string(all.size()) + " collection" + (all.size() == 1 ? "" : "s");
		log(LogLevel::Info, what + (eventFile ? ". Saving removes " + file : std::string()));
	}

	void App::drawVaultModals()
	{
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		auto begin = [&](const char* name)
		{
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		};
		auto done = [&]()
		{
			pending_ = Pending::None;
			pendingTarget_ = nullptr;
			pendingVault_ = nullptr;
			pendingPack_ = nullptr;
			ImGui::CloseCurrentPopup();
		};
		auto nameBox = [&](const char* hint)
		{
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
			return ImGui::InputTextWithHint("##name", hint, &nameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
		};

		if (begin("New Vault"))
		{
			LoadedPack* pk = pendingPack_;
			if (!db_ || !pk)
			{
				done();
			}
			else
			{
				ImGui::Text("New vault in %s", FileOf(*pk).c_str());
				ImGui::TextDisabled("Collections go in with Move to Vault, or by choosing it when you make one. The game\n"
									"loads a gameplay element's children from the vault its gameplayvault names.");
				const bool enter = nameBox("vault name");
				const std::string problem = nameInput_.empty() ? std::string() : VaultNameProblem(*db_, *pk, nameInput_);
				if (!problem.empty()) ImGui::TextColored(kError, "%s", problem.c_str());
				const bool ok = !nameInput_.empty() && problem.empty();
				ImGui::BeginDisabled(!ok);
				if (ImGui::Button("Create") || (enter && ok))
				{
					try
					{
						const Vault* like = !pk->vaults.empty() ? pk->vaults.front().get() : EventVaultModel(*db_);
						if (!like) throw std::runtime_error("no vault is open to copy the header from");
						nameAction("New vault " + nameInput_);
						CreateVault(*pk, nameInput_, *like);
						pk->dirty = true;
						noteEdit(pk, nullptr);
						structureChanged();
						updateTitle();
						log(LogLevel::Info, "Made vault " + nameInput_ + " in " + FileOf(*pk));
					}
					catch (const std::exception& e)
					{
						log(LogLevel::Error, e.what());
					}
					done();
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			}
			ImGui::EndPopup();
		}

		if (begin("Rename Vault"))
		{
			Vault* v = pendingVault_;
			if (!db_ || !v || !v->pack)
			{
				done();
			}
			else
			{
				const KeyNames& names = db_->names();
				ImGui::Text("Rename vault %s (%s)", v->name.c_str(), FileOf(*v->pack).c_str());
				const bool enter = nameBox("new name");
				const bool same = nameInput_ == v->name;
				const std::string problem = nameInput_.empty() || same ? std::string() : VaultNameProblem(*db_, *v->pack, nameInput_, v);
				if (!problem.empty()) ImGui::TextColored(kError, "%s", problem.c_str());
				const std::vector<Collection*> naming = CollectionsNamingVault(*db_, v->name);
				if (!naming.empty())
					ImGui::TextDisabled("The gameplayvault of %s names it and takes the new name.", SomeNames(names, naming).c_str());
				else
					ImGui::TextColored(kWarn, "Nothing in the database names this vault. If the game's code loads it by name, as\n"
						"it does gpcore, it won't find it under another.");
				const bool ok = !nameInput_.empty() && !same && problem.empty();
				ImGui::BeginDisabled(!ok);
				if (ImGui::Button("Rename") || (enter && ok))
				{
					try
					{
						const std::string from = v->name;
						nameAction("Rename vault " + from + " to " + nameInput_);
						noteEdit(v->pack, nullptr);
						for (Collection* c : RenameVault(*db_, *v, nameInput_)) markDirty(c);
						v->pack->dirty = true;
						structureChanged();
						updateTitle();
						log(LogLevel::Info, "Renamed vault " + from + " to " + v->name +
							(naming.empty() ? std::string() : " and its " + std::to_string(naming.size()) + " gameplayvault value" + (naming.size() == 1 ? "" : "s")));
					}
					catch (const std::exception& e)
					{
						log(LogLevel::Error, e.what());
					}
					done();
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			}
			ImGui::EndPopup();
		}

		if (begin("Delete Vault"))
		{
			Vault* v = pendingVault_;
			if (!db_ || !v || !v->pack)
			{
				done();
			}
			else
			{
				const KeyNames& names = db_->names();
				const std::vector<Collection*> all = VaultContents(*v);
				const size_t elsewhere = size_t(std::count_if(all.begin(), all.end(), [&](const Collection* c) { return c->vault != v; }));
				const bool eventFile = v->pack->format == PackFormat::VaultFile;
				ImGui::Text("Delete vault %s (%s)?", v->name.c_str(), FileOf(*v->pack).c_str());
				if (all.empty())
					ImGui::TextDisabled("It holds no collections.");
				else
					ImGui::TextColored(kWarn, "Its %zu collection%s go%s too%s.", v->collections.size(), v->collections.size() == 1 ? "" : "s",
						v->collections.size() == 1 ? "es" : "",
						elsewhere ? (", and the " + std::to_string(elsewhere) + " in other vaults made from them").c_str() : "");
				if (eventFile) ImGui::TextDisabled("Saving removes its file and keeps the first original as .bak.");
				const std::vector<Collection*> naming = CollectionsNamingVault(*db_, v->name);
				if (!naming.empty()) ImGui::TextDisabled("The gameplayvault of %s still names it.", SomeNames(names, naming).c_str());
				if (!all.empty()) ImGui::TextDisabled("References to its collections elsewhere are not changed.");
				bool del = ImGui::Button("Delete");
				ImGui::SameLine();
				const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
				if (ConfirmKey()) del = true;
				if (del)
				{
					deleteVault(*v);
					done();
				}
				else if (cancel)
				{
					done();
				}
			}
			ImGui::EndPopup();
		}

		if (begin("New Event Vault File"))
		{
			if (!canMakeEventVaults())
			{
				done();
			}
			else
			{
				ImGui::TextUnformatted("New event vault file");
				ImGui::TextDisabled("World loads each file in gc.vaults as a vault of its own, one per event. It is\n"
									"written when you save.");
				const bool enter = nameBox("vault name");
				LoadedPack probe;
				probe.format = PackFormat::VaultFile;
				std::string problem = nameInput_.empty() ? std::string() : VaultNameProblem(*db_, probe, nameInput_);
				const std::string path = nameInput_.empty() ? std::string() : NewEventVaultPath(*db_, nameInput_);
				if (std::error_code ec; problem.empty() && !path.empty() && fs::exists(FromUtf8(path), ec))
					problem = "gc.vaults has a file " + nameInput_ + ".bin already. Open it, or choose another name.";
				if (!problem.empty()) ImGui::TextColored(kError, "%s", problem.c_str());
				else if (!path.empty()) ImGui::TextDisabled("%s", path.c_str());
				const bool ok = !nameInput_.empty() && problem.empty();
				ImGui::BeginDisabled(!ok);
				if (ImGui::Button("Create") || (enter && ok))
				{
					try
					{
						nameAction("New event vault " + nameInput_);
						LoadedPack& made = CreateEventVaultFile(*db_, path, nameInput_, *EventVaultModel(*db_));
						noteEdit(&made, nullptr);
						structureChanged();
						updateTitle();
						log(LogLevel::Info, "Made the event vault " + nameInput_ + ". Saving writes " + path);
					}
					catch (const std::exception& e)
					{
						log(LogLevel::Error, e.what());
					}
					done();
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			}
			ImGui::EndPopup();
		}
	}

} // namespace app
