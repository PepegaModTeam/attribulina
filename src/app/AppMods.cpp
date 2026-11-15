// The game's list of installed mods in the editor (core/ModList.h): read as
// the database opens, shown in the Installed Mods panel, and added to by Save
// All with the mods File > Apply Script and File > Run nfsms Script applied.

#include "App.h"

#include "core/Install.h"
#include "core/Version.h"
#include "core/Writer.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <filesystem>

namespace app
{

	using namespace attrib;
	namespace fs = std::filesystem;

	namespace
	{
		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}
		std::string Named(const InstalledMod& m)
		{
			return m.name() + (m.version.empty() ? "" : " " + m.version);
		}
	} // namespace

	void App::readModList()
	{
		modList_.reset();
		modListError_.clear();
		modsView_.forget();
		modListFor_ = databaseFolder();
		if (modListFor_.empty()) return;
		try
		{
			modList_ = ReadModList(modListFor_);
		}
		catch (const std::exception& e)
		{
			modListError_ = e.what();
			log(LogLevel::Warning, "The list of installed mods doesn't read, so saving leaves it as it is: " + modListError_);
			return;
		}
		if (!modList_) return; // no mods listed: the usual case
		std::string names;
		for (const InstalledMod& m : modList_->mods) names += (names.empty() ? ": " : ", ") + Named(m);
		log(LogLevel::Info, ModListPath(modListFor_) + " lists " + std::to_string(modList_->mods.size()) +
			(modList_->mods.size() == 1 ? " mod" : " mods") + " installed here" + (names.empty() ? std::string(".") : names));
	}

	void App::draftMod(const std::string& path, const std::string& text, bool nfsms, const script::Script& meta)
	{
		modDraft_.reset();
		if (!trackMods_ || !db_) return;
		modDraft_ = install::DescribeMod(path, text, nfsms, meta);
		if (const std::string note = draftNote(); !note.empty()) log(LogLevel::Warning, note);
	}

	std::string App::draftNote() const
	{
		if (!modDraft_ || !trackMods_) return {};
		// The list, and what this session applied since it was saved.
		ModList all = modList_.value_or(ModList{});
		all.mods.insert(all.mods.end(), pendingMods_.begin(), pendingMods_.end());
		const ModIdentity id = install::IdentityOf(*modDraft_);
		return InstalledNote(FindInstalled(all, id), id);
	}

	void App::modApplied(const script::ApplyReport& r, const std::map<std::string, std::string>& options, const nfsms::Choices* choices)
	{
		if (!modDraft_ || !db_) return;
		InstalledMod m = std::move(*modDraft_);
		modDraft_.reset();
		install::CompleteRecord(m, *db_, std::string("Attribulina ") + Version::GetAppVersionString(), options, choices);
		const size_t failed = r.errors();
		m.result = failed ? "installed, " + std::to_string(failed) + (failed == 1 ? " line" : " lines") + " failed" : "installed";
		// What saving writes for it: the packs it changed (a .lzc with its
		// twin), the memory files holding copies of them, the files it removes,
		// and what nfsms file_copy wrote already.
		std::vector<std::string> files;
		for (const LoadedPack* pk : r.touched)
		{
			std::vector<std::string> pack{ pk->path };
			if (const std::optional<std::string> twin = PackTwin(pk->path)) pack.push_back(*twin);
			for (const std::string& f : pack)
			{
				files.push_back(f);
				if (updateMemoryFiles_)
					for (const MemoryFileCopy& c : MemoryFileCopiesOf(f)) files.push_back(c.memoryFile);
			}
		}
		files.insert(files.end(), r.removedFiles.begin(), r.removedFiles.end());
		files.insert(files.end(), r.copiedFiles.begin(), r.copiedFiles.end());
		const std::string game = ToUtf8(FromUtf8(databaseFolder()).parent_path());
		for (const std::string& f : files)
		{
			const std::string name = ModListFileName(f, game);
			if (std::find(m.files.begin(), m.files.end(), name) == m.files.end()) m.files.push_back(name);
		}
		std::sort(m.files.begin(), m.files.end());
		log(LogLevel::Info, Named(m) + " goes on the list of installed mods (" + std::string(kModListFile) + ") when the changes are saved.");
		pendingMods_.push_back(std::move(m));
	}

	void App::writePendingMods()
	{
		if (pendingMods_.empty()) return;
		std::vector<InstalledMod> mods;
		mods.swap(pendingMods_);
		for (UndoState& st : undo_) st.mods.clear(); // saved: undoing past them doesn't take them back
		if (!trackMods_) return;
		const std::string folder = databaseFolder();
		std::string names;
		for (const InstalledMod& m : mods) names += (names.empty() ? "" : ", ") + Named(m);
		try
		{
			modList_ = AddToModList(folder, mods);
			modListFor_ = folder;
			modListError_.clear();
			modsView_.forget();
			log(LogLevel::Info, "Added " + names + " to the list of installed mods, " + ModListPath(folder));
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Warning, "The list of installed mods wasn't updated with " + names + ": " + e.what());
		}
	}

	void App::drawModsPanel()
	{
		if (!showMods_) return;
		const float em = ImGui::GetFontSize();
		ImGui::SetNextWindowSize(ImVec2(em * 46, em * 24), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Installed Mods", &showMods_))
		{
			ImGui::End();
			return;
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) lastPanel_ = Panel::Other;
		ImGui::PushTextWrapPos(0.0f);
		if (Checkbox("Keep a list of installed mods", &trackMods_)) ImGui::MarkIniSettingsDirty();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On (the default): File > Apply Script and File > Run nfsms Script add the mod they apply to the\n"
				"database folder's attribulina_mods.txt when the changes are saved, as the mod installer does.");
		if (!db_ || db_->packs().empty())
		{
			ImGui::TextDisabled("Open a database: the mods its folder lists show here.");
		}
		else
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Reload")) readModList();
			const std::string path = ModListPath(databaseFolder());
			if (!modListError_.empty()) ImGui::TextColored(kWarn, "%s doesn't read: %s", path.c_str(), modListError_.c_str());
			else ImGui::TextDisabled("%s, oldest first.", path.c_str());
			if (!pendingMods_.empty())
				ImGui::TextColored(kWarn, "%zu applied since the last save, which Save All adds.", pendingMods_.size());
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			if (modListError_.empty()) modsView_.draw(modList_ ? &*modList_ : nullptr, pendingMods_);
			ImGui::End();
			return;
		}
		ImGui::PopTextWrapPos();
		ImGui::End();
	}

} // namespace app
