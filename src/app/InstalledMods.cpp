#include "InstalledMods.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace app
{

	using attrib::InstalledMod;

	namespace
	{
		void Row(const char* label, const std::string& value)
		{
			if (value.empty()) return;
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine(ImGui::GetFontSize() * 6);
			ImGui::TextUnformatted(value.c_str());
		}
	} // namespace

	void ModListView::draw(const std::vector<attrib::TracedMod>& rows, bool listed, float height)
	{
		if (rows.empty())
		{
			ImGui::TextDisabled("%s", listed ? "The list has no mods." : "No mods are listed: there is no list in this folder.");
			return;
		}
		if (picked_ >= int(rows.size())) picked_ = -1;

		const float em = ImGui::GetFontSize();
		const float avail = height > 0 ? height : ImGui::GetContentRegionAvail().y;
		// The table above, the picked mod below.
		const float tableHeight = picked_ >= 0 ? std::max(em * 5, avail * 0.45f) : avail;
		const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
			ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("##mods", 5, tf, ImVec2(0, tableHeight)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch, 3.0f);
			ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch, 1.5f);
			ImGui::TableSetupColumn("Installed", ImGuiTableColumnFlags_WidthStretch, 2.0f);
			ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 2.0f);
			ImGui::TableHeadersRow();
			for (size_t i = 0; i < rows.size(); ++i)
			{
				const InstalledMod& m = rows[i].mod;
				ImGui::PushID(int(i));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				const std::string label = m.name() + (rows[i].pending() ? " (not saved yet)" : "");
				if (ImGui::Selectable(label.c_str(), picked_ == int(i), ImGuiSelectableFlags_SpanAllColumns))
					picked_ = picked_ == int(i) ? -1 : int(i);
				if (ImGui::IsItemHovered() && !m.script.empty()) ImGui::SetTooltip("%s", m.script.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(m.version.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(m.author.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(m.installed.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(m.result.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (picked_ < 0)
		{
			return;
		}
		const attrib::TracedMod& row = rows[size_t(picked_)];
		const InstalledMod& m = row.mod;
		ImGui::BeginChild("##picked", ImVec2(0, std::max(em * 4, avail - tableHeight - ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_Borders);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(m.name().c_str());
		if (m.title.empty()) ImGui::TextDisabled("It names no title, so its script file's SHA-256 tells it apart.");
		ImGui::Spacing();
		Row("Script", m.script + (m.script.empty() ? "" : m.nfsms ? " (nfsms)" : " (mod script)"));
		Row("SHA-256", m.sha256);
		if (!m.sha256.empty())
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Copy##sha256")) ImGui::SetClipboardText(m.sha256.c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("certutil -hashfile <script> SHA256 prints the same for the same file.");
		}
		Row("Game", m.game);
		Row("By", m.by);
		if (!m.options.empty())
		{
			std::string opts;
			for (const auto& [k, v] : m.options) opts += (opts.empty() ? "" : ", ") + k + " = " + v;
			Row("Options", opts);
		}
		ImGui::Spacing();
		if (row.trace)
		{
			if (describedTrace_ != row.trace)
			{
				describedTrace_ = row.trace;
				described_ = attrib::DescribePlaces(attrib::PlacesOf(*row.trace));
			}
			Row("Trace", row.pending() ? std::string("written when the changes are saved") : m.trace);
			ImGui::TextDisabled("What uninstalling sets back: %s.", described_.c_str());
		}
		else
		{
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "It can't be uninstalled: %s.", row.problem.c_str());
		}
		if (!m.files.empty())
		{
			ImGui::Spacing();
			ImGui::TextDisabled("Files it changed:");
			for (const std::string& f : m.files) ImGui::BulletText("%s", f.c_str());
		}
		if (!m.unknown.empty())
		{
			ImGui::Spacing();
			ImGui::TextDisabled("And %zu %s a later Attribulina wrote, kept as they are.", m.unknown.size(), m.unknown.size() == 1 ? "line" : "lines");
		}
		ImGui::PopTextWrapPos();
		ImGui::EndChild();
	}

} // namespace app
