#include "Install.h"

#include "ClassList.h"
#include "Folder.h"
#include "Gameplay.h"
#include "Schema.h"
#include "VaultLib.h"
#include "Writer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace attrib::install
{

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
		std::string Lower(std::string s)
		{
			for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		std::vector<std::string> TextFilesIn(const fs::path& dir)
		{
			std::vector<std::string> v;
			std::error_code ec;
			for (const auto& e : fs::directory_iterator(dir, ec))
				if (e.is_regular_file(ec) && Lower(ToUtf8(e.path().extension())) == ".txt") v.push_back(ToUtf8(e.path()));
			std::sort(v.begin(), v.end());
			return v;
		}

		// The entry of `dir` named `name` in any case (the games were made on
		// Windows: GLOBAL, Global, ATTRIBUTES.BIN, attributes.bin).
		std::optional<fs::path> FindIn(const fs::path& dir, const std::string& name, bool folder)
		{
			std::error_code ec;
			if (folder ? fs::is_directory(dir / name, ec) : fs::is_regular_file(dir / name, ec)) return dir / name;
			for (const auto& e : fs::directory_iterator(dir, ec))
			{
				if (Lower(ToUtf8(e.path().filename())) != name) continue;
				if (folder ? e.is_directory(ec) : e.is_regular_file(ec)) return e.path();
			}
			return std::nullopt;
		}

		// NFS-VltEd's game codes, as the games' names.
		std::string CodeName(const std::string& code)
		{
			const std::string c = Lower(code);
			if (c == "mw") return "Most Wanted";
			if (c == "c") return "Carbon";
			if (c == "ps") return "Pro Street";
			if (c == "uc") return "Undercover";
			if (c == "w") return "World";
			if (c == "wb") return "World's betas";
			return code;
		}

		std::string Joined(const std::vector<std::string>& v, const std::string& sep)
		{
			std::string s;
			for (const std::string& x : v) s += (s.empty() ? "" : sep) + x;
			return s;
		}

		// Keeps a file's first original as <file>.bak.
		void KeepOriginal(const std::string& file, const std::function<void(SaveNote::Kind, std::string)>& say)
		{
			std::error_code ec;
			const fs::path t = FromUtf8(file);
			fs::path bak = t;
			bak += ".bak";
			if (!fs::exists(t, ec) || fs::exists(bak, ec)) return;
			fs::copy_file(t, bak, ec);
			if (!ec) say(SaveNote::Kind::Backup, "kept the original as " + ToUtf8(bak));
		}
	} // namespace

	// ------------------------------------------------------------- data files ---

	DataFiles DataFiles::Load(const std::string& dataDir)
	{
		DataFiles d;
		std::error_code ec;
		const fs::path dir = FromUtf8(dataDir);
		if (dataDir.empty() || !fs::is_directory(dir / "schema", ec))
		{
			d.problems.push_back("no data folder at " + (dataDir.empty() ? std::string("(none)") : dataDir) +
				": names show as hashes and the game's structs as bytes");
			return d;
		}
		for (const std::string& f : TextFilesIn(dir / "schema"))
		{
			try
			{
				d.schemas.push_back(TypeSchema::Load(f));
			}
			catch (const std::exception& e)
			{
				d.problems.push_back(std::string("type schema: ") + e.what());
			}
		}
		for (const std::string& f : TextFilesIn(dir / "classes"))
		{
			try
			{
				d.classLists.push_back(ClassList::Load(f));
			}
			catch (const std::exception& e)
			{
				d.problems.push_back(std::string("class list: ") + e.what());
			}
		}
		for (const std::string& f : TextFilesIn(dir / "nfsms"))
		{
			try
			{
				d.nfsmsWords.push_back(ns::PropertyNames::Load(f));
			}
			catch (const std::exception& e)
			{
				d.problems.push_back(e.what());
			}
		}
		for (const std::string& f : TextFilesIn(dir / "messages"))
		{
			try
			{
				for (const MessageDef& m : LoadMessages(f)) d.messageNames.push_back(m.name);
			}
			catch (const std::exception& e)
			{
				d.problems.push_back(std::string("messages: ") + e.what());
			}
		}
		if (fs::is_regular_file(dir / "attrib_keys.txt", ec)) d.keysFile = ToUtf8(dir / "attrib_keys.txt");
		if (fs::is_regular_file(dir / "bin_keys.txt", ec)) d.binKeysFile = ToUtf8(dir / "bin_keys.txt");
		return d;
	}

	std::unique_ptr<Database> DataFiles::newDatabase(const std::string& userKeys) const
	{
		auto db = std::make_unique<Database>();
		for (const auto& s : schemas) db->addTypeSchema(s);
		for (const auto& l : classLists) db->addClassList(l);
		if (!keysFile.empty())
		{
			db->names().loadFile(keysFile);
			db->binNames().loadFile(keysFile);
		}
		if (!binKeysFile.empty()) db->binNames().loadFile(binKeysFile);
		if (std::error_code ec; !userKeys.empty() && fs::exists(FromUtf8(userKeys), ec))
		{
			db->names().loadFile(userKeys);
			db->binNames().loadFile(userKeys);
		}
		for (const std::string& n : messageNames) db->names().add(n);
		return db;
	}

	// ------------------------------------------------------------ game folder ---

	std::optional<GameFolder> FindGameFolder(const std::string& chosen, std::string* why)
	{
		auto fail = [&](std::string w) -> std::optional<GameFolder>
		{
			if (why) *why = std::move(w);
			return std::nullopt;
		};
		if (chosen.empty()) return fail("Choose the game's folder.");
		std::error_code ec;
		fs::path p = fs::absolute(FromUtf8(chosen), ec).lexically_normal();
		if (ec) p = FromUtf8(chosen);
		if (!p.has_filename() && p.has_relative_path()) p = p.parent_path();
		if (!fs::exists(p, ec)) return fail("There is no folder " + chosen + ".");
		// attributes.bin itself, or a pack beside it.
		if (fs::is_regular_file(p, ec)) p = p.parent_path();
		GameFolder g;
		if (const auto attrs = Lower(ToUtf8(p.filename())) == "global" ? FindIn(p, "attributes.bin", false) : std::nullopt)
		{
			g.game = ToUtf8(p.parent_path());
			g.global = ToUtf8(p);
			g.database = ToUtf8(*attrs);
			return g;
		}
		const std::optional<fs::path> global = FindIn(p, "global", true);
		if (!global) return fail(ToUtf8(p) + " has no GLOBAL folder. Choose the folder the game is installed in, the one with its .exe.");
		const std::optional<fs::path> attrs = FindIn(*global, "attributes.bin", false);
		if (!attrs) return fail(ToUtf8(*global) + " has no attributes.bin: this isn't a game whose database Attribulina reads.");
		g.game = ToUtf8(p);
		g.global = ToUtf8(*global);
		g.database = ToUtf8(*attrs);
		return g;
	}

	// -------------------------------------------------------------------- mod ---

	bool Mod::hasOptions() const
	{
		return isNfsms ? nfsms.hasSetup() : !amod.options().empty();
	}

	Mod ReadMod(const std::string& path)
	{
		const fs::path p = FromUtf8(path);
		std::ifstream in(p, std::ios::binary);
		if (!in) throw std::runtime_error("can't read " + path);
		std::ostringstream s;
		s << in.rdbuf();
		Mod m;
		m.path = path;
		const fs::path folder = p.parent_path();
		m.isNfsms = Lower(ToUtf8(p.extension())) == ns::kExtension;
		if (m.isNfsms)
		{
			m.nfsms = ns::Parse(s.str(), ToUtf8(p.filename()), ToUtf8(folder));
			m.title = ToUtf8(p.stem());
			std::vector<std::string> lines;
			for (const ns::SetupItem& item : m.nfsms.setup)
				if (item.kind == ns::SetupItem::Kind::Text) lines.push_back(item.text);
			while (!lines.empty() && lines.back().find_first_not_of(" \t") == std::string::npos) lines.pop_back();
			m.description = Joined(lines, "\n");
			return m;
		}
		m.amod = sc::Parse(s.str(), ToUtf8(p.filename()), folder.empty() ? std::string(".") : ToUtf8(folder));
		sc::LoadIncludes(m.amod);
		m.title = m.amod.meta("title").empty() ? ToUtf8(p.stem()) : m.amod.meta("title");
		m.author = m.amod.meta("author");
		m.version = m.amod.meta("version");
		m.description = m.amod.meta("description");
		return m;
	}

	std::string ModProblem(const Mod& mod, const Database& db)
	{
		const Target here = db.target();
		if (mod.isNfsms)
		{
			if (const std::string why = ns::Incompatibility(db); !why.empty()) return why;
			const std::string mine = ns::GameCode(db);
			for (const ns::Command& c : mod.nfsms.commands)
			{
				if (c.name != "game" || c.args.empty() || mine.empty()) continue;
				if (std::find(c.args.begin(), c.args.end(), mine) != c.args.end()) continue;
				std::vector<std::string> names;
				for (const std::string& g : c.args) names.push_back(CodeName(g));
				return "This mod is for " + Joined(names, ", ") + ", and the folder holds " + InfoOf(here.game).name + ".";
			}
			return {};
		}
		const std::string game = mod.amod.meta("game");
		const std::optional<ScriptTarget> written = ParseScriptTarget(game);
		if (written && here.known() && written->game != here.game)
			return std::string("This mod is for ") + InfoOf(written->game).name + ", and the folder holds " + InfoOf(here.game).name + ".";
		return {};
	}

	std::string ModWarning(const Mod& mod, const Database& db)
	{
		if (mod.isNfsms) return {};
		const std::string game = mod.amod.meta("game");
		if (game.empty()) return {};
		const std::optional<ScriptTarget> written = ParseScriptTarget(game);
		if (!written) return "The mod names its game as " + game + ", which isn't a game and platform Attribulina knows.";
		if (db.target().known() && written->game == db.target().game && !written->fits(db.target()))
			return "The mod was written for " + written->name() + "; this is " + db.target().name() +
				". Its values go in by the fields' types, so it should still fit.";
		return {};
	}

	// ------------------------------------------------------------------ packs ---

	void LoadPacks(Database& db, const std::string& path, std::vector<std::string>& movedFrom)
	{
		std::error_code ec;
		const fs::path p = FromUtf8(path);
		if (fs::is_directory(p, ec))
		{
			for (const FolderFile& f : FolderPacks(path))
			{
				if (!f.skipped.empty()) continue;
				LoadedPack& pk = db.loadPack(f.path);
				// OGVI's save of a World event vault, beside attributes.bin: saved
				// to gc.vaults as the game's event vault file, its own file removed.
				if (!f.saveAs.empty())
				{
					vaultlib::MoveToEventVaultFile(pk, f.saveAs);
					movedFrom.push_back(f.path);
				}
			}
			return;
		}
		if (!fs::exists(p, ec)) throw std::runtime_error("no file or folder " + path);
		const std::optional<std::string> attrs = DatabaseFileFor(path);
		std::error_code e2;
		if (attrs && !fs::equivalent(FromUtf8(*attrs), p, e2)) db.loadPack(*attrs);
		db.loadPack(path);
	}

	// -------------------------------------------------------------------- run ---

	sc::ApplyReport RunMod(Database& db, const Mod& mod, const ModSettings& settings,
		const std::vector<std::shared_ptr<const ns::PropertyNames>>& words)
	{
		if (!mod.isNfsms)
		{
			sc::ApplySettings st;
			st.options = settings.options;
			st.strict = settings.strict;
			return sc::Apply(db, mod.amod, st);
		}
		ns::RunSettings st;
		st.choices = settings.choices;
		st.strict = settings.strict;
		st.vltEd = settings.vltEd;
		st.names = ns::NamesFor(db, words);
		st.targetFolder = settings.gameFolder;
		st.doFiles = settings.doFiles;
		return ns::Run(db, mod.nfsms, st);
	}

	std::string MessageLine(const std::string& source, const sc::Message& m)
	{
		if (m.level == sc::Message::Level::Echo) return m.text;
		std::string s;
		// A line of an included script: that file and line.
		if (!m.file.empty())
		{
			s = m.file;
			if (m.fileLine) s += ":" + std::to_string(m.fileLine);
		}
		else
		{
			s = source;
			if (m.pos.line) s += ":" + std::to_string(m.pos.line) + ":" + std::to_string(m.pos.col);
		}
		s += m.level == sc::Message::Level::Error ? ": error: " : m.level == sc::Message::Level::Warning ? ": warning: " : ": note: ";
		return s + m.text;
	}

	// ----------------------------------------------------------------- saving ---

	Changes ChangesOf(const Database& db, std::vector<std::string> removed)
	{
		Changes c;
		for (const auto& pk : db.packs())
			if (pk->dirty) c.packs.push_back(pk.get());
		// A file made again after its vault was deleted stays.
		std::erase_if(removed, [&](const std::string& f)
		{
			return std::any_of(db.packs().begin(), db.packs().end(), [&](const auto& pk) { return pk->path == f; });
		});
		std::sort(removed.begin(), removed.end());
		removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
		c.removed = std::move(removed);
		return c;
	}

	SaveResult SaveChanges(const Database& db, const Changes& changes, const SaveSettings& settings,
		const std::function<void(const SaveNote&)>& note)
	{
		auto say = [&](SaveNote::Kind k, std::string text)
		{
			if (note) note(SaveNote{ k, std::move(text) });
		};
		SaveResult result;
		const std::string& outDir = settings.outDir;
		if (!outDir.empty())
		{
			std::error_code ec;
			fs::create_directories(FromUtf8(outDir), ec);
		}
		std::vector<SavedFile> saved;
		for (LoadedPack* p : changes.packs)
		{
			const std::string target = outDir.empty() ? p->path : ToUtf8(FromUtf8(outDir) / FromUtf8(p->path).filename());
			const std::optional<std::string> twin = PackTwin(target);
			if (outDir.empty() && settings.backup)
			{
				KeepOriginal(target, say);
				if (twin) KeepOriginal(*twin, say);
			}
			// A new event vault file's folder may not be there yet.
			if (std::error_code ec; !fs::exists(FromUtf8(target).parent_path(), ec)) fs::create_directories(FromUtf8(target).parent_path(), ec);
			const std::string source = p->path; // SavePack moves it to `target`
			for (SavedFile& f : SavePack(db, *p, target))
			{
				result.written.push_back(f.path);
				// With an outDir, the memory files that count are the ones the
				// game has beside the original.
				if (!outDir.empty()) f.path = ToUtf8(FromUtf8(source).parent_path() / FromUtf8(f.path).filename());
				saved.push_back(std::move(f));
			}
			say(SaveNote::Kind::Saved, "saved " + target + (twin ? " and " + ToUtf8(FromUtf8(*twin).filename()) : std::string()));
		}
		for (const std::string& f : changes.removed)
		{
			std::error_code ec;
			const fs::path t = FromUtf8(f);
			if (!outDir.empty() || !fs::exists(t, ec))
			{
				if (!outDir.empty()) say(SaveNote::Kind::Removed, "deleted the vault of " + f + " (left as it is: --out writes elsewhere)");
				continue;
			}
			fs::path bak = t;
			bak += ".bak";
			if (settings.backup && !fs::exists(bak, ec))
			{
				fs::rename(t, bak, ec);
				if (!ec) say(SaveNote::Kind::Removed, "removed " + f + ", kept as " + ToUtf8(bak));
			}
			else
			{
				fs::remove(t, ec);
				if (!ec) say(SaveNote::Kind::Removed, "removed " + f);
			}
			if (ec) throw std::runtime_error("could not remove " + f + ": " + ec.message());
			result.removed.push_back(f);
		}

		// The memory files beside the saved packs (core/MemoryFile.h): their
		// copies take the saved bytes, and those of removed files go.
		if (!settings.memoryFiles)
		{
			for (const SavedFile& s : saved)
				for (const MemoryFileCopy& c : MemoryFileCopiesOf(s.path))
					if (outDir.empty() ? !c.same : true)
						say(SaveNote::Kind::MemoryFileLeft, ToUtf8(FromUtf8(c.memoryFile).filename()) + " holds another copy of " + c.gamePath +
							", which the game reads in place of the file; it is left as it is");
			return result;
		}
		for (MemoryFileUpdate u : PlanMemoryFileUpdates(saved, outDir.empty() ? changes.removed : std::vector<std::string>()))
		{
			const std::string from = u.path;
			if (!outDir.empty()) u.path = ToUtf8(FromUtf8(outDir) / FromUtf8(u.path).filename());
			if (outDir.empty() && settings.backup) KeepOriginal(u.path, say);
			SaveMemoryFileUpdate(u);
			result.memoryFiles.push_back(u.path);
			std::string text = "updated " + u.path;
			if (!outDir.empty()) text += " (from " + from + ")";
			for (size_t i = 0; i < u.updated.size(); ++i) text += (i ? ", " : ": its copy of ") + u.updated[i];
			for (size_t i = 0; i < u.dropped.size(); ++i) text += (i ? ", " : u.updated.empty() ? ": dropped its copy of " : "; dropped its copy of ") + u.dropped[i];
			say(SaveNote::Kind::MemoryFile, text);
		}
		return result;
	}

	void RememberNames(const std::string& userKeys, const std::vector<std::string>& names)
	{
		if (names.empty() || userKeys.empty()) return;
		const fs::path file = FromUtf8(userKeys);
		std::set<std::string> known;
		bool newline = false; // a file edited by hand may end without one
		{
			std::ifstream in(file, std::ios::binary);
			for (std::string l; std::getline(in, l);)
			{
				newline = in.eof() && !l.empty();
				if (!l.empty() && l.back() == '\r') l.pop_back();
				known.insert(l);
			}
		}
		std::ofstream out(file, std::ios::binary | std::ios::app);
		for (const std::string& n : names)
			if (known.insert(n).second)
			{
				if (newline) out << "\n";
				newline = false;
				out << n << "\n";
			}
	}

} // namespace attrib::install
