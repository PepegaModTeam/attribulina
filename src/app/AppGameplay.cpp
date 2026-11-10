// The gameplay view: the gameplay class as folders (element paths) and
// templates, an inspector header with an element's template, owner and vaults,
// and the commands that keep gameplay's conventions (core/Gameplay.h).

#include "App.h"

#include "core/Edit.h"
#include "core/Hash.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <map>

namespace app
{

	using namespace attrib;

	namespace
	{

		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);

		std::string LastSegment(const std::string& path)
		{
			const size_t at = path.rfind('/');
			return at == std::string::npos ? path : path.substr(at + 1);
		}

		std::string ParentPath(const std::string& path)
		{
			const size_t at = path.rfind('/');
			return at == std::string::npos ? std::string() : path.substr(0, at);
		}

		// "autobahn_gd_grip_1" -> "autobahn_gd_grip_2" (the first free number), else "<name>_copy".
		std::string NextFreeName(const Database& db, uint32_t classKey, const std::string& path)
		{
			size_t digits = path.size();
			while (digits > 0 && std::isdigit(static_cast<unsigned char>(path[digits - 1]))) --digits;
			const std::string stem = digits < path.size() ? path.substr(0, digits) : path + "_";
			int n = digits < path.size() ? std::atoi(path.c_str() + digits) + 1 : 2;
			for (; n < 10000; ++n)
			{
				const std::string candidate = stem + std::to_string(n);
				if (!db.findCollection(classKey, StringHash32(candidate))) return candidate;
			}
			return path + "_copy";
		}

		// The gameplayvault name siblings with the same template suggest for `leaf`:
		// "<leaf>" (track layouts) or "<prefix><leaf>" (races: "an_2_1_" + "gr_1").
		std::string SuggestChildrenVault(const GameplayModel& m, const KeyNames& names, const Collection* owner,
										 const Collection* templ, const std::string& leaf)
		{
			if (leaf.empty()) return {};
			if (owner)
				for (const Collection* s : m.listedChildren(owner))
				{
					if (templ && s->parent != templ) continue;
					const std::string v = m.childrenVaultName(s);
					const std::string sl = LastSegment(names.format(s->key));
					if (v.empty() || sl.empty()) continue;
					if (v == sl) return leaf;
					if (v.size() > sl.size() && v.compare(v.size() - sl.size(), sl.size(), sl) == 0)
						return v.substr(0, v.size() - sl.size()) + leaf;
				}
			return leaf;
		}

		// Open or close a folder's or template's node and every one below it, without
		// drawing them; the ID stack must be where the node is drawn.
		void SetFolderOpen(const GameplayModel::Node* n, bool open)
		{
			if (n->children.empty()) return;
			const ImGuiID id = ImGui::GetID(n->segment.c_str());
			ImGui::TreeNodeSetOpen(id, open);
			ImGui::PushOverrideID(id);
			for (const GameplayModel::Node* ch : n->children) SetFolderOpen(ch, open);
			ImGui::PopID();
		}

