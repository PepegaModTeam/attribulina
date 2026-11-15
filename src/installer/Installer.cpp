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
				case Step::Mod: return "Mod and Game";
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
		setStatus(Level::Info, "Choose the mod to install and the game's folder.");
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
			else if (l.rfind("TrackMods=", 0) == 0) trackMods_ = l.substr(10) != "0";
		}
	}

	void Installer::saveSettings()
	{
		if (paths_.userDir.empty()) return;
		std::ofstream out(FromUtf8(paths_.userDir) / kSettingsFileName, std::ios::binary | std::ios::trunc);
		out << "[" << kTitle << "]\n";
		out << "TrackMods=" << (trackMods_ ? 1 : 0) << "\n";
		for (const std::string& g : recentGames_) out << "GameFolder=" << g << "\n";
	}

	void Installer::setTrackMods(bool on)
	{
		if (trackMods_ == on) return;
		trackMods_ = on;
		saveSettings();
		post(Level::Info, on ? "The game's list of installed mods (attribulina_mods.txt) is kept again: installing adds to it."
			: "The list of installed mods is off: installing neither checks attribulina_mods.txt nor adds to it.");
		refreshModChecks();
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
		std::optional<ModsDone> mods;
		{
			std::lock_guard lock(m_);
			game.swap(gameDone_);
			result.swap(installDone_);
			mods.swap(modsDone_);
		}
		if (game || result || mods) work_ = Work::None;
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
			conflicts_ = result_.conflicts;
			conflictError_ = result_.conflictError;
			if (result_.mods)
			{
				game_.mods = result_.mods;
				game_.traced = result_.traced;
				game_.modsError.clear();
				modsView_.forget();
			}
		}
		if (mods)
		{
			modsMessage_ = mods->message;
			modsMessageLevel_ = mods->level;
			if (mods->ask)
			{
				// Worked out what uninstalling takes: the confirmation, or why not.
				uninstallAsk_ = std::move(mods->ask);
				if (!uninstallAsk_->error.empty())
				{
					modsMessage_ = uninstallAsk_->mod.nameAndVersion() + " can't be uninstalled: " + uninstallAsk_->error;
					modsMessageLevel_ = Level::Error;
					uninstallAsk_.reset();
				}
				else
				{
					confirm_ = Confirm::Uninstall;
				}
			}
			if (mods->mods)
			{
				game_.mods = std::move(mods->mods);
				game_.traced = std::move(mods->traced);
				game_.modsError.clear();
				modsView_.forget();
				// The list changed under what the Install step found.
				conflicts_.clear();
				conflictError_.clear();
				if (!finished()) refreshModChecks();
			}
		}
	}

	void Installer::refreshModChecks()
	{
		modProblem_.clear();
		modWarning_.clear();
		installedNote_.clear();
		installAnyway_ = false;
		if (!mod_ || !db_ || !game_.ok || busy_) return;
		modProblem_ = install::ModProblem(*mod_, *db_);
		modWarning_ = install::ModWarning(*mod_, *db_);
		if (trackMods_ && game_.mods)
		{
			installedNote_ = InstalledNote(FindInstalled(*game_.mods, mod_->identity()), mod_->identity());
			if (!installedNote_.empty()) post(Level::Warning, installedNote_);
		}
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

	bool Installer::refuseFinished()
	{
		if (!finished()) return false;
		setStatus(Level::Info, "This installation is done. Install Another Mod starts over.");
		return true;
	}

	void Installer::dropped(const std::string& path)
	{
		if (busy_ || refuseFinished()) return;
		std::error_code ec;
		if (!fs::is_directory(FromUtf8(path), ec))
		{
			openMod(path);
			return;
		}
		step_ = Step::Mod; // the folder chosen on its page
		chooseGame(path);
	}

	bool Installer::openMod(std::string path)
	{
		if (busy_ || refuseFinished()) return false;
		modInput_ = path;
		outcome_ = Outcome::None;
		result_ = {};
		conflicts_.clear();
		conflictError_.clear();
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
		step_ = Step::Mod;
		modError_.clear();
		resetOptions();
		images_.release();
		if (mod_->isNfsms) images_.load(host_, mod_->nfsms);
		std::string what = mod_->isNfsms ? "an nfsms script" : "a mod script";
		if (mod_->hasOptions()) what += " with options";
		post(Level::Info, "Read " + path + ": " + mod_->title + (mod_->version.empty() ? "" : " " + mod_->version) + ", " + what +
			(mod_->titled ? "." : "; it names no title, so its file's SHA-256 (" + mod_->sha256.substr(0, 12) + "...) tells it apart."));
		for (const std::string& w : mod_->warnings) post(Level::Warning, w);
		host_.setWindowTitle(std::string(kTitle) + " - " + mod_->title);
		refreshModChecks();
		if (game_.ok && modProblem_.empty()) setStatus(Level::Info, "Read the mod. The game is " + game_.target.name() + ": Next goes on.");
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
		if (busy_ || refuseFinished()) return;
		gameInput_ = folder;
		outcome_ = Outcome::None;
		result_ = {};
		conflicts_.clear();
		conflictError_.clear();
		game_ = {};
		modProblem_.clear();
		modWarning_.clear();
		installedNote_.clear();
		installAnyway_ = false;
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
			try
			{
				found.mods = ReadModList(f.global);
				if (found.mods)
				{
					found.traced = ReadTraces(f.global, *found.mods);
					std::vector<std::string> names;
					for (const InstalledMod& m : found.mods->mods) names.push_back(m.name() + (m.version.empty() ? "" : " " + m.version));
					post(Level::Info, ModListPath(f.global) + " lists " + std::to_string(names.size()) + (names.size() == 1 ? " mod" : " mods") +
						" installed here" + (names.empty() ? "." : ": " + Joined(names, ", ")));
				}
			}
			catch (const std::exception& e)
			{
				found.modsError = e.what();
				post(Level::Warning, "The list of installed mods doesn't read, so installing leaves it as it is: " + found.modsError);
			}
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
		return mod_ && game_.ok && modProblem_.empty() && (installedNote_.empty() || installAnyway_) && conflicts_.empty() && !busy_ && !finished();
	}

	bool Installer::finished() const
	{
		return step_ == Step::Install && outcome_ != Outcome::None;
	}

	bool Installer::canGoTo(Step s) const
	{
		if (busy_) return false;
		// Done: the steps before stay as they were installed, and Install
		// Another Mod is the way to start over.
		if (finished()) return s == Step::Install;
		switch (s)
		{
			case Step::Mod: return true;
			case Step::Options:
			case Step::Install: return mod_ && game_.ok && modProblem_.empty() && (installedNote_.empty() || installAnyway_);
		}
		return false;
	}

	void Installer::goTo(Step s)
	{
		if (!canGoTo(s) || s == step_) return;
		step_ = s;
		// The options may change: installing finds the conflicts again.
		conflicts_.clear();
		conflictError_.clear();
		switch (s)
		{
			case Step::Mod:
				if (!game_.ok && game_.error.empty() && !gameInput_.empty()) chooseGame(gameInput_);
				else if (!modProblem_.empty()) setStatus(Level::Error, modProblem_);
				else if (!mod_) setStatus(Level::Info, game_.ok ? "The game is " + game_.target.name() + ". Choose the mod to install." : "Choose the mod to install and the game's folder.");
				else setStatus(Level::Info, game_.ok ? "The mod is " + mod_->title + ", the game " + game_.target.name() + "." : "Choose the game's folder.");
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
		conflicts_.clear();
		conflictError_.clear();
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
		setStatus(Level::Info, game_.ok ? "The game is " + game_.target.name() + ". Choose the mod to install." : "Choose the mod to install.");
	}

	void Installer::install()
	{
		if (!canInstall()) return;
		startInstall({});
	}

	void Installer::uninstallTickedAndInstall()
	{
		if (conflicts_.empty() || busy_ || finished()) return;
		Plan plan;
		plan.conflicts = conflicts_;
		auto add = [&](const InstalledMod& m)
		{
			if (std::none_of(plan.uninstall.begin(), plan.uninstall.end(), [&](const InstalledMod& o) { return SameEntry(o, m); }))
				plan.uninstall.push_back(m);
		};
		for (const Conflict& c : conflicts_)
			if (c.uninstall && c.cannot.empty())
			{
				add(c.mod);
				for (const InstalledMod& w : c.with) add(w); // the confirmation listed them
			}
		if (plan.uninstall.empty()) return;
		// The ones left ticked off stay, and it goes over them.
		plan.overConflicts = true;
		startInstall(std::move(plan));
	}

	void Installer::installOverConflicts()
	{
		if (conflicts_.empty() || busy_ || finished()) return;
		Plan plan;
		plan.overConflicts = true;
		startInstall(std::move(plan));
	}

	void Installer::startInstall(Plan plan)
	{
		outcome_ = Outcome::None;
		result_ = {};
		conflicts_.clear();
		conflictError_.clear();
		step_ = Step::Install;
		install::ModSettings st;
		st.options = options_;
		st.choices = choices_;
		st.gameFolder = game_.folder.game;
		st.vltEd = true; // nfsms mods are made for NFS-VltEd
		setStatus(Level::Info, "Installing " + mod_->title + "...");
		startWork(Work::Installing, [this, mod = mod_, st, game = game_, track = trackMods_, plan = std::move(plan)]()
		{
			installJob(*mod, st, game, track, plan);
		});
	}

	void Installer::reloadIfStale(const FoundGame& game)
	{
		if (db_ && !dbStale_) return;
		// A run before changed this one: the files as they are now.
		std::vector<std::string> moved;
		db_.reset();
		db_ = loadDatabase(game.folder, game.target, moved, nullptr);
		movedFrom_ = std::move(moved);
		dbStale_ = false;
	}

	InstalledMod Installer::entryFor(const install::Mod& mod, const install::ModSettings& st) const
	{
		InstalledMod m;
		m.title = mod.titled ? mod.title : std::string();
		m.author = mod.author;
		m.version = mod.version;
		m.script = mod.path;
		m.sha256 = mod.sha256;
		m.nfsms = mod.isNfsms;
		install::CompleteRecord(m, *db_, std::string(kTitle) + " " + Version::GetAppVersionString(),
			mod.isNfsms ? std::map<std::string, std::string>() : st.options, mod.isNfsms ? &st.choices : nullptr);
		return m;
	}

	void Installer::writeList(const FoundGame& game, const ModListUpdate& u, Result& r)
	{
		try
		{
			std::vector<std::string> problems;
			r.mods = UpdateModList(game.folder.global, u, &problems);
			for (const std::string& p : problems) post(Level::Warning, p);
			r.traced = ReadTraces(game.folder.global, *r.mods);
		}
		catch (const std::exception& e)
		{
			r.mods.reset();
			post(Level::Warning, std::string("The list of installed mods wasn't updated: ") + e.what());
		}
	}

	void Installer::installJob(const install::Mod& mod, const install::ModSettings& st, const FoundGame& game, bool track, const Plan& plan)
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
		bool wrote = false;
		InstalledMod entry;
		ModListUpdate update;
		std::shared_ptr<const sc::Script> trace;
		std::vector<std::string> uninstalled;
		sc::ApplyReport rep;
		// Stopped before writing anything: nothing more to say than the reason.
		bool stopped = false;
		auto stopAt = [&](std::vector<Conflict> found)
		{
			stopped = true;
			r.outcome = Outcome::None;
			std::string names;
			for (size_t i = 0; i < found.size(); ++i)
			{
				post(Level::Warning, mod.title + " " + found[i].text);
				names += (i == 0 ? "" : i + 1 == found.size() ? " and " : ", ") + found[i].mod.nameAndVersion();
			}
			post(Level::Warning, "Nothing was written. Uninstall " + names + " first, or install " + mod.title + " over " +
				(found.size() == 1 ? "it." : "them."));
			setStatus(Level::Warning, mod.title + " changes what " + names + (found.size() == 1 ? " changed" : " changed") +
				". Nothing was written yet: choose what to do.");
			r.conflicts = std::move(found);
		};
		try
		{
			reloadIfStale(game);
			ensureData();
			dbStale_ = true;
			entry = entryFor(mod, st);
			std::vector<TracedMod> rows = track ? game.traced : std::vector<TracedMod>{};
			std::vector<std::string> removed = movedFrom_;

			// The mods ticked to go first.
			if (!plan.uninstall.empty())
			{
				std::vector<size_t> which;
				for (const InstalledMod& m : plan.uninstall)
					for (size_t i = 0; i < rows.size(); ++i)
						if (SameEntry(rows[i].mod, m)) which.push_back(i);
				// With the mods that build on them, as the page said.
				const UninstallPlan up = PlanUninstall(*db_, rows, which);
				if (up.ok())
					for (size_t i : up.which) uninstalled.push_back(rows[i].mod.nameAndVersion());
				std::string names;
				for (size_t i = 0; i < uninstalled.size(); ++i) names += (i == 0 ? "" : i + 1 == uninstalled.size() ? " and " : ", ") + uninstalled[i];
				if (up.ok()) post(Level::Info, "Uninstalling " + names + " first");
				UninstallResult u;
				if (!up.ok()) u.error = up.error;
				else u = Uninstall(*db_, rows, up.which, {}, up.around);
				for (const std::string& n : u.notes) post(Level::Warning, n);
				if (!u.ok)
				{
					stopped = true;
					r.conflicts = plan.conflicts;
					r.conflictError = "Uninstalling didn't work, so nothing was written: " + u.error;
					post(Level::Error, r.conflictError);
					setStatus(Level::Error, "Uninstalling the ticked mods didn't work. Nothing was written.");
					std::lock_guard lock(m_);
					installDone_ = std::move(r);
					return;
				}
				post(Level::Info, "Uninstalled " + names + ": " + u.summary());
				install::RememberNames(userKeysFile(), u.newNames);
				removed.insert(removed.end(), u.removedFiles.begin(), u.removedFiles.end());
				FoldUninstall(update, rows, u);
				rows = WithUpdate(std::move(rows), update);
			}

			// What it changes, against the mods on the list (theirs from their traces).
			const bool check = track && !plan.overConflicts &&
				std::any_of(rows.begin(), rows.end(), [](const TracedMod& t) { return t.trace != nullptr; });
			auto conflictsOf = [&](const sc::Script& t)
			{
				std::vector<Conflict> out;
				for (const ModConflict& c : FindConflicts(PlacesOf(t), rows))
				{
					Conflict k;
					k.mod = rows[c.index].mod;
					k.text = ConflictText(c, k.mod.nameAndVersion());
					out.push_back(std::move(k));
				}
				return out;
			};
			std::unique_ptr<Database> before = track ? sc::CloneDatabase(*db_) : nullptr;
			const bool copies = mod.isNfsms && CopiesFiles(mod.nfsms);
			if (check && copies)
			{
				// Its files aren't copied until it is known that it goes in.
				post(Level::Info, "Checking what it changes against the installed mods, without copying its files yet");
				std::unique_ptr<Database> trial = sc::CloneDatabase(*db_);
				install::ModSettings dry = st;
				dry.doFiles = false;
				const sc::ApplyReport tried = install::RunMod(*trial, mod, dry, data_->nfsmsWords);
				if (tried.ok)
					if (std::vector<Conflict> found = conflictsOf(MakeTrace(*before, *trial, entry)); !found.empty())
					{
						planConflicts(*before, rows, found);
						stopAt(std::move(found));
					}
			}
			if (!stopped)
			{
				rep = install::RunMod(*db_, mod, st, data_->nfsmsWords);
				for (const sc::Message& m : rep.messages) post(LevelOf(m.level), install::MessageLine(source, m), m.level != sc::Message::Level::Echo);
			}
			if (stopped)
			{
			}
			else if (!rep.ok)
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
				if (track) trace = std::make_shared<const sc::Script>(MakeTrace(*before, *db_, entry));
				if (check && !copies)
					if (std::vector<Conflict> found = conflictsOf(*trace); !found.empty())
					{
						planConflicts(*before, rows, found);
						stopAt(std::move(found));
					}
				before.reset();
			}
			if (!stopped && rep.ok)
			{
				install::RememberNames(userKeysFile(), rep.newNames);
				removed.insert(removed.end(), rep.removedFiles.begin(), rep.removedFiles.end());
				const install::Changes changes = install::ChangesOf(*db_, std::move(removed));
				const size_t failed = rep.errors(), warnings = rep.warnings();
				post(Level::Info, source + ": " + rep.summary() + (failed ? "; " + std::to_string(failed) + " line(s) failed" : "") +
					(warnings ? " (" + std::to_string(warnings) + " warning(s))" : ""));
				for (const std::string& f : install::ChangedFiles(rep))
				{
					const std::string name = ModListFileName(f, game.folder.game);
					if (std::find(entry.files.begin(), entry.files.end(), name) == entry.files.end()) entry.files.push_back(name);
				}
				std::sort(entry.files.begin(), entry.files.end());
				wrote = !rep.copiedFiles.empty();
				const install::SaveResult res = install::SaveChanges(*db_, changes, {}, [&](const install::SaveNote& n)
				{
					saved += n.kind == install::SaveNote::Kind::Saved;
					wrote |= !n.files.empty();
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
				if (!uninstalled.empty())
				{
					std::string names;
					for (size_t i = 0; i < uninstalled.size(); ++i) names += (i == 0 ? "" : i + 1 == uninstalled.size() ? " and " : ", ") + uninstalled[i];
					r.detail += "\n" + names + (uninstalled.size() == 1 ? " was" : " were") + " uninstalled first.";
				}
				if (warnings) r.detail += "\nThe script gave " + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings") + "; the Log lists them.";
				if (track)
				{
					entry.result = failed ? "installed, " + std::to_string(failed) + (failed == 1 ? " line" : " lines") + " failed" : "installed";
					update.add.push_back({ entry, trace });
					writeList(game, update, r);
					if (r.mods)
					{
						const std::string at = r.mods->mods.empty() ? std::string() : r.mods->mods.back().trace;
						post(Level::Info, "Added " + entry.name() + " to the list of installed mods, " + ModListPath(game.folder.global) +
							(at.empty() ? std::string() : ", with what it changed in " + TracePath(game.folder.global, at)));
						r.detail += "\nIt is on the game's list of installed mods (attribulina_mods.txt in GLOBAL), which can uninstall it.";
					}
				}
			}
		}
		catch (const std::exception& e)
		{
			r.outcome = Outcome::Failed;
			r.conflicts.clear();
			stopped = false;
			r.headline = "The mod wasn't installed.";
			r.detail = e.what();
			if (saved) r.detail += "\nSome files were saved before this; the Log lists them, and their originals are kept beside them as .bak files.";
			else r.detail += "\nNothing was saved: the game's files are as they were.";
			post(Level::Error, e.what());
			// Files changed all the same: the list says so.
			if (track && wrote)
			{
				entry.result = std::string("saving failed halfway: ") + e.what();
				update.add.push_back({ entry, trace });
				writeList(game, update, r);
			}
		}
		if (!stopped)
		{
			const Level level = r.outcome == Outcome::Succeeded ? Level::Success : r.outcome == Outcome::Partly ? Level::Warning : Level::Error;
			const char* word = r.outcome == Outcome::Succeeded ? "Installation successful" : r.outcome == Outcome::Partly ? "Installation finished with errors"
				: "Installation failed";
			post(level, std::string(word) + ": " + r.headline);
			setStatus(level, std::string(word) + ".");
		}
		std::lock_guard lock(m_);
		installDone_ = std::move(r);
	}

	// --------------------------------------------------- the list's commands ---

	void Installer::planConflicts(const Database& before, const std::vector<TracedMod>& rows, std::vector<Conflict>& found)
	{
		for (Conflict& c : found)
		{
			size_t at = rows.size();
			for (size_t i = 0; i < rows.size(); ++i)
				if (SameEntry(rows[i].mod, c.mod)) at = i;
			if (at == rows.size()) continue;
			if (!rows[at].trace)
			{
				c.cannot = "it can't be uninstalled: " + rows[at].problem;
				c.uninstall = false;
				continue;
			}
			const UninstallPlan p = PlanUninstall(before, rows, { at });
			if (!p.ok())
			{
				c.cannot = "it can't be uninstalled: " + p.error;
				c.uninstall = false;
				continue;
			}
			for (const auto& [i, why] : p.dependents) c.with.push_back(rows[i].mod);
			c.withText = DependentsText(p, rows);
			if (!c.withText.empty()) post(Level::Warning, "Uninstalling " + c.mod.nameAndVersion() + " first: " + c.withText);
		}
	}

	bool Installer::canUninstall(size_t index) const
	{
		return game_.ok && !busy_ && index < game_.traced.size() && game_.traced[index].trace != nullptr;
	}

	std::vector<TracedMod> Installer::listNow(const FoundGame& game, const std::vector<InstalledMod>& mods, std::vector<size_t>& at) const
	{
		const std::optional<ModList> list = ReadModList(game.folder.global);
		if (!list) throw std::runtime_error("the game has no list of installed mods any more");
		std::vector<TracedMod> rows = ReadTraces(game.folder.global, *list);
		at.clear();
		for (const InstalledMod& m : mods)
		{
			size_t i = 0;
			while (i < rows.size() && !SameEntry(rows[i].mod, m)) ++i;
			if (i == rows.size()) throw std::runtime_error(m.nameAndVersion() + " isn't on the list any more");
			at.push_back(i);
		}
		return rows;
	}

	void Installer::askUninstall(size_t index)
	{
		if (!canUninstall(index)) return;
		modsMessage_.clear();
		uninstallAsk_.reset();
		confirm_ = Confirm::None;
		const InstalledMod which = game_.traced[index].mod;
		setStatus(Level::Info, "Working out what uninstalling " + which.nameAndVersion() + " takes...");
		startWork(Work::Planning, [this, game = game_, which]() { planJob(game, which); });
	}

	void Installer::planJob(const FoundGame& game, const InstalledMod& which)
	{
		ModsDone d;
		UninstallAsk ask;
		ask.mod = which;
		try
		{
			std::vector<size_t> at;
			const std::vector<TracedMod> rows = listNow(game, { which }, at);
			reloadIfStale(game);
			// On copies: the database stays as it is.
			const UninstallPlan p = PlanUninstall(*db_, rows, at);
			if (!p.ok())
			{
				ask.error = p.error;
			}
			else
			{
				for (size_t i : p.which) ask.all.push_back(rows[i].mod);
				ask.dependents = DependentsText(p, rows);
				ask.around = p.around;
				if (!ask.dependents.empty()) post(Level::Warning, ask.dependents);
			}
		}
		catch (const std::exception& e)
		{
			ask.error = e.what();
		}
		if (!ask.error.empty()) post(Level::Error, which.nameAndVersion() + " can't be uninstalled: " + ask.error);
		const std::string name = which.nameAndVersion();
		if (!ask.error.empty()) setStatus(Level::Error, name + " can't be uninstalled.");
		else if (ask.all.size() > 1) setStatus(Level::Warning, "Mods build on " + name + ": Installed Mods lists them before uninstalling.");
		else setStatus(Level::Info, "Installed Mods asks before uninstalling " + name + ".");
		d.ask = std::move(ask);
		std::lock_guard lock(m_);
		modsDone_ = std::move(d);
	}

	void Installer::uninstallAsked()
	{
		if (!uninstallAsk_ || busy_ || !game_.ok) return;
		const UninstallAsk ask = std::move(*uninstallAsk_);
		uninstallAsk_.reset();
		confirm_ = Confirm::None;
		modsMessage_.clear();
		setStatus(Level::Info, "Uninstalling " + ask.mod.nameAndVersion() + (ask.all.size() > 1 ? " and the mods that build on it" : "") + "...");
		startWork(Work::Uninstalling, [this, game = game_, ask]() { uninstallJob(game, ask); });
	}

	void Installer::uninstallJob(const FoundGame& game, const UninstallAsk& ask)
	{
		ModsDone d;
		std::vector<std::string> list;
		for (const InstalledMod& m : ask.all) list.push_back(m.nameAndVersion());
		std::string names;
		for (size_t i = 0; i < list.size(); ++i) names += (i == 0 ? "" : i + 1 == list.size() ? " and " : ", ") + list[i];
		post(Level::Info, "Uninstalling " + names + " from " + game.folder.game);
		size_t saved = 0;
		try
		{
			// The list as the file has it now.
			std::vector<size_t> at;
			const std::vector<TracedMod> rows = listNow(game, ask.all, at);
			reloadIfStale(game);
			dbStale_ = true;
			const UninstallResult u = Uninstall(*db_, rows, at, {}, ask.around);
			for (const std::string& n : u.notes) post(Level::Warning, n);
			if (!u.ok)
			{
				d.message = names + (list.size() == 1 ? " wasn't" : " weren't") + " uninstalled: " + u.error +
					(u.buildsOn ? " The list changed since the question: ask again." : "") + " Nothing was written.";
				d.level = Level::Error;
				post(Level::Error, d.message);
			}
			else
			{
				install::RememberNames(userKeysFile(), u.newNames);
				std::vector<std::string> removed = movedFrom_;
				removed.insert(removed.end(), u.removedFiles.begin(), u.removedFiles.end());
				const install::Changes changes = install::ChangesOf(*db_, std::move(removed));
				install::SaveChanges(*db_, changes, {}, [&](const install::SaveNote& n)
				{
					saved += n.kind == install::SaveNote::Kind::Saved;
					post(Level::Info, n.text);
				});
				ModListUpdate up;
				FoldUninstall(up, rows, u);
				std::vector<std::string> problems;
				d.mods = UpdateModList(game.folder.global, up, &problems);
				for (const std::string& p : problems) post(Level::Warning, p);
				d.traced = ReadTraces(game.folder.global, *d.mods);
				d.message = "Uninstalled " + names + ": " + u.summary() + "." +
					(changes.empty() ? " No file needed changing." : " The files are saved.") +
					(u.redone ? " The mods installed after that change the same things were undone and done again, so what they set stays." : "") +
					(list.size() == 1 ? " Files it copied into the game's folder stay as they are." : " Files they copied into the game's folder stay as they are.");
				d.level = Level::Success;
				post(Level::Success, d.message);
			}
		}
		catch (const std::exception& e)
		{
			d.message = names + (list.size() == 1 ? " wasn't" : " weren't") + " uninstalled: " + e.what() +
				(saved ? std::string(" Some files were saved before this; the Log lists them.") : std::string(" Nothing was written."));
			d.level = Level::Error;
			post(Level::Error, d.message);
		}
		setStatus(d.level, d.message);
		std::lock_guard lock(m_);
		modsDone_ = std::move(d);
	}

	void Installer::forgetMod(size_t index)
	{
		if (busy_ || !game_.ok || index >= game_.traced.size()) return;
		const InstalledMod which = game_.traced[index].mod;
		ModListUpdate u;
		u.remove.push_back(which);
		try
		{
			std::vector<std::string> problems;
			game_.mods = UpdateModList(game_.folder.global, u, &problems);
			for (const std::string& p : problems) post(Level::Warning, p);
			game_.traced = ReadTraces(game_.folder.global, *game_.mods);
			game_.modsError.clear();
			modsMessage_ = "Took " + which.nameAndVersion() + " off the list. The game's files are as they were.";
			modsMessageLevel_ = Level::Info;
			post(Level::Info, modsMessage_);
		}
		catch (const std::exception& e)
		{
			modsMessage_ = std::string("The list wasn't changed: ") + e.what();
			modsMessageLevel_ = Level::Error;
			post(Level::Error, modsMessage_);
		}
		modsView_.forget();
		conflicts_.clear();
		conflictError_.clear();
		if (!finished()) refreshModChecks();
	}

	bool Installer::requestClose()
	{
		if (busy_ && (work_ == Work::Installing || work_ == Work::Uninstalling))
		{
			setStatus(Level::Warning, std::string(work_ == Work::Installing ? "The mod is being installed" : "A mod is being uninstalled") +
				". Close the window once it's done, or the game's files may be left half written.");
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------- drawing ---

	void Installer::frame()
	{
		harvest();
		images_.collect();
		// The folder chosen last loads at once: its list of installed mods is a click away.
		if (!loadedFirst_)
		{
			loadedFirst_ = true;
			if (!busy_ && !game_.ok && game_.error.empty() && !gameInput_.empty()) chooseGame(gameInput_);
		}
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
			case Step::Options: drawOptionsPage(); break;
			case Step::Install: drawInstallPage(); break;
		}
		ImGui::PopTextWrapPos();
		ImGui::EndChild();
		if (showLog_) drawLog(logHeight);
		ImGui::Separator();
		drawStatus();
		drawButtons();
		drawModsWindow();
		ImGui::End();
	}

	void Installer::drawSteps()
	{
		const Step all[] = { Step::Mod, Step::Options, Step::Install };
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
		ImGui::PushFont(nullptr, style.FontSizeBase * 1.2f);
		ImGui::TextUnformatted("The mod");
		ImGui::PopFont();
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
		if (mod_)
		{
			ImGui::Spacing();
			ImGui::Text("%s", mod_->title.c_str());
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
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		drawGamePart();
	}

	void Installer::drawGamePart()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::PushFont(nullptr, style.FontSizeBase * 1.2f);
		ImGui::TextUnformatted("The game");
		ImGui::PopFont();
		ImGui::TextUnformatted("Choose the folder the game is installed in: the one with its .exe and its GLOBAL folder, which holds the database. "
			"No mod is needed for this: Installed Mods then shows what is installed in it, and uninstalls.");
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
		ImGui::Text("%s", game_.target.name().c_str());
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
		if (game_.mods)
			ImGui::TextDisabled("Its list of installed mods has %s. Installed Mods (below) shows them.",
				game_.mods->mods.size() == 1 ? "1 entry" : (std::to_string(game_.mods->mods.size()) + " entries").c_str());
		else if (game_.modsError.empty())
			ImGui::TextDisabled("No mod installed here is on a list yet.");
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
		if (trackMods_ && !game_.modsError.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
			ImGui::TextUnformatted(("The list of installed mods doesn't read, so installing leaves it as it is: " + game_.modsError).c_str());
			ImGui::PopStyleColor();
		}
		if (!installedNote_.empty() && modProblem_.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
			ImGui::TextUnformatted(installedNote_.c_str());
			ImGui::PopStyleColor();
			app::Checkbox("Install it anyway", &installAnyway_);
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
		if (!conflicts_.empty() && mod_)
		{
			drawConflicts();
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
		ImGui::Spacing();
		if (!trackMods_) ImGui::TextDisabled("The list of installed mods is off (Advanced Options): the mod isn't added to it.");
		else if (!game_.modsError.empty()) ImGui::TextDisabled("The list of installed mods doesn't read, so the mod isn't added to it.");
		else ImGui::TextDisabled("The mod goes on the game's list of installed mods, attribulina_mods.txt in GLOBAL, with a trace of what it "
			"changes in the database, so it can be uninstalled. If it changes what a mod on the list changed, installing stops before "
			"writing anything and asks what to do.");
		if (!installedNote_.empty())
		{
			ImGui::Spacing();
			ImGui::TextColored(kWarn, "%s", "It installs over the same mod, which the list has already (the first step says more).");
		}
	}

	void Installer::drawConflicts()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		std::string names;
		for (size_t i = 0; i < conflicts_.size(); ++i)
			names += (i == 0 ? "" : i + 1 == conflicts_.size() ? " and " : ", ") + conflicts_[i].mod.nameAndVersion();
		ImGui::PushFont(nullptr, style.FontSizeBase * 1.2f);
		ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
		ImGui::TextUnformatted((mod_->title + " changes what " + (conflicts_.size() == 1 ? names : std::to_string(conflicts_.size()) + " installed mods") +
			" changed").c_str());
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::Spacing();
		ImGui::TextUnformatted("Nothing has been written yet. Tick the mods to uninstall first: uninstalling one sets back what it changed in the "
			"game's database, and files it copied into the game's folder stay. Or install it anyway: where both change the same thing, this "
			"mod's value is the one that stays, and uninstalling either of them later keeps the other's changes.");
		ImGui::Spacing();
		for (size_t i = 0; i < conflicts_.size(); ++i)
		{
			Conflict& c = conflicts_[i];
			ImGui::PushID(int(i));
			ImGui::BeginDisabled(!c.cannot.empty());
			app::Checkbox(("Uninstall " + c.mod.nameAndVersion() + " first").c_str(), &c.uninstall);
			ImGui::EndDisabled();
			ImGui::Indent();
			if (!c.mod.installed.empty()) ImGui::TextDisabled("Installed %s.", c.mod.installed.c_str());
			ImGui::TextDisabled("%s %s.", mod_->title.c_str(), c.text.c_str());
			if (!c.cannot.empty()) ImGui::TextColored(kError, "%s", ("It can't be ticked: " + c.cannot).c_str());
			else if (!c.withText.empty()) ImGui::TextColored(kWarn, "%s", ("Ticked, more goes: " + c.withText).c_str());
			ImGui::Unindent();
			ImGui::PopID();
		}
		if (!conflictError_.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, kError);
			ImGui::TextUnformatted(conflictError_.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
		const bool ticked = std::any_of(conflicts_.begin(), conflicts_.end(), [](const Conflict& c) { return c.uninstall && c.cannot.empty(); });
		ImGui::BeginDisabled(!ticked || busy_);
		if (ImGui::Button("Uninstall the Ticked Mods, Then Install")) uninstallTickedAndInstall();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(busy_);
		if (ImGui::Button("Install Anyway")) installOverConflicts();
		ImGui::EndDisabled();
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
		const bool done = finished();
		const int count = done ? 1 : 3;
		drawAdvanced();
		ImGui::SameLine();
		ImGui::BeginDisabled(!game_.ok || busy_);
		if (ImGui::Button("Installed Mods...")) showMods_ = true;
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", game_.ok ? "The mods installed in this game: uninstall one, or take one off the list."
				: "Choose the game's folder first: no mod is needed for this.");
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - (size.x * float(count) + style.ItemSpacing.x * float(count - 1)));
		if (done)
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
		ImGui::BeginDisabled(busy_ && (work_ == Work::Installing || work_ == Work::Uninstalling));
		if (ImGui::Button("Cancel", size)) host_.quit();
		ImGui::EndDisabled();
	}

	void Installer::drawAdvanced()
	{
		if (ImGui::Button("Advanced Options")) ImGui::OpenPopup("##advanced");
		if (!ImGui::BeginPopup("##advanced")) return;
		const float em = ImGui::GetFontSize();
		ImGui::PushTextWrapPos(em * 26);
		ImGui::BeginDisabled(busy_);
		bool track = trackMods_;
		if (app::Checkbox("Keep a list of installed mods", &track)) setTrackMods(track);
		ImGui::EndDisabled();
		ImGui::TextDisabled("On (the default), each mod installed goes on the game's list, attribulina_mods.txt in its GLOBAL folder, with "
			"a trace of what it changed in attribulina_traces beside it. A mod the list has already asks before it is installed again, and "
			"one that changes what a listed mod changed asks whether to uninstall that one first. The game reads neither. The Installed "
			"Mods button shows the list and uninstalls a mod.");
		ImGui::PopTextWrapPos();
		ImGui::EndPopup();
	}

	void Installer::drawModsWindow()
	{
		const char* name = "Installed Mods";
		if (showMods_ && !ImGui::IsPopupOpen(name))
		{
			ImGui::OpenPopup(name);
			modsView_.forget();
			confirm_ = Confirm::None;
			uninstallAsk_.reset();
			modsMessage_.clear();
		}
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.85f, vp->WorkSize.y * 0.8f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(name, &showMods_)) return;
		const std::string path = game_.ok ? ModListPath(game_.folder.global) : std::string();
		ImGui::PushTextWrapPos(0.0f);
		if (!game_.ok) ImGui::TextUnformatted("Choose the game's folder first.");
		else if (!game_.modsError.empty()) ImGui::TextColored(kWarn, "%s doesn't read: %s", path.c_str(), game_.modsError.c_str());
		else ImGui::TextDisabled("The mods installed in %s, oldest first, from %s.", game_.target.name().c_str(), path.c_str());
		if (!trackMods_) ImGui::TextColored(kWarn, "%s", "The list is off (Advanced Options): installing doesn't add to it.");
		ImGui::PopTextWrapPos();
		ImGui::Spacing();

		const int picked = modsView_.picked();
		const bool hasPick = picked >= 0 && size_t(picked) < game_.traced.size();
		if (!hasPick) confirm_ = Confirm::None;
		// Below the table: what the last command did, a question, the buttons.
		const float line = ImGui::GetTextLineHeightWithSpacing();
		float below = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
		if (!modsMessage_.empty() || (busy_ && (work_ == Work::Uninstalling || work_ == Work::Planning))) below += line * 3;
		if (confirm_ != Confirm::None) below += line * 3;
		if (confirm_ == Confirm::Uninstall && uninstallAsk_ && !uninstallAsk_->dependents.empty()) below += line * 4;
		if (game_.ok && game_.modsError.empty())
			modsView_.draw(game_.traced, game_.mods.has_value(), std::max(ImGui::GetFontSize() * 6, ImGui::GetContentRegionAvail().y - below));
		ImGui::Spacing();
		ImGui::PushTextWrapPos(0.0f);
		if (busy_ && work_ == Work::Uninstalling) ImGui::TextUnformatted("Uninstalling...");
		else if (busy_ && work_ == Work::Planning) ImGui::TextUnformatted("Working out which mods go with it (those that build on it)...");
		else if (!modsMessage_.empty()) ImGui::TextColored(ColourOf(modsMessageLevel_), "%s", modsMessage_.c_str());
		const TracedMod* row = hasPick ? &game_.traced[size_t(picked)] : nullptr;
		// The question is about the mod it was asked for.
		if (confirm_ == Confirm::Uninstall && (!row || !uninstallAsk_ || !SameEntry(uninstallAsk_->mod, row->mod)))
		{
			confirm_ = Confirm::None;
			uninstallAsk_.reset();
		}
		if (confirm_ != Confirm::None && row)
		{
			const std::string mod = row->mod.nameAndVersion();
			if (confirm_ == Confirm::Uninstall)
			{
				ImGui::TextColored(kWarn, "Uninstall %s? This sets back what it changed in the game's database and saves the files "
					"(each file's first original stays as .bak). Files it copied into the game's folder stay.", mod.c_str());
				if (!uninstallAsk_->dependents.empty())
					ImGui::TextColored(kError, "Warning: %s", uninstallAsk_->dependents.c_str());
			}
			else
			{
				ImGui::TextColored(kWarn, "Take %s off the list? The game's files stay as they are, and it can't be uninstalled "
					"from here after.", mod.c_str());
			}
		}
		ImGui::PopTextWrapPos();
		if (confirm_ != Confirm::None && row)
		{
			const bool more = confirm_ == Confirm::Uninstall && uninstallAsk_->all.size() > 1;
			if (ImGui::Button(confirm_ == Confirm::Uninstall ? (more ? "Uninstall All of Them" : "Uninstall") : "Remove from List"))
			{
				const Confirm what = confirm_;
				confirm_ = Confirm::None;
				if (what == Confirm::Uninstall) uninstallAsked();
				else forgetMod(size_t(picked));
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel##confirm"))
			{
				confirm_ = Confirm::None;
				uninstallAsk_.reset();
			}
		}
		else
		{
			ImGui::BeginDisabled(!hasPick || !canUninstall(size_t(std::max(picked, 0))));
			if (ImGui::Button("Uninstall...")) askUninstall(size_t(picked));
			ImGui::EndDisabled();
			if (hasPick && !row->trace && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", ("It can't be uninstalled: " + row->problem + ".").c_str());
			ImGui::SameLine();
			ImGui::BeginDisabled(!hasPick || busy_);
			if (ImGui::Button("Remove from List..."))
			{
				confirm_ = Confirm::Forget;
				modsMessage_.clear();
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Takes the mod off the list and changes nothing else: for a mod put back by hand (its .bak files).");
		}
		ImGui::SameLine();
		const bool working = busy_ && (work_ == Work::Uninstalling || work_ == Work::Planning);
		ImGui::BeginDisabled(working);
		const bool close = ImGui::Button("Close");
		ImGui::EndDisabled();
		if (close || (!working && ImGui::IsKeyPressed(ImGuiKey_Escape) && confirm_ == Confirm::None))
		{
			showMods_ = false;
			ImGui::CloseCurrentPopup();
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			confirm_ = Confirm::None;
			uninstallAsk_.reset();
		}
		ImGui::EndPopup();
	}

} // namespace installer
