// Classes in the editor (core/ClassDesign.h, core/ClassList.h,
// core/GenHeader.h): the Class Designer, deleting a class, restoring one of
// the game's, and the class commands in the menus.

#include "App.h"

#include "core/ClassList.h"
#include "core/Hash.h"
#include "core/Values.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <fstream>

namespace app
{

	using namespace attrib;
	namespace fs = std::filesystem;

	namespace
	{
		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);
		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const char* kWindowClassDesigner = "Class Designer";

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}

		Vault* DatabaseVault(Database& db)
		{
			for (const auto& pk : db.packs())
				for (const auto& v : pk->vaults)
					if (v->isDatabase) return v.get();
			return nullptr;
		}

		bool IsHashName(const std::string& n)
		{
			return n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X');
		}

		// A copy of a design for a class of the user's: the stored values only the
		// game's classes need go (the rules lay a new class out), and static
		// fields, which a new class can't have.
		ClassDesign Fresh(ClassDesign d, size_t* droppedStatics)
		{
			d.collectionReserve.reset();
			d.layoutSize.reset();
			d.layoutCount.reset();
			d.layoutKeyShift.reset();
			d.staticSize.reset();
			const size_t before = d.fields.size();
			d.fields.erase(std::remove_if(d.fields.begin(), d.fields.end(), [](const FieldDesign& f)
			{
				return f.storage == FieldStorage::Static;
			}), d.fields.end());
			if (droppedStatics) *droppedStatics = before - d.fields.size();
			for (FieldDesign& f : d.fields)
			{
				f.offset.reset();
				f.alignment.reset();
				f.flags.reset();
				f.maxCount.reset();
			}
			return d;
		}
	} // namespace

	void App::openClassDesigner(const ClassDef* cls, bool header)
	{
		if (!db_) return;
		ClassDesigner& d = designer_;
		d.open = true;
		d.focus = true;
		d.headerTab = header;
		d.fieldsTab = !header;
		d.editing = cls ? cls->key : 0;
		d.readOnly = cls && IsGameClass(*db_, cls->key);
		if (cls)
		{
			d.draft = DesignOf(*db_, *cls);
		}
		else
		{
			d.draft = ClassDesign{};
			FieldDesign f;
			f.storage = FieldStorage::Layout;
			f.typeHash = StringHash32("EA::Reflection::Float");
			d.draft.fields.push_back(f);
		}
	}

	std::string App::designerHeader()
	{
		if (!db_) return {};
		ClassDesign dd = designer_.draft;
		const ClassDef* existing = designer_.editing ? db_->findClass(designer_.editing) : nullptr;
		if (!DesignProblems(*db_, dd, existing).empty()) return {};
		// The names typed so far, so the header says them.
		if (!dd.name.empty()) db_->names().add(dd.name);
		for (const FieldDesign& f : dd.fields)
			if (!f.name.empty()) db_->names().add(f.name);
		ClassDef laid;
		if (existing) laid.collections = existing->collections;
		LayOutClass(*db_, dd, laid);
		return GenHeader(*db_, laid, designer_.header);
	}

	bool App::applyClassDesign()
	{
		if (!db_) return false;
		ClassDesigner& d = designer_;
		if (d.readOnly)
		{
			log(LogLevel::Error, "A class of the game can't be changed: copy it to make a class of your own.");
			return false;
		}
		ClassDef* existing = d.editing ? db_->findClass(d.editing) : nullptr;
		const ClassDesign draft = d.draft;
		const std::string label = draft.name.empty() ? db_->names().format(draft.key) : draft.name;
		const std::vector<std::string> problems = DesignProblems(*db_, draft, existing && existing->key == draft.key ? existing : nullptr);
		// A rename keeps the class's own collections, so only another class's name is taken.
		std::vector<std::string> real;
		for (const std::string& p : problems)
			if (!(existing && db_->findClass(draft.key) == existing && p.find("there is a class named") == 0)) real.push_back(p);
		if (!real.empty())
		{
			log(LogLevel::Error, "Class " + label + ": " + real.front());
			return false;
		}
		try
		{
			ClassEditor ed(*db_);
			if (!existing)
			{
				Vault* v = DatabaseVault(*db_);
				if (!v)
				{
					log(LogLevel::Error, "Open the database (attributes.bin) first: classes go in its vault.");
					return false;
				}
				nameAction("New class " + label);
				ClassDef& c = ed.create(*v, draft);
				d.editing = c.key;
				log(LogLevel::Info, "Created class " + label + " with " + std::to_string(c.fields.size()) + " fields in vault " + v->name);
			}
			else
			{
				nameAction("Change class " + db_->names().format(existing->key));
				if (draft.key != existing->key) ed.rename(*existing, draft.name);
				const bool changed = ed.redesign(*existing, draft);
				d.editing = existing->key;
				log(LogLevel::Info, changed ? "Changed class " + label + "; its " + std::to_string(existing->collections.size()) + " collections follow"
					: "Class " + label + " is as designed already");
			}
			if (!draft.name.empty()) rememberName(draft.name);
			for (const FieldDesign& f : draft.fields)
				if (!f.name.empty() && !IsHashName(f.name)) rememberName(f.name);
			arrayTails_.clear(); // kept for fields that may have another type now
			structureChanged();
			if (ClassDef* c = db_->findClass(d.editing)) d.draft = DesignOf(*db_, *c);
			return true;
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, "Class " + label + ": " + e.what());
			structureChanged();
			return false;
		}
	}

	void App::forgetClass(ClassDef* cls)
	{
		for (Collection* c : cls->collections) forgetCollection(c, nullptr, false);
		if (pendingClass_ == cls) pendingClass_ = nullptr;
		if (dropClass_ == cls) dropClass_ = nullptr;
		if (explorerFocus_.cls == cls) explorerFocus_ = {};
		if (explorerTarget_.cls == cls) explorerTarget_ = {};
	}

	bool App::removeClass(ClassDef* cls)
	{
		if (!db_ || !cls) return false;
		const std::string name = db_->names().format(cls->key);
		const size_t n = cls->collections.size();
		nameAction("Delete class " + name);
		forgetClass(cls);
		arrayTails_.clear();
		ClassEditor(*db_).remove(*cls);
		structureChanged();
		log(LogLevel::Info, "Deleted class " + name + (n ? " and its " + std::to_string(n) + " collection" + (n == 1 ? "" : "s") : std::string()));
		return true;
	}

	bool App::restoreGameClass(uint32_t classKey)
	{
		if (!db_) return false;
		const std::string name = db_->names().format(classKey);
		nameAction("Restore class " + name);
		const script::ApplyReport r = RestoreRetailClass(*db_, classKey);
		if (!r.ok)
		{
			for (const script::Message& m : r.messages)
				if (m.level == script::Message::Level::Error) log(LogLevel::Error, "Restore " + name + ": " + m.text);
			structureChanged();
			return false;
		}
		structureChanged();
		const ClassList* l = RetailClassesFor(*db_);
		log(LogLevel::Info, "Restored class " + name + " as " + (l ? l->title : std::string("the game's files")) +
			" define it, without collections");
		return true;
	}

	// ------------------------------------------------------------------ menus ---

	void App::drawClassItems(ClassDef* cls)
	{
		if (!db_ || !cls) return;
		const bool game = IsGameClass(*db_, cls->key);
		if (ImGui::MenuItem(game ? "View Class Design..." : "Class Design...")) openClassDesigner(cls);
		if (game && ImGui::IsItemHovered())
			ImGui::SetTooltip("A class of the game: its compiled code reads it as it is, so it can't be changed.\n"
							  "The designer shows it and copies it to make a class of your own.");
		if (ImGui::MenuItem("Attrib::Gen Header...")) openClassDesigner(cls, true);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("C++ for reading its collections the way the game's own classes do");
		if (ImGui::MenuItem("Delete Class..."))
		{
			pendingClass_ = cls;
			request(Pending::DeleteClass);
		}
	}

	void App::drawNewClassItems()
	{
		if (!db_ || !db_->hasSchema()) return;
		if (ImGui::MenuItem("New Class...", nullptr, false, DatabaseVault(*db_) != nullptr)) openClassDesigner(nullptr);
		const ClassList* list = RetailClassesFor(*db_);
		const std::vector<const script::Statement*> missing = MissingRetailClasses(*db_);
		if (ImGui::BeginMenu("Restore Game Class", !missing.empty()))
		{
			ImGui::TextDisabled("As %s defines them", list->title.c_str());
			ImGui::Separator();
			for (const script::Statement* s : missing)
			{
				const uint32_t key = ClassStatementKey(*s);
				size_t fields = 0;
				for (const script::Statement& f : s->body) fields += f.kind == script::Statement::Kind::ClassField && f.word != "bytes";
				const std::string label = db_->names().format(key) + "  (" + std::to_string(fields) + " fields)";
				if (ImGui::MenuItem(label.c_str())) restoreGameClass(key);
			}
			ImGui::EndMenu();
		}
		if (missing.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(list ? "Every class of %s is here" : "No list of the game's classes for %s", list ? list->title.c_str() : db_->target().name().c_str());
	}

	void App::drawDeleteClassPrompt()
	{
		if (pending_ == Pending::DeleteClass && !ImGui::IsPopupOpen("Delete Class")) ImGui::OpenPopup("Delete Class");
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal("Delete Class", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
		auto done = [&]()
		{
			pending_ = Pending::None;
			pendingClass_ = nullptr;
			ImGui::CloseCurrentPopup();
		};
		ClassDef* cls = pendingClass_;
		if (!db_ || !cls)
		{
			done();
			ImGui::EndPopup();
			return;
		}
		const std::string name = db_->names().format(cls->key);
		const size_t n = cls->collections.size();
		ImGui::Text("Delete class %s%s?", name.c_str(), n ? (" and its " + std::to_string(n) + " collection" + (n == 1 ? "" : "s")).c_str() : "");
		if (IsGameClass(*db_, cls->key))
			ImGui::TextColored(kWarn, "%s is a class of the game, and its code uses it: Modify and GenerateUniqueKey\n"
							   "on it crash, and so may code that expects its collections. Edit > Restore\n"
							   "Game Class puts it back as the game's files define it, without its collections.",
				name.c_str());
		ImGui::TextDisabled("References to its collections from other classes are not changed.");
		const bool del = ImGui::Button("Delete");
		ImGui::SameLine();
		const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
		const bool enter = ConfirmKey();
		if (del || (enter && !cancel))
		{
			removeClass(cls);
			done();
		}
		else if (cancel)
		{
			done();
		}
		ImGui::EndPopup();
	}

	// --------------------------------------------------------------- designer ---

	void App::drawClassDesigner()
	{
		ClassDesigner& d = designer_;
		if (!d.open) return;
		const float fs = ImGui::GetFontSize();
		ImGui::SetNextWindowSize(ImVec2(fs * 56, fs * 34), ImGuiCond_FirstUseEver);
		if (d.focus) ImGui::SetNextWindowFocus();
		d.focus = false;
		if (!ImGui::Begin(kWindowClassDesigner, &d.open))
		{
			ImGui::End();
			return;
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) lastPanel_ = Panel::Other;
		if (!db_ || !db_->hasSchema())
		{
			ImGui::TextDisabled("Open a database first (File > Open Folder...).");
			ImGui::End();
			return;
		}
		const KeyNames& names = db_->names();
		ClassDef* existing = d.editing ? db_->findClass(d.editing) : nullptr;
		if (d.editing && !existing)
		{
			// Deleted, or undone: the design stays, as a new class.
			d.editing = 0;
			d.readOnly = false;
		}

		// What it is.
		if (d.readOnly)
			ImGui::TextColored(kWarn, "A class of the game. Its compiled code reads it as it is, so it can't be changed; copy it to make one of your own.");
		else if (existing)
			ImGui::TextDisabled("Your class %s, %zu collection%s. A change carries their values over.", names.format(existing->key).c_str(),
				existing->collections.size(), existing->collections.size() == 1 ? "" : "s");
		else
			ImGui::TextDisabled("A new class, in vault %s of attributes.bin, where the game loads its classes first.",
				DatabaseVault(*db_) ? DatabaseVault(*db_)->name.c_str() : "db");

		ImGui::BeginDisabled(d.readOnly);
		ImGui::SetNextItemWidth(fs * 20);
		if (ImGui::InputTextWithHint("Name", "class name", &d.draft.name)) d.draft.key = d.draft.name.empty() ? 0 : StringHash32(d.draft.name);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("key 0x%08X", d.draft.key);

		// The layout as the rules give it, for the offsets and sizes.
		const std::vector<std::string> problems = DesignProblems(*db_, d.draft, existing);
		std::vector<std::string> shown;
		for (const std::string& p : problems)
			if (!(existing && db_->findClass(d.draft.key) == existing && p.find("there is a class named") == 0)) shown.push_back(p);
		ClassDef laid;
		bool laidOut = false;
		if (shown.empty())
		{
			try
			{
				if (existing) laid.collections = existing->collections;
				LayOutClass(*db_, d.draft, laid);
				laidOut = true;
			}
			catch (const std::exception& e)
			{
				shown.push_back(e.what());
			}
		}

		if (ImGui::BeginTabBar("##designer"))
		{
			const bool toHeader = d.headerTab, toFields = d.fieldsTab;
			d.headerTab = d.fieldsTab = false;
			if (ImGui::BeginTabItem("Fields", nullptr, toFields ? ImGuiTabItemFlags_SetSelected : 0))
			{
				std::vector<const TypeInfo*> types;
				for (const TypeInfo& t : db_->types())
					if (t.size) types.push_back(&t);
				std::sort(types.begin(), types.end(), [](const TypeInfo* a, const TypeInfo* b)
				{
					return ToLower(TypeLabel(a)) < ToLower(TypeLabel(b));
				});
				int moveUp = -1, moveDown = -1, remove = -1;
				const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
				const float footer = ImGui::GetFrameHeightWithSpacing() * 3 + ImGui::GetTextLineHeightWithSpacing() * float(1 + shown.size());
				if (ImGui::BeginTable("##fields", 8, tf, ImVec2(0, -footer)))
				{
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn("Kept in", 0, 0.11f);
					ImGui::TableSetupColumn("Field", 0, 0.24f);
					ImGui::TableSetupColumn("Type", 0, 0.24f);
					ImGui::TableSetupColumn("Array", 0, 0.12f);
					ImGui::TableSetupColumn("Searchable", 0, 0.08f);
					ImGui::TableSetupColumn("Offset", 0, 0.07f);
					ImGui::TableSetupColumn("Size", 0, 0.06f);
					ImGui::TableSetupColumn("", 0, 0.08f);
					ImGui::TableHeadersRow();
					for (size_t i = 0; i < d.draft.fields.size(); ++i)
					{
						FieldDesign& f = d.draft.fields[i];
						ImGui::PushID(int(i));
						ImGui::TableNextRow();
						ImGui::BeginDisabled(d.readOnly);
						bool edited = false;

						ImGui::TableSetColumnIndex(0);
						ImGui::SetNextItemWidth(-FLT_MIN);
						if (ImGui::BeginCombo("##storage", f.storage == FieldStorage::Layout ? "layout" : f.storage == FieldStorage::Static ? "static" : "entry"))
						{
							if (ImGui::Selectable("layout", f.storage == FieldStorage::Layout)) f.storage = FieldStorage::Layout, edited = true;
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every collection holds its own value, at a fixed place in its layout block");
							if (ImGui::Selectable("entry", f.storage == FieldStorage::Entry)) f.storage = FieldStorage::Entry, edited = true;
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("Optional: a collection sets it or inherits it from its parent");
							ImGui::EndCombo();
						}

						ImGui::TableSetColumnIndex(1);
						ImGui::SetNextItemWidth(-FLT_MIN);
						if (ImGui::InputTextWithHint("##name", "field name", &f.name))
						{
							f.key = f.name.empty() ? 0 : StringHash32(f.name);
							edited = true;
						}

						ImGui::TableSetColumnIndex(2);
						const TypeInfo* t = db_->typeByHash(f.typeHash);
						ImGui::SetNextItemWidth(-FLT_MIN);
						if (ImGui::BeginCombo("##type", t ? TypeLabel(t).c_str() : names.format(f.typeHash).c_str(), ImGuiComboFlags_HeightLarge))
						{
							if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
							ImGui::SetNextItemWidth(-FLT_MIN);
							ImGui::InputTextWithHint("##typefilter", "filter", &d.typeFilter);
							const std::string lf = ToLower(d.typeFilter);
							for (const TypeInfo* ti : types)
							{
								const std::string label = TypeLabel(ti);
								if (!lf.empty() && ToLower(label).find(lf) == std::string::npos && ToLower(ti->name).find(lf) == std::string::npos) continue;
								ImGui::PushID(ti);
								if (ImGui::Selectable(label.c_str(), ti->hash == f.typeHash))
								{
									f.typeHash = ti->hash;
									f.size.reset();
									edited = true;
								}
								if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %u bytes", ti->name.c_str(), ti->size);
								ImGui::PopID();
							}
							ImGui::EndCombo();
						}
						if (t && ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %u bytes", t->name.c_str(), t->size);

						ImGui::TableSetColumnIndex(3);
						if (f.storage != FieldStorage::Static)
						{
							if (Checkbox("##array", &f.array))
							{
								if (f.array && f.storage == FieldStorage::Layout && !f.count) f.count = 4;
								edited = true;
							}
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip(f.storage == FieldStorage::Layout ? "A fixed number of slots in the layout" : "As many elements as a collection sets");
							if (f.array)
							{
								ImGui::SameLine();
								ImGui::SetNextItemWidth(-FLT_MIN);
								if (ImGui::InputScalar("##count", ImGuiDataType_U16, &f.count)) edited = true;
								if (ImGui::IsItemHovered())
									ImGui::SetTooltip(f.storage == FieldStorage::Layout ? "Its slots in every collection's layout"
										: "The most elements it may hold (0: no limit)");
							}
						}

						ImGui::TableSetColumnIndex(4);
						if (f.storage == FieldStorage::Layout)
						{
							if (Checkbox("##search", &f.searchable)) edited = true;
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("Found by key (Instance::Get, TAttrib, Lua) as well as through _LayoutStruct");
						}
						ImGui::EndDisabled();

						const FieldDef* lf = laidOut ? laid.field(f.key) : nullptr;
						ImGui::TableSetColumnIndex(5);
						if (lf && !lf->isArray() && f.storage != FieldStorage::Entry) ImGui::Text("%u", LayoutOffset(*lf));
						else if (lf && f.storage == FieldStorage::Layout) ImGui::Text("%u", lf->offset);
						ImGui::TableSetColumnIndex(6);
						if (lf) ImGui::Text("%u", f.storage == FieldStorage::Layout ? LayoutExtent(*lf, lf->maxCount) : uint32_t(lf->size));

						ImGui::TableSetColumnIndex(7);
						if (!d.readOnly)
						{
							if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveUp = int(i);
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("Earlier in the layout");
							ImGui::SameLine(0, 2);
							if (ImGui::ArrowButton("##down", ImGuiDir_Down)) moveDown = int(i);
							ImGui::SameLine(0, 2);
							if (ImGui::SmallButton("x")) remove = int(i);
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the field");
						}
						// A changed field is laid out by the rules again.
						if (edited)
						{
							f.offset.reset();
							f.alignment.reset();
							f.flags.reset();
							f.maxCount.reset();
						}
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
				auto& fv = d.draft.fields;
				if (moveUp > 0) std::swap(fv[size_t(moveUp)], fv[size_t(moveUp) - 1]);
				if (moveDown >= 0 && size_t(moveDown) + 1 < fv.size()) std::swap(fv[size_t(moveDown)], fv[size_t(moveDown) + 1]);
				if (remove >= 0) fv.erase(fv.begin() + remove);

				if (!d.readOnly && ImGui::Button("Add Field"))
				{
					FieldDesign f;
					f.typeHash = StringHash32("EA::Reflection::Float");
					fv.push_back(f);
				}
				if (!d.readOnly) ImGui::SameLine();
				if (laidOut)
				{
					uint16_t searchable = laid.layoutCount;
					ImGui::TextDisabled("Layout %u bytes, %u searchable fields; %zu field%s in all", laid.layoutSize, searchable, laid.fields.size(),
						laid.fields.size() == 1 ? "" : "s");
				}
				for (const std::string& p : shown) ImGui::TextColored(kError, "%s", p.c_str());
				if (shown.empty())
					for (const std::string& w : DesignWarnings(*db_, d.draft)) ImGui::TextColored(kWarn, "%s", w.c_str());

				if (d.readOnly)
				{
					if (ImGui::Button("Copy as New Class"))
					{
						size_t dropped = 0;
						d.draft = Fresh(d.draft, &dropped);
						d.draft.name += "_copy";
						d.draft.key = StringHash32(d.draft.name);
						d.editing = 0;
						d.readOnly = false;
						if (dropped)
							log(LogLevel::Warning, "The copy leaves out " + std::to_string(dropped) +
								" static field(s): the game keeps static values only for its own classes.");
					}
				}
				else
				{
					ImGui::BeginDisabled(!shown.empty());
					if (ImGui::Button(existing ? "Apply Changes" : "Create Class")) applyClassDesign();
					ImGui::EndDisabled();
					if (existing)
					{
						ImGui::SameLine();
						if (ImGui::Button("Revert")) d.draft = DesignOf(*db_, *existing);
					}
				}
				if (existing)
				{
					ImGui::SameLine();
					if (ImGui::Button("Delete Class..."))
					{
						pendingClass_ = existing;
						request(Pending::DeleteClass);
					}
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Attrib::Gen Header", nullptr, toHeader ? ImGuiTabItemFlags_SetSelected : 0))
			{
				Checkbox("Set_ accessors", &d.header.setters);
				ImGui::SameLine();
				Checkbox("The game's types from the schema", &d.header.types);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Definitions of the structs and enums its fields use, for code that hasn't got them");
				std::string text = designerHeader();
				ImGui::SameLine();
				ImGui::BeginDisabled(text.empty());
				if (ImGui::Button("Copy")) ImGui::SetClipboardText(text.c_str());
				ImGui::SameLine();
				if (ImGui::Button("Save As..."))
				{
					const std::string file = CppIdentifier(d.draft.name.empty() ? names.format(d.draft.key) : d.draft.name) + ".h";
					if (auto p = host_.saveFileDialog("Save Attrib::Gen header", file, { {"C++ headers", "*.h;*.hpp"} }))
					{
						std::ofstream out(FromUtf8(*p), std::ios::binary);
						out << text;
						if (out) log(LogLevel::Info, "Saved " + *p);
						else log(LogLevel::Error, "Could not write " + *p);
					}
				}
				ImGui::EndDisabled();
				if (text.empty())
				{
					ImGui::TextColored(kError, "%s", shown.empty() ? "No header." : ("Fix the design first: " + shown.front()).c_str());
				}
				else
				{
					if (codeFont_) ImGui::PushFont(codeFont_, 0.0f);
					ImGui::InputTextMultiline("##header", &text, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
					if (codeFont_) ImGui::PopFont();
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();
	}

} // namespace app
