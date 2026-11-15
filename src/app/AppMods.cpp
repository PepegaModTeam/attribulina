// The game's list of installed mods in the editor (core/ModList.h): read as
// the database opens with the mods' traces (core/ModTrace.h), shown in the
// Installed Mods panel, and changed by Save All: the mods File > Apply Script
// and File > Run nfsms Script installed, each traced, and the ones the panel
// uninstalled. Only those two menu items install: the Script panels' own
// Apply and Run, the console, a run of nfsms lines and edits by hand are
// scripts being written, and are never traced or listed.

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
	namespace sc = attrib::script;

	namespace
	{
		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}
		std::string Names(const std::vector<std::string>& names)
		{
			std::string s;
			for (size_t i = 0; i < names.size(); ++i) s += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
			return s;
		}
	} // namespace

	void App::readModList()
	{
		modList_.reset();
		modTraced_.clear();
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
		modTraced_ = ReadTraces(modListFor_, *modList_);
		std::vector<std::string> names;
		size_t untraced = 0;
		for (const TracedMod& t : modTraced_)
		{
			names.push_back(t.mod.nameAndVersion());
			untraced += !t.trace;
		}
		log(LogLevel::Info, ModListPath(modListFor_) + " lists " + std::to_string(names.size()) + (names.size() == 1 ? " mod" : " mods") +
			" installed here" + (names.empty() ? std::string(".") : ": " + Names(names)));
		if (untraced)
			log(LogLevel::Info, std::to_string(untraced) + (untraced == 1 ? " of them has" : " of them have") + " no trace of what " +
				(untraced == 1 ? "it" : "they") + " changed, so View > Installed Mods can't uninstall " + (untraced == 1 ? "it." : "them."));
	}

	std::vector<TracedMod> App::modRows() const
	{
		return WithUpdate(modTraced_, pendingList_);
	}

	void App::draftMod(const std::string& path, const std::string& text, bool nfsms, const script::Script& meta)
	{
		modDraft_.reset();
		modBefore_.reset();
		if (!trackMods_ || !db_) return;
		modDraft_ = install::DescribeMod(path, text, nfsms, meta);
		if (const std::string note = draftNote(); !note.empty()) log(LogLevel::Warning, note);
	}

	std::string App::draftNote() const
	{
		if (!modDraft_ || !trackMods_) return {};
		// The list, and what this session applied since it was saved.
		ModList all;
		for (const TracedMod& t : modRows()) all.mods.push_back(t.mod);
		const ModIdentity id = install::IdentityOf(*modDraft_);
		return InstalledNote(FindInstalled(all, id), id);
	}

	void App::takeModBefore()
	{
		modBefore_.reset();
		if (!modDraft_ || !trackMods_ || !db_) return;
		try
		{
			modBefore_ = sc::CloneDatabase(*db_);
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Warning, "What the mod changes can't be traced (" + std::string(e.what()) + "), so it can't be uninstalled later.");
		}
	}

	void App::modApplied(const script::ApplyReport& r, const std::map<std::string, std::string>& options, const nfsms::Choices* choices)
	{
		if (!modDraft_ || !db_)
		{
			modBefore_.reset();
			return;
		}
		InstalledMod m = std::move(*modDraft_);
		modDraft_.reset();
		install::CompleteRecord(m, *db_, std::string("Attribulina ") + Version::GetAppVersionString(), options, choices);
		const size_t failed = r.errors();
		m.result = failed ? "installed, " + std::to_string(failed) + (failed == 1 ? " line" : " lines") + " failed" : "installed";
		// What saving writes for it: the packs it changed (a .lzc with its
		// twin), the memory files holding copies of them, the files it removes,
		// and what nfsms file_copy wrote already.
		const std::string game = ToUtf8(FromUtf8(databaseFolder()).parent_path());
		for (const std::string& f : install::ChangedFiles(r, updateMemoryFiles_))
		{
			const std::string name = ModListFileName(f, game);
			if (std::find(m.files.begin(), m.files.end(), name) == m.files.end()) m.files.push_back(name);
		}
		std::sort(m.files.begin(), m.files.end());
		std::shared_ptr<const sc::Script> trace;
		if (modBefore_)
		{
			try
			{
				trace = std::make_shared<const sc::Script>(MakeTrace(*modBefore_, *db_, m));
			}
			catch (const std::exception& e)
			{
				log(LogLevel::Warning, "What " + m.nameAndVersion() + " changed can't be traced (" + e.what() + "), so it can't be uninstalled later.");
			}
			modBefore_.reset();
		}
		const std::vector<TracedMod> rows = modRows();
		log(LogLevel::Info, m.nameAndVersion() + " goes on the list of installed mods (" + std::string(kModListFile) + ") when the changes are saved.");
		pendingList_.add.push_back({ m, trace });
		if (!trace) return;
		// The mods it changes the same things as.
		modClashes_.clear();
		for (const ModConflict& c : FindConflicts(PlacesOf(*trace), rows))
		{
			const InstalledMod& other = rows[c.index].mod;
			modClashes_.push_back({ other, ConflictText(c, other.nameAndVersion()), true });
			log(LogLevel::Warning, m.nameAndVersion() + " " + modClashes_.back().text);
		}
		if (modClashes_.empty()) return;
		modClashFor_ = m.nameAndVersion();
		modClashesWaiting_ = true;
	}

	bool App::uninstallMods(const std::vector<size_t>& which)
	{
		modsMessage_.clear();
		modsMessageError_ = false;
		if (!db_ || which.empty()) return false;
		const std::vector<TracedMod> rows = modRows();
		std::vector<std::string> list;
		for (size_t i : which)
			if (i < rows.size()) list.push_back(rows[i].mod.nameAndVersion());
		const std::string names = Names(list);
		// Tried on a copy first: a trace that doesn't apply leaves the database as it was.
		UninstallResult trial;
		try
		{
			std::unique_ptr<Database> copy = sc::CloneDatabase(*db_);
			trial = Uninstall(*copy, rows, which);
		}
		catch (const std::exception& e)
		{
			trial.ok = false;
			trial.error = e.what();
		}
		if (!trial.ok)
		{
			modsMessage_ = names + " wasn't uninstalled: " + trial.error + " Nothing was changed.";
			modsMessageError_ = true;
			log(LogLevel::Error, modsMessage_);
			return false;
		}
		nameAction("Uninstall " + names);
		beginScriptRun();
		const UninstallResult u = Uninstall(*db_, rows, which, [this](Collection& c) { forgetCollection(&c); });
		endScriptRun();
		for (const std::string& n : u.newNames) rememberName(n);
		structureChanged();
		updateTitle();
		for (const std::string& n : u.notes) log(LogLevel::Warning, n);
		if (!u.ok)
		{
			modsMessage_ = names + " wasn't uninstalled all the way: " + u.error + " Undo puts the database back.";
			modsMessageError_ = true;
			log(LogLevel::Error, modsMessage_);
			return false;
		}
		FoldUninstall(pendingList_, rows, u);
		modsView_.forget();
		modsMessage_ = "Uninstalled " + names + ": " + u.summary() + "." +
			(u.redone ? " The mods installed after it that change the same things were undone and done again, so what they set stays." : "") +
			" Save All writes the files and the list; files a mod copied stay as they are.";
		log(LogLevel::Info, modsMessage_);
		return true;
	}

	bool App::forgetMod(size_t row)
	{
		modsMessage_.clear();
		modsMessageError_ = false;
		const std::vector<TracedMod> rows = modRows();
		if (row >= rows.size()) return false;
		const TracedMod& t = rows[row];
		const std::string name = t.mod.nameAndVersion();
		if (t.pending())
		{
			pendingList_.add.erase(pendingList_.add.begin() + t.added);
			modsMessage_ = name + " won't go on the list when the changes are saved. Its changes stay in the database.";
		}
		else
		{
			ModListUpdate u;
			u.remove.push_back(t.mod);
			try
			{
				std::vector<std::string> problems;
				modList_ = UpdateModList(databaseFolder(), u, &problems);
				for (const std::string& p : problems) log(LogLevel::Warning, p);
				modTraced_ = ReadTraces(databaseFolder(), *modList_);
				modListError_.clear();
			}
			catch (const std::exception& e)
			{
				modsMessage_ = std::string("The list wasn't changed: ") + e.what();
				modsMessageError_ = true;
				log(LogLevel::Error, modsMessage_);
				return false;
			}
			std::erase_if(pendingList_.retrace, [&](const auto& e) { return SameEntry(e.first, t.mod); });
			modsMessage_ = "Took " + name + " off the list. The database and the game's files are as they were.";
		}
		modsView_.forget();
		log(LogLevel::Info, modsMessage_);
		return true;
	}

	void App::writePendingMods()
	{
		if (pendingList_.empty()) return;
		ModListUpdate u = std::move(pendingList_);
		pendingList_ = {};
		for (UndoState& st : undo_) st.mods = {}; // saved: undoing past them doesn't take them back
		// Off, no mod goes on the list; one uninstalled still comes off it.
		if (!trackMods_) u.add.clear();
		if (u.empty()) return;
		const std::string folder = databaseFolder();
		std::vector<std::string> added, removed;
		for (const ModListUpdate::Added& a : u.add) added.push_back(a.mod.nameAndVersion());
		for (const InstalledMod& m : u.remove) removed.push_back(m.nameAndVersion());
		try
		{
			std::vector<std::string> problems;
			modList_ = UpdateModList(folder, u, &problems);
			for (const std::string& p : problems) log(LogLevel::Warning, p);
			modListFor_ = folder;
			modListError_.clear();
			modTraced_ = ReadTraces(folder, *modList_);
			modsView_.forget();
			std::string what;
			if (!added.empty()) what = "added " + Names(added);
			if (!removed.empty()) what += (what.empty() ? "took " : ", took ") + Names(removed) + " off";
			if (what.empty()) what = "updated the traces of the mods installed after the one uninstalled";
			what[0] = char(std::toupper(static_cast<unsigned char>(what[0])));
			log(LogLevel::Info, what + ": the list of installed mods, " + ModListPath(folder));
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Warning, std::string("The list of installed mods wasn't updated: ") + e.what());
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
				"database folder's attribulina_mods.txt when the changes are saved, with a trace of what it changed,\n"
				"as the mod installer does.");
		if (!db_ || db_->packs().empty())
		{
			ImGui::TextDisabled("Open a database: the mods its folder lists show here.");
			ImGui::PopTextWrapPos();
			ImGui::End();
			return;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Reload")) readModList();
		const std::string path = ModListPath(databaseFolder());
		if (!modListError_.empty()) ImGui::TextColored(kWarn, "%s doesn't read: %s", path.c_str(), modListError_.c_str());
		else ImGui::TextDisabled("%s, oldest first.", path.c_str());
		const std::vector<TracedMod> rows = modRows();
		const size_t waiting = pendingList_.add.size() + pendingList_.remove.size();
		if (waiting)
			ImGui::TextColored(kWarn, "%zu %s since the last save, which Save All writes to the list.", waiting,
				waiting == 1 ? "change" : "changes");
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		if (!modListError_.empty())
		{
			ImGui::End();
			return;
		}
		const float below = ImGui::GetFrameHeightWithSpacing() + (modsMessage_.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 3);
		modsView_.draw(rows, modList_.has_value(), std::max(em * 6, ImGui::GetContentRegionAvail().y - below));
		if (!modsMessage_.empty())
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(modsMessageError_ ? kError : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", modsMessage_.c_str());
			ImGui::PopTextWrapPos();
		}
		const int picked = modsView_.picked();
		const bool hasPick = picked >= 0 && size_t(picked) < rows.size();
		ImGui::BeginDisabled(!hasPick || !rows[size_t(std::max(picked, 0))].trace);
		if (ImGui::Button("Uninstall..."))
		{
			modsPick_ = picked;
			modsPickEntry_ = rows[size_t(picked)].mod;
			request(Pending::UninstallMod);
		}
		ImGui::EndDisabled();
		if (hasPick && !rows[size_t(picked)].trace && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", ("It can't be uninstalled: " + rows[size_t(picked)].problem + ".").c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(!hasPick);
		if (ImGui::Button("Remove from List..."))
		{
			modsPick_ = picked;
			modsPickEntry_ = rows[size_t(picked)].mod;
			request(Pending::ForgetMod);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Takes the mod off the list and changes nothing else: for a mod put back by hand (its .bak files).");
		ImGui::End();
	}

	void App::drawModsModals()
	{
		// After the prompt that applied the mod has closed.
		if (modClashesWaiting_ && pending_ == Pending::None)
		{
			modClashesWaiting_ = false;
			request(Pending::ModConflicts);
		}
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		auto begin = [&](const char* name)
		{
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		};
		auto done = [&]()
		{
			pending_ = Pending::None;
			ImGui::CloseCurrentPopup();
		};
		const float wrap = ImGui::GetFontSize() * 34;
		// The row a prompt is about, while it is still that mod.
		auto pickedRow = [&](const std::vector<TracedMod>& rows) -> int
		{
			if (modsPick_ < 0 || size_t(modsPick_) >= rows.size() || !SameEntry(rows[size_t(modsPick_)].mod, modsPickEntry_)) return -1;
			return modsPick_;
		};

		if (begin("Uninstall Mod"))
		{
			const std::vector<TracedMod> rows = modRows();
			const int row = pickedRow(rows);
			if (row < 0 || !rows[size_t(row)].trace)
			{
				done();
			}
			else
			{
				const TracedMod& t = rows[size_t(row)];
				ImGui::PushTextWrapPos(wrap);
				ImGui::Text("Uninstall %s?", t.mod.nameAndVersion().c_str());
				ImGui::TextDisabled("This sets back what it changed in the database (%s). Save All writes the files and the list; "
					"until then, Undo takes it back. Files it copied into the game's folder stay as they are.",
					DescribePlaces(PlacesOf(*t.trace)).c_str());
				if (size_t(row) + 1 < rows.size())
					ImGui::TextDisabled("Mods installed after it that change the same things are undone and done again around it, so what "
						"they set stays.");
				ImGui::PopTextWrapPos();
				bool ok = ImGui::Button("Uninstall");
				ImGui::SameLine();
				const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
				ok |= ConfirmKey();
				if (cancel)
				{
					done();
				}
				else if (ok)
				{
					done();
					uninstallMods({ size_t(row) });
				}
			}
			ImGui::EndPopup();
		}

		if (begin("Remove Mod from List"))
		{
			const std::vector<TracedMod> rows = modRows();
			const int row = pickedRow(rows);
			if (row < 0)
			{
				done();
			}
			else
			{
				const TracedMod& t = rows[size_t(row)];
				ImGui::PushTextWrapPos(wrap);
				ImGui::Text("Take %s off the list?", t.mod.nameAndVersion().c_str());
				ImGui::TextDisabled("%s", t.pending() ? "It won't go on the list when the changes are saved. Its changes stay in the database."
					: "The list changes now. The database and the game's files stay as they are, and the mod can't be uninstalled "
					  "from here after.");
				ImGui::PopTextWrapPos();
				bool ok = ImGui::Button("Remove from List");
				ImGui::SameLine();
				const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
				ok |= ConfirmKey();
				if (cancel)
				{
					done();
				}
				else if (ok)
				{
					done();
					forgetMod(size_t(row));
				}
			}
			ImGui::EndPopup();
		}

		if (begin("Mod Conflicts"))
		{
			std::vector<std::string> names;
			for (const ModClash& c : modClashes_) names.push_back(c.mod.nameAndVersion());
			ImGui::PushTextWrapPos(wrap);
			ImGui::TextColored(kWarn, "%s changes what %s changed.", modClashFor_.c_str(),
				names.size() == 1 ? names[0].c_str() : (std::to_string(names.size()) + " installed mods").c_str());
			ImGui::TextDisabled("It is applied. Tick the mods to uninstall: uninstalling one sets back what it changed, and where both "
				"change the same thing, %s's value stays. Or keep them installed: %s's values win where both change the same thing, and "
				"uninstalling either of them later keeps the other's changes.", modClashFor_.c_str(), modClashFor_.c_str());
			ImGui::Spacing();
			for (size_t i = 0; i < modClashes_.size(); ++i)
			{
				ModClash& c = modClashes_[i];
				ImGui::PushID(int(i));
				Checkbox(("Uninstall " + c.mod.nameAndVersion()).c_str(), &c.uninstall);
				ImGui::Indent();
				ImGui::TextDisabled("%s %s.", modClashFor_.c_str(), c.text.c_str());
				ImGui::Unindent();
				ImGui::PopID();
			}
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			const bool ticked = std::any_of(modClashes_.begin(), modClashes_.end(), [](const ModClash& c) { return c.uninstall; });
			ImGui::BeginDisabled(!ticked);
			const bool uninstall = ImGui::Button("Uninstall the Ticked Mods");
			ImGui::EndDisabled();
			ImGui::SameLine();
			const bool keep = ImGui::Button("Keep Them Installed") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			ImGui::SameLine();
			const bool undoIt = ImGui::Button(("Undo " + modClashFor_).c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Takes back applying it, as Edit > Undo does.");
			if (uninstall)
			{
				const std::vector<TracedMod> rows = modRows();
				std::vector<size_t> which;
				for (const ModClash& c : modClashes_)
					if (c.uninstall)
						for (size_t i = 0; i < rows.size(); ++i)
							if (SameEntry(rows[i].mod, c.mod)) which.push_back(i);
				done();
				modClashes_.clear();
				uninstallMods(which);
				showMods_ = true; // what happened shows there
			}
			else if (undoIt)
			{
				done();
				modClashes_.clear();
				undo();
			}
			else if (keep)
			{
				done();
				modClashes_.clear();
			}
			ImGui::EndPopup();
		}
	}

} // namespace app