		void SetTemplateOpen(const Collection* t, const std::unordered_map<const Collection*, std::vector<Collection*>>& kids, bool open)
		{
			const auto it = kids.find(t);
			if (it == kids.end()) return;
			const ImGuiID id = ImGui::GetID(KeyId(t->key));
			ImGui::TreeNodeSetOpen(id, open);
			ImGui::PushOverrideID(id);
			for (const Collection* k : it->second) SetTemplateOpen(k, kids, open);
			ImGui::PopID();
		}

	} // namespace

	bool App::isGameplay(const Collection* c) const
	{
		return c && gameplay_ && c->cls == gameplay_->cls();
	}

	// --------------------------------------------------------------- explorer ---

	void App::drawGameplayTab()
	{
		const GameplayModel& m = *gameplay_;
		ImGui::BeginChild("##gameplay", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		resetTree(Tree::Gameplay);
		// A command from a menu below can rebuild gameplay_ (and free `m`): the walks stop then.
		const uint64_t walk = cacheVersion_;
		bool all = false;
		const bool toggleAll = takeTreeToggleAll(Tree::Gameplay, all); // the sections that are open
		if (ImGui::CollapsingHeader("Folders", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::PushID("folders");
			if (toggleAll)
				for (const GameplayModel::Node* n : m.root().children) SetFolderOpen(n, all);
			for (const GameplayModel::Node* n : m.root().children)
			{
				drawGameplayFolder(n);
				if (cachesRebuiltSince(walk)) break;
			}
			ImGui::PopID();
		}
		if (!cachesRebuiltSince(walk) && gameplay_ && ImGui::CollapsingHeader("Templates"))
		{
			// Templates nest by inheritance: baseelement > activity > race > ...
			std::unordered_map<const Collection*, std::vector<Collection*>> kids;
			std::vector<Collection*> roots;
			for (Collection* t : m.templates())
			{
				if (t->parent && m.isTemplate(t->parent)) kids[t->parent].push_back(t);
				else roots.push_back(t);
			}
			ImGui::PushID("templates");
			if (toggleAll)
				for (Collection* t : roots) SetTemplateOpen(t, kids, all);
			for (Collection* t : roots)
			{
				drawGameplayTemplate(t, kids);
				if (cachesRebuiltSince(walk)) break;
			}
			ImGui::PopID();
		}
		if (revealIn_ == 2)
		{
			reveal_.clear();
			revealIn_ = 0;
		}
		treeBackgroundMenu(Tree::Gameplay);
		ImGui::EndChild();
	}

	void App::drawGameplayFolder(const GameplayModel::Node* n)
	{
		const uint64_t walk = cacheVersion_;
		Collection* c = n->collection;
		const bool leaf = n->children.empty();
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanLabelWidth;
		if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if (c && c == selected_) flags |= ImGuiTreeNodeFlags_Selected;
		const ImGuiID id = ImGui::GetID(n->segment.c_str()); // by name: an undo makes a new model
		if (bool to; !leaf && takeTreeToggle(id, to)) SetFolderOpen(n, to);
		const bool enter = c && NavEnterOn(id);
		if (reveal_.count(n) || (enter && !leaf))
		{
			// Enter opens the element: cancel the toggle it also makes.
			const bool want = reveal_.count(n) || ImGui::TreeNodeGetOpen(id);
			ImGui::SetNextItemOpen(enter && !leaf ? !want : want);
		}
		if (!c) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		const bool open = ImGui::TreeNodeEx(n->segment.c_str(), flags, "%s", n->segment.c_str());
		if (!c) ImGui::PopStyleColor();
		if (c && (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) &&
			!ImGui::IsItemToggledOpen())
			select(c);
		if (enter) openItem(c);
		linkClicks(c);
		if (c && ImGui::IsItemFocused()) explorerFocus_ = { c, c->cls, EditTarget::Kind::Element };
		if (!leaf) noteTreeFocus(id, open);
		if (c && c == selected_ && scrollToSelection_)
		{
			ImGui::SetScrollHereY(0.4f);
			scrollToSelection_ = false;
		}
		gameplayContextMenu(c, n, leaf ? 0 : id, open);
		if (cachesRebuiltSince(walk))
		{
			// The model `n` belongs to is gone.
			if (open && !leaf) ImGui::TreePop();
			return;
		}
		// The template, and how many elements the folder holds.
		if (c && c->parent)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", db_->names().format(c->parent->key).c_str());
		}
		if (!leaf)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%zu)", n->elementCount - (c ? 1 : 0));
		}
		if (open && !leaf)
		{
			for (const GameplayModel::Node* ch : n->children)
			{
				drawGameplayFolder(ch);
				if (cachesRebuiltSince(walk)) break;
			}
			ImGui::TreePop();
		}
	}

	void App::drawGameplayTemplate(Collection* t, const std::unordered_map<const Collection*, std::vector<Collection*>>& kids)
	{
		const uint64_t walk = cacheVersion_;
		const auto it = kids.find(t);
		const bool leaf = it == kids.end();
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanLabelWidth;
		if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if (t == selected_) flags |= ImGuiTreeNodeFlags_Selected;
		const ImGuiID id = ImGui::GetID(KeyId(t->key));
		if (bool to; !leaf && takeTreeToggle(id, to)) SetTemplateOpen(t, kids, to);
		const bool enter = NavEnterOn(id);
		if (enter && !leaf) ImGui::SetNextItemOpen(!ImGui::TreeNodeGetOpen(id)); // Enter opens, it doesn't toggle
		const bool open = ImGui::TreeNodeEx(KeyId(t->key), flags, "%s", db_->names().format(t->key).c_str());
		if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) && !ImGui::IsItemToggledOpen())
			select(t);
		if (enter) openItem(t);
		linkClicks(t);
		if (ImGui::IsItemFocused()) explorerFocus_ = { t, t->cls, EditTarget::Kind::Template };
		if (!leaf) noteTreeFocus(id, open);
		if (ImGui::BeginPopupContextItem())
		{
			ImGui::TextDisabled("template %s", db_->names().format(t->key).c_str());
			ImGui::Separator();
			if (ImGui::MenuItem("Open in New Tab", "Ctrl+Enter")) openInNewTab(t, false);
			if (!leaf) treeToggleItem(id, open);
			if (!gameplay_->statesOf(t).empty() && ImGui::MenuItem("Open in State Graph Builder")) openStateGraphBuilder(t);
			ImGui::Separator();
			drawEditItems({ t, t->cls, EditTarget::Kind::Template });
			ImGui::EndPopup();
		}
		if (cachesRebuiltSince(walk) || !gameplay_)
		{
			if (open && !leaf) ImGui::TreePop();
			return;
		}
		const size_t states = gameplay_->statesOf(t).empty() ? 0 : gameplay_->allStatesOf(t).size();
		if (states)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%zu states", states);
		}
		if (open && !leaf)
		{
			for (Collection* k : it->second)
			{
				drawGameplayTemplate(k, kids);
				if (cachesRebuiltSince(walk)) break;
			}
			ImGui::TreePop();
		}
	}

	void App::gameplayContextMenu(Collection* c, const GameplayModel::Node* folder, ImGuiID treeNode, bool open)
	{
		if (!ImGui::BeginPopupContextItem()) return;
		const KeyNames& names = db_->names();
		ImGui::TextDisabled("%s", folder ? folder->path.c_str() : names.format(c->key).c_str());
		ImGui::Separator();
		if (treeNode && !c)
		{
			treeToggleItem(treeNode, open);
			ImGui::Separator();
		}
		if (c)
		{
			if (ImGui::MenuItem("Open in New Tab", "Ctrl+Enter")) openInNewTab(c, false);
			if (treeNode) treeToggleItem(treeNode, open);
			ImGui::Separator();
			drawEditItems({ c, c->cls, EditTarget::Kind::Element });
			ImGui::Separator();
			if (ImGui::MenuItem(isFavorite(c) ? "Remove from Favorites" : "Add to Favorites")) toggleFavorite(c);
		}
		else
		{
			// A path no element has: what is under it all the same.
			if (folder && ImGui::BeginMenu("Add to Script"))
			{
				if (ImGui::MenuItem("With Everything Under It")) addGameplayToScript(GameplayFolderContents(*gameplay_, *folder));
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("The elements under this path and those their Children lists name, with the vaults\n"
									  "they are in and their owners' Children lists, so the game finds them");
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(folder ? folder->path.c_str() : "");
		}
		ImGui::EndPopup();
	}

	void App::requestGameplayDuplicate(Collection* c)
	{
		request(Pending::GameplayDuplicate, c);
		nameInput_ = NextFreeName(*db_, c->classKey, db_->names().format(c->key));
		withChildren_ = true;
	}

	// -------------------------------------------------------------- inspector ---

	// gameplay \ folder \ ... \ element, following the element's path.
	void App::drawGameplayBreadcrumb(Collection* c)
	{
		const KeyNames& names = db_->names();
		std::vector<const GameplayModel::Node*> chain;
		for (const auto* n = gameplay_->nodeOf(c); n && n->parent; n = n->parent) chain.push_back(n);
		std::reverse(chain.begin(), chain.end());
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", names.format(c->classKey).c_str());
		for (const auto* n : chain)
		{
			ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::TextDisabled("\\");
			ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::PushID(n);
			if (n->collection == c)
			{
				ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
				ImGui::TextUnformatted(n->segment.c_str());
				ImGui::PopFont();
			}
			else if (n->collection)
			{
				if (ImGui::TextLink(n->segment.c_str())) select(n->collection, true);
				linkClicks(n->collection);
			}
			else
			{
				ImGui::TextDisabled("%s", n->segment.c_str());
			}
			ImGui::PopID();
		}
	}

	// Template, owner and vaults, then the element commands.
	void App::drawGameplayHeader(Collection* c)
	{
		const GameplayModel& m = *gameplay_;
		const KeyNames& names = db_->names();
		auto link = [&](const char* what, Collection* target)
		{
			ImGui::TextDisabled("%s", what);
			ImGui::SameLine();
			if (!target)
			{
				ImGui::TextDisabled("(none)");
				return;
			}
			ImGui::PushID(what);
			if (ImGui::TextLink(names.format(target->key).c_str())) select(target, true);
			linkClicks(target);
			ImGui::PopID();
		};
		link("Template:", c->parent);
		ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
		link("Owner:", m.owner(c));
		ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
		ImGui::TextDisabled("Stored in:");
		ImGui::SameLine();
		ImGui::TextUnformatted(c->vault ? c->vault->name.c_str() : "?");
		if (c->vault && c->vault->pack && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c->vault->pack->path.c_str());
		ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
		const std::string cv = m.childrenVaultName(c);
		ImGui::TextDisabled("Children vault:");
		ImGui::SameLine();
		if (cv.empty())
		{
			ImGui::TextDisabled("(none)");
		}
		else
		{
			const bool exists = c->vault && c->vault->pack && FindVault(*c->vault->pack, cv);
			ImGui::TextUnformatted(cv.c_str());
			if (!exists)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(created with the first child)");
			}
		}

		const size_t kids = m.listedChildren(c).size();
		if (ImGui::Button("New Element Here...")) requestGameplayNew(c, nullptr);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new element listed in %s's Children", names.format(c->key).c_str());
		ImGui::SameLine();
		if (ImGui::Button("Duplicate...")) requestGameplayDuplicate(c);
		ImGui::SameLine();
		if (ImGui::Button("Delete...")) request(Pending::GameplayDelete, c);
		ImGui::SameLine();
		if (ImGui::Button("References")) findReferences(c->classKey, c->key);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("What refers to this element (Shift+F12 in the Explorer)");
		// Activities, their states and message handlers open in the State Graph Builder.
		const bool scripted = !m.statesOf(c).empty() || c->find(StringHash32("templateref")) || c->find(StringHash32("stateref"));
		if (scripted)
		{
			ImGui::SameLine();
			if (ImGui::Button("State Graph Builder")) openStateGraphBuilder(c);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("  %zu listed children", kids);
	}

	// ----------------------------------------------------------------- modals ---

	void App::requestGameplayNew(Collection* owner, Collection* templ)
	{
		request(Pending::GameplayNew, owner);
		// Default template: what most of the owner's children are made from.
		if (!templ && owner && gameplay_)
		{
			std::map<Collection*, int> count;
			for (Collection* s : gameplay_->listedChildren(owner))
				if (s->parent) ++count[s->parent];
			int best = 0;
			for (const auto& [t, n] : count)
				if (n > best)
				{
					best = n; templ = t;
				}
		}
		pendingTemplate_ = templ;
		vaultInput_.clear();
		templateFilter_.clear();
	}

	void App::drawGameplayModals()
	{
		if (!gameplay_) return;
		const GameplayModel& m = *gameplay_;
		const KeyNames& names = db_->names();
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
			pendingTemplate_ = nullptr;
			ImGui::CloseCurrentPopup();
		};
		const float w = ImGui::GetFontSize() * 28;

		if (begin("New Gameplay Element"))
		{
			Collection* owner = pendingTarget_;
			const std::string ownerPath = owner ? names.format(owner->key) : std::string();
			if (owner) ImGui::Text("New element listed in %s", ownerPath.c_str());
			else ImGui::TextUnformatted("New top-level element (no owner lists it)");

			// Template: the ones the owner's children use first, then the rest.
			std::vector<Collection*> usual;
			if (owner)
				for (Collection* s : m.listedChildren(owner))
					if (s->parent && std::find(usual.begin(), usual.end(), s->parent) == usual.end()) usual.push_back(s->parent);
			ImGui::SetNextItemWidth(w);
			if (ImGui::BeginCombo("template", pendingTemplate_ ? names.format(pendingTemplate_->key).c_str() : "(choose)",
								  ImGuiComboFlags_HeightLarge))
			{
				if (ImGui::IsWindowAppearing())
				{
					templateFilter_.clear();
					ImGui::SetKeyboardFocusHere();
				}
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint("##tf", "search", &templateFilter_);
				const std::string f = ToLower(templateFilter_);
				auto row = [&](Collection* t)
				{
					const std::string n = names.format(t->key);
					if (!f.empty() && ToLower(n).find(f) == std::string::npos) return;
					ImGui::PushID(t);
					if (ImGui::Selectable(n.c_str(), t == pendingTemplate_)) pendingTemplate_ = t;
					if (t->parent)
					{
						ImGui::SameLine();
						ImGui::TextDisabled("%s", names.format(t->parent->key).c_str());
					}
					ImGui::PopID();
				};
				if (!usual.empty())
				{
					ImGui::SeparatorText("used here");
					for (Collection* t : usual) row(t);
					ImGui::SeparatorText("all templates");
				}
				for (Collection* t : m.templates()) row(t);
				ImGui::EndCombo();
			}
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(w);
			const bool enter = ImGui::InputTextWithHint("name", "e.g. gr_3", &nameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
			const std::string path = nameInput_.empty() ? std::string() : (owner ? ownerPath + "/" + nameInput_ : nameInput_);
			const bool taken = !path.empty() && db_->findCollection(m.cls()->key, StringHash32(path));
			ImGui::TextDisabled("%s", path.empty() ? "path: -" : ("path: " + path).c_str());
			if (taken) ImGui::TextColored(kError, "That element already exists.");

			const std::string suggested = SuggestChildrenVault(m, names, owner, pendingTemplate_, nameInput_);
			ImGui::SetNextItemWidth(w);
			ImGui::InputTextWithHint("children vault", suggested.empty() ? "(none)" : suggested.c_str(), &vaultInput_);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Its gameplayvault: the vault the game loads its children from. It is created\n"
								  "when the first child is added. Empty uses the suggestion; '-' leaves it unset.");
			const std::string vault = vaultInput_ == "-" ? std::string() : vaultInput_.empty() ? suggested : vaultInput_;
			const std::string storage = StorageVaultName(m, owner);
			const bool storageExists = !storage.empty() && owner && owner->vault && owner->vault->pack &&
				FindVault(*owner->vault->pack, storage);
			if (!storage.empty())
				ImGui::TextDisabled("stored in vault %s%s", storage.c_str(), storageExists ? "" : " (new)");
			else
				ImGui::TextDisabled("stored in vault %s", owner && owner->vault ? owner->vault->name.c_str()
									: pendingTemplate_ && pendingTemplate_->vault ? pendingTemplate_->vault->name.c_str()
									: "?");

			const bool ok = !path.empty() && !taken && pendingTemplate_;
			ImGui::BeginDisabled(!ok);
			if (ImGui::Button("Create") || (enter && ok))
			{
				try
				{
					NewGameplayElement e{ path, pendingTemplate_, owner, vault };
					Collection& n = CreateGameplayElement(*db_, m, e);
					rememberName(path);
					nameAction("New " + path);
					markDirty(&n);
					if (owner) markDirty(owner);
					structureChanged();
					select(&n, true);
					log(LogLevel::Info, "Created " + path + " from " + names.format(e.templ->key) + " in vault " + n.vault->name);
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Error, ex.what());
				}
				done();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			ImGui::EndPopup();
		}

		if (begin("Duplicate Element"))
		{
			Collection* src = pendingTarget_;
			const std::string oldPath = names.format(src->key);
			ImGui::Text("Duplicate %s", oldPath.c_str());
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(w);
			const bool enter = ImGui::InputTextWithHint("new path", "path", &nameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
			const std::vector<Collection*> subtree = GameplaySubtree(m, *src);
			Checkbox("With everything listed under it", &withChildren_);
			ImGui::SameLine();
			ImGui::TextDisabled("(%zu elements)", subtree.size());
			const bool taken = !nameInput_.empty() && db_->findCollection(m.cls()->key, StringHash32(nameInput_));
			if (taken) ImGui::TextColored(kError, "That element already exists.");
			if (ParentPath(nameInput_) != ParentPath(oldPath))
				ImGui::TextColored(kWarn, "The copy stays listed under %s; only its name moves.", ParentPath(oldPath).c_str());
			size_t handlers = 0;
			for (Collection* x : withChildren_ ? subtree : std::vector<Collection*>{ src })
				handlers += x->find(StringHash32("bytecode")) != nullptr;
			if (handlers)
				ImGui::TextDisabled("%zu message handler(s) are recompiled with their new function names.", handlers);
			const bool ok = !nameInput_.empty() && !taken && nameInput_ != oldPath;
			ImGui::BeginDisabled(!ok);
			if (ImGui::Button("Duplicate") || (enter && ok))
			{
				try
				{
					std::vector<std::string> warnings;
					Collection& n = DuplicateGameplayElement(*db_, m, *src, nameInput_, withChildren_, &warnings);
					for (const std::string& w : warnings) log(LogLevel::Warning, w);
					rememberName(nameInput_);
					nameAction("Duplicate " + oldPath);
					markDirty(&n);
					structureChanged();
					select(&n, true);
					log(LogLevel::Info, "Duplicated " + oldPath + " as " + nameInput_);
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Error, ex.what());
				}
				done();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			ImGui::EndPopup();
		}

		if (begin("Delete Element"))
		{
			Collection* c = pendingTarget_;
			const std::vector<Collection*> subtree = GameplaySubtree(m, *c);
			ImGui::Text("Delete %s", names.format(c->key).c_str());
			if (subtree.size() > 1) ImGui::Text("and the %zu elements listed under it?", subtree.size() - 1);
			ImGui::TextDisabled("It is taken out of its owner's Children. Other references to it are not changed.");
			bool del = ImGui::Button("Delete");
			ImGui::SameLine();
			const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			if (ConfirmKey()) del = true; // Enter deletes, Escape cancels
			if (del)
			{
				try
				{
					Collection* owner = m.owner(c);
					nameAction("Delete " + names.format(c->key));
					markDirty(c);
					for (Collection* x : subtree) forgetCollection(x, owner);
					const std::string name = names.format(c->key);
					DeleteGameplayElement(*db_, m, *c);
					if (owner) markDirty(owner);
					structureChanged();
					log(LogLevel::Info, "Deleted " + name + (subtree.size() > 1 ? " and " + std::to_string(subtree.size() - 1) + " elements under it" : ""));
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Error, ex.what());
				}
				done();
			}
			else if (cancel)
			{
				done();
			}
			ImGui::EndPopup();
		}
	}

} // namespace app
