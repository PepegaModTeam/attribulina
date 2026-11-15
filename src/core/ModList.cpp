#include "ModList.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace attrib
{

	namespace fs = std::filesystem;

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
		std::string Trimmed(std::string_view s)
		{
			size_t b = 0, e = s.size();
			while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
			while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
			return std::string(s.substr(b, e - b));
		}
		std::string Folded(std::string_view s)
		{
			std::string t = Trimmed(s);
			for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
			return t;
		}

		// A text in quotes, a quote inside it doubled.
		std::string Quoted(const std::string& s)
		{
			std::string q = "\"";
			for (char c : s)
			{
				if (c == '"') q += '"';
				q += c;
			}
			return q + "\"";
		}
		// One line's text in quotes, as written: a line break inside would end it.
		std::string OneLine(const std::string& s)
		{
			std::string t = s;
			for (char& c : t)
				if (c == '\n' || c == '\r') c = ' ';
			return t;
		}

		// A line's words: bare ones end at a space, quoted ones at a quote that
		// isn't doubled. False for a quote that doesn't end.
		bool Words(std::string_view line, std::vector<std::string>& out)
		{
			size_t i = 0;
			while (true)
			{
				while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
				if (i >= line.size()) return true;
				std::string w;
				if (line[i] == '"')
				{
					++i;
					while (true)
					{
						if (i >= line.size()) return false;
						if (line[i] == '"')
						{
							if (i + 1 < line.size() && line[i + 1] == '"')
							{
								w += '"';
								i += 2;
								continue;
							}
							++i;
							break;
						}
						w += line[i++];
					}
				}
				else
				{
					while (i < line.size() && line[i] != ' ' && line[i] != '\t') w += line[i++];
				}
				out.push_back(std::move(w));
			}
		}
	} // namespace

	std::string InstalledMod::name() const
	{
		if (!Trimmed(title).empty()) return title;
		if (script.empty()) return "(a mod without a title)";
		// The file's name, its folders split by either slash (a Windows path read elsewhere).
		const size_t slash = script.find_last_of("\\/");
		return slash == std::string::npos ? script : script.substr(slash + 1);
	}

	std::string InstalledMod::nameAndVersion() const
	{
		return name() + (Trimmed(version).empty() ? "" : " " + version);
	}

	bool SameEntry(const InstalledMod& a, const InstalledMod& b)
	{
		return a.title == b.title && a.version == b.version && a.script == b.script && a.sha256 == b.sha256 && a.installed == b.installed &&
			a.by == b.by && a.trace == b.trace;
	}

	// ------------------------------------------------------------- the file ---

	ModList ModList::Parse(std::string_view text, const std::string& source)
	{
		ModList list;
		InstalledMod* mod = nullptr;
		bool header = false;
		int n = 0;
		size_t b = 0;
		while (b <= text.size())
		{
			size_t e = text.find('\n', b);
			if (e == std::string_view::npos) e = text.size();
			++n;
			std::string_view line = text.substr(b, e - b);
			b = e + 1;
			if (n == 1 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
			if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
			auto fail = [&](const std::string& why)
			{
				throw std::runtime_error(source + ":" + std::to_string(n) + ": " + why);
			};
			const std::string t = Trimmed(line);
			if (t.empty() || t[0] == '#')
			{
				if (e >= text.size()) break;
				continue;
			}
			std::vector<std::string> w;
			if (!Words(t, w)) fail("a quote that doesn't end");
			const std::string& key = w[0];
			auto args = [&](size_t count)
			{
				if (w.size() != count + 1) fail(key + " takes " + std::to_string(count) + (count == 1 ? " text" : " texts"));
			};
			if (!header)
			{
				if (w.size() != 3 || w[0] != "attribulina" || w[1] != "mods") fail("the file starts with `attribulina mods 1`");
				list.version = std::atoi(w[2].c_str());
				if (list.version < 1) fail("`attribulina mods` takes a version number");
				header = true;
			}
			else if (!mod)
			{
				if (key != "mod") fail("`" + key + "` outside a mod: each starts with `mod`");
				if (w.size() > 2) fail("mod takes its title, in quotes when it has spaces");
				list.mods.emplace_back();
				mod = &list.mods.back();
				if (w.size() == 2) mod->title = w[1];
			}
			else if (key == "end")
			{
				args(0);
				mod = nullptr;
			}
			else if (key == "mod")
			{
				fail("mod inside a mod: the one before has no `end`");
			}
			else if (key == "nfsms")
			{
				args(0);
				mod->nfsms = true;
			}
			else if (key == "option")
			{
				args(2);
				mod->options.emplace_back(w[1], w[2]);
			}
			else if (key == "file")
			{
				args(1);
				mod->files.push_back(w[1]);
			}
			else
			{
				std::string* field = key == "version" ? &mod->version : key == "author" ? &mod->author : key == "script" ? &mod->script
					: key == "sha256" ? &mod->sha256 : key == "game" ? &mod->game : key == "installed" ? &mod->installed
					: key == "by" ? &mod->by : key == "result" ? &mod->result : key == "trace" ? &mod->trace : nullptr;
				if (field)
				{
					args(1);
					*field = w[1];
				}
				else
				{
					// A later version's: kept for the next one that writes the file.
					mod->unknown.push_back(t);
				}
			}
			if (e >= text.size()) break;
		}
		if (!header) throw std::runtime_error(source + ": the file is empty: it starts with `attribulina mods 1`");
		if (mod) throw std::runtime_error(source + ":" + std::to_string(n) + ": the last mod has no `end`");
		return list;
	}

	std::string ModList::print() const
	{
		std::string s;
		s += "# The mods installed in this game, in the order they were installed. The\n"
			 "# Attribulina Mod Installer adds each mod it installs here, and so do the\n"
			 "# editor and attribcli. The game doesn't read this file, and Attribulina\n"
			 "# doesn't need it: deleting it forgets the list.\n";
		s += "attribulina mods " + std::to_string(std::max(version, kModListVersion)) + "\n";
		for (const InstalledMod& m : mods)
		{
			s += "\nmod";
			if (!m.title.empty()) s += " " + Quoted(OneLine(m.title));
			s += "\n";
			auto line = [&](const char* key, const std::string& v)
			{
				if (!v.empty()) s += std::string("    ") + key + " " + Quoted(OneLine(v)) + "\n";
			};
			line("version", m.version);
			line("author", m.author);
			line("script", m.script);
			if (m.nfsms) s += "    nfsms\n";
			if (!m.sha256.empty()) s += "    sha256 " + m.sha256 + "\n";
			if (!m.game.empty()) s += "    game " + m.game + "\n";
			line("installed", m.installed);
			line("by", m.by);
			line("result", m.result);
			for (const auto& [k, v] : m.options) s += "    option " + Quoted(OneLine(k)) + " " + Quoted(OneLine(v)) + "\n";
			for (const std::string& f : m.files) s += "    file " + Quoted(OneLine(f)) + "\n";
			line("trace", m.trace);
			for (const std::string& u : m.unknown) s += "    " + u + "\n";
			s += "end\n";
		}
		return s;
	}

	std::string ModListPath(const std::string& folder)
	{
		return ToUtf8(FromUtf8(folder) / kModListFile);
	}

	std::optional<ModList> ReadModList(const std::string& folder)
	{
		const fs::path p = FromUtf8(ModListPath(folder));
		std::error_code ec;
		if (!fs::exists(p, ec)) return std::nullopt;
		std::ifstream in(p, std::ios::binary);
		if (!in) throw std::runtime_error("can't read " + ToUtf8(p));
		std::ostringstream s;
		s << in.rdbuf();
		return ModList::Parse(s.str(), ToUtf8(p));
	}

	void WriteModList(const std::string& folder, const ModList& list)
	{
		const fs::path p = FromUtf8(ModListPath(folder));
		fs::path tmp = p;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			const std::string text = list.print();
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

	ModList AddToModList(const std::string& folder, const std::vector<InstalledMod>& mods)
	{
		ModList list = ReadModList(folder).value_or(ModList{});
		list.mods.insert(list.mods.end(), mods.begin(), mods.end());
		WriteModList(folder, list);
		return list;
	}

	std::string ModListFileName(const std::string& path, const std::string& gameFolder)
	{
		std::error_code ec;
		const fs::path abs = fs::absolute(FromUtf8(path), ec).lexically_normal();
		const fs::path base = fs::absolute(FromUtf8(gameFolder), ec).lexically_normal();
		const fs::path rel = abs.lexically_relative(base);
		std::string r = ToUtf8(rel);
		if (gameFolder.empty() || r.empty() || r == "." || r.rfind("..", 0) == 0) r = path;
		std::replace(r.begin(), r.end(), '/', '\\');
		return r;
	}

	// ------------------------------------------------------------- matching ---

	std::vector<ModMatch> FindInstalled(const ModList& list, const ModIdentity& mod)
	{
		std::vector<ModMatch> out;
		const std::string title = Folded(mod.title), version = Folded(mod.version);
		for (const InstalledMod& m : list.mods)
		{
			const bool sameFile = !mod.sha256.empty() && Folded(m.sha256) == Folded(mod.sha256);
			const bool sameTitle = !title.empty() && Folded(m.title) == title;
			if (!sameFile && !sameTitle) continue;
			out.push_back({ &m, sameFile || (sameTitle && Folded(m.version) == version) });
		}
		return out;
	}

	std::string InstalledNote(const std::vector<ModMatch>& matches, const ModIdentity& mod)
	{
		if (matches.empty()) return {};
		const ModMatch& last = matches.back();
		const InstalledMod& m = *last.mod;
		std::string name = m.name();
		if (!m.version.empty()) name += " " + m.version;
		const std::string when = m.installed.empty() ? std::string() : " (installed " + m.installed + ")";
		if (last.sameVersion)
			return name + " is installed here already" + when + ". Installing it again runs its script over its own changes, "
				"which can fail: a line that adds a node finds it there.";
		const std::string now = mod.version.empty() ? std::string("this version") : "version " + mod.version;
		return name + " is installed here" + when + ". Installing " + now + " over it runs its script over the other version's "
			"changes, which can fail where both change the same things.";
	}

} // namespace attrib
