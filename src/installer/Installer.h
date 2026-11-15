#pragma once

// The mod installer: a small program beside the editor that installs one mod
// into a game, step by step. It asks for the mod (a mod script or an nfsms
// script; a path on the command line counts) and the game's folder, whose
// GLOBAL folder holds the database, on one page (the game loads without a mod,
// so its list of installed mods can be seen and changed alone), then the
// mod's options, and installs it:
// the script runs on the game's packs, and the ones it changed are saved with
// the copies the game's memory files hold of them, each file's first
// original kept as .bak (core/Install.h). nfsms scripts always run NFS-VltEd
// compatible, as the mods were made for NFS-VltEd (the user's call).
//
// The window shows the step, a status line and, on request, the Log, which
// ends with how the installation went. Every run also writes a timestamped
// log file into the folder the program runs in. Each mod installed goes on
// the game's list of installed mods (core/ModList.h, attribulina_mods.txt
// in GLOBAL) with the trace of what it changed (core/ModTrace.h), and
// installing one the list has already asks first. A mod that changes what
// mods on the list changed stops before anything is written and asks:
// uninstall those first, or install over them. The Installed Mods button
// shows the list; its window uninstalls a mod (with the mods that build on
// it, which the confirmation lists first) or takes it off the list. Advanced
// Options turns the list off. Platform independent: a Host (app/Host.h)
// gives the window and the dialogs. doc/installer.md.

#include "app/CodeEditor.h"
#include "app/Host.h"
#include "app/InstalledMods.h"
#include "app/ScriptSetup.h"

