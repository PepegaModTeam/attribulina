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
			ImGui::Checkbox(searchQuery_.mode == SearchMode::Values ? "Whole value" : "Whole name", &searchQuery_.wholeValue);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(searchQuery_.mode == SearchMode::Values ? "Off: values that contain the text. Numbers always match whole."
								  : "Off: fields whose name contains the text.");
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
					ImGui::BeginDisabled(!c);
					if (ImGui::Selectable(label.c_str(), c && c == selected_, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
					{
						open = size_t(i);
						openInTab = ImGui::GetIO().KeyCtrl;
					}
					if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
					{
						open = size_t(i);
						openInTab = true;
					}
					ImGui::EndDisabled();
					if (c && ImGui::BeginPopupContextItem())
					{
						if (ImGui::MenuItem("Open")) open = size_t(i);
						if (ImGui::MenuItem("Open in New Tab", "Ctrl+Enter"))
						{
							open = size_t(i);
							openInTab = true;
						}
						if (ImGui::MenuItem("Find References to It")) refsOf = size_t(i);
						if (ImGui::MenuItem("Copy Name")) ImGui::SetClipboardText(names.format(h.key).c_str());
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
