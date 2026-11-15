#include "ModTrace.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace attrib
{

	namespace fs = std::filesystem;
	namespace sc = attrib::script;
	using K = sc::Statement::Kind;

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
		uint64_t Id(uint32_t cls, uint32_t key)
		{
			return (uint64_t(cls) << 32) | key;
		}
		std::string Plural(size_t n, const char* one, const char* many)
		{
			return std::to_string(n) + " " + (n == 1 ? one : many);
		}

		void WriteText(const fs::path& p, const std::string& text)
		{
			fs::path tmp = p;
			tmp += ".tmp";
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				out.write(text.data(), std::streamsize(text.size()));
				out.flush();
				if (!out) throw std::runtime_error("can't write " + ToUtf8(tmp));
			}
			std::error_code ec;
			fs::rename(tmp, p, ec);
			if (ec)
			{
				fs::remove(tmp, ec);
				throw std::runtime_error("can't write " + ToUtf8(p) + ": " + ec.message());
			}
		}

		// A file name from a mod's name: what Windows can't have in one goes.
		std::string FileNamePart(const std::string& s)
		{
			std::string out;
			for (unsigned char c : s)
			{
				if (c < 32 || std::string_view("<>:\"/\\|?*").find(char(c)) != std::string_view::npos) out += '_';
				else out += char(c);
			}
			while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
			while (!out.empty() && out.front() == ' ') out.erase(out.begin());
			if (out.size() > 60) out.resize(60);
			while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
			return out.empty() ? "mod" : out;
		}
	} // namespace

	// ----------------------------------------------------------------- making ---

	script::Script MakeTrace(Database& before, const Database& after, const InstalledMod& mod)
	{
		before.names() = after.names();
		sc::Script s = sc::Diff(after, before);
		// The header says the format its statements need (remove by value is 2's).
		const int format = sc::FormatNeeded(s.statements);
		for (sc::Statement& st : s.statements)
			if (st.kind == K::Header) st.number = std::max(st.number, format);
		s.setMeta("title", "Undo " + mod.nameAndVersion());
		std::vector<sc::Statement> notes;
		auto note = [&](const std::string& t)
		{
			sc::Statement c;
			c.kind = K::Comment;
			c.comment = t.empty() ? std::string() : " " + t;
			notes.push_back(std::move(c));
		};
		note("The trace of " + mod.nameAndVersion() + (mod.installed.empty() ? "" : ", installed " + mod.installed) +
			(mod.by.empty() ? "" : " by " + mod.by) + ".");
		note("Applied to the database as the mod left it, it sets back what the mod changed: uninstalling");
		note("the mod applies it. Files the mod copied aren't in it. Each `was` is the value the mod set.");
		// After the header lines.
		size_t at = 0;
		while (at < s.statements.size() && (s.statements[at].kind == K::Header || s.statements[at].kind == K::Meta)) ++at;
		s.statements.insert(s.statements.begin() + std::ptrdiff_t(at), notes.begin(), notes.end());
		return s;
	}

	// ----------------------------------------------------------------- places ---

	namespace
	{
		ModPlace CollectionPlace(ModPlace::Kind kind, const sc::Target& t, const char* what = nullptr)
		{
			ModPlace p;
			p.kind = kind;
			p.cls = sc::NameKey(t.cls);
			p.key = sc::NameKey(t.name);
			p.text = t.text() + (what ? std::string(" (") + what + ")" : std::string());
			return p;
		}

		void AddBody(std::vector<ModPlace>& out, const sc::Target& t, const std::vector<sc::Statement>& body)
		{
			for (const sc::Statement& b : body)
			{
				switch (b.kind)
				{
					case K::Set:
					case K::Revert:
					case K::Resize:
					case K::Insert:
					case K::Remove:
					case K::Append:
					{
						ModPlace p = CollectionPlace(ModPlace::Kind::Value, t);
						p.field = sc::NameKey(b.path.field);
						p.steps = b.path.steps;
						// Insert, resize and remove by index move what comes after: all of the array.
						if (b.kind == K::Insert || b.kind == K::Resize || (b.kind == K::Remove && !b.value))
							if (!p.steps.empty()) p.steps.pop_back();
						p.incremental = b.kind == K::Append || (b.kind == K::Remove && b.value);
						sc::FieldPath shown = b.path;
						shown.steps = p.steps;
						p.text = t.text() + " " + shown.text();
						out.push_back(std::move(p));
						break;
					}
					case K::If:
						AddBody(out, t, b.body);
						AddBody(out, t, b.elseBody);
						break;
					default:
						break;
				}
			}
		}

		void AddStatements(std::vector<ModPlace>& out, const std::vector<sc::Statement>& statements)
		{
			for (const sc::Statement& s : statements)
			{
				switch (s.kind)
				{
					case K::Edit:
						AddBody(out, s.target, s.body);
						break;
					case K::Create:
					case K::Collection:
					case K::Delete:
						out.push_back(CollectionPlace(ModPlace::Kind::Collection, s.target));
						break;
					case K::Rename:
					{
						out.push_back(CollectionPlace(ModPlace::Kind::Collection, s.target));
						sc::Target to = s.target;
						to.name = s.word;
						out.push_back(CollectionPlace(ModPlace::Kind::Collection, to));
						break;
					}
					case K::Move:
						out.push_back(CollectionPlace(ModPlace::Kind::Parent, s.target, "its parent"));
						break;
					case K::MoveToVault:
						out.push_back(CollectionPlace(ModPlace::Kind::InVault, s.target, "its vault"));
						break;
					case K::CreateVault:
					case K::DeleteVault:
					case K::RenameVault:
					{
						for (const std::string& v : { s.vault, s.kind == K::RenameVault ? s.word : std::string() })
						{
							if (v.empty()) continue;
							ModPlace p;
							p.kind = ModPlace::Kind::Vault;
							p.vault = Lower(v);
							p.text = "vault " + v;
							out.push_back(std::move(p));
						}
						break;
					}
					case K::Class:
					case K::DeleteClass:
					{
						ModPlace p;
						p.kind = ModPlace::Kind::Class;
						p.cls = sc::NameKey(s.word);
						p.text = "class " + s.word;
						out.push_back(std::move(p));
						break;
					}
					case K::If:
						AddStatements(out, s.body);
						AddStatements(out, s.elseBody);
						break;
					default:
						break;
				}
			}
		}

		bool StepsMeet(const std::vector<sc::FieldPath::Step>& a, const std::vector<sc::FieldPath::Step>& b)
		{
			const size_t n = std::min(a.size(), b.size());
			for (size_t i = 0; i < n; ++i)
			{
				if (a[i].member.empty() != b[i].member.empty()) return true; // not the same kind of step: can't tell, so they meet
				if (a[i].member.empty() ? a[i].index != b[i].index : a[i].member != b[i].member) return false;
			}
			return true;
		}

		// A trace's places by what they are in, for finding clashes fast.
		struct PlaceIndex
		{
			std::unordered_map<uint64_t, std::vector<const ModPlace*>> byCollection;
			std::unordered_set<uint32_t> classes;     // Class places
			std::unordered_set<uint32_t> usedClasses; // classes any collection place is in
			std::unordered_set<std::string> vaults;

			explicit PlaceIndex(const std::vector<ModPlace>& places)
			{
				for (const ModPlace& p : places)
				{
					switch (p.kind)
					{
						case ModPlace::Kind::Class: classes.insert(p.cls); break;
						case ModPlace::Kind::Vault: vaults.insert(p.vault); break;
						default:
							byCollection[Id(p.cls, p.key)].push_back(&p);
							usedClasses.insert(p.cls);
							break;
					}
				}
			}
			bool clashes(const ModPlace& p) const
			{
				switch (p.kind)
				{
					case ModPlace::Kind::Class: return classes.count(p.cls) || usedClasses.count(p.cls);
					case ModPlace::Kind::Vault: return vaults.count(p.vault) > 0;
					default:
						if (classes.count(p.cls)) return true;
						if (auto it = byCollection.find(Id(p.cls, p.key)); it != byCollection.end())
							for (const ModPlace* q : it->second)
								if (Clash(p, *q)) return true;
						return false;
				}
			}
		};
	} // namespace

	std::vector<ModPlace> PlacesOf(const script::Script& trace)
	{
		std::vector<ModPlace> out;
		AddStatements(out, trace.statements);
		return out;
	}

	std::string DescribePlaces(const std::vector<ModPlace>& places)
	{
		size_t values = 0, whole = 0, moved = 0, classes = 0, vaults = 0;
		std::unordered_set<uint64_t> of;
		for (const ModPlace& p : places)
		{
			switch (p.kind)
			{
				case ModPlace::Kind::Value: ++values; of.insert(Id(p.cls, p.key)); break;
				case ModPlace::Kind::Collection: ++whole; break;
				case ModPlace::Kind::Parent:
				case ModPlace::Kind::InVault: ++moved; break;
				case ModPlace::Kind::Class: ++classes; break;
				case ModPlace::Kind::Vault: ++vaults; break;
			}
		}
		std::vector<std::string> parts;
		if (values) parts.push_back(Plural(values, "value", "values") + " of " + Plural(of.size(), "collection", "collections"));
		if (whole) parts.push_back(Plural(whole, "collection", "collections") + " made, deleted or renamed");
		if (moved) parts.push_back(Plural(moved, "collection", "collections") + " moved");
		if (classes) parts.push_back(Plural(classes, "class", "classes"));
		if (vaults) parts.push_back(Plural(vaults, "vault", "vaults"));
		if (parts.empty()) return "nothing";
		std::string s;
		for (size_t i = 0; i < parts.size(); ++i) s += (i ? ", " : "") + parts[i];
		return s;
	}

	bool Clash(const ModPlace& a, const ModPlace& b)
	{
		using PK = ModPlace::Kind;
		if (a.kind == PK::Vault || b.kind == PK::Vault) return a.kind == b.kind && a.vault == b.vault;
		if (a.kind == PK::Class || b.kind == PK::Class) return a.cls == b.cls;
		if (a.cls != b.cls || a.key != b.key) return false;
		if (a.kind == PK::Collection || b.kind == PK::Collection) return true;
		if (a.kind != b.kind) return false; // a value, the parent and the vault are apart
		if (a.kind != PK::Value) return true;
		if (a.field != b.field) return false;
		if (a.incremental && b.incremental) return false; // each adds or takes its own elements
		return StepsMeet(a.steps, b.steps);
	}

	std::string TracePath(const std::string& folder, const std::string& trace)
	{
		std::string t = trace;
		std::replace(t.begin(), t.end(), '\\', '/');
		return ToUtf8(FromUtf8(folder) / FromUtf8(t));
	}

	std::vector<TracedMod> ReadTraces(const std::string& folder, const ModList& list)
	{
		std::vector<TracedMod> out;
		for (const InstalledMod& m : list.mods)
		{
			TracedMod t;
			t.mod = m;
			if (m.trace.empty())
			{
				t.problem = "what it changed wasn't kept when it was installed (it has no trace)";
				out.push_back(std::move(t));
				continue;
			}
			const std::string path = TracePath(folder, m.trace);
			std::ifstream in(FromUtf8(path), std::ios::binary);
			if (!in)
			{
				t.problem = "its trace, " + path + ", isn't there";
				out.push_back(std::move(t));
				continue;
			}
			std::ostringstream s;
			s << in.rdbuf();
			try
			{
				t.trace = std::make_shared<const sc::Script>(sc::Parse(s.str(), path));
			}
			catch (const std::exception& e)
			{
				t.problem = std::string("its trace doesn't read: ") + e.what();
			}
			out.push_back(std::move(t));
		}
		return out;
	}

	std::vector<ModConflict> FindConflicts(const std::vector<ModPlace>& places, const std::vector<TracedMod>& mods, size_t maxPlaces)
	{
		std::vector<ModConflict> out;
		if (places.empty()) return out;
		for (size_t i = 0; i < mods.size(); ++i)
		{
			if (!mods[i].trace) continue;
			const std::vector<ModPlace> theirs = PlacesOf(*mods[i].trace);
			if (theirs.empty()) continue;
			const PlaceIndex index(theirs);
			ModConflict c;
			c.index = i;
			std::unordered_set<std::string> seen;
			for (const ModPlace& p : places)
			{
				if (!index.clashes(p) || !seen.insert(p.text).second) continue;
				++c.count;
				if (c.places.size() < maxPlaces) c.places.push_back(p.text);
			}
			if (c.count) out.push_back(std::move(c));
		}
		return out;
	}

	std::string ConflictText(const ModConflict& c, const std::string& other)
	{
		std::string s = "changes " + std::string(c.count == 1 ? "1 thing" : std::to_string(c.count) + " things") + " " + other + " changed: ";
		for (size_t i = 0; i < c.places.size(); ++i)
		{
			if (i) s += i + 1 == c.places.size() && c.count == c.places.size() ? " and " : ", ";
			s += c.places[i];
		}
		if (c.count > c.places.size()) s += " and " + std::to_string(c.count - c.places.size()) + " more";
		return s;
	}

	// ----------------------------------------------------------- uninstalling ---

	std::string UninstallResult::summary() const
	{
		std::string s;
		auto add = [&](size_t n, const char* one, const char* many)
		{
			if (!n) return;
			s += (s.empty() ? "" : ", ") + Plural(n, one, many);
		};
		add(valuesSet, "value set back", "values set back");
		add(created, "collection made again", "collections made again");
		add(deleted, "collection deleted", "collections deleted");
		return s.empty() ? "nothing to set back" : s;
	}

	UninstallResult Uninstall(Database& db, const std::vector<TracedMod>& mods, std::vector<size_t> which,
		const std::function<void(Collection&)>& beforeRemove)
	{
		UninstallResult r;
		std::sort(which.begin(), which.end());
		which.erase(std::unique(which.begin(), which.end()), which.end());
		if (which.empty())
		{
			r.ok = true;
			return r;
		}
		for (size_t w : which)
		{
			if (w >= mods.size())
			{
				r.error = "no such mod on the list";
				return r;
			}
			if (!mods[w].trace)
			{
				r.error = mods[w].mod.nameAndVersion() + " can't be uninstalled: " + mods[w].problem + ".";
				return r;
			}
		}
		const std::set<size_t> gone(which.begin(), which.end());
		auto names = [&](const std::vector<size_t>& list)
		{
			std::string s;
			for (size_t i = 0; i < list.size(); ++i)
				s += (i == 0 ? "" : i + 1 == list.size() ? " and " : ", ") + mods[list[i]].mod.nameAndVersion();
			return s;
		};
		const std::string goneNames = names(which);

		// The mods after the first one going, and which of them are known.
		std::vector<size_t> later, unknown;
		for (size_t j = which.front() + 1; j < mods.size(); ++j)
		{
			if (gone.count(j)) continue;
			(mods[j].trace ? later : unknown).push_back(j);
		}
		if (!unknown.empty())
			r.notes.push_back(names(unknown) + (unknown.size() == 1 ? " was" : " were") + " installed after " + goneNames +
				" and what " + (unknown.size() == 1 ? "it" : "they") + " changed isn't known (no trace): where " +
				(unknown.size() == 1 ? "it" : "they") + " changed the same values, they go back to what they were before " + goneNames + ".");

		// Do the later mods change any of the same things? Then they are undone
		// and done again around it; otherwise its trace alone does.
		std::map<size_t, std::vector<ModPlace>> places;
		for (size_t i : which) places[i] = PlacesOf(*mods[i].trace);
		for (size_t j : later) places[j] = PlacesOf(*mods[j].trace);
		std::vector<size_t> clashing;
		for (size_t j : later)
		{
			const PlaceIndex index(places[j]);
			bool clash = false;
			for (size_t w : which)
			{
				if (w > j) continue;
				for (const ModPlace& p : places[w])
				{
					// A vault made or deleted holds what came after: always done around.
					if (p.kind == ModPlace::Kind::Vault || index.clashes(p))
					{
						clash = true;
						break;
					}
				}
				if (clash) break;
			}
			if (clash) clashing.push_back(j);
		}
		r.redone = !clashing.empty();

		sc::ApplySettings st;
		st.checkReferences = false;
		st.beforeRemove = beforeRemove;
		auto take = [&](const sc::ApplyReport& rep, bool counts)
		{
			r.touched.insert(rep.touched.begin(), rep.touched.end());
			r.removedFiles.insert(r.removedFiles.end(), rep.removedFiles.begin(), rep.removedFiles.end());
			r.newNames.insert(r.newNames.end(), rep.newNames.begin(), rep.newNames.end());
			if (!counts) return;
			r.valuesSet += rep.valuesSet + rep.reverted;
			r.created += rep.created;
			r.deleted += rep.deleted;
		};
		auto firstError = [](const sc::ApplyReport& rep)
		{
			for (const sc::Message& m : rep.messages)
				if (m.level == sc::Message::Level::Error) return m.text;
			return std::string("it stopped");
		};
		// A mod going: its trace, with what no longer is as it left it.
		auto undo = [&](size_t i) -> bool
		{
			const sc::ApplyReport rep = sc::Apply(db, *mods[i].trace, st);
			take(rep, true);
			if (!rep.ok)
			{
				r.error = "the trace of " + mods[i].mod.nameAndVersion() + " doesn't apply: " + firstError(rep) + ".";
				if (!later.empty() && !r.redone)
					r.error += " A mod installed after it (" + names(later) + ") may build on what it changed: uninstall that first.";
				return false;
			}
			std::vector<std::string> changed;
			for (const sc::Message& m : rep.messages)
				if (m.level == sc::Message::Level::Warning && m.text.find(" in the script's database, not here") != std::string::npos)
					changed.push_back(m.text);
			if (!changed.empty())
			{
				std::string n = mods[i].mod.nameAndVersion() + ": " + Plural(changed.size(), "value isn't", "values aren't") +
					" what it set (changed by hand since, or by a mod not on the list); they go back to what they were before it all the same: ";
				for (size_t k = 0; k < changed.size() && k < 3; ++k) n += (k ? "; " : "") + changed[k];
				if (changed.size() > 3) n += "; and " + std::to_string(changed.size() - 3) + " more";
				r.notes.push_back(n);
			}
			return true;
		};

		if (!r.redone)
		{
			for (auto it = which.rbegin(); it != which.rend(); ++it)
				if (!undo(*it)) return r;
			r.removed = which;
			r.ok = true;
			return r;
		}

		// Undo, newest first, down to the first going, keeping what each later
		// mod did as the database had it (its trace's places only).
		std::vector<size_t> stack;
		for (size_t j = which.front(); j < mods.size(); ++j)
			if (gone.count(j) || mods[j].trace) stack.push_back(j);
		std::map<size_t, sc::Script> redo;
		sc::DiffSettings ds;
		ds.recordWas = false; // the values under them change: that is the point
		for (auto it = stack.rbegin(); it != stack.rend(); ++it)
		{
			const size_t j = *it;
			if (gone.count(j))
			{
				if (!undo(j)) return r;
				continue;
			}
			std::unique_ptr<Database> as = sc::CloneDatabase(db);
			const sc::ApplyReport rep = sc::Apply(db, *mods[j].trace, st);
			take(rep, false);
			if (!rep.ok)
			{
				r.error = "the trace of " + mods[j].mod.nameAndVersion() + ", installed after " + goneNames + ", doesn't apply: " + firstError(rep) +
					". It has to be undone and done again, as it changes some of the same things.";
				return r;
			}
			redo[j] = sc::Diff(db, *as, ds);
		}
		// Done again, oldest first, each traced again.
		for (size_t j : stack)
		{
			if (gone.count(j)) continue;
			std::unique_ptr<Database> before = sc::CloneDatabase(db);
			const sc::ApplyReport rep = sc::Apply(db, redo[j], st);
			take(rep, false);
			if (!rep.ok)
			{
				r.error = mods[j].mod.nameAndVersion() + " builds on what " + goneNames + " changed, so it can't stay installed without " +
					(which.size() == 1 ? "it" : "them") + " (" + firstError(rep) + "). Uninstall it too.";
				return r;
			}
			r.retraced.emplace_back(j, std::make_shared<const sc::Script>(MakeTrace(*before, db, mods[j].mod)));
		}
		r.notes.push_back(names(clashing) + (clashing.size() == 1 ? " changes" : " change") + " some of what " + goneNames +
			" changed, so " + (stack.size() - which.size() == 1 ? "the mod installed after it was" : "the mods installed after it were") +
			" undone and done again: what " + (clashing.size() == 1 ? "it" : "they") + " set stays.");
		r.removed = which;
		r.ok = true;
		return r;
	}

	// ------------------------------------------------------- changing the list ---

	std::vector<TracedMod> WithUpdate(std::vector<TracedMod> saved, const ModListUpdate& u)
	{
		std::vector<TracedMod> out;
		for (TracedMod& t : saved)
		{
			if (std::any_of(u.remove.begin(), u.remove.end(), [&](const InstalledMod& m) { return SameEntry(m, t.mod); })) continue;
			for (const auto& [m, trace] : u.retrace)
				if (SameEntry(m, t.mod))
				{
					t.trace = trace;
					t.problem.clear();
				}
			out.push_back(std::move(t));
		}
		for (size_t i = 0; i < u.add.size(); ++i)
		{
			TracedMod t;
			t.mod = u.add[i].mod;
			t.trace = u.add[i].trace;
			if (!t.trace) t.problem = "what it changed wasn't kept (it has no trace)";
			t.added = int(i);
			out.push_back(std::move(t));
		}
		return out;
	}

	void FoldUninstall(ModListUpdate& u, const std::vector<TracedMod>& rows, const UninstallResult& r)
	{
		for (const auto& [i, trace] : r.retraced)
		{
			if (i >= rows.size()) continue;
			const TracedMod& t = rows[i];
			if (t.pending())
			{
				if (size_t(t.added) < u.add.size()) u.add[size_t(t.added)].trace = trace;
				continue;
			}
			auto it = std::find_if(u.retrace.begin(), u.retrace.end(), [&](const auto& e) { return SameEntry(e.first, t.mod); });
			if (it != u.retrace.end()) it->second = trace;
			else u.retrace.emplace_back(t.mod, trace);
		}
		std::vector<size_t> dropAdded;
		for (size_t i : r.removed)
		{
			if (i >= rows.size()) continue;
			const TracedMod& t = rows[i];
			if (t.pending())
			{
				dropAdded.push_back(size_t(t.added));
				continue;
			}
			std::erase_if(u.retrace, [&](const auto& e) { return SameEntry(e.first, t.mod); });
			u.remove.push_back(t.mod);
		}
		std::sort(dropAdded.rbegin(), dropAdded.rend());
		for (size_t a : dropAdded)
			if (a < u.add.size()) u.add.erase(u.add.begin() + std::ptrdiff_t(a));
	}

	ModList UpdateModList(const std::string& folder, const ModListUpdate& u, std::vector<std::string>* problems)
	{
		ModList list = ReadModList(folder).value_or(ModList{});
		const fs::path traces = FromUtf8(folder) / kTraceFolder;
		auto problem = [&](const std::string& p)
		{
			if (problems) problems->push_back(p);
		};
		std::error_code ec;
		auto writeTrace = [&](const sc::Script& trace, const std::string& name) -> bool
		{
			fs::create_directories(traces, ec);
			try
			{
				WriteText(FromUtf8(TracePath(folder, name)), sc::Print(trace));
				return true;
			}
			catch (const std::exception& e)
			{
				problem(e.what());
				return false;
			}
		};
		// A new trace's file: the time and the mod, numbered when taken.
		auto newName = [&](const InstalledMod& m)
		{
			std::string stamp = m.installed;
			stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
			const std::string base = FileNamePart((stamp.empty() ? "" : stamp + " ") + m.nameAndVersion());
			std::string name = base + ".amod";
			for (int n = 2; n < 1000; ++n)
			{
				const bool listed = std::any_of(list.mods.begin(), list.mods.end(), [&](const InstalledMod& x) { return Lower(x.trace) == Lower(std::string(kTraceFolder) + "\\" + name); });
				if (!listed && !fs::exists(traces / FromUtf8(name), ec)) break;
				name = base + " (" + std::to_string(n) + ").amod";
			}
			return std::string(kTraceFolder) + "\\" + name;
		};

		for (const auto& [m, trace] : u.retrace)
		{
			auto it = std::find_if(list.mods.begin(), list.mods.end(), [&](const InstalledMod& x) { return SameEntry(x, m); });
			if (it == list.mods.end() || !trace) continue;
			const std::string name = it->trace.empty() ? newName(*it) : it->trace;
			if (writeTrace(*trace, name)) it->trace = name;
			else problem("the trace of " + it->nameAndVersion() + " wasn't updated, so uninstalling it later may set some values back to what "
				"they were before a mod uninstalled since");
		}
		std::vector<std::string> dropped;
		for (const InstalledMod& m : u.remove)
		{
			auto it = std::find_if(list.mods.begin(), list.mods.end(), [&](const InstalledMod& x) { return SameEntry(x, m); });
			if (it == list.mods.end()) continue;
			if (!it->trace.empty()) dropped.push_back(it->trace);
			list.mods.erase(it);
		}
		for (const ModListUpdate::Added& a : u.add)
		{
			InstalledMod m = a.mod;
			m.trace.clear();
			if (a.trace)
			{
				const std::string name = newName(m);
				if (writeTrace(*a.trace, name)) m.trace = name;
				else problem(m.nameAndVersion() + " is listed without its trace, so it can't be uninstalled");
			}
			list.mods.push_back(std::move(m));
		}
		WriteModList(folder, list);
		for (const std::string& d : dropped)
		{
			if (std::any_of(list.mods.begin(), list.mods.end(), [&](const InstalledMod& x) { return x.trace == d; })) continue;
			fs::remove(FromUtf8(TracePath(folder, d)), ec);
			if (ec) problem("can't delete " + TracePath(folder, d) + ": " + ec.message());
		}
		return list;
	}

} // namespace attrib
