// The Search panel: values, field names and references across the whole
// database (Search.h), with hits that open their collection and point at the
// field.

#include "App.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <cfloat>
#include <chrono>

namespace app
{

	using namespace attrib;

	namespace
	{
		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);
		const char* kWindowSearch = "Search";

		// The result table's columns as text, for the clipboard.
		enum Column
		{
			kCollection, kField, kValue
		};
		std::string Cell(const KeyNames& names, const SearchHit& h, int column)
		{
			switch (column)
			{
				case kCollection: return names.format(h.classKey) + "/" + names.format(h.key);
				case kField: return h.where;
				default: return h.value;
			}
		}
		// One hit as a line: its columns split by tabs, which a text editor shows
		// as columns and a spreadsheet pastes into them.
		std::string Line(const KeyNames& names, const SearchHit& h)
		{
			return Cell(names, h, kCollection) + "\t" + Cell(names, h, kField) + "\t" + Cell(names, h, kValue);
		}
		std::string Lines(const KeyNames& names, const std::vector<SearchHit>& hits, int column = -1)
		{
			std::string out;
			if (column < 0) out = "Collection\tField\tValue\n";
			for (const SearchHit& h : hits) out += (column < 0 ? Line(names, h) : Cell(names, h, column)) + "\n";
			return out;
		}
		// A value cell on its own: a text without the quotes it is shown in.
		std::string Unquoted(const std::string& v)
		{
			return v.size() >= 2 && v.front() == '"' && v.back() == '"' ? v.substr(1, v.size() - 2) : v;
		}
	} // namespace

	void App::openSearch()
	{
		// Find in Database looks for values; references start from a collection.
		if (searchQuery_.mode == SearchMode::References) 
			searchQuery_.mode = SearchMode::Values;
		showSearch_ = true;
		focusSearch_ = true;
		bringSearch_ = true;
	}

	void App::findReferences(uint32_t classKey, uint32_t key)
	{
		if (!db_) return;
		const KeyNames& names = db_->names();
		searchQuery_ = {};
		searchQuery_.mode = SearchMode::References;
		searchText_ = names.format(classKey) + (key ? "/" + names.format(key) : std::string());
		showSearch_ = true;
		bringSearch_ = true;
		runSearch();
	}

	void App::runSearch()
	{
		if (!db_) return;
		SearchQuery q = searchQuery_;
		q.text = searchText_;
		searchRan_ = true;
		if (q.mode == SearchMode::References)
		{
			if (!ParseReferenceTarget(*db_, searchText_, q.refClass, q.refKey))
			{
				searchResult_ = {};
				searchResult_.error = searchText_.empty() ? "Type class/collection, or a class name"
					: "No collection " + searchText_ + " (type class/collection, or a class name)";
				return;
			}
		}
		const auto t0 = std::chrono::steady_clock::now();
		searchResult_ = RunSearch(*db_, q);
		searchMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}

	void App::drawSearch()
	{
		if (!showSearch_) return;
		// Docked with the Log the first time it shows.
		if (ImGuiWindow* logWindow = ImGui::FindWindowByName("Log"); logWindow && logWindow->DockId)
			ImGui::SetNextWindowDockID(logWindow->DockId, ImGuiCond_FirstUseEver);
		if (bringSearch_)
		{
			ImGui::SetNextWindowFocus();
			bringSearch_ = false;
		}
		if (!ImGui::Begin(kWindowSearch, &showSearch_))
		{
			ImGui::End();
			return;
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) lastPanel_ = Panel::Other;
		if (!db_ || !db_->hasSchema())
		{
			ImGui::TextDisabled("Open a database first.");
			ImGui::End();
			return;
		}
		const KeyNames& names = db_->names();
		const float em = ImGui::GetFontSize();

		// Mode, query, Search.
		int mode = int(searchQuery_.mode);
		ImGui::SetNextItemWidth(em * 8);
		if (ImGui::Combo("##mode", &mode, "Values\0Field names\0References to\0")) searchQuery_.mode = SearchMode(mode);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Values: text, names, numbers or 0x... in any field.\nField names: collections that set a field.\n"
							  "References to: what points at a collection (class/collection) or a class.");
		ImGui::SameLine();
		const char* hint = searchQuery_.mode == SearchMode::Values ? "a value: text, a name, a number or 0x..."
			: searchQuery_.mode == SearchMode::Fields ? "part of a field name"
			: "class/collection, or a class";
		ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("Search").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x));
		if (focusSearch_)
		{
			ImGui::SetKeyboardFocusHere();
			focusSearch_ = false;
		}
		const bool enter = ImGui::InputTextWithHint("##query", hint, &searchText_,
													ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		ImGui::SameLine();
		if (ImGui::Button("Search") || enter) 
			runSearch();

		// Options.
		const ClassDef* in = searchQuery_.inClass ? db_->findClass(searchQuery_.inClass) : nullptr;
		ImGui::SetNextItemWidth(em * 14);
		if (ImGui::BeginCombo("##in", in ? ("in " + names.format(in->key)).c_str() : "in all classes", ImGuiComboFlags_HeightLarge))
		{
			if (ImGui::IsWindowAppearing())
			{
				searchClassFilter_.clear();
				ImGui::SetKeyboardFocusHere();
			}
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##classfilter", "filter", &searchClassFilter_);
			if (ImGui::Selectable("all classes", !in)) searchQuery_.inClass = 0;
			const std::string f = ToLower(searchClassFilter_);
			for (const ClassDef* cls : index_.classes)
			{
				const std::string n = names.format(cls->key);
				if (!f.empty() && ToLower(n).find(f) == std::string::npos) continue;
				ImGui::PushID(cls);
				if (ImGui::Selectable(n.c_str(), cls == in)) searchQuery_.inClass = cls->key;
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		if (searchQuery_.mode != SearchMode::References)
		{
			ImGui::SameLine();
			Checkbox(searchQuery_.mode == SearchMode::Values ? "Whole value" : "Whole name", &searchQuery_.wholeValue);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(searchQuery_.mode == SearchMode::Values ? "Off: values that contain the text. Numbers always match whole."
								  : "Off: fields whose name contains the text.");
			ImGui::SameLine();
			int hash = int(searchQuery_.hexHash);
			ImGui::SetNextItemWidth(em * 10);
			if (ImGui::Combo("##hash", &hash, "0x is StringHash32\0" "0x is bStringHash\0")) searchQuery_.hexHash = SearchHash(hash);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("A 0x... number finds what holds that number, and the names and text it is the hash of.\n"
								  "StringHash32 hashes keys and collection and field names, and 0xBD014895 is HELLO's.\n"
								  "bStringHash hashes type_bStringHash values, and 0x02E8DF33 is HELLO's.\n"
								  "A name nobody knows shows as its hash, and part of the hash finds it too, as 0xBD01 does.");
		}
		ImGui::SameLine();
		const SearchResult& r = searchResult_;
		if (!r.error.empty())
		{
			ImGui::TextColored(kError, "%s", r.error.c_str());
		}
		else if (searchRan_)
		{
			ImGui::TextDisabled("%zu hit%s in %zu collections searched, %.0f ms%s", r.hits.size(), r.hits.size() == 1 ? "" : "s",
								r.collectionsSearched, searchMs_, r.truncated ? " (stopped at the limit)" : "");
		}
		else
		{
			ImGui::TextDisabled("Enter searches. Shift+F12 on a collection in the Explorer finds what refers to it.");
		}
		if (searchQuery_.mode == SearchMode::Fields && !r.fields.empty())
		{
			std::string defs;
			for (size_t i = 0; i < r.fields.size() && i < 12; ++i)
				defs += (i ? ", " : "") + names.format(r.fields[i].first) + "." + names.format(r.fields[i].second);
			if (r.fields.size() > 12) defs += ", ... (" + std::to_string(r.fields.size()) + " in all)";
			ImGui::TextWrapped("Defined in %s", defs.c_str());
		}

		// Hits: click opens the collection at the field, middle-click a background tab.
		const ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
		if (!r.hits.empty() && ImGui::BeginTable("##hits", 3, tf))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Collection", ImGuiTableColumnFlags_WidthStretch, 0.4f);
			ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 0.3f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.3f);
			ImGui::TableHeadersRow();
			std::optional<size_t> open, refsOf;
			bool openInTab = false;
			auto copy = [](const std::string& text) { ImGui::SetClipboardText(text.c_str()); };
			ImGuiListClipper clip;
			clip.Begin(int(r.hits.size()));
			while (clip.Step())
				for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
				{
					const SearchHit& h = r.hits[size_t(i)];
					Collection* c = db_->findCollection(h.classKey, h.key);
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::PushID(i);
					const std::string label = names.format(h.classKey) + "/" + names.format(h.key);
					// A hit whose collection is gone (unloaded) is grey, not disabled: a
					// disabled item never opens its menu, and its text can still be copied.
					if (!c) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					if (ImGui::Selectable(label.c_str(), c && c == selected_, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) && c)
					{
						open = size_t(i);
						openInTab = ImGui::GetIO().KeyCtrl;
					}
					if (!c) ImGui::PopStyleColor();
					if (c && ImGui::IsItemClicked(ImGuiMouseButton_Middle))
					{
						open = size_t(i);
						openInTab = true;
					}
					if (ImGui::IsItemFocused() && !ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteFocused))
						copy(Line(names, h));
					if (ImGui::BeginPopupContextItem())
					{
						ImGui::BeginDisabled(!c);
						if (ImGui::MenuItem("Open")) open = size_t(i);
						if (ImGui::MenuItem("Open in New Tab", "Ctrl+Enter"))
						{
							open = size_t(i);
							openInTab = true;
						}
						if (ImGui::MenuItem("Find References to It")) refsOf = size_t(i);
						ImGui::EndDisabled();
						ImGui::Separator();
						if (ImGui::MenuItem("Copy Line", "Ctrl+C")) copy(Line(names, h));
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collection, field and value, split by tabs");
						if (ImGui::MenuItem("Copy Collection")) copy(Cell(names, h, kCollection));
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label.c_str());
						if (ImGui::MenuItem("Copy Name")) copy(names.format(h.key));
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", names.format(h.key).c_str());
						if (ImGui::MenuItem("Copy Field")) copy(h.where);
						if (ImGui::MenuItem("Copy Value")) copy(Unquoted(h.value));
						ImGui::Separator();
						if (ImGui::BeginMenu("Copy Column"))
						{
							// Every hit's, one per line, as the table lists them.
							if (ImGui::MenuItem("Collection")) copy(Lines(names, r.hits, kCollection));
							if (ImGui::MenuItem("Field")) copy(Lines(names, r.hits, kField));
							if (ImGui::MenuItem("Value")) copy(Lines(names, r.hits, kValue));
							ImGui::EndMenu();
						}
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("One column of every hit, one per line");
						if (ImGui::MenuItem("Copy All")) copy(Lines(names, r.hits));
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("All %zu hits, one per line, split by tabs, with the column names first", r.hits.size());
						ImGui::EndPopup();
					}
					ImGui::PopID();
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(h.where.c_str());
					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(h.value.c_str());
				}
			ImGui::EndTable();
			if (refsOf)
			{
				const SearchHit h = r.hits[*refsOf];
				findReferences(h.classKey, h.key);
			}
			else if (open)
			{
				const SearchHit h = r.hits[*open];
				if (Collection* c = db_->findCollection(h.classKey, h.key))
				{
					if (openInTab)
					{
						openInNewTab(c, false);
					}
					else
					{
						select(c, true);
						if (h.field)
						{
							// Bring the field into view, unfiltered.
							highlightField_ = h.field;
							revealSteps_.clear();
							highlightUntil_ = ImGui::GetTime() + 2.0;
							scrollToField_ = true;
							if (!fieldFilter_.empty() && ToLower(names.format(h.field)).find(ToLower(fieldFilter_)) == std::string::npos)
								fieldFilter_.clear();
						}
					}
				}
			}
		}
		ImGui::End();
	}

} // namespace app
