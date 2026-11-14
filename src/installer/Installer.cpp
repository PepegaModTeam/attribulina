#include "Installer.h"

#include "app/ValueEditors.h"

#include "core/Version.h"
#include "core/Writer.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <fstream>

namespace installer
{

	using namespace attrib;
	namespace fs = std::filesystem;
	namespace sc = attrib::script;
	namespace ns = attrib::nfsms;

	namespace
	{
		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}
		std::string FileName(const std::string& path)
		{
			return ToUtf8(FromUtf8(path).filename());
		}
		// `path` from the game's folder, when it is in it.
		std::string Shown(const std::string& path, const std::string& folder)
		{
			const fs::path rel = FromUtf8(path).lexically_relative(FromUtf8(folder));
			const std::string r = ToUtf8(rel);
			return r.empty() || r.rfind("..", 0) == 0 ? path : r;
		}
		std::string Joined(const std::vector<std::string>& v, const std::string& sep)
		{
			std::string s;
			for (const std::string& x : v) s += (s.empty() ? "" : sep) + x;
			return s;
		}

		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);
		const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);

		ImVec4 ColourOf(Level l)
		{
			switch (l)
			{
				case Level::Warning: return kWarn;
				case Level::Error: return kError;
				case Level::Success: return kGood;
				default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
			}
		}

		Level LevelOf(sc::Message::Level l)
		{
			return l == sc::Message::Level::Error ? Level::Error : l == sc::Message::Level::Warning ? Level::Warning : Level::Info;
		}

		const char* StepName(Step s)
		{
			switch (s)
			{
				case Step::Mod: return "Mod";
				case Step::Game: return "Game";
				case Step::Options: return "Options";
				case Step::Install: return "Install";
			}
			return "";
		}

		constexpr size_t kMaxLogLines = 5000; // shown; the log file keeps all
		constexpr size_t kMaxRecent = 8;

		bool CopiesFiles(const ns::Script& s)
		{
			for (const ns::Command& c : s.commands)
				if (c.name == "file_copy" || c.name == "folder_create") return true;
			return false;
		}
	} // namespace

	// ------------------------------------------------------------------ setup ---

	Installer::Installer(app::Host& host) : host_(host)
	{
	}

	Installer::~Installer()
	{
		if (worker_.joinable()) worker_.join();
	}

	void Installer::setup(float dpiScale, const Paths& paths)
	{
		paths_ = paths;
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr; // one window: nothing to lay out
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		// The editor's theme, when its settings say one.
		app::Theme theme = app::Theme::Dark;
		if (!paths_.userDir.empty())
		{
			std::ifstream in(FromUtf8(paths_.userDir) / "attribulina.ini", std::ios::binary);
			for (std::string l; std::getline(in, l);)
			{
				if (!l.empty() && l.back() == '\r') l.pop_back();
				if (l.rfind("Theme=", 0) == 0)
					if (const auto t = app::ParseTheme(l.substr(6))) theme = *t;
			}
		}
		app::ApplyTheme(theme);
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 0.0f;
		style.FrameRounding = 3.0f;
		style.GrabRounding = 3.0f;
		style.FontSizeBase = 15.0f;
		style.ScaleAllSizes(dpiScale);
		style.FontScaleDpi = dpiScale;
		const fs::path font = FromUtf8(paths_.exeDir) / "data" / "RobotoFlex.ttf";
		if (std::error_code ec; !paths_.exeDir.empty() && fs::exists(font, ec)) io.Fonts->AddFontFromFileTTF(font.string().c_str());
		else io.Fonts->AddFontDefaultVector();

		// The log file: in the first folder that takes it, named by the time (a
		// second run in the same second gets " (2)").
		const std::string stamp = std::string("AttribulinaModInstaller_") + LocalTimestamp(true);
		for (const std::string& dir : paths_.logDirs)
		{
			if (dir.empty()) continue;
			fs::path file = FromUtf8(dir) / (stamp + ".log");
			std::error_code ec;
			for (int n = 2; fs::exists(file, ec) && n < 100; ++n) file = FromUtf8(dir) / (stamp + " (" + std::to_string(n) + ").log");
			if (logFile_.open(ToUtf8(file))) break;
		}
		if (logFile_.isOpen()) logFile_.writeRaw(std::string(kTitle) + ", " + Version::GetAppVersionString() + ", " + LocalTimestamp());
		post(Level::Info, std::string(kTitle) + ", " + Version::GetAppVersionString());
		if (logFile_.isOpen()) post(Level::Info, "This log is also written to " + logFile_.path() + ".");
		else if (!paths_.logDirs.empty()) post(Level::Warning, "No log file could be written in " + Joined(paths_.logDirs, ", ") + ".");

		loadSettings();
		if (!recentGames_.empty()) gameInput_ = recentGames_.front();
		host_.setWindowTitle(kTitle);
		setStatus(Level::Info, "Choose the mod to install.");
	}

	std::string Installer::dataDir() const
	{
		if (!paths_.dataDir.empty()) return paths_.dataDir;
		return paths_.exeDir.empty() ? std::string() : ToUtf8(FromUtf8(paths_.exeDir) / "data");
	}

	std::string Installer::userKeysFile() const
	{
		return paths_.userDir.empty() ? std::string() : ToUtf8(FromUtf8(paths_.userDir) / "user_keys.txt");
	}

	void Installer::loadSettings()
	{
		if (paths_.userDir.empty()) return;
		std::ifstream in(FromUtf8(paths_.userDir) / kSettingsFileName, std::ios::binary);
		for (std::string l; std::getline(in, l);)
		{
			if (!l.empty() && l.back() == '\r') l.pop_back();
			if (l.rfind("GameFolder=", 0) == 0 && l.size() > 11 && recentGames_.size() < kMaxRecent) recentGames_.push_back(l.substr(11));
		}
	}

	void Installer::saveSettings()
	{
		if (paths_.userDir.empty()) return;
		std::ofstream out(FromUtf8(paths_.userDir) / kSettingsFileName, std::ios::binary | std::ios::trunc);
		out << "[" << kTitle << "]\n";
		for (const std::string& g : recentGames_) out << "GameFolder=" << g << "\n";
	}

	void Installer::rememberGame(const std::string& folder)
	{
		std::erase(recentGames_, folder);
		recentGames_.insert(recentGames_.begin(), folder);
		if (recentGames_.size() > kMaxRecent) recentGames_.resize(kMaxRecent);
		saveSettings();
	}

	// ---------------------------------------------------------- log and status ---

	void Installer::post(Level level, std::string text, bool levelInText)
	{
		std::lock_guard lock(m_);
		logFile_.write(levelInText ? "" : level == Level::Error ? "error" : level == Level::Warning ? "warning" : "", text);
		log_.push_back({ level, std::move(text) });
		while (log_.size() > kMaxLogLines)
		{
			logDropped_ += log_.front().text.size() + 1;
			log_.pop_front();
		}
		++logVersion_;
	}

	void Installer::setStatus(Level level, std::string text)
	{
		std::lock_guard lock(m_);
		statusLevel_ = level;
		status_ = std::move(text);
	}

	std::string Installer::status() const
	{
		std::lock_guard lock(m_);
		return status_;
	}

	Level Installer::statusLevel() const
	{
		std::lock_guard lock(m_);
		return statusLevel_;
	}

	std::vector<LogLine> Installer::log() const
	{
		std::lock_guard lock(m_);
		return std::vector<LogLine>(log_.begin(), log_.end());
	}

	// ------------------------------------------------------------------- work ---

	void Installer::startWork(Work kind, std::function<void()> job)
	{
		if (worker_.joinable()) worker_.join();
		work_ = kind;
		busy_ = true;
		auto run = [this, job = std::move(job)]()
		{
			try
			{
				job();
			}
			catch (const std::exception& e)
			{
				post(Level::Error, e.what());
			}
			busy_ = false;
		};
		if (background_) worker_ = std::thread(run);
		else run();
	}

	void Installer::wait()
	{
		if (worker_.joinable()) worker_.join();
		harvest();
	}

	void Installer::harvest()
	{
		if (busy_) return;
		if (worker_.joinable()) worker_.join();
		std::optional<FoundGame> game;
		std::optional<Result> result;
		{
			std::lock_guard lock(m_);
			game.swap(gameDone_);
			result.swap(installDone_);
		}
		if (game || result) work_ = Work::None;
		if (game)
		{
			game_ = std::move(*game);
			if (game_.ok) rememberGame(game_.folder.game);
			refreshModChecks();
		}
		if (result)
		{
			result_ = std::move(*result);
			outcome_ = result_.outcome;
		}
	}

	void Installer::refreshModChecks()
	{
		modProblem_.clear();
		modWarning_.clear();
		if (!mod_ || !db_ || !game_.ok || busy_) return;
		modProblem_ = install::ModProblem(*mod_, *db_);
		modWarning_ = install::ModWarning(*mod_, *db_);
		if (!modProblem_.empty())
		{
			post(Level::Error, modProblem_);
			setStatus(Level::Error, modProblem_);
		}
		else if (!modWarning_.empty())
		{
			post(Level::Warning, modWarning_);
		}
	}

	void Installer::ensureData()
	{
		if (data_) return;
		data_ = install::DataFiles::Load(dataDir());
		for (const std::string& p : data_->problems) post(Level::Warning, p);
	}

	std::unique_ptr<Database> Installer::loadDatabase(const install::GameFolder& f, const Target& target, std::vector<std::string>& movedFrom,
		FoundGame* found)
	{
		ensureData();
		auto db = data_->newDatabase(userKeysFile());
		db->setTarget(target);
		install::LoadPacks(*db, f.global, movedFrom);
		for (const auto& pk : db->packs())
		{
			const std::string name = FileName(pk->path);
			for (const std::string& w : PackWarnings(*pk))
			{
				post(Level::Warning, name + ": " + w);
				if (found) ++found->warnings;
			}
			if (!found) continue;
			found->packs.push_back(name);
			std::vector<std::string> files{ pk->path };
			if (const std::optional<std::string> twin = PackTwin(pk->path)) files.push_back(*twin);
			for (const std::string& file : files)
				for (const MemoryFileCopy& c : MemoryFileCopiesOf(file))
					found->copies.push_back(FileName(c.memoryFile) + " holds a copy of " + c.gamePath + (c.same ? "" : ", another than the file"));
		}
		return db;
	}

	// ------------------------------------------------------------------ steps ---

	void Installer::openArguments(const std::vector<std::string>& args)
	{
		std::string modPath;
		for (const std::string& a : args)
		{
			std::error_code ec;
			if (fs::is_directory(FromUtf8(a), ec)) gameInput_ = a;
			else modPath = a;
		}
		if (!modPath.empty()) openMod(modPath);
	}

	void Installer::dropped(const std::string& path)
	{
		if (busy_) return;
		std::error_code ec;
		if (!fs::is_directory(FromUtf8(path), ec))
		{
			openMod(path);
			return;
		}
		gameInput_ = path;
		if (mod_)
		{
			goTo(Step::Game);
			chooseGame(path);
		}
	}

	bool Installer::openMod(std::string path)
	{
		if (busy_) return false;
		modInput_ = path;
		outcome_ = Outcome::None;
		result_ = {};
		try
		{
			auto m = std::make_shared<install::Mod>(install::ReadMod(path));
			// The option defaults: a default that doesn't read is the script's mistake.
			if (!m->isNfsms) sc::ResolveOptions(m->amod, {});
			mod_ = std::move(m);
		}
		catch (const std::exception& e)
		{
			mod_.reset();
			images_.release();
			modError_ = e.what();
			post(Level::Error, "Couldn't read " + path + ": " + modError_);
			setStatus(Level::Error, "The mod couldn't be read.");
			step_ = Step::Mod;
			refreshModChecks();
			return false;
		}
		modError_.clear();
		resetOptions();
		images_.release();
		if (mod_->isNfsms) images_.load(host_, mod_->nfsms);
		std::string what = mod_->isNfsms ? "an nfsms script" : "a mod script";
		if (mod_->hasOptions()) what += " with options";
		post(Level::Info, "Read " + path + ": " + mod_->title + ", " + what + ".");
		host_.setWindowTitle(std::string(kTitle) + " - " + mod_->title);
		refreshModChecks();
		step_ = Step::Game;
		if (game_.ok && modProblem_.empty()) setStatus(Level::Info, "Read the mod. The game is " + game_.target.name() + ".");
		else if (modProblem_.empty()) setStatus(Level::Info, "Read the mod. Now choose the game's folder.");
		// The folder entered last time loads at once.
		if (!game_.ok && game_.error.empty() && !gameInput_.empty()) chooseGame(gameInput_);
		return true;
	}

	void Installer::resetOptions()
	{
		options_.clear();
		choices_ = {};
		if (!mod_) return;
		if (mod_->isNfsms)
		{
			choices_ = ns::DefaultChoices(mod_->nfsms);
			return;
		}
		for (const auto& [id, v] : sc::ResolveOptions(mod_->amod, {})) options_[id] = v->text;
	}

	void Installer::chooseGame(std::string folder, std::optional<Target> target)
	{
		if (busy_) return;
		gameInput_ = folder;
		outcome_ = Outcome::None;
		result_ = {};
		game_ = {};
		modProblem_.clear();
		modWarning_.clear();
		std::string why;
		const std::optional<install::GameFolder> found = install::FindGameFolder(folder, &why);
		if (!found)
		{
			game_.error = why;
			post(Level::Error, why);
			setStatus(Level::Error, why);
			return;
		}
		setStatus(Level::Info, "Reading the game's files in " + found->global + "...");
		post(Level::Info, "Reading the game's files in " + found->global);
		startWork(Work::Loading, [this, f = *found, target]() { loadGameJob(f, target); });
	}

	void Installer::loadGameJob(const install::GameFolder& f, std::optional<Target> target)
	{
		FoundGame found;
		found.folder = f;
		try
		{
			found.detection = DetectTarget(f.global);
			found.target = target ? *target : found.detection.target;
			found.chosen = target.has_value();
			std::vector<std::string> moved;
			auto db = loadDatabase(f, found.target, moved, &found);
			std::string how = found.chosen ? "as chosen" : found.detection.certain ? "from its files" : "a guess: " + found.detection.reason;
			post(Level::Info, "Found " + found.target.name() + " (" + how + ") in " + f.global + ": " + Joined(found.packs, ", "));
			for (const std::string& c : found.copies) post(Level::Info, c + "; installing updates it too.");
			db_ = std::move(db);
			movedFrom_ = std::move(moved);
			dbStale_ = false;
			found.ok = true;
			if (!SupportedOn(found.target.game, found.target.platform))
				post(Level::Warning, found.target.name() + " isn't one Attribulina edits yet: values it doesn't know are kept as bytes.");
			setStatus(Level::Success, "Found " + found.target.name() + ".");
		}
		catch (const std::exception& e)
		{
			found.ok = false;
			found.error = std::string("The game's files didn't load: ") + e.what();
			db_.reset();
			post(Level::Error, found.error);
			setStatus(Level::Error, found.error);
		}
		std::lock_guard lock(m_);
		gameDone_ = std::move(found);
	}

	bool Installer::canInstall() const
	{
		return mod_ && game_.ok && modProblem_.empty() && !busy_;
	}

	bool Installer::canGoTo(Step s) const
	{
		if (busy_) return false;
		switch (s)
		{
			case Step::Mod: return true;
			case Step::Game: return mod_ != nullptr;
			case Step::Options:
			case Step::Install: return mod_ && game_.ok && modProblem_.empty();
		}
		return false;
	}

	void Installer::goTo(Step s)
	{
		if (!canGoTo(s) || s == step_) return;
		// Leaving a finished installation: the next one starts afresh.
		if (step_ == Step::Install && outcome_ != Outcome::None)
		{
			outcome_ = Outcome::None;
			result_ = {};
		}
		step_ = s;
		switch (s)
		{
			case Step::Mod:
				setStatus(Level::Info, mod_ ? "The mod is " + mod_->title + "." : "Choose the mod to install.");
				break;
			case Step::Game:
				if (!game_.ok && game_.error.empty() && !gameInput_.empty()) chooseGame(gameInput_);
				else if (game_.ok) setStatus(modProblem_.empty() ? Level::Info : Level::Error, modProblem_.empty() ? "The game is " + game_.target.name() + "." : modProblem_);
				else if (gameInput_.empty()) setStatus(Level::Info, "Choose the game's folder.");
				break;
			case Step::Options:
				setStatus(Level::Info, mod_->hasOptions() ? "Choose the mod's options." : "This mod has no options.");
				break;
			case Step::Install:
				setStatus(Level::Info, "Ready to install.");
				break;
		}
	}

	void Installer::installAnother()
	{
		if (busy_) return;
		outcome_ = Outcome::None;
		result_ = {};
		mod_.reset();
		images_.release();
		modInput_.clear();
		modError_.clear();
		modProblem_.clear();
		modWarning_.clear();
		options_.clear();
		choices_ = {};
		step_ = Step::Mod;
		host_.setWindowTitle(kTitle);
		setStatus(Level::Info, "Choose the mod to install.");
	}

	void Installer::install()
	{
		if (!canInstall()) return;
		outcome_ = Outcome::None;
		result_ = {};
		step_ = Step::Install;
		install::ModSettings st;
		st.options = options_;
		st.choices = choices_;
		st.gameFolder = game_.folder.game;
		st.vltEd = true; // nfsms mods are made for NFS-VltEd
		setStatus(Level::Info, "Installing " + mod_->title + "...");
		startWork(Work::Installing, [this, mod = mod_, st, game = game_]() { installJob(*mod, st, game); });
	}

	void Installer::installJob(const install::Mod& mod, const install::ModSettings& st, const FoundGame& game)
	{
		Result r;
		const std::string source = mod.isNfsms ? mod.nfsms.source : mod.amod.source;
		post(Level::Info, "Installing " + mod.title + " (" + mod.path + ") into " + game.folder.game);
		if (mod.isNfsms)
		{
			for (const auto& [name, on] : st.choices.checkboxes) post(Level::Info, "  " + name + ": " + (on ? "on" : "off"));
			for (const auto& [group, name] : st.choices.radios) post(Level::Info, "  " + group + ": " + name);
		}
		else
		{
			for (const auto& [id, value] : st.options) post(Level::Info, "  option " + id + " = " + value);
		}
		size_t saved = 0;
		try
		{
			if (!db_ || dbStale_)
			{
				// A run before changed this one: the files as they are now.
				std::vector<std::string> moved;
				db_.reset();
				db_ = loadDatabase(game.folder, game.target, moved, nullptr);
				movedFrom_ = std::move(moved);
			}
			ensureData();
			dbStale_ = true;
			const sc::ApplyReport rep = install::RunMod(*db_, mod, st, data_->nfsmsWords);
			for (const sc::Message& m : rep.messages) post(LevelOf(m.level), install::MessageLine(source, m), m.level != sc::Message::Level::Echo);
			if (!rep.ok)
			{
				std::string first;
				for (const sc::Message& m : rep.messages)
					if (m.level == sc::Message::Level::Error && first.empty()) first = install::MessageLine(source, m);
				r.outcome = Outcome::Failed;
				r.headline = "The mod wasn't installed.";
				r.detail = (first.empty() ? std::string("Its script stopped.") : "Its script stopped at " + first) +
					"\nNothing was saved: the game's files are as they were.";
			}
			else
			{
				install::RememberNames(userKeysFile(), rep.newNames);
				std::vector<std::string> removed = movedFrom_;
				removed.insert(removed.end(), rep.removedFiles.begin(), rep.removedFiles.end());
				const install::Changes changes = install::ChangesOf(*db_, std::move(removed));
				const size_t failed = rep.errors(), warnings = rep.warnings();
				post(Level::Info, source + ": " + rep.summary() + (failed ? "; " + std::to_string(failed) + " line(s) failed" : "") +
					(warnings ? " (" + std::to_string(warnings) + " warning(s))" : ""));
				const install::SaveResult res = install::SaveChanges(*db_, changes, {}, [&](const install::SaveNote& n)
				{
					saved += n.kind == install::SaveNote::Kind::Saved;
					post(Level::Info, n.text);
				});
				for (const std::string& f : res.written) r.files.push_back(Shown(f, game.folder.game));
				for (const std::string& f : res.memoryFiles) r.files.push_back(Shown(f, game.folder.game));
				for (const std::string& f : res.removed) r.files.push_back(Shown(f, game.folder.game) + " (removed)");
				if (failed)
				{
					r.outcome = Outcome::Partly;
					r.headline = "The mod is installed, but " + std::to_string(failed) + (failed == 1 ? " line" : " lines") + " of its script failed.";
					r.detail = "The other lines ran and their changes are saved. The Log lists the lines that failed.";
				}
				else
				{
					r.outcome = Outcome::Succeeded;
					r.headline = "The mod is installed.";
					r.detail = changes.empty() ? "The game's files already held everything it sets, so no file needed changing."
						: "The first time a file is changed, its original is kept beside it as a .bak file.";
				}
				if (warnings) r.detail += "\nThe script gave " + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings") + "; the Log lists them.";
			}
		}
		catch (const std::exception& e)
		{
			r.outcome = Outcome::Failed;
			r.headline = "The mod wasn't installed.";
			r.detail = e.what();
			if (saved) r.detail += "\nSome files were saved before this; the Log lists them, and their originals are kept beside them as .bak files.";
			else r.detail += "\nNothing was saved: the game's files are as they were.";
			post(Level::Error, e.what());
		}
		const Level level = r.outcome == Outcome::Succeeded ? Level::Success : r.outcome == Outcome::Partly ? Level::Warning : Level::Error;
		const char* word = r.outcome == Outcome::Succeeded ? "Installation successful" : r.outcome == Outcome::Partly ? "Installation finished with errors"
			: "Installation failed";
		post(level, std::string(word) + ": " + r.headline);
		setStatus(level, std::string(word) + ".");
		std::lock_guard lock(m_);
		installDone_ = std::move(r);
	}

	bool Installer::requestClose()
	{
		if (busy_ && work_ == Work::Installing)
		{
			setStatus(Level::Warning, "The mod is being installed. Close the window once it's done, or the game's files may be left half written.");
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------- drawing ---

	void Installer::frame()
	{
		harvest();
		images_.collect();
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos);
		ImGui::SetNextWindowSize(vp->WorkSize);
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBringToFrontOnFocus;
		ImGui::Begin(kTitle, nullptr, flags);
		const ImGuiStyle& style = ImGui::GetStyle();
		const float em = ImGui::GetFontSize();

		ImGui::PushFont(nullptr, style.FontSizeBase * 1.4f);
		ImGui::TextUnformatted(kTitle);
		ImGui::PopFont();
		ImGui::Separator();

		// Status and buttons at the bottom, the Log above them when shown.
		const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + style.ItemSpacing.y * 2 + 2;
		const float avail = ImGui::GetContentRegionAvail().y;
		const float logHeight = showLog_ ? std::max(em * 8, (avail - footer) * 0.45f) : 0.0f;
		const float body = std::max(em * 6, avail - footer - logHeight);

		ImGui::BeginChild("##steps", ImVec2(em * 9, body));
		drawSteps();
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##page", ImVec2(0, body));
		ImGui::PushTextWrapPos(0.0f);
		switch (step_)
		{
			case Step::Mod: drawModPage(); break;
			case Step::Game: drawGamePage(); break;
			case Step::Options: drawOptionsPage(); break;
			case Step::Install: drawInstallPage(); break;
		}
		ImGui::PopTextWrapPos();
		ImGui::EndChild();
		if (showLog_) drawLog(logHeight);
		ImGui::Separator();
		drawStatus();
		drawButtons();
		ImGui::End();
	}

	void Installer::drawSteps()
	{
		const Step all[] = { Step::Mod, Step::Game, Step::Options, Step::Install };
		for (Step s : all)
		{
			const bool current = s == step_;
			const std::string label = std::to_string(int(s) + 1) + "   " + StepName(s);
			ImGui::BeginDisabled(!current && !canGoTo(s));
			if (ImGui::Selectable(label.c_str(), current)) goTo(s);
			ImGui::EndDisabled();
		}
	}

	void Installer::drawModPage()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::TextUnformatted("Choose the mod to install: a mod script (.amod) or an nfsms script (.nfsms), with the files that came with it.");
		ImGui::Spacing();
		const float browse = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2;
		ImGui::SetNextItemWidth(-(browse + style.ItemSpacing.x));
		if (ImGui::InputTextWithHint("##mod", "The mod's .amod or .nfsms file", &modInput_, ImGuiInputTextFlags_EnterReturnsTrue)) openMod(modInput_);
		ImGui::SameLine();
		if (ImGui::Button("Browse...##mod"))
			if (const auto p = host_.openFileDialog("Choose the mod", { { "Mod scripts", "*.amod;*.nfsms" }, { "All files", "*.*" } })) openMod(*p);
		if (!modError_.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, kError);
			ImGui::TextUnformatted(modError_.c_str());
			ImGui::PopStyleColor();
		}
		if (!mod_) return;
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::PushFont(nullptr, style.FontSizeBase * 1.2f);
		ImGui::TextUnformatted(mod_->title.c_str());
		ImGui::PopFont();
		std::string by;
		if (!mod_->author.empty()) by = "by " + mod_->author;
		if (!mod_->version.empty()) by += (by.empty() ? "version " : ", version ") + mod_->version;
		if (!by.empty()) ImGui::TextDisabled("%s", by.c_str());
		ImGui::TextDisabled("%s", mod_->isNfsms ? "An nfsms script, run as NFS-VltEd 4.6 runs it." : "An Attribulina mod script.");
		if (!mod_->isNfsms)
			if (const auto t = ParseScriptTarget(mod_->amod.meta("game"))) ImGui::TextDisabled("Written for %s.", t->name().c_str());
		if (!mod_->description.empty())
		{
			ImGui::Spacing();
			ImGui::TextUnformatted(mod_->description.c_str());
		}
	}

	void Installer::drawGamePage()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::TextUnformatted("Choose the folder the game is installed in: the one with its .exe and its GLOBAL folder, which holds the database.");
		ImGui::Spacing();
		const float browse = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2;
		const float recent = recentGames_.empty() ? 0.0f : ImGui::GetFrameHeight() + style.ItemSpacing.x;
		ImGui::SetNextItemWidth(-(browse + style.ItemSpacing.x + recent));
		ImGui::BeginDisabled(busy_);
		if (ImGui::InputTextWithHint("##game", "The game's folder", &gameInput_, ImGuiInputTextFlags_EnterReturnsTrue)) chooseGame(gameInput_);
		if (!recentGames_.empty())
		{
			ImGui::SameLine(0, style.ItemSpacing.x);
			if (ImGui::BeginCombo("##recent", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft))
			{
				ImGui::TextDisabled("Folders chosen before");
				for (const std::string& g : std::vector<std::string>(recentGames_))
					if (ImGui::Selectable(g.c_str(), g == gameInput_)) chooseGame(g);
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Folders chosen before");
		}
		ImGui::SameLine();
		if (ImGui::Button("Browse...##game"))
			if (const auto p = host_.openFolderDialog("Choose the game's folder")) chooseGame(*p);
		ImGui::EndDisabled();
		ImGui::Spacing();
		if (busy_ && work_ == Work::Loading)
		{
			ImGui::TextDisabled("Reading the game's files...");
			return;
		}
		if (!game_.error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kError);
			ImGui::TextUnformatted(game_.error.c_str());
			ImGui::PopStyleColor();
			return;
		}
		if (!game_.ok) return;
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::PushFont(nullptr, style.FontSizeBase * 1.2f);
		ImGui::TextUnformatted(game_.target.name().c_str());
		ImGui::PopFont();
		if (!game_.detection.certain || game_.chosen)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
			ImGui::TextUnformatted(game_.chosen ? "As chosen below."
				: ("The files don't say for certain which game and platform they are from (" + game_.detection.reason + "). If this is wrong, choose the right one:").c_str());
			ImGui::PopStyleColor();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
			if (ImGui::BeginCombo("##target", game_.target.name().c_str()))
			{
				for (const attrib::GameInfo& g : Games())
					for (Platform p : g.supported)
					{
						const Target t{ g.game, p };
						if (ImGui::Selectable(t.name().c_str(), t == game_.target) && !(t == game_.target)) chooseGame(game_.folder.game, t);
					}
				ImGui::EndCombo();
			}
		}
		ImGui::Spacing();
		ImGui::TextDisabled("The database: %s", game_.folder.global.c_str());
		ImGui::TextDisabled("Its packs: %s", Joined(game_.packs, ", ").c_str());
		for (const std::string& c : game_.copies) ImGui::TextDisabled("%s; installing updates it too, as the game reads the copy.", c.c_str());
		if (game_.warnings)
			ImGui::TextColored(kWarn, "%zu %s about the files; the Log lists them.", game_.warnings, game_.warnings == 1 ? "warning" : "warnings");
		if (!modProblem_.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, kError);
			ImGui::TextUnformatted(modProblem_.c_str());
			ImGui::PopStyleColor();
		}
		if (!modWarning_.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
			ImGui::TextUnformatted(modWarning_.c_str());
			ImGui::PopStyleColor();
		}
	}

	void Installer::drawOptionsPage()
	{
		if (!mod_) return;
		if (mod_->isNfsms)
		{
			ImGui::TextDisabled("nfsms scripts run as NFS-VltEd 4.6 runs them.");
			ImGui::Spacing();
			if (!mod_->nfsms.hasSetup())
			{
				ImGui::TextUnformatted("This mod has no options.");
				return;
			}
			ImGui::TextUnformatted("Choose how to install the mod:");
			ImGui::Spacing();
			ImGui::BeginChild("##setup", ImVec2(0, 0), ImGuiChildFlags_Borders);
			app::DrawNfsmsSetup(mod_->nfsms, choices_, images_);
			ImGui::EndChild();
			return;
		}
		const auto options = mod_->amod.options();
		if (options.empty())
		{
			ImGui::TextUnformatted("This mod has no options.");
			return;
		}
		ImGui::TextUnformatted("Choose how to install the mod:");
		ImGui::Spacing();
		app::DrawScriptOptions(options, options_);
		ImGui::Spacing();
		if (ImGui::Button("Back to the Defaults")) resetOptions();
	}

	void Installer::drawInstallPage()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		if (busy_ && work_ == Work::Installing)
		{
			ImGui::PushFont(nullptr, style.FontSizeBase * 1.4f);
			ImGui::TextUnformatted("Installing...");
			ImGui::PopFont();
			ImGui::TextDisabled("%s", mod_ ? mod_->title.c_str() : "");
			return;
		}
		if (outcome_ != Outcome::None)
		{
			const Level level = outcome_ == Outcome::Succeeded ? Level::Success : outcome_ == Outcome::Partly ? Level::Warning : Level::Error;
			ImGui::PushFont(nullptr, style.FontSizeBase * 1.4f);
			ImGui::PushStyleColor(ImGuiCol_Text, ColourOf(level));
			ImGui::TextUnformatted(result_.headline.c_str());
			ImGui::PopStyleColor();
			ImGui::PopFont();
			ImGui::Spacing();
			ImGui::TextUnformatted(result_.detail.c_str());
			if (!result_.files.empty())
			{
				ImGui::Spacing();
				ImGui::TextUnformatted("Files changed:");
				for (const std::string& f : result_.files) ImGui::BulletText("%s", f.c_str());
			}
			ImGui::Spacing();
			if (!logFile_.path().empty())
			{
				ImGui::TextDisabled("The log: %s", logFile_.path().c_str());
				if (host_.canOpenPaths() && ImGui::Button("Open the Log File")) host_.openPath(logFile_.path());
				ImGui::SameLine();
			}
			if (ImGui::Button("Install Another Mod")) installAnother();
			return;
		}
		ImGui::TextUnformatted("Ready to install.");
		ImGui::Spacing();
		if (!mod_ || !game_.ok) return;
		ImGui::Text("The mod: %s (%s)", mod_->title.c_str(), FileName(mod_->path).c_str());
		ImGui::Text("The game: %s, in %s", game_.target.name().c_str(), game_.folder.game.c_str());
		if (mod_->isNfsms)
		{
			std::vector<std::string> chosen;
			for (const ns::Control& c : mod_->nfsms.controls)
			{
				if (c.kind == ns::Control::Kind::Checkbox)
				{
					auto it = choices_.checkboxes.find(c.name);
					if (it != choices_.checkboxes.end() && it->second) chosen.push_back(c.label);
				}
				else if (auto it = choices_.radios.find(c.group); it != choices_.radios.end() && it->second == c.name)
				{
					chosen.push_back(c.label);
				}
			}
			if (mod_->nfsms.hasSetup()) ImGui::Text("Chosen: %s", chosen.empty() ? "nothing" : Joined(chosen, ", ").c_str());
		}
		else
		{
			for (const sc::OptionDef* o : mod_->amod.options())
			{
				auto it = options_.find(o->id);
				ImGui::Text("%s: %s", (o->label.empty() ? o->id : o->label).c_str(), it == options_.end() ? "" : it->second.c_str());
			}
		}
		ImGui::Spacing();
		ImGui::TextUnformatted("Installing runs the mod's script on the game's database and saves the files it changes. The first time a file is "
			"changed, its original is kept beside it as a .bak file. The memory files that hold copies of them (GlobalMemoryFile.bin) are "
			"updated too, since the game reads the copies.");
		if (mod_->isNfsms && CopiesFiles(mod_->nfsms))
		{
			ImGui::Spacing();
			ImGui::Text("The mod also copies files into %s.", game_.folder.game.c_str());
		}
	}

	void Installer::drawStatus()
	{
		std::string status;
		Level level;
		{
			std::lock_guard lock(m_);
			status = status_;
			level = statusLevel_;
		}
		const ImGuiStyle& style = ImGui::GetStyle();
		const char* toggle = "Show the Log";
		const float toggleWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(toggle).x;
		// One line: what is going on, cut short with ... when it doesn't fit.
		ImGui::AlignTextToFramePadding();
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x - toggleWidth - style.ItemSpacing.x);
		ImGui::Dummy(ImVec2(width, ImGui::GetFrameHeight()));
		if (ImGui::IsItemHovered() && ImGui::CalcTextSize(status.c_str()).x > width) ImGui::SetTooltip("%s", status.c_str());
		ImGui::PushStyleColor(ImGuiCol_Text, ColourOf(level));
		const float y = pos.y + style.FramePadding.y;
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(pos.x, y), ImVec2(pos.x + width, y + ImGui::GetFontSize()), pos.x + width,
			status.c_str(), nullptr, nullptr);
		ImGui::PopStyleColor();
		ImGui::SameLine(right - toggleWidth);
		app::Checkbox(toggle, &showLog_);
	}

	void Installer::drawLog(float height)
	{
		{
			std::lock_guard lock(m_);
			if (logShown_ != logVersion_)
			{
				logText_.clear();
				logLevels_.clear();
				for (const LogLine& l : log_)
				{
					logText_ += l.text;
					logText_ += '\n';
					logLevels_.push_back(uint8_t(l.level));
				}
				logView_.droppedFront(logDropped_);
				logDropped_ = 0;
				logShown_ = logVersion_;
			}
		}
		ImGui::Separator();
		if (ImGui::SmallButton("Copy All")) ImGui::SetClipboardText(logText_.c_str());
		if (!logFile_.path().empty() && host_.canOpenPaths())
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Open the Log File")) host_.openPath(logFile_.path());
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", logFile_.path().empty() ? "Select text to copy it." : logFile_.path().c_str());
		const ImU32 colours[4] = { ImGui::GetColorU32(ImGuiCol_Text), ImGui::GetColorU32(kWarn), ImGui::GetColorU32(kError), ImGui::GetColorU32(kGood) };
		logView_.draw("##log", logText_, logLevels_, colours, ImVec2(-FLT_MIN, std::max(ImGui::GetFontSize() * 4, height - ImGui::GetFrameHeightWithSpacing() - 2)));
	}

	void Installer::drawButtons()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float em = ImGui::GetFontSize();
		const ImVec2 size(em * 7, 0);
		const bool finished = step_ == Step::Install && outcome_ != Outcome::None;
		const int count = finished ? 1 : 3;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - (size.x * float(count) + style.ItemSpacing.x * float(count - 1)));
		if (finished)
		{
			if (ImGui::Button("Close", size)) host_.quit();
			return;
		}
		const int at = int(step_);
		ImGui::BeginDisabled(at == 0 || busy_);
		if (ImGui::Button("< Back", size)) goTo(Step(at - 1));
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (step_ == Step::Install)
		{
			ImGui::BeginDisabled(!canInstall());
			if (ImGui::Button("Install", size)) install();
			ImGui::EndDisabled();
		}
		else
		{
			ImGui::BeginDisabled(!canGoTo(Step(at + 1)));
			if (ImGui::Button("Next >", size)) goTo(Step(at + 1));
			ImGui::EndDisabled();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(busy_ && work_ == Work::Installing);
		if (ImGui::Button("Cancel", size)) host_.quit();
		ImGui::EndDisabled();
	}

} // namespace installer