#include "core/Install.h"
#include "core/LogFile.h"
#include "core/ModTrace.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace installer
{

	inline constexpr const char* kTitle = "Attribulina Mod Installer";
	inline constexpr const char* kSettingsFileName = "mod_installer.ini";

	enum class Level : uint8_t
	{
		Info, Warning, Error, Success
	};

	struct LogLine
	{
		Level level;
		std::string text;
	};

	enum class Step : uint8_t
	{
		Mod,     // the mod and the game's folder, on one page
		Options,
		Install
	};

	// How the installation went.
	enum class Outcome : uint8_t
	{
		None,      // not installed yet
		Succeeded,
		Partly,    // installed, but lines of its nfsms script failed
		Failed,    // nothing was saved (or saving failed halfway)
	};

	struct Paths
	{
		std::string exeDir;  // data/ is looked for beside the program
		std::string dataDir; // instead of <exeDir>/data
		// The user's folder (core/UserFolder.h): user_keys.txt, the editor's
		// attribulina.ini (for its theme) and mod_installer.ini. Empty: none.
		std::string userDir;
		// Where the timestamped log goes: the first of these that takes it (the
		// folder the program runs in, then its own, then the user's). Empty:
		// no log file.
		std::vector<std::string> logDirs;
	};

	// A mod on the list that changes some of what the mod being installed
	// changes. `uninstall`: the user's tick, uninstall it first.
	struct Conflict
	{
		attrib::InstalledMod mod;
		std::string text; // "changes 3 things Shifted 1.2 changed: ..."
		bool uninstall = true;
		// The mods that build on it, which go with it (attrib::PlanUninstall),
		// and what to tell the user about them.
		std::vector<attrib::InstalledMod> with;
		std::string withText;
		std::string cannot; // why it can't be uninstalled: no tick then
	};

	// What uninstalling a mod of the list takes, worked out before the
	// confirmation asks (attrib::PlanUninstall).
	struct UninstallAsk
	{
		attrib::InstalledMod mod;               // the one asked for
		std::vector<attrib::InstalledMod> all;  // it and the ones that build on it, oldest first
		std::string dependents;                 // attrib::DependentsText; empty when it goes alone
		bool around = false;
		std::string error;                      // why it can't be uninstalled
	};

	class Installer
	{
	public:
		explicit Installer(app::Host& host);
		~Installer(); // waits for the work in progress
		Installer(const Installer&) = delete;
		Installer& operator=(const Installer&) = delete;

		// Once, after ImGui::CreateContext(): style, fonts, the log file and
		// the folders chosen before. dpiScale 1.0 = 96 DPI.
		void setup(float dpiScale, const Paths& paths);
		// One frame, between ImGui::NewFrame() and ImGui::Render().
		void frame();
		// The window's close button: false while installing (the status says
		// to wait), as closing then could leave the game's files half written.
		bool requestClose();

		// Commands. The buttons call these, and so do the host (the command
		// line, files dropped on the window) and tests.
		// The command line: a folder is the game's, a file the mod.
		void openArguments(const std::vector<std::string>& args);
		// A file is the mod, a folder the game's.
		void dropped(const std::string& path);
		// Reads the mod; the folder entered for the game loads too, if it hasn't.
		bool openMod(std::string path); // by value: callers pass the window's own strings
		// Finds the database in the game's folder and reads it (in the
		// background), as the game and platform its files say unless `target`
		// names them. No mod is needed: the list of installed mods shows then.
		void chooseGame(std::string folder, std::optional<attrib::Target> target = std::nullopt);
		// Runs the mod and saves what it changed (in the background).
		void install();
		bool canInstall() const;
		// When installing stopped at mods on the list that change the same
		// things: those, with the user's ticks. Uninstalling the ticked ones
		// first, or installing over all of them, runs the mod again.
		std::vector<Conflict>& conflicts()
		{
			return conflicts_;
		}
		const std::string& conflictError() const
		{
			return conflictError_;
		}
		void uninstallTickedAndInstall();
		void installOverConflicts();
		// Starts over with another mod, the game's folder kept. Once a mod is
		// installed (or failed to), the steps before are locked, and this is
		// the only way back: they show what was installed.
		void installAnother();
		bool finished() const; // the Install step, with how it went
		void goTo(Step s);
		bool canGoTo(Step s) const;

		// The work (reading the game's files, installing) runs on a thread of
		// its own so the window keeps drawing. Off, it runs in place (tests).
		void setBackground(bool on)
		{
			background_ = on;
		}
		bool busy() const
		{
			return busy_;
		}
		// Waits for the work in progress and takes in its results.
		void wait();

		Step step() const
		{
			return step_;
		}
		Outcome outcome() const
		{
			return outcome_;
		}
		std::string status() const;
		Level statusLevel() const;
		std::vector<LogLine> log() const;
		const std::string& logFilePath() const
		{
			return logFile_.path();
		}
		const attrib::install::Mod* mod() const
		{
			return mod_.get();
		}
		const std::string& modError() const
		{
			return modError_;
		}
		// What reading the game's folder found.
		struct FoundGame
		{
			bool ok = false;
			std::string error;
			attrib::install::GameFolder folder;
			attrib::Detection detection;
			attrib::Target target;
			bool chosen = false; // the target was picked, not read from the files
			std::vector<std::string> packs;  // their file names
			std::vector<std::string> copies; // "GlobalMemoryFile.bin holds a copy of GLOBAL\ATTRIBUTES.BIN"
			size_t warnings = 0;
			// Its attribulina_mods.txt: none when there is no file, or when it
			// doesn't read (modsError says why).
			std::optional<attrib::ModList> mods;
			std::string modsError;
			std::vector<attrib::TracedMod> traced; // the list's mods with their traces
		};
		const FoundGame& game() const
		{
			return game_;
		}
		bool gameLoaded() const
		{
			return game_.ok;
		}
		// Why the mod can't go into this game, or what to know first.
		const std::string& modProblem() const
		{
			return modProblem_;
		}
		const std::string& modWarning() const
		{
			return modWarning_;
		}
		// The game's list has the mod already (this version or another): what
		// to know, and the user's go-ahead, which the steps after Game wait for.
		const std::string& installedNote() const
		{
			return installedNote_;
		}
		bool& installAnyway()
		{
			return installAnyway_;
		}
		// Advanced Options: keep the list of installed mods (on by default,
		// TrackMods in mod_installer.ini). Off, installing neither reads it
		// nor adds to it.
		bool trackMods() const
		{
			return trackMods_;
		}
		void setTrackMods(bool on);
		// The Installed Mods window, with the game's list.
		void showInstalledMods()
		{
			showMods_ = true;
		}
		// The window's commands, on the game's list (game().traced). Asking to
		// uninstall works out, in the background, which mods go with it (those
		// that build on it); uninstallAsk() has it, and the window's
		// confirmation lists them. Uninstalling then sets back what they
		// changed in the database (in the background) and saves; forget takes
		// a mod off the list and changes nothing else.
		bool canUninstall(size_t index) const;
		void askUninstall(size_t index);
		const std::optional<UninstallAsk>& uninstallAsk() const
		{
			return uninstallAsk_;
		}
		void uninstallAsked(); // the confirmation's Uninstall
		void forgetMod(size_t index);
		// How the last of those went, for the window.
		const std::string& modsMessage() const
		{
			return modsMessage_;
		}
		Level modsMessageLevel() const
		{
			return modsMessageLevel_;
		}
		struct Result
		{
			Outcome outcome = Outcome::None;
			std::string headline, detail;
			std::vector<std::string> files; // written and removed, from the game's folder
			std::optional<attrib::ModList> mods; // the list, the mod added
			std::vector<attrib::TracedMod> traced;
			// Outcome None with these: it stopped before writing anything, as
			// mods on the list change the same things (or uninstalling the ones
			// ticked failed: conflictError).
			std::vector<Conflict> conflicts;
			std::string conflictError;
		};
		const Result& result() const
		{
			return result_;
		}
		// A mod script's option values by id; an nfsms setup's choices.
		std::map<std::string, std::string>& options()
		{
			return options_;
		}
		attrib::nfsms::Choices& choices()
		{
			return choices_;
		}
		bool& showLog()
		{
			return showLog_;
		}
		const std::vector<std::string>& recentGames() const
		{
			return recentGames_;
		}

	private:
		enum class Work : uint8_t
		{
			None, Loading, Installing, Planning, Uninstalling
		};
		// What installing does first: the mods on the list to uninstall, and
		// whether it goes over the conflicts it finds.
		struct Plan
		{
			std::vector<attrib::InstalledMod> uninstall;
			bool overConflicts = false;
			std::vector<Conflict> conflicts; // as they were shown, for when uninstalling fails
		};
		struct ModsDone
		{
			std::optional<attrib::ModList> mods;
			std::vector<attrib::TracedMod> traced;
			std::string message;
			Level level = Level::Info;
			std::optional<UninstallAsk> ask; // planning done: the confirmation's
		};

		// A line for the Log and the log file. `levelInText`: the text names its
		// level already (a script's "x.amod:3:1: error: ..."), so the file
		// doesn't put it in front again.
		void post(Level level, std::string text, bool levelInText = false);
		bool refuseFinished(); // finished: says to use Install Another Mod
		void setStatus(Level level, std::string text);
		void startWork(Work kind, std::function<void()> job);
		void harvest(); // after the work: its results into the window's state
		void refreshModChecks();
		void ensureData(); // on the worker
		std::string dataDir() const;
		std::string userKeysFile() const;
		// On the worker: a fresh database of the folder's packs.
		std::unique_ptr<attrib::Database> loadDatabase(const attrib::install::GameFolder& f, const attrib::Target& target,
			std::vector<std::string>& movedFrom, FoundGame* found);
		void loadGameJob(const attrib::install::GameFolder& f, std::optional<attrib::Target> target);
		void startInstall(Plan plan);
		void installJob(const attrib::install::Mod& mod, const attrib::install::ModSettings& settings, const FoundGame& game, bool track,
			const Plan& plan);
		// The mod's entry on the list, before it is installed: all but how it went and its files.
		attrib::InstalledMod entryFor(const attrib::install::Mod& mod, const attrib::install::ModSettings& settings) const;
		// On the worker: `u` onto the game's list (r.mods, r.traced).
		void writeList(const FoundGame& game, const attrib::ModListUpdate& u, Result& r);
		// On the worker: the list as its file has it, and the rows of `mods`.
		std::vector<attrib::TracedMod> listNow(const FoundGame& game, const std::vector<attrib::InstalledMod>& mods,
			std::vector<size_t>& at) const;
		void planJob(const FoundGame& game, const attrib::InstalledMod& which);
		void uninstallJob(const FoundGame& game, const UninstallAsk& ask);
		// The mods that go with each conflict, on the database before the mod.
		void planConflicts(const attrib::Database& before, const std::vector<attrib::TracedMod>& rows, std::vector<Conflict>& found);
		void reloadIfStale(const FoundGame& game); // on the worker
		void resetOptions();
		void loadSettings();
		void saveSettings();
		void rememberGame(const std::string& folder);

		void drawSteps();
		void drawModPage(); // the mod and the game
		void drawGamePart();
		void drawOptionsPage();
		void drawInstallPage();
		void drawConflicts();
		void drawStatus();
		void drawLog(float height);
		void drawButtons();
		void drawAdvanced();
		void drawModsWindow();

		app::Host& host_;
		Paths paths_;

		// The window's (the UI thread's).
		Step step_ = Step::Mod;
		std::shared_ptr<const attrib::install::Mod> mod_;
		std::string modInput_, modError_;
		std::string gameInput_;
		std::vector<std::string> recentGames_;
		std::map<std::string, std::string> options_;
		attrib::nfsms::Choices choices_;
		app::SetupImages images_;
		FoundGame game_;
		std::string modProblem_, modWarning_;
		std::string installedNote_;
		bool installAnyway_ = false;
		std::vector<Conflict> conflicts_;
		std::string conflictError_;
		std::string modsMessage_;
		Level modsMessageLevel_ = Level::Info;
		std::optional<UninstallAsk> uninstallAsk_;
		enum class Confirm : uint8_t
		{
			None, Uninstall, Forget
		} confirm_ = Confirm::None;
		bool trackMods_ = true;
		bool showMods_ = false;
		bool loadedFirst_ = false; // the folder chosen last loads once, at the first frame
		app::ModListView modsView_;
		Result result_;
		Outcome outcome_ = Outcome::None;
		bool showLog_ = false;
		app::LogView logView_;
		std::string logText_;
		std::vector<uint8_t> logLevels_;
		uint64_t logShown_ = ~uint64_t(0);
		Work work_ = Work::None;

		// Shared with the worker, under m_.
		mutable std::mutex m_;
		std::deque<LogLine> log_;
		size_t logDropped_ = 0; // bytes dropped from the front since the Log was drawn
		uint64_t logVersion_ = 0;
		attrib::LogFile logFile_;
		Level statusLevel_ = Level::Info;
		std::string status_;
		std::optional<FoundGame> gameDone_;  // what the worker left for harvest()
		std::optional<Result> installDone_;
		std::optional<ModsDone> modsDone_;

		// The worker's while busy_, the window's otherwise.
		std::atomic<bool> busy_{ false };
		bool background_ = true;
		std::thread worker_;
		std::optional<attrib::install::DataFiles> data_;
		std::unique_ptr<attrib::Database> db_;
		std::vector<std::string> movedFrom_;
		bool dbStale_ = false; // a script ran on it: installing again reads the files again
	};

} // namespace installer
