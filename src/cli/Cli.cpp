#include "Cli.h"

#include "core/ClassList.h"
#include "core/Compression.h"
#include "core/Database.h"
#include "core/Folder.h"
#include "core/Gameplay.h"
#include "core/GenHeader.h"
#include "core/Hash.h"
#include "core/Nfsms.h"
#include "core/Script.h"
#include "core/VaultLib.h"
#include "core/Writer.h"
#include "core/Version.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace cli
{

	using namespace attrib;
	namespace sc = attrib::script;
	namespace ns = attrib::nfsms;
	namespace fs = std::filesystem;

	namespace
	{

		const char* kUsage = R"(attribcli: Attribulina on the command line

  attribcli apply  <database> [<script.amod|script.nfsms>...] [-e <statements>]... [options]
  attribcli check  <script.amod> [<database>] [--set id=value]...
  attribcli diff   <base> <database> [-o <out.amod>] [--no-was] [--class <name>]...
  attribcli export <database> <class>[/<collection>]... [-o <out.amod>]
                   [--children] [--field <name>]... [--exact] [--edit]
  attribcli get    <database> <class>/<collection> [<field>...]
  attribcli convert <database> <script.nfsms> [-o <out.amod>] [--vlted]
  attribcli list   <database> [<class>]
  attribcli header <database> <class> [-o <out.h>] [--types] [--no-setters]
  attribcli info   <database>
  attribcli repair <database> [--out <dir>] [--dry-run] [--no-backup]

A <database> is a game folder (with attributes.bin) or one pack file in it.
Its game and platform come from its files (the Vers value of its vaults);
--target <game>_<platform> sets them for files that don't say (Most Wanted's)
or to read them as another. Games: mw, carbon, prostreet, undercover, world,
nitro; platforms: pc, ps2, ps3, xbox, x360, gamecube, wii. A script's `game`
line names one of these, or <game>_all for every platform. nfsms scripts run
on every platform of a game NFS-VltEd edits (all but Nitro).
A script named - is read from standard input. Relative `include` paths start
from the including script's folder (for - and -e, the current folder).

apply:
  -e <statements>   statements to apply after the script files
  --set id=value    a value for a script option (also for check); for an nfsms
                    setup, --set box=true or --set group=choice
  --dry-run         apply in memory only and save nothing
  --strict          `was` mismatches, a script for another game or platform and
                    an array made longer than its definition allows are errors;
                    for nfsms, warnings are errors and the first failing line stops it
  --out <dir>       write the changed packs there instead of over the originals
  --compress <kind> save gameplay.lzc with jdlz (as retail), huff, raww (stored)
                    or rfpk (EA RefPack, which only Undercover's code reads)
  --no-backup       don't keep the first original as <file>.bak
  --vlted           read nfsms scripts as NFS-VltEd 4.6 does (doc/nfsms.md,
                    "NFS-VltEd compatible"): its value widths and quirks

diff: the script that turns <base> into <database>. --no-was leaves out the
old values, --class limits it to some classes.

export: `collection` statements that recreate collections (a class name
alone: every collection of it). --children adds what's under them, --field
only those fields, --exact makes applying it revert what it doesn't list,
--edit writes `edit` blocks instead (the collections must exist).

get: a collection's values as a script; with fields, those (inherited ones
too, marked).

header: the class's Attrib::Gen C++ header, for code that reads its
collections the way the game's own classes do. --types adds the game's structs
and enums its fields use, from the type schema; --no-setters leaves out Set_.

repair: saves what opening the database repaired, for files the community
Attribulator or OGVI saved (VaultLib): each vault takes its game's header back,
a class too short for its fields its size, array pads the game's values, and
OGVI's World event vaults go back to gc.vaults as the game's event files (the
files it put beside attributes.bin are removed). What can't be undone is a
warning. Only repaired packs are written; --out, --dry-run and --no-backup work
as for apply.

convert: an nfsms script (and the scripts it includes) as a mod script. Its
setup becomes options and `if` blocks. It is tried on a copy of the database,
which decides how values read; lines that fail come out as comments.
)";

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

		struct Failed
		{
			std::string message;
		};

		// Arguments after the command: positionals, and options with or without a value.
		struct Args
		{
			std::vector<std::string> positional;
			std::multimap<std::string, std::string> options;

			bool has(const std::string& o) const
			{
				return options.count(o) != 0;
			}
			std::string get(const std::string& o) const
			{
				auto it = options.find(o);
				return it == options.end() ? std::string() : it->second;
			}
			std::vector<std::string> all(const std::string& o) const
			{
				std::vector<std::string> v;
				for (auto [it, end] = options.equal_range(o); it != end; ++it) v.push_back(it->second);
				return v;
			}
		};

		Args Parse(const std::vector<std::string>& in, const std::vector<std::string>& withValue, const std::vector<std::string>& flags)
		{
			Args a;
			for (size_t i = 0; i < in.size(); ++i)
			{
				const std::string& s = in[i];
				if (s.size() > 1 && s[0] == '-' && s != "-")
				{
					if (std::find(withValue.begin(), withValue.end(), s) != withValue.end())
					{
						if (i + 1 >= in.size()) throw Failed{ s + " needs a value" };
						a.options.emplace(s, in[++i]);
					}
					else if (std::find(flags.begin(), flags.end(), s) != flags.end())
					{
						a.options.emplace(s, "");
					}
					else
					{
						throw Failed{ "unknown option " + s };
					}
				}
				else
				{
					a.positional.push_back(s);
				}
			}
			return a;
		}

		class Tool
		{
		public:
			Tool(std::ostream& out, std::ostream& err, const Env& env) : out_(out), err_(err), env_(env)
			{
				findData();
			}

			int run(const std::vector<std::string>& args)
			{
				if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h")
				{
					out_ << Version::GetAppVersionString() << '\n';
					out_ << kUsage;
					return args.empty() ? 2 : 0;
				}

				if (args[0] == "version" || args[0] == "--version" || args[0] == "-v")
				{
					out_ << Version::GetAppVersionString() << '\n';
					return 0;
				}

				const std::string cmd = args[0];
				std::vector<std::string> rest;
				try
				{
					// --target, for any command that reads a database.
					for (size_t i = 1; i < args.size(); ++i)
					{
						if (args[i] != "--target")
						{
							rest.push_back(args[i]);
							continue;
						}
						if (i + 1 >= args.size()) throw Failed{ "--target needs a game and platform, such as prostreet_ps2" };
						target_ = ParseTarget(args[++i]);
						if (!target_) throw Failed{ "unknown --target " + args[i] + ": it names a game and a platform, such as prostreet_pc or prostreet_ps2" };
					}
					if (cmd == "apply") return apply(rest);
					if (cmd == "repair") return repair(rest);
					if (cmd == "check") return check(rest);
					if (cmd == "diff") return diff(rest);
					if (cmd == "export") return exportCmd(rest);
					if (cmd == "get") return get(rest);
					if (cmd == "list") return list(rest);
					if (cmd == "info") return info(rest);
					if (cmd == "header") return header(rest);
					if (cmd == "convert") return convert(rest);
					err_ << "attribcli: unknown command " << cmd << " (attribcli help lists them)\n";
					return 2;
				}
				catch (const Failed& f)
				{
					err_ << "attribcli: " << f.message << "\n";
					return 2;
				}
				catch (const sc::ScriptError& e)
				{
					err_ << e.what() << "\n";
					return 1;
				}
				catch (const std::exception& e)
				{
					err_ << "attribcli: " << e.what() << "\n";
					return 1;
				}
			}

		private:
			// ------------------------------------------------------------ data ---

			void findData()
			{
				std::error_code ec;
				if (!env_.dataDir.empty())
				{
					dataDir_ = FromUtf8(env_.dataDir);
					return;
				}
				for (const fs::path& d : { FromUtf8(env_.exeDir) / "data", fs::current_path(ec) / "data" })
					if (fs::is_directory(d / "schema", ec))
					{
						dataDir_ = d;
						break;
					}
			}

			// NFS-VltEd's words for each game's values (data/nfsms).
			const std::vector<std::shared_ptr<const ns::PropertyNames>>& nfsmsNames()
			{
				if (nfsmsNames_.empty() && !dataDir_.empty())
					for (const std::string& f : filesIn(dataDir_ / "nfsms"))
					{
						try
						{
							nfsmsNames_.push_back(ns::PropertyNames::Load(f));
						}
						catch (const std::exception& e)
						{
							err_ << "attribcli: " << e.what() << "\n";
						}
					}
				return nfsmsNames_;
			}

			std::vector<std::string> filesIn(const fs::path& dir)
			{
				std::vector<std::string> v;
				std::error_code ec;
				for (const auto& e : fs::directory_iterator(dir, ec))
					if (e.is_regular_file(ec) && Lower(ToUtf8(e.path().extension())) == ".txt") v.push_back(ToUtf8(e.path()));
				std::sort(v.begin(), v.end());
				return v;
			}

			std::unique_ptr<Database> newDatabase()
			{
				auto db = std::make_unique<Database>();
				if (dataDir_.empty())
				{
					err_ << "attribcli: no data folder beside the program or here; names show as hashes and structs as bytes\n";
					return db;
				}
				if (schemas_.empty())
					for (const std::string& f : filesIn(dataDir_ / "schema")) schemas_.push_back(TypeSchema::Load(f));
				for (const auto& s : schemas_) db->addTypeSchema(s);
				if (classLists_.empty())
					for (const std::string& f : filesIn(dataDir_ / "classes")) classLists_.push_back(ClassList::Load(f));
				for (const auto& l : classLists_) db->addClassList(l);
				auto names = [&](const std::string& f, bool bin)
				{
					std::error_code ec;
					if (!fs::exists(FromUtf8(f), ec)) return;
					db->names().loadFile(f);
					if (bin) db->binNames().loadFile(f);
				};
				names(ToUtf8(dataDir_ / "attrib_keys.txt"), true);
				for (const std::string& f : filesIn(dataDir_ / "bin_keys")) db->binNames().loadFile(f);
				if (const std::string uk = userKeysFile(); !uk.empty()) names(uk, true);
				for (const std::string& f : filesIn(dataDir_ / "messages"))
					for (const MessageDef& m : LoadMessages(f)) db->names().add(m.name);
				return db;
			}

			// A folder, or one pack (after the folder's attributes.bin). The game
			// and platform are --target's, else what the files say.
			std::unique_ptr<Database> load(const std::string& path)
			{
				auto db = newDatabase();
				std::error_code ec;
				const fs::path p = FromUtf8(path);
				detection_ = DetectTarget(path);
				const Target t = target_ ? *target_ : detection_.target;
				if (!target_ && !detection_.certain && fs::exists(p, ec))
					err_ << "attribcli: " << path << " doesn't say which game and platform it is from (" << detection_.reason << "); reading it as "
					<< t.name() << " (" << t.id() << "). --target names another.\n";
				db->setTarget(t);
				if (fs::is_directory(p, ec))
				{
					for (const FolderFile& f : FolderPacks(path))
					{
						if (!f.skipped.empty()) continue;
						LoadedPack& pk = db->loadPack(f.path);
						// OGVI's save of a World event vault, beside attributes.bin: saved
						// to gc.vaults as the game's event vault file, its own file removed.
						if (!f.saveAs.empty())
						{
							vaultlib::MoveToEventVaultFile(pk, f.saveAs);
							movedFrom_.push_back(f.path);
						}
					}
				}
				else
				{
					if (!fs::exists(p, ec)) throw std::runtime_error("no file or folder " + path);
					const std::optional<std::string> attrs = DatabaseFileFor(path);
					std::error_code e2;
					if (attrs && !fs::equivalent(FromUtf8(*attrs), p, e2)) db->loadPack(*attrs);
					db->loadPack(path);
				}
				for (const auto& pk : db->packs())
					for (const std::string& w : PackWarnings(*pk)) err_ << "warning: " << w << "\n";
				return db;
			}

			// With the scripts it includes. A script that isn't a file includes from
			// the current folder.
			sc::Script readScript(const std::string& path)
			{
				sc::Script script;
				if (path == "-")
				{
					if (!env_.in) throw Failed{ "no standard input" };
					std::ostringstream s;
					s << env_.in->rdbuf();
					script = sc::Parse(s.str(), "<stdin>", ".");
				}
				else
				{
					std::ifstream in(FromUtf8(path), std::ios::binary);
					if (!in) throw std::runtime_error("can't read " + path);
					std::ostringstream s;
					s << in.rdbuf();
					const fs::path folder = FromUtf8(path).parent_path();
					script = sc::Parse(s.str(), path, folder.empty() ? std::string(".") : ToUtf8(folder));
				}
				sc::LoadIncludes(script);
				return script;
			}

			static void includedFiles(const std::vector<sc::Statement>& list, std::vector<std::string>& out)
			{
				for (const sc::Statement& st : list)
				{
					if (st.kind == sc::Statement::Kind::If)
					{
						includedFiles(st.body, out);
						includedFiles(st.elseBody, out);
					}
					else if (st.kind == sc::Statement::Kind::Include && st.included &&
							 std::find(out.begin(), out.end(), st.included->source) == out.end())
					{
						out.push_back(st.included->source);
						includedFiles(st.included->statements, out);
					}
				}
			}

			ns::Script readNfsms(const std::string& path)
			{
				std::ifstream in(FromUtf8(path), std::ios::binary);
				if (!in) throw std::runtime_error("can't read " + path);
				std::ostringstream s;
				s << in.rdbuf();
				return ns::Parse(s.str(), ToUtf8(FromUtf8(path).filename()), ToUtf8(FromUtf8(path).parent_path()));
			}

			// Where nfsms file_copy writes: the game's folder (above GLOBAL).
			static std::string gameFolderOf(const std::string& db)
			{
				fs::path p = FromUtf8(db);
				std::error_code ec;
				if (!fs::is_directory(p, ec)) p = p.parent_path();
				if (Lower(ToUtf8(p.filename())) == "global") p = p.parent_path();
				return ToUtf8(p);
			}

			void write(const std::string& text, const std::string& to)
			{
				if (to.empty() || to == "-")
				{
					out_ << text;
					return;
				}
				std::ofstream f(FromUtf8(to), std::ios::binary | std::ios::trunc);
				f << text;
				if (!f) throw std::runtime_error("can't write " + to);
				err_ << "wrote " << to << "\n";
			}

			void report(const std::string& source, const sc::ApplyReport& r)
			{
				for (const sc::Message& m : r.messages)
				{
					if (m.level == sc::Message::Level::Echo)
					{
						out_ << m.text << "\n"; // what the script printed
						continue;
					}
					// A line of an included script: that file and line.
					if (!m.file.empty())
					{
						err_ << m.file;
						if (m.fileLine) err_ << ":" << m.fileLine;
					}
					else
					{
						err_ << source;
						if (m.pos.line) err_ << ":" << m.pos.line << ":" << m.pos.col;
					}
					err_ << ": " << (m.level == sc::Message::Level::Error ? "error" : m.level == sc::Message::Level::Warning ? "warning" : "note")
						<< ": " << m.text << "\n";
				}
			}

			// The editor's user_keys.txt: in its user folder, else beside the program.
			std::string userKeysFile() const
			{
				const std::string& dir = env_.userDir.empty() ? env_.exeDir : env_.userDir;
				return dir.empty() ? std::string() : ToUtf8(FromUtf8(dir) / "user_keys.txt");
			}

			// Names a script gave new collections and keys: the packs store only
			// hashes, so they go to user_keys.txt, as names typed in the editor do.
			void rememberNames(const std::vector<std::string>& names)
			{
				const std::string path = userKeysFile();
				if (names.empty() || path.empty()) return;
				const fs::path file = FromUtf8(path);
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

			std::map<std::string, std::string> optionValues(const Args& a)
			{
				std::map<std::string, std::string> v;
				for (const std::string& s : a.all("--set"))
				{
					const size_t eq = s.find('=');
					if (eq == std::string::npos || eq == 0) throw Failed{ "--set takes id=value, not " + s };
					v[s.substr(0, eq)] = s.substr(eq + 1);
				}
				return v;
			}

			// ---------------------------------------------------------- apply ---

			int apply(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, { "-e", "--set", "--out", "--compress" }, { "--dry-run", "--strict", "--no-backup", "--vlted" });
				if (a.positional.empty()) throw Failed{ "apply needs a database" };
				std::optional<LzKind> compress;
				if (a.has("--compress") && !(compress = ParseLzKind(a.get("--compress"))))
					throw Failed{ "--compress takes jdlz, huff, raww or rfpk, not " + a.get("--compress") };
				if (a.positional.size() < 2 && !a.has("-e") && !compress) throw Failed{ "apply needs a script (or -e statements)" };
				std::vector<sc::Script> scripts;
				std::vector<ns::Script> nfsmsScripts;
				std::vector<std::pair<bool, size_t>> order; // in command line order: (nfsms, index)
				for (size_t i = 1; i < a.positional.size(); ++i)
				{
					const std::string& f = a.positional[i];
					if (Lower(ToUtf8(FromUtf8(f).extension())) == ns::kExtension)
					{
						order.emplace_back(true, nfsmsScripts.size());
						nfsmsScripts.push_back(readNfsms(f));
					}
					else
					{
						order.emplace_back(false, scripts.size());
						scripts.push_back(readScript(f));
					}
				}
				int n = 0;
				for (const std::string& e : a.all("-e"))
				{
					order.emplace_back(false, scripts.size());
					scripts.push_back(sc::Parse(e, "-e#" + std::to_string(++n), "."));
					sc::LoadIncludes(scripts.back());
				}

				const std::map<std::string, std::string> given = optionValues(a);
				for (const auto& [id, value] : given)
				{
					bool used = false;
					for (const sc::Script& s : scripts)
						for (const sc::OptionDef* o : s.options()) used |= o->id == id;
					for (const ns::Script& s : nfsmsScripts)
						for (const ns::Control& c : s.controls) used |= c.name == id || c.group == id;
					if (!used) throw Failed{ "no script has an option " + id };
				}

				auto db = load(a.positional[0]);
				// World event vault files whose vault a script deleted, and OGVI's
				// saves of event vaults that go to gc.vaults (vaultlib::MoveToEventVaultFile).
				std::vector<std::string> removed = movedFrom_;
				for (const auto& [isNfsms, index] : order)
				{
					if (!isNfsms)
					{
						if (!applyOne(*db, scripts[index], a, given, removed)) return 1;
						continue;
					}
					const ns::Script& s = nfsmsScripts[index];
					ns::RunSettings st;
					st.strict = a.has("--strict");
					st.vltEd = a.has("--vlted");
					st.choices = ns::DefaultChoices(s);
					// --set kits=true for a checkbox, --set tier=race for a radio group.
					for (const ns::Control& c : s.controls)
					{
						if (c.kind == ns::Control::Kind::Checkbox)
						{
							if (auto it = given.find(c.name); it != given.end()) st.choices.checkboxes[c.name] = Lower(it->second) == "true" || it->second == "1";
						}
						else if (auto it = given.find(c.group); it != given.end())
						{
							st.choices.radios[c.group] = it->second;
						}
					}
					st.names = ns::NamesFor(*db, nfsmsNames());
					const std::string target = gameFolderOf(a.positional[0]);
					st.targetFolder = target;
					st.doFiles = !a.has("--dry-run") && a.get("--out").empty();
					// Line by line, as NFS-VltEd runs it: a failing line is reported
					// and the next one runs (--strict stops at it, saving nothing).
					const sc::ApplyReport r = ns::Run(*db, s, st);
					report(s.source, r);
					if (!r.ok)
					{
						err_ << "attribcli: stopped; nothing was saved\n";
						return 1;
					}
					const size_t failed = r.errors();
					out_ << s.source << ": " << r.summary() << (failed ? "; " + std::to_string(failed) + " line(s) failed" : "")
						<< (r.warnings() ? " (" + std::to_string(r.warnings()) + " warning(s))" : "") << "\n";
					if (!a.has("--dry-run")) rememberNames(r.newNames);
				}

				if (compress)
				{
					bool any = false;
					for (const auto& pk : db->packs())
					{
						if (!pk->compression) continue;
						any = true;
						if (*pk->compression == *compress) continue;
						pk->compression = compress;
						pk->dirty = true;
					}
					if (!any) err_ << "attribcli: warning: --compress: no compressed pack (gameplay.lzc) is loaded\n";
				}
				return saveChanged(*db, std::move(removed), a);
			}

			// Writes the database's dirty packs (with a .lzc's twin) and removes
			// `removed`, as apply and repair save: --dry-run, --out and --no-backup.
			int saveChanged(Database& dbRef, std::vector<std::string> removed, const Args& a)
			{
				Database* db = &dbRef;
				std::vector<LoadedPack*> dirty;
				for (const auto& pk : db->packs())
					if (pk->dirty) dirty.push_back(pk.get());
				// A file made again after its vault was deleted stays.
				std::erase_if(removed, [&](const std::string& f)
				{
					return std::any_of(db->packs().begin(), db->packs().end(), [&](const auto& pk) { return pk->path == f; });
				});
				std::sort(removed.begin(), removed.end());
				removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
				if (a.has("--dry-run"))
				{
					for (const LoadedPack* p : dirty) out_ << "would write " << p->path << "\n";
					for (const std::string& f : removed) out_ << "would remove " << f << "\n";
					if (dirty.empty() && removed.empty()) out_ << "nothing would change\n";
					return 0;
				}
				const std::string outDir = a.get("--out");
				if (!outDir.empty())
				{
					std::error_code ec;
					fs::create_directories(FromUtf8(outDir), ec);
				}
				for (LoadedPack* p : dirty)
				{
					const std::string target = outDir.empty() ? p->path : ToUtf8(FromUtf8(outDir) / FromUtf8(p->path).filename());
					const std::optional<std::string> twin = PackTwin(target);
					if (outDir.empty() && !a.has("--no-backup"))
					{
						std::vector<std::string> files{ target };
						if (twin) files.push_back(*twin);
						for (const std::string& f : files)
						{
							std::error_code ec;
							const fs::path t = FromUtf8(f);
							fs::path bak = t;
							bak += ".bak";
							if (fs::exists(t, ec) && !fs::exists(bak, ec))
							{
								fs::copy_file(t, bak, ec);
								if (!ec) err_ << "kept the original as " << ToUtf8(bak) << "\n";
							}
						}
					}
					// A new event vault file's folder may not be there yet.
					if (std::error_code ec; !fs::exists(FromUtf8(target).parent_path(), ec)) fs::create_directories(FromUtf8(target).parent_path(), ec);
					SavePack(*db, *p, target);
					out_ << "saved " << target << (twin ? " and " + ToUtf8(FromUtf8(*twin).filename()) : std::string()) << "\n";
				}
				for (const std::string& f : removed)
				{
					std::error_code ec;
					const fs::path t = FromUtf8(f);
					if (!outDir.empty() || !fs::exists(t, ec))
					{
						if (!outDir.empty()) out_ << "deleted the vault of " << f << " (left as it is: --out writes elsewhere)\n";
						continue;
					}
					fs::path bak = t;
					bak += ".bak";
					if (!a.has("--no-backup") && !fs::exists(bak, ec))
					{
						fs::rename(t, bak, ec);
						if (!ec) out_ << "removed " << f << ", kept as " << ToUtf8(bak) << "\n";
					}
					else
					{
						fs::remove(t, ec);
						if (!ec) out_ << "removed " << f << "\n";
					}
					if (ec) throw Failed{ "could not remove " + f + ": " + ec.message() };
				}
				if (dirty.empty() && removed.empty()) out_ << "nothing changed\n";
				return 0;
			}

			// Opens a database and saves what loading repaired: files another tool
			// wrote wrong (VaultLib, which the community Attribulator and OGVI save
			// with: core/VaultLib.h), each fix reported as a warning.
			int repair(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, { "--out" }, { "--dry-run", "--no-backup" });
				if (a.positional.size() != 1) throw Failed{ "repair takes a database" };
				movedFrom_.clear();
				auto db = load(a.positional[0]);
				size_t vaults = 0, packs = 0;
				for (const auto& pk : db->packs())
				{
					size_t here = 0;
					for (const auto& v : pk->vaults) here += v->repaired;
					vaults += here;
					packs += here != 0;
				}
				if (!vaults)
				{
					out_ << "nothing to repair: no vault of " << a.positional[0] << " was written wrong in a way Attribulina knows\n";
					return 0;
				}
				out_ << "repaired " << vaults << " vault(s) in " << packs << " pack(s); the warnings above say what changed\n";
				// Only what was repaired: a pack loading marked dirty.
				return saveChanged(*db, movedFrom_, a);
			}

			bool applyOne(Database& db, const sc::Script& s, const Args& a, const std::map<std::string, std::string>& given,
						  std::vector<std::string>& removed)
			{
				sc::ApplySettings st;
				st.strict = a.has("--strict");
				st.fragment = s.source.rfind("-e#", 0) == 0;
				for (const sc::OptionDef* o : s.options())
					if (auto it = given.find(o->id); it != given.end()) st.options[o->id] = it->second;
				const sc::ApplyReport r = sc::Apply(db, s, st);
				report(s.source, r);
				if (!r.ok)
				{
					err_ << "attribcli: stopped; nothing was saved\n";
					return false;
				}
				out_ << s.source << ": " << r.summary() << (r.warnings() ? " (" + std::to_string(r.warnings()) + " warning(s))" : "") << "\n";
				if (!a.has("--dry-run")) rememberNames(r.newNames);
				removed.insert(removed.end(), r.removedFiles.begin(), r.removedFiles.end());
				return true;
			}

			int check(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, { "--set" }, { "--strict" });
				if (a.positional.empty() || a.positional.size() > 2) throw Failed{ "check takes a script and optionally a database" };
				const sc::Script s = readScript(a.positional[0]);
				out_ << s.source << ": reads fine";
				if (!s.meta("title").empty()) out_ << "; " << s.meta("title");
				if (!s.meta("game").empty()) out_ << " (for " << s.meta("game") << ")";
				out_ << "\n";
				std::vector<std::string> included;
				includedFiles(s.statements, included);
				for (const std::string& f : included) out_ << "  includes " << f << "\n";
				for (const sc::OptionDef* o : s.options())
				{
					out_ << "  option " << o->id << " (" << sc::OptionTypeName(o->type) << ")";
					if (!o->label.empty()) out_ << " " << o->label;
					if (o->defaultValue) out_ << ", default " << sc::PrintValue(*o->defaultValue);
					if (!o->choices.empty())
					{
						out_ << ", one of";
						for (const std::string& c : o->choices) out_ << " " << c;
					}
					out_ << "\n";
				}
				sc::ApplySettings st;
				st.options = optionValues(a);
				st.strict = a.has("--strict");
				sc::ResolveOptions(s, st); // throws on bad values
				if (a.positional.size() < 2) return 0;
				auto db = load(a.positional[1]);
				const sc::ApplyReport r = sc::Apply(*db, s, st);
				report(s.source, r);
				out_ << (r.ok ? "applies: " + r.summary() : std::string("does not apply")) << "\n";
				return r.ok ? 0 : 1;
			}

			// ------------------------------------------------------- generate ---

			int diff(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, { "-o", "--class" }, { "--no-was" });
				if (a.positional.size() != 2) throw Failed{ "diff takes the base database and the changed one" };
				auto base = load(a.positional[0]);
				auto cur = load(a.positional[1]);
				sc::DiffSettings st;
				st.recordWas = !a.has("--no-was");
				for (const std::string& c : a.all("--class"))
				{
					if (!cur->findClass(StringHash32(c))) throw std::runtime_error("no class " + c);
					st.classes.insert(StringHash32(c));
				}
				sc::Script s = sc::Diff(*base, *cur, st);
				write(sc::Print(s), a.get("-o"));
				return 0;
			}

			std::vector<const Collection*> collections(Database& db, const std::string& spec, bool children)
			{
				const size_t slash = spec.find('/');
				const std::string clsName = spec.substr(0, slash);
				const ClassDef* cls = db.findClass(StringHash32(clsName));
				if (!cls) throw std::runtime_error("no class " + clsName);
				std::vector<const Collection*> out;
				std::function<void(const Collection*)> add = [&](const Collection* c)
				{
					if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
					if (children)
						for (const Collection* k : c->children) add(k);
				};
				if (slash == std::string::npos)
				{
					for (const Collection* c : cls->collections) add(c);
					return out;
				}
				const std::string name = spec.substr(slash + 1);
				const Collection* c = db.findCollection(cls->key, StringHash32(name));
				if (!c && name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
					c = db.findCollection(cls->key, uint32_t(std::strtoul(name.c_str() + 2, nullptr, 16)));
				if (!c) throw std::runtime_error("no collection " + spec);
				add(c);
				return out;
			}

			int exportCmd(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, { "-o", "--field" }, { "--children", "--exact", "--edit" });
				if (a.positional.size() < 2) throw Failed{ "export takes a database and what to export" };
				auto db = load(a.positional[0]);
				std::vector<const Collection*> cols;
				for (size_t i = 1; i < a.positional.size(); ++i)
					for (const Collection* c : collections(*db, a.positional[i], a.has("--children")))
						if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
				sc::ExportSettings st;
				st.exact = a.has("--exact");
				st.editOnly = a.has("--edit");
				for (const std::string& f : a.all("--field")) st.fields.insert(StringHash32(f));
				write(sc::Print(sc::NewScript(*db, sc::Export(*db, cols, st))), a.get("-o"));
				return 0;
			}

			int convert(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, { "-o" }, { "--vlted" });
				if (a.positional.size() != 2) throw Failed{ "convert takes a database and an nfsms script" };
				auto db = load(a.positional[0]);
				const ns::Script s = readNfsms(a.positional[1]);
				ns::RunSettings st;
				st.vltEd = a.has("--vlted");
				st.names = ns::NamesFor(*db, nfsmsNames());
				sc::ApplyReport r;
				const sc::Script out = ns::ToModScript(*db, s, st, &r); // db is a copy: nothing is saved
				for (const sc::Message& m : r.messages)
					if (m.level == sc::Message::Level::Error || m.level == sc::Message::Level::Warning)
						err_ << s.source << ":" << m.pos.line << ": " << (m.level == sc::Message::Level::Error ? "error: " : "warning: ") << m.text
						<< "\n";
				write(sc::Print(out), a.get("-o"));
				if (r.errors()) err_ << "attribcli: " << r.errors() << " line(s) could not be converted; they are comments in the script\n";
				return 0;
			}

			// ----------------------------------------------------------- read ---

			int get(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, {}, {});
				if (a.positional.size() < 2) throw Failed{ "get takes a database and class/collection" };
				auto db = load(a.positional[0]);
				const std::vector<const Collection*> cols = collections(*db, a.positional[1], false);
				if (cols.size() != 1 || a.positional[1].find('/') == std::string::npos) throw Failed{ "get takes one class/collection" };
				const Collection& c = *cols[0];
				sc::Script s;
				if (a.positional.size() == 2)
				{
					sc::ExportSettings st;
					st.editOnly = true;
					s.statements = sc::Export(*db, cols, st);
				}
				else
				{
					sc::Statement e = sc::ExportValue(*db, c, 0);
					for (size_t i = 2; i < a.positional.size(); ++i)
					{
						const uint32_t f = StringHash32(a.positional[i]);
						if (!c.cls || !c.cls->field(f)) throw std::runtime_error(db->names().format(c.classKey) + " has no field " + a.positional[i]);
						const sc::Statement one = sc::ExportValue(*db, c, f);
						if (one.body.empty())
						{
							sc::Statement note;
							note.kind = sc::Statement::Kind::Comment;
							note.comment = " " + a.positional[i] + " isn't set here or above";
							e.body.push_back(note);
							continue;
						}
						sc::Statement v = one.body[0];
						const Collection* owner = nullptr;
						c.resolve(f, &owner);
						if (owner && owner != &c) v.comment = " from " + db->names().format(owner->key);
						e.body.push_back(v);
					}
					s.statements.push_back(e);
				}
				out_ << sc::Print(s);
				return 0;
			}

			int list(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, {}, {});
				if (a.positional.empty() || a.positional.size() > 2) throw Failed{ "list takes a database and optionally a class" };
				auto db = load(a.positional[0]);
				const KeyNames& names = db->names();
				if (a.positional.size() == 1)
				{
					std::vector<std::pair<std::string, size_t>> rows;
					for (const auto& cls : db->classes()) rows.emplace_back(names.format(cls->key), cls->collections.size());
					std::sort(rows.begin(), rows.end());
					for (const auto& [n, count] : rows) out_ << n << " (" << count << ")\n";
					return 0;
				}
				const ClassDef* cls = db->findClass(StringHash32(a.positional[1]));
				if (!cls) throw std::runtime_error("no class " + a.positional[1]);
				std::function<void(const Collection*, int)> tree = [&](const Collection* c, int depth)
				{
					out_ << std::string(size_t(depth) * 2, ' ') << names.format(c->key) << "\n";
					std::vector<const Collection*> kids(c->children.begin(), c->children.end());
					std::sort(kids.begin(), kids.end(), [&](const Collection* x, const Collection* y)
					{
						return names.format(x->key) < names.format(y->key);
					});
					for (const Collection* k : kids) tree(k, depth + 1);
				};
				std::vector<const Collection*> roots;
				for (const Collection* c : cls->collections)
					if (!c->parent) roots.push_back(c);
				std::sort(roots.begin(), roots.end(), [&](const Collection* x, const Collection* y)
				{
					return names.format(x->key) < names.format(y->key);
				});
				for (const Collection* r : roots) tree(r, 0);
				return 0;
			}

			int header(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, { "-o" }, { "--types", "--no-setters" });
				if (a.positional.size() != 2) throw Failed{ "header takes a database and a class" };
				auto db = load(a.positional[0]);
				const ClassDef* cls = db->findClass(StringHash32(a.positional[1]));
				if (!cls) throw std::runtime_error("no class " + a.positional[1]);
				GenHeaderOptions o;
				o.types = a.has("--types");
				o.setters = !a.has("--no-setters");
				write(GenHeader(*db, *cls, o), a.get("-o"));
				return 0;
			}

			int info(const std::vector<std::string>& in)
			{
				const Args a = Parse(in, {}, {});
				if (a.positional.size() != 1) throw Failed{ "info takes a database" };
				auto db = load(a.positional[0]);
				const Target& t = db->target();
				out_ << "game: " << t.name() << " (" << t.id() << "), "
					<< (target_ ? "as --target says" : detection_.certain ? "from " + detection_.reason : "a guess: " + detection_.reason) << "\n";
				if (const TypeSchema* ts = db->typeSchema()) out_ << "schema: " << ToUtf8(FromUtf8(ts->source).filename()) << "\n";
				else out_ << "schema: none for " << t.name() << " (structs and enums show as bytes)\n";
				out_ << db->classes().size() << " classes, " << db->collectionCount() << " collections\n";
				for (const auto& pk : db->packs())
				{
					size_t cols = 0;
					for (const auto& v : pk->vaults) cols += v->collections.size();
					out_ << pk->path << ": " << pk->vaults.size() << " vault(s), " << cols << " collections";
					if (pk->compression) out_ << ", " << LzKindName(*pk->compression) << " compressed";
					out_ << "\n";
				}
				return 0;
			}

			std::ostream& out_;
			std::ostream& err_;
			const Env& env_;
			fs::path dataDir_;
			std::optional<Target> target_; // --target
			Detection detection_;          // of the last database loaded
			std::vector<std::string> movedFrom_; // files of packs saved elsewhere (FolderFile::saveAs)
			std::vector<std::shared_ptr<const ns::PropertyNames>> nfsmsNames_;
			std::vector<std::shared_ptr<TypeSchema>> schemas_;
			std::vector<std::shared_ptr<const ClassList>> classLists_;
		};

	} // namespace

	int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, const Env& env)
	{
		Tool t(out, err, env);
		return t.run(args);
	}

} // namespace cli
