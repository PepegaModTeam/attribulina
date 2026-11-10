// nfsms: reading NFS-VltEd / Attribulator mod scripts and running them
// (Nfsms.h, doc/nfsms.md). Value commands become mod script statements and go
// through script::Apply; the others call the edit API the way those tools do.

#include "Nfsms.h"

#include "Edit.h"
#include "Game.h"
#include "Hash.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace attrib::nfsms
{

	namespace sc = attrib::script;
	namespace fs = std::filesystem;
	using K = sc::Statement::Kind;

	namespace
	{

		std::string Lower(std::string s)
		{
			for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		bool IsHex(std::string_view s)
		{
			if (s.size() < 3 || s.size() > 10 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
			for (size_t i = 2; i < s.size(); ++i)
				if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
			return true;
		}

		bool IsNumber(const std::string& s)
		{
			if (s.empty()) return false;
			char* end = nullptr;
			std::strtod(s.c_str(), &end);
			return end && !*end;
		}

		uint32_t KeyOf(const std::string& t)
		{
			return IsHex(t) ? uint32_t(std::strtoul(t.c_str() + 2, nullptr, 16)) : StringHash32(t);
		}

		std::string Hex32(uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "0x%08X", v);
			return b;
		}

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		// A path as a script writes it, with \ between folders.
		fs::path Written(std::string s)
		{
			std::replace(s.begin(), s.end(), '\\', '/');
			return FromUtf8(s);
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}

		// A path inside the target folder: relative, without "..".
		bool SafeRelative(const std::string& p)
		{
			const fs::path f = Written(p);
			if (p.empty() || f.is_absolute() || f.has_root_name() || f.has_root_directory()) return false;
			for (const fs::path& part : f)
				if (part == "..") return false;
			return true;
		}

		// What a command sets: the value, a reference's class or collection, or
		// (NFS-VltEd's Data) the value's bytes.
		enum class RefPart
		{
			Whole, Class, Collection, Data
		};

	} // namespace

	const std::vector<std::string>& CommandNames()
	{
		static const std::vector<std::string> names = {
			"version", "game", "add_field", "delete_field", "resize_field", "copy_fields", "update_field", "convert_field",
			"file_copy", "add_node", "change_vault", "copy_node", "delete_nodes", "delete_node", "rename_node", "move_node",
			"script", "ui_image", "ui_text", "ui_control", "ui_option", "folder_create", "update_collection",
			"resize_collection",
			// Attribulator's additions.
			"append_array", "add_overwrite", "copy_overwrite" };
		return names;
	}

	size_t CommandAt(std::string_view line)
	{
		size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
		size_t best = 0;
		for (const std::string& n : CommandNames())
			if (n.size() > best && line.substr(i, n.size()) == n) best = n.size();
		return best;
	}

	std::string ScriptFile(const std::string& folder, const std::string& written)
	{
		return ToUtf8(FromUtf8(folder) / Written(written));
	}

	bool Script::hasDescription() const
	{
		for (const std::string& t : texts)
			if (t.find_first_not_of(" \t") != std::string::npos) return true;
		return false;
	}

	Command ParseLine(std::string_view line, int lineNumber)
	{
		Command c;
		c.line = lineNumber;
		size_t i = 0;
		auto space = [&](char ch)
		{
			return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
		};
		auto rest = [&](size_t from)
		{
			std::string t(line.substr(std::min(from, line.size())));
			while (!t.empty() && space(t.back())) t.pop_back();
			return t;
		};
		while (i < line.size() && space(line[i])) ++i;
		if (i >= line.size()) return c;
		if (line[i] == '#' || line.substr(i, 2) == "//")
		{
			c.comment = rest(i + (line[i] == '#' ? 1 : 2));
			return c;
		}
		// NFS-VltEd's pattern is ^\s*(command|...)\s*(params up to #): the command
		// needs no space after it, and a line without one is a comment (mods
		// leave plain notes and stray characters between commands).
		const size_t n = CommandAt(line.substr(i));
		if (!n)
		{
			c.comment = rest(i);
			return c;
		}
		c.name = std::string(line.substr(i, n));
		i += n;
		if (c.name == "ui_text")
		{
			// The rest of the line up to a # is the text; "ui_text#" is a blank line.
			while (i < line.size() && space(line[i])) ++i;
			const size_t hash = line.find('#', i);
			std::string t(line.substr(i, hash == std::string_view::npos ? std::string_view::npos : hash - i));
			while (!t.empty() && space(t.back())) t.pop_back();
			c.args.push_back(t);
			if (hash != std::string_view::npos) c.comment = rest(hash + 1);
			return c;
		}
		while (i < line.size())
		{
			while (i < line.size() && space(line[i])) ++i;
			if (i >= line.size()) break;
			if (line[i] == '#')
			{
				c.comment = rest(i + 1);
				break;
			}
			if (line[i] == '"')
			{
				const size_t e = line.find('"', i + 1);
				const size_t end = e == std::string_view::npos ? line.size() : e;
				c.args.emplace_back(line.substr(i + 1, end - i - 1));
				i = e == std::string_view::npos ? line.size() : e + 1;
				continue;
			}
			const size_t b = i;
			while (i < line.size() && !space(line[i]) && line[i] != '#') ++i;
			c.args.emplace_back(line.substr(b, i - b));
		}
		return c;
	}

	Script Parse(std::string_view text, const std::string& source, const std::string& folder)
	{
		Script s;
		s.source = source;
		s.folder = folder;
		int n = 0;
		size_t b = 0;
		int radioPanel = -1; // the setup item the last radio button went into
		while (b <= text.size())
		{
			size_t e = text.find('\n', b);
			if (e == std::string_view::npos) e = text.size();
			++n;
			Command c = ParseLine(text.substr(b, e - b), n);
			b = e + 1;
			if (c.name.empty())
			{
				if (e >= text.size()) break;
				continue;
			}
			if (c.name == "ui_text")
			{
				s.texts.push_back(c.args.empty() ? std::string() : c.args[0]);
				s.setup.push_back({ SetupItem::Kind::Text, s.texts.back(), {} });
			}
			else if (c.name == "ui_image")
			{
				if (c.args.size() == 1) s.images.push_back(c.args[0]);
				else c.error = "ui_image takes one file";
			}
			else if (c.name == "ui_option")
			{
				// NFS-VltEd can't read a bare ui_option, so it doesn't end a block there.
				if (c.args.empty() || c.args.size() > 2)
					c.error = "ui_option takes a checkbox name, or a radio button group and name";
			}
			else if (c.name == "ui_control")
			{
				Control ctl;
				ctl.line = n;
				const std::string type = c.args.empty() ? std::string() : Lower(c.args[0]);
				if (type == "checkbox" && c.args.size() == 4 && (Lower(c.args[2]) == "true" || Lower(c.args[2]) == "false"))
				{
					ctl.kind = Control::Kind::Checkbox;
					ctl.name = c.args[1];
					ctl.checked = Lower(c.args[2]) == "true";
					ctl.label = c.args[3];
					s.setup.push_back({ SetupItem::Kind::Checkbox, {}, {int(s.controls.size())} });
					s.controls.push_back(ctl);
				}
				else if (type == "radiobutton" && c.args.size() == 4)
				{
					ctl.kind = Control::Kind::Radio;
					ctl.group = c.args[1];
					ctl.name = c.args[2];
					ctl.label = c.args[3];
					// A new panel when the group differs from the last radio button's.
					if (radioPanel < 0 || s.controls[s.setup[size_t(radioPanel)].controls.back()].group != ctl.group)
					{
						radioPanel = int(s.setup.size());
						s.setup.push_back({ SetupItem::Kind::Radios, {}, {} });
					}
					s.setup[size_t(radioPanel)].controls.push_back(int(s.controls.size()));
					s.controls.push_back(ctl);
				}
				else
				{
					c.error = "ui_control is checkbox <name> <true|false> \"label\" or radiobutton <group> <name> \"label\"";
				}
			}
			s.commands.push_back(std::move(c));
			if (e >= text.size()) break;
		}
		return s;
	}

	// ------------------------------------------------------------ names ---

	const PropertyNames::Property* PropertyNames::Type::find(std::string_view word) const
	{
		for (const Property& p : properties)
			if (p.word == word) return &p;
		const std::string l = Lower(std::string(word));
		for (const Property& p : properties)
			if (Lower(p.word) == l) return &p;
		return nullptr;
	}

	const PropertyNames::Property* PropertyNames::Type::at(uint32_t offset) const
	{
		for (const Property& p : properties)
			if (p.offset == offset) return &p;
		return nullptr;
	}

	const PropertyNames::Property* PropertyNames::Type::atPath(std::string_view path) const
	{
		const std::string l = Lower(std::string(path));
		for (const Property& p : properties)
			if (!p.path.empty() && Lower(p.pathText()) == l) return &p;
		return nullptr;
	}

	bool PropertyNames::Type::hasPaths() const
	{
		return std::any_of(properties.begin(), properties.end(), [](const Property& p)
		{
			return !p.path.empty();
		});
	}

	std::string PropertyNames::Property::pathText() const
	{
		std::string s;
		for (const Step& st : path)
		{
			if (!s.empty()) s += '.';
			s += st.member;
			if (st.index >= 0) s += "[" + std::to_string(st.index) + "]";
		}
		if (rest) s += "+" + std::to_string(rest);
		return s;
	}

	const PropertyNames::Type* PropertyNames::find(const std::string& typeName) const
	{
		// As written, else with the prefix the editor's type labels leave off
		// (TypeLabel: Vector3 for Attrib::Types::Vector3).
		for (const char* prefix : { "", "Attrib::Types::", "Attrib::", "EA::Reflection::" })
			if (auto it = types.find(prefix + typeName); it != types.end()) return &it->second;
		return nullptr;
	}

	PropertyNames PropertyNames::Parse(std::string_view text, const std::string& source)
	{
		PropertyNames n;
		Type* cur = nullptr;
		int line = 0;
		size_t b = 0;
		while (b < text.size())
		{
			size_t e = text.find('\n', b);
			if (e == std::string_view::npos) e = text.size();
			std::string l(text.substr(b, e - b));
			b = e + 1;
			++line;
			if (const size_t h = l.find('#'); h != std::string::npos) l.erase(h);
			std::istringstream in(l);
			std::string w;
			if (!(in >> w)) continue;
			auto bad = [&](const std::string& why)
			{
				throw std::runtime_error(source + ":" + std::to_string(line) + ": " + why);
			};
			if (w == "game")
			{
				in >> n.game;
			}
			else if (w == "type")
			{
				std::string t;
				if (!(in >> t)) bad("type needs a name");
				cur = &n.types[t];
			}
			else if (w == "end")
			{
				cur = nullptr;
			}
			else if (!cur)
			{
				bad("'" + w + "' outside a type");
			}
			else if (w == "single")
			{
				cur->single = true;
			}
			else
			{
				Property p;
				p.word = w;
				if (!(in >> p.offset)) bad(w + " needs the offset it sets");
				std::string k;
				if (in >> k && k != "list")
				{
					// The member path: name[.name...], each with an optional [index],
					// then +bytes into the last one.
					if (const size_t plus = k.find('+'); plus != std::string::npos)
					{
						char* stop = nullptr;
						p.rest = uint32_t(std::strtoul(k.c_str() + plus + 1, &stop, 10));
						if (plus + 1 == k.size() || *stop) bad("bad member path " + k);
						k.erase(plus);
					}
					size_t i = 0;
					while (i < k.size())
					{
						Step st;
						const size_t end = k.find_first_of(".[", i);
						st.member = k.substr(i, end == std::string::npos ? std::string::npos : end - i);
						if (st.member.empty()) bad("bad member path " + k);
						i = end == std::string::npos ? k.size() : end;
						if (i < k.size() && k[i] == '[')
						{
							const size_t close = k.find(']', i);
							if (close == std::string::npos) bad("bad member path " + k);
							char* stop = nullptr;
							st.index = std::strtoll(k.c_str() + i + 1, &stop, 10);
							if (stop != k.c_str() + close || st.index < 0) bad("bad index in " + k);
							i = close + 1;
						}
						if (i < k.size() && k[i] != '.') bad("bad member path " + k);
						if (i < k.size()) ++i;
						p.path.push_back(std::move(st));
					}
					if (!(in >> k)) k.clear();
				}
				if (!k.empty())
				{
					if (k != "list") bad("unknown '" + k + "' after " + w);
					p.list = true;
					for (std::string el; in >> el;) p.elements.push_back(el);
				}
				cur->properties.push_back(std::move(p));
			}
		}
		return n;
	}

	std::shared_ptr<const PropertyNames> PropertyNames::Load(const std::string& path)
	{
		std::ifstream in(FromUtf8(path), std::ios::binary);
		if (!in) throw std::runtime_error("can't read " + path);
		std::ostringstream ss;
		ss << in.rdbuf();
		return std::make_shared<const PropertyNames>(Parse(ss.str(), ToUtf8(FromUtf8(path).filename())));
	}

	std::shared_ptr<const PropertyNames> NamesFor(const Database& db, const std::vector<std::shared_ptr<const PropertyNames>>& tables)
	{
		const std::string game = GameCode(db);
		for (const auto& t : tables)
			if (t && !game.empty() && t->game == game) return t;
		return nullptr;
	}

	Choices DefaultChoices(const Script& script)
	{
		Choices c;
		for (const Control& ctl : script.controls)
		{
			if (ctl.kind == Control::Kind::Checkbox) c.checkboxes[ctl.name] = ctl.checked;
			else if (ctl.checked) c.radios[ctl.group] = ctl.name;
		}
		return c;
	}

	std::string GameCode(const Database& db)
	{
		switch (db.target().game)
		{
			case Game::MostWanted: return "MW";
			case Game::Carbon: return "C";
			case Game::ProStreet: return "PS";
			case Game::Undercover: return "UC";
			case Game::World:
				// NFS-VltEd's WB: the betas, by the database vault's Vers.
				for (const auto& p : db.packs())
					for (const auto& v : p->vaults)
						if (v->isDatabase) return IsWorldBetaVersion(v->raw.version) ? "WB" : "W";
				return "W";
			case Game::Nitro: case Game::Unknown: break;
		}
		return {};
	}

	std::string Incompatibility(const Database& db)
	{
		const Target& t = db.target();
		if (!t.known() || !GameCode(db).empty()) return {};
		return "nfsms scripts are NFS-VltEd's, which never read " + t.name() + "'s game";
	}

	// ---------------------------------------------------------------- running ---

	namespace
	{

		class Runner
		{
		public:
			Runner(Database& db, const RunSettings& st) : db_(db), st_(st)
			{
			}

			// ToModScript: the statements each command stands for go to `out` (it
			// still runs, on a scratch copy). Setup blocks become `if`s on the options.
			struct Options
			{
				std::map<std::string, std::string> boxes;                            // checkbox -> option id
				std::map<std::string, std::string> groups;                           // radio group -> option id
				std::map<std::pair<std::string, std::string>, std::string> choices;  // group, name -> the choice's text
			};
			sc::ApplyReport convert(const Script& s, std::vector<sc::Statement>& out, const Options& options)
			{
				out_ = &out;
				options_ = &options;
				return run(s);
			}

			sc::ApplyReport run(const Script& s)
			{
				try
				{
					runScript(s, 0);
					rep_.ok = true;
					checkReferences();
				}
				catch (const Stopped&)
				{
					// strict: the first error stopped it
				}
				return rep_;
			}

		private:
			struct Stopped
			{
			};
			[[noreturn]] void fail(const std::string& m) const
			{
				throw sc::ScriptError({ line_, 1 }, m, source_);
			}
			void message(sc::Message::Level l, const std::string& t)
			{
				// An included script's lines are named by file, and carry where they are.
				if (!depth_)
				{
					rep_.messages.push_back({ l, {line_, 1}, t });
					return;
				}
				rep_.messages.push_back({ l, {lineTop_, 1}, source_ + ":" + std::to_string(line_) + ": " + t, path_, line_ });
			}
			void warn(const std::string& t)
			{
				if (st_.strict) fail(t + " (strict)");
				message(sc::Message::Level::Warning, t);
			}

			// -------------------------------------------------- converting ---

			std::vector<sc::Statement>* out_ = nullptr;
			const Options* options_ = nullptr;
			sc::Target targetOf(const Collection& x) const
			{
				sc::Target t;
				t.cls = name(x.classKey);
				t.name = name(x.key);
				return t;
			}
			sc::Target root() const
			{
				sc::Target t;
				t.root = true;
				return t;
			}
			void emit(sc::Statement st)
			{
				if (!out_) return;
				// Edits of the same collection one after another make one block.
				if (st.kind == K::Edit && !out_->empty() && out_->back().kind == K::Edit && out_->back().target.cls == st.target.cls &&
					out_->back().target.name == st.target.name)
				{
					for (sc::Statement& b : st.body) out_->back().body.push_back(std::move(b));
					return;
				}
				out_->push_back(std::move(st));
			}
			void emitComment(const std::string& text)
			{
				sc::Statement c;
				c.kind = K::Comment;
				c.comment = " " + text;
				emit(std::move(c));
			}
			void emitEdit(const Collection& x, std::vector<sc::Statement> body)
			{
				if (!out_ || body.empty()) return;
				sc::Statement e;
				e.kind = K::Edit;
				e.target = targetOf(x);
				e.body = std::move(body);
				emit(std::move(e));
			}
			// The `if` a ui_option line opens: on the checkbox's option, or the group's
			// option being that choice.
			sc::Statement optionIf(const Command& c) const
			{
				std::string cond;
				if (c.args.size() == 1)
				{
					auto it = options_->boxes.find(c.args[0]);
					cond = "$" + (it != options_->boxes.end() ? it->second : c.args[0]);
				}
				else
				{
					auto g = options_->groups.find(c.args[0]);
					auto ch = options_->choices.find({ c.args[0], c.args[1] });
					cond = "$" + (g != options_->groups.end() ? g->second : c.args[0]) + " == " +
						sc::Quote(ch != options_->choices.end() ? ch->second : c.args[1]);
				}
				const sc::Script one = sc::Parse("attribulina 1\nif " + cond + " {\n}\n", "convert");
				return one.statements.back();
			}

			// References set to collections that aren't there, checked once the whole
			// script ran: mods set a reference's collection a line before its class,
			// and point at collections they make further down.
			struct SetRef
			{
				uint32_t cls, key;
				sc::FieldPath path;
				int line, lineTop, depth;
				std::string source, file;
				std::string written; // the value as the script wrote it: a name the dictionary may not know
			};
			std::vector<SetRef> setRefs_;
			void checkReferences()
			{
				std::set<std::pair<uint64_t, std::string>> seen;
				for (const SetRef& r : setRefs_)
				{
					if (!seen.insert({ (uint64_t(r.cls) << 32) | r.key, r.path.text() }).second) continue;
					Collection* x = db_.findCollection(r.cls, r.key);
					if (!x) continue; // deleted since
					Bytes cur;
					ValueType rt;
					try
					{
						rt = sc::TypeAt(db_, *x, r.path, &cur);
						if ((rt.kind != TypeKind::RefSpec || cur.size() < 8) && (rt.kind != TypeKind::ClassRefSpec || cur.size() < 4)) continue;
					}
					catch (const std::exception&)
					{
						continue;
					}
					const Endian e = x->vault ? x->vault->raw.endian : Endian::Little;
					const RefSpecLayout rl = RefSpecAt(uint32_t(cur.size()));
					const bool one = rt.kind == TypeKind::ClassRefSpec; // the class is the type's
					const uint32_t ck = one ? ClassRefSpecClass(rt.typeName) : LoadKey(db_, cur.data() + rl.cls, rl.keySize, e);
					const uint32_t k = one ? load32(cur.data(), e) : LoadKey(db_, cur.data() + rl.col, rl.keySize, e);
					if (!k || db_.findCollection(ck, k)) continue;
					line_ = r.line;
					lineTop_ = r.lineTop;
					depth_ = r.depth;
					source_ = r.source;
					path_ = r.file;
					const std::string t = "no collection " + name(ck) + "/" + (KeyOf(r.written) == k ? r.written : name(k)) +
						" for the reference " + r.path.text() + " (set anyway)";
					if (st_.strict)
					{
						// Warnings are errors: the run fails at the line that set it.
						message(sc::Message::Level::Error, t + " (strict)");
						rep_.ok = false;
						return;
					}
					warn(t);
				}
			}
			void lineFailed(const std::string& t)
			{
				message(sc::Message::Level::Error, t);
				if (st_.strict) throw Stopped{};
			}
			void note(const std::string& t)
			{
				message(sc::Message::Level::Info, t);
			}
			void need(const Command& c, size_t lo, size_t hi) const
			{
				if (c.args.size() < lo || c.args.size() > hi)
					fail(c.name + " takes " + (lo == hi ? std::to_string(lo) : std::to_string(lo) + " to " + std::to_string(hi)) +
						 " parameters, not " + std::to_string(c.args.size()));
			}
			std::string name(uint32_t k) const
			{
				return db_.names().format(k);
			}
			std::string label(const Collection& c) const
			{
				return name(c.classKey) + "/" + name(c.key);
			}
			void learn(const std::string& n)
			{
				if (n.empty() || IsHex(n) || db_.names().find(StringHash32(n))) return;
				db_.names().add(n);
				rep_.newNames.push_back(n);
			}
			void touch(Vault* v)
			{
				if (!v || !v->pack) return;
				v->pack->dirty = true;
				rep_.touched.insert(v->pack);
			}

			ClassDef& cls(const std::string& t)
			{
				ClassDef* k = db_.findClass(KeyOf(t));
				if (!k) fail("class '" + t + "' does not exist");
				return *k;
			}
			Collection* find(const ClassDef& k, const std::string& t)
			{
				return db_.findCollection(k.key, KeyOf(t));
			}
			Collection& node(const ClassDef& k, const std::string& t)
			{
				Collection* c = find(k, t);
				if (!c) fail("class '" + name(k.key) + "' does not contain node '" + t + "'");
				learn(t);
				return *c;
			}

			// ---------------------------------------------------- structure ---

			void runScript(const Script& s, int depth)
			{
				const std::string savedSource = source_, savedPath = path_;
				const int savedDepth = depth_;
				source_ = s.source;
				path_ = s.path;
				depth_ = depth;
				bool inOption = false, skip = false, skipNoted = false;
				std::vector<sc::Statement>* blockOut = nullptr; // converting: where statements go after the block
				for (const Command& c : s.commands)
				{
					line_ = c.line;
					if (depth == 0) lineTop_ = c.line;
					const bool inRange = depth != 0 || !st_.firstLine || (c.line >= st_.firstLine && c.line <= st_.lastLine);
					if (c.name == "ui_option")
					{
						// A ui_option line opens a block that runs when its control is
						// set; the next one closes it, whatever it names (mods repeat
						// the opening line), as in NFS-VltEd.
						if (!c.error.empty())
						{
							if (inRange) lineFailed(c.error);
						}
						else if (!inOption)
						{
							inOption = true;
							skip = !out_ && !active(c); // converting: every block, as an `if`
							skipNoted = false;
							if (out_)
							{
								blockOut = out_;
								out_->push_back(optionIf(c));
								out_ = &out_->back().body;
							}
						}
						else
						{
							inOption = false;
							if (blockOut) out_ = blockOut;
							blockOut = nullptr;
						}
						continue;
					}
					if (c.name == "script" && depth >= 16)
					{
						lineFailed("scripts include each other too deep");
						continue;
					}
					if (!inRange) continue;
					if (inOption && skip)
					{
						// Running chosen lines: say why they did nothing.
						if (depth == 0 && st_.firstLine && !skipNoted)
						{
							note("skipped: the setup choice its ui_option block needs isn't made");
							skipNoted = true;
						}
						continue;
					}
					// A line that fails is reported and the next one runs, as in NFS-VltEd.
					try
					{
						if (!c.error.empty()) fail(c.error);
						exec(c, s, depth);
					}
					catch (const Stopped&)
					{
						throw;
					}
					catch (const sc::ScriptError& e)
					{
						if (out_) emitComment("could not convert " + c.name + " (" + source_ + ":" + std::to_string(c.line) + "): " + e.message);
						lineFailed(e.message);
					}
					catch (const std::exception& e)
					{
						if (out_) emitComment("could not convert " + c.name + " (" + source_ + ":" + std::to_string(c.line) + "): " + e.what());
						lineFailed(e.what());
					}
				}
				if (blockOut) out_ = blockOut; // a block left open at the end of the file
				source_ = savedSource;
				path_ = savedPath;
				depth_ = savedDepth;
			}

			// Whether the control a ui_option names (one checkbox, or a radio button
			// group and name; Parse checked the count) is set.
			bool active(const Command& c)
			{
				if (c.args.size() == 2)
				{
					auto it = st_.choices.radios.find(c.args[0]);
					return it != st_.choices.radios.end() && it->second == c.args[1];
				}
				auto it = st_.choices.checkboxes.find(c.args[0]);
				return it != st_.choices.checkboxes.end() && it->second;
			}

			void exec(const Command& c, const Script& s, int depth)
			{
				const std::string& n = c.name;
				if (n == "version")
				{
					need(c, 1, 1);
					if (c.args[0] != "4.6") warn("written for NFS-VltEd " + c.args[0] + "; read as 4.6");
				}
				else if (n == "game")
				{
					const std::string mine = GameCode(db_);
					bool ok = mine.empty() || c.args.empty();
					for (const std::string& g : c.args) ok |= Lower(g) == Lower(mine);
					if (!ok)
					{
						std::string list;
						for (const std::string& g : c.args) list += (list.empty() ? "" : ", ") + g;
						warn("the script is for " + list + "; this database is " + mine);
					}
				}
				else if (n == "ui_text" || n == "ui_image" || n == "ui_control")
				{
					// The setup; shown before running.
					if (n == "ui_image" && !c.args.empty()) emitComment("picture " + c.args[0] + " (a mod script shows none)");
				}
				else if (n == "script")
				{
					need(c, 1, 1);
					emitComment("from " + c.args[0]);
					const fs::path p = FromUtf8(s.folder) / Written(c.args[0]);
					std::string text;
					if (st_.readFile)
					{
						text = st_.readFile(ToUtf8(p));
					}
					else
					{
						std::ifstream in(p, std::ios::binary);
						if (!in) fail("can't read " + ToUtf8(p));
						std::ostringstream ss;
						ss << in.rdbuf();
						text = ss.str();
					}
					Script sub = Parse(text, c.args[0], ToUtf8(p.parent_path()));
					sub.path = ToUtf8(p);
					runScript(sub, depth + 1);
				}
				else if (n == "file_copy")
				{
					need(c, 2, 2);
					emitComment("file_copy " + c.args[0] + " " + c.args[1] + ": copy it into the game's folder yourself");
					fileCopy(c, s);
				}
				else if (n == "folder_create")
				{
					need(c, 1, 1);
					if (!SafeRelative(c.args[0])) fail("folder_create needs a folder inside the game's: " + c.args[0]);
					emitComment("folder_create " + c.args[0] + ": make it in the game's folder yourself");
					if (st_.doFiles && !st_.targetFolder.empty())
					{
						std::error_code ec;
						fs::create_directories(FromUtf8(st_.targetFolder) / Written(c.args[0]), ec);
						if (ec) fail("can't make " + c.args[0] + ": " + ec.message());
						note("made the folder " + c.args[0]);
					}
				}
				else if (n == "update_field" || n == "update_collection")
				{
					update(c);
				}
				else if (n == "add_node" || n == "add_overwrite")
				{
					addNode(c, n == "add_overwrite");
				}
				else if (n == "copy_node" || n == "copy_overwrite")
				{
					copyNode(c, n == "copy_overwrite");
				}
				else if (n == "delete_node")
				{
					need(c, 2, 2);
					remove(node(cls(c.args[0]), c.args[1]));
				}
				else if (n == "delete_nodes")
				{
					need(c, 1, 1);
					ClassDef& k = cls(c.args[0]);
					emitComment("delete_nodes " + c.args[0] + ": every one there was");
					std::vector<Collection*> roots;
					for (Collection* x : k.collections)
						if (!x->parent) roots.push_back(x);
					for (Collection* r : roots) remove(*r);
				}
				else if (n == "rename_node")
				{
					need(c, 3, 3);
					ClassDef& k = cls(c.args[0]);
					Collection& x = node(k, c.args[1]);
					if (find(k, c.args[2])) fail("there is already a node called '" + c.args[2] + "'");
					if (IsHex(c.args[2])) fail("the new name is a name, not a hash");
					const bool known = db_.names().find(StringHash32(c.args[2])) != nullptr;
					sc::Statement r;
					r.kind = K::Rename;
					r.target = targetOf(x);
					r.word = c.args[2];
					emit(std::move(r));
					CollectionEditor(db_).rename(x, c.args[2]);
					if (!known) rep_.newNames.push_back(c.args[2]);
					setCollectionName(x, c.args[2]);
					touch(x.vault);
					++rep_.renamed;
				}
				else if (n == "move_node")
				{
					need(c, 2, 3);
					ClassDef& k = cls(c.args[0]);
					Collection& x = node(k, c.args[1]);
					Collection* parent = c.args.size() == 3 ? &node(k, c.args[2]) : nullptr;
					if (parent == x.parent && (parent || !x.parentKey)) return;
					if (!CollectionEditor(db_).move(x, parent)) fail("'" + c.args[2] + "' is under '" + c.args[1] + "'");
					sc::Statement m;
					m.kind = K::Move;
					m.target = targetOf(x);
					m.hasUnder = true;
					m.under = parent ? targetOf(*parent) : root();
					emit(std::move(m));
					touch(x.vault);
					++rep_.moved;
				}
				else if (n == "change_vault")
				{
					need(c, 3, 3);
					changeVault(c);
				}
				else if (n == "resize_field")
				{
					need(c, 4, 4);
					ClassDef& k = cls(c.args[0]);
					Collection& x = node(k, c.args[1]);
					const FieldDef* f = k.field(KeyOf(c.args[2]));
					if (!f) fail("the class '" + c.args[0] + "' does not contain '" + c.args[2] + "'");
					if (!f->isArray()) fail("'" + c.args[2] + "' is not an array");
					sc::Statement r;
					r.kind = K::Resize;
					r.path.field = c.args[2];
					r.count = uint32_t(std::strtoul(c.args[3].c_str(), nullptr, 10));
					amod(x, { r });
				}
				else if (n == "resize_collection")
				{
					need(c, 4, 64);
					ClassDef& k = cls(c.args[0]);
					Collection& x = node(k, c.args[1]);
					sc::FieldPath p = fieldPath(x, c.args[2]);
					RefPart part = RefPart::Whole;
					for (size_t i = 3; i + 1 < c.args.size(); ++i) property(x, p, c.args[i], part);
					sc::Statement r;
					r.kind = K::Resize;
					r.path = p;
					r.count = uint32_t(std::strtoul(c.args.back().c_str(), nullptr, 10));
					amod(x, { r });
				}
				else if (n == "add_field")
				{
					need(c, 3, 4);
					addField(c);
				}
				else if (n == "delete_field")
				{
					need(c, 3, 3);
					ClassDef& k = cls(c.args[0]);
					Collection& x = node(k, c.args[1]);
					const FieldDef* f = k.field(KeyOf(c.args[2]));
					if (!f) fail("the class '" + c.args[0] + "' does not contain '" + c.args[2] + "'");
					if (f->inLayout()) fail("'" + c.args[2] + "' cannot be removed: only optional fields can be removed");
					if (RevertAttribute(x, f->key))
					{
						++rep_.reverted;
						touch(x.vault);
						sc::Statement r;
						r.kind = K::Revert;
						r.path.field = name(f->key);
						emitEdit(x, { r });
					}
				}
				else if (n == "append_array")
				{
					need(c, 3, 4);
					appendArray(c);
				}
				else if (n == "copy_fields")
				{
					need(c, 4, 4);
					copyFields(c);
				}
				else if (n == "convert_field")
				{
					emitComment("convert_field: not run (NFS-VltEd doesn't run it either)");
					warn("convert_field isn't supported (NFS-VltEd doesn't run it either); skipped");
				}
			}

			void remove(Collection& c)
			{
				std::vector<Collection*> all;
				std::function<void(Collection*)> walk = [&](Collection* x)
				{
					all.push_back(x);
					for (Collection* k : x->children) walk(k);
				};
				walk(&c);
				sc::Statement d;
				d.kind = K::Delete;
				d.target = targetOf(c);
				d.withChildren = all.size() > 1;
				emit(std::move(d));
				CollectionEditor ed(db_);
				for (auto it = all.rbegin(); it != all.rend(); ++it)
				{
					if (st_.beforeRemove) st_.beforeRemove(**it);
					touch((*it)->vault);
					ed.remove(**it);
					++rep_.deleted;
				}
			}

			// A class with a CollectionName field keeps the name there too (Attribulator does this).
			void setCollectionName(Collection& c, const std::string& n)
			{
				if (!c.cls || !c.cls->field(StringHash32("CollectionName"))) return;
				sc::Statement s;
				s.kind = K::Set;
				s.path.field = "CollectionName";
				s.value = sc::MakeString(n);
				amod(c, { s });
			}

			Vault* vaultFor(const ClassDef& k, Collection* parent)
			{
				if (parent) return parent->vault;
				for (const auto& pk : db_.packs())
					for (const auto& v : pk->vaults)
						for (const Collection* x : v->collections)
							if (x->cls == &k) return v.get();
				return k.vault;
			}

			void addNode(const Command& c, bool overwrite)
			{
				need(c, 2, 3);
				ClassDef& k = cls(c.args[0]);
				Collection* parent = c.args.size() == 3 ? &node(k, c.args[1]) : nullptr;
				const std::string& n = c.args.back();
				if (n.empty() || n.find_first_of(" \t") != std::string::npos) fail("a node name can't be empty or hold spaces");
				if (Collection* old = find(k, n))
				{
					if (!overwrite) fail("there is already a node called '" + n + "'");
					remove(*old);
				}
				Vault* vault = vaultFor(k, parent);
				if (!vault) fail("can't tell which vault the new node goes in");
				CollectionEditor ed(db_);
				// A new node starts from zeros, not from its parent's layout values,
				// as in NFS-VltEd and Attribulator.
				const bool known = IsHex(n) || db_.names().find(StringHash32(n)) != nullptr;
				Collection& x = IsHex(n) ? ed.create(*vault, k, KeyOf(n), nullptr) : ed.create(*vault, k, n, nullptr);
				if (!known) rep_.newNames.push_back(n);
				if (parent) ed.move(x, parent);
				sc::Statement cr;
				cr.kind = K::Create;
				cr.target = targetOf(x);
				cr.hasUnder = parent != nullptr;
				if (parent) cr.under = targetOf(*parent);
				cr.vault = vault->name;
				emit(std::move(cr));
				setCollectionName(x, n);
				touch(vault);
				++rep_.created;
			}

			void copyNode(const Command& c, bool overwrite)
			{
				need(c, 3, 4);
				ClassDef& k = cls(c.args[0]);
				Collection& src = node(k, c.args[1]);
				Collection* parent = c.args.size() == 4 ? &node(k, c.args[2]) : nullptr;
				const std::string& n = c.args.back();
				if (n.empty() || n.find_first_of(" \t") != std::string::npos) fail("a node name can't be empty or hold spaces");
				if (Collection* old = find(k, n))
				{
					if (!overwrite) fail("there is already a node called '" + n + "'");
					if (old == &src) fail("copy_overwrite can't copy a node over itself");
					remove(*old);
				}
				CollectionEditor ed(db_);
				const bool known = IsHex(n) || db_.names().find(StringHash32(n)) != nullptr;
				Collection& x = IsHex(n) ? ed.create(*src.vault, k, KeyOf(n), nullptr) : ed.create(*src.vault, k, n, nullptr);
				if (!known) rep_.newNames.push_back(n);
				x.layout = src.layout;
				DetachCopy(x.layout);
				x.attributes = src.attributes;
				for (Attribute& a : x.attributes) DetachCopy(a);
				if (parent) ed.move(x, parent);
				sc::Statement cr;
				cr.kind = K::Create;
				cr.target = targetOf(x);
				cr.hasFrom = true;
				cr.from = targetOf(src);
				cr.hasUnder = true; // nfsms makes a copy without a parent a root
				cr.under = parent ? targetOf(*parent) : root();
				cr.vault = x.vault ? x.vault->name : std::string();
				emit(std::move(cr));
				setCollectionName(x, n);
				touch(x.vault);
				++rep_.created;
			}

			void changeVault(const Command& c)
			{
				ClassDef& k = cls(c.args[0]);
				Collection& x = node(k, c.args[1]);
				const std::string& vn = c.args[2];
				Vault* target = nullptr;
				for (const auto& pk : db_.packs())
					if (Vault* v = FindVault(*pk, vn)) target = v;
				Vault* old = x.vault;
				if (!target)
				{
					// A gameplay node may name a new vault: it is made in the same pack.
					if (name(k.key) != "gameplay" || !old || !old->pack) fail("vault '" + vn + "' does not exist");
					target = &CreateVault(*old->pack, vn, *old);
					touch(target);
					note("made the vault " + vn);
					sc::Statement cv;
					cv.kind = K::CreateVault;
					cv.vault = vn;
					cv.word = ToUtf8(FromUtf8(old->pack->path).filename());
					emit(std::move(cv));
				}
				if (target == old) return;
				touch(old);
				sc::Statement mv;
				mv.kind = K::MoveToVault;
				mv.target = targetOf(x);
				mv.vault = vn;
				emit(std::move(mv));
				CollectionEditor(db_).moveToVault(x, *target);
				touch(target);
				++rep_.moved;
				// A gameplay vault left empty goes, as in Attribulator.
				if (old && old->pack && name(k.key) == "gameplay" && old->collections.empty() && old->classes.empty() && !old->isDatabase)
				{
					sc::Statement dv;
					dv.kind = K::DeleteVault;
					dv.vault = old->name;
					emit(std::move(dv));
					RemoveVault(*old->pack, *old);
				}
			}

			void addField(const Command& c)
			{
				ClassDef& k = cls(c.args[0]);
				Collection& x = node(k, c.args[1]);
				const FieldDef* f = k.field(KeyOf(c.args[2]));
				if (!f) fail("the class '" + c.args[0] + "' does not contain '" + c.args[2] + "'");
				if (f->inLayout()) fail("only optional fields can be added; '" + c.args[2] + "' is a base field");
				if (x.find(f->key)) return; // there already
				const uint32_t count = c.args.size() == 4 ? uint32_t(std::strtoul(c.args[3].c_str(), nullptr, 10)) : 0;
				if (f->isArray() && count > 0xFFFF) fail("'" + c.args[2] + "' holds at most 65535 elements");
				// A new value of zeros, not a copy of the inherited one.
				Attribute& a = OverrideAttribute(db_, x, *f);
				if (std::string w = ElementLimitWarning(a, count, c.args[2]); !w.empty()) warn(w);
				a.data.ptrs.clear();
				a.dataSeq = kNewPtr;
				a.count = a.isArray ? count : 1;
				a.capacity = uint16_t(a.isArray ? count : 0);
				a.data.bytes.assign(size_t(a.count) * a.elemSize, 0);
				touch(x.vault);
				++rep_.valuesSet;
				if (out_) emitEdit(x, sc::ExportValue(db_, x, f->key).body);
			}

			void appendArray(const Command& c)
			{
				ClassDef& k = cls(c.args[0]);
				Collection& x = node(k, c.args[1]);
				const FieldDef* f = k.field(KeyOf(c.args[2]));
				if (!f) fail("the class '" + c.args[0] + "' does not contain '" + c.args[2] + "'");
				if (!f->isArray()) fail("'" + c.args[2] + "' is not an array");
				const Attribute* a = x.find(f->key);
				if (!a) fail(label(x) + " has no entry for " + c.args[2]);
				sc::Statement s;
				s.kind = K::Insert;
				s.path.field = c.args[2];
				s.path.steps.push_back({ std::string(), int64_t(a->count) });
				if (c.args.size() == 4)
				{
					const ValueType vt = TypeOfAttribute(*a, c.args[2]);
					Bytes zero(vt.size, 0);
					s.value = value(c.args[3], vt, zero, RefPart::Whole, x);
				}
				amod(x, { s });
			}

			void copyFields(const Command& c)
			{
				ClassDef& k = cls(c.args[0]);
				Collection& src = node(k, c.args[1]);
				Collection& dst = node(k, c.args[2]);
				if (&src == &dst) fail("fields cannot be copied to the same node");
				bool base = false, optional = false, overwrite = false;
				std::stringstream ss(c.args[3]);
				for (std::string o; std::getline(ss, o, '|');)
				{
					o = Lower(o);
					base |= o == "base";
					optional |= o == "optional";
					overwrite |= o == "overwrite";
				}
				std::vector<uint32_t> copied;
				for (const Attribute& a : src.attributes)
				{
					if (a.inLayout ? !base : !optional) continue;
					Attribute copy = a;
					DetachCopy(copy);
					Attribute* there = dst.find(a.key);
					if (there && !a.inLayout && !overwrite) continue;
					if (there) *there = std::move(copy);
					else dst.attributes.push_back(std::move(copy));
					++rep_.valuesSet;
					copied.push_back(a.key);
				}
				touch(dst.vault);
				if (out_)
				{
					std::vector<sc::Statement> body;
					for (uint32_t key : copied)
						for (sc::Statement& v : sc::ExportValue(db_, dst, key).body) body.push_back(std::move(v));
					emitEdit(dst, std::move(body));
				}
			}

			void fileCopy(const Command& c, const Script& s)
			{
				const fs::path from = FromUtf8(s.folder) / Written(c.args[0]);
				std::error_code ec;
				if (!fs::exists(from, ec)) fail("file does not exist: " + c.args[0]);
				if (!SafeRelative(c.args[1])) fail("file_copy needs a destination inside the game's folder: " + c.args[1]);
				if (!st_.doFiles || st_.targetFolder.empty()) return;
				const fs::path to = FromUtf8(st_.targetFolder) / Written(c.args[1]);
				fs::create_directories(to.parent_path(), ec);
				fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
				if (ec) fail("can't copy " + c.args[0] + " to " + c.args[1] + ": " + ec.message());
				note("copied " + c.args[0] + " to " + c.args[1]);
			}

			// -------------------------------------------------------- values ---

			// field or field[i] or field[^] (the last element).
			sc::FieldPath fieldPath(Collection& x, const std::string& token)
			{
				sc::FieldPath p;
				const size_t lb = token.find('[');
				p.field = token.substr(0, lb);
				if (lb == std::string::npos) return p;
				const size_t rb = token.find(']', lb);
				const std::string idx = token.substr(lb + 1, rb == std::string::npos ? std::string::npos : rb - lb - 1);
				int64_t i;
				if (idx == "^")
				{
					const Attribute* a = x.resolve(KeyOf(p.field));
					if (!a || !a->count) fail(p.field + " has no elements");
					i = int64_t(a->count) - 1;
				}
				else
				{
					char* end = nullptr;
					i = std::strtoll(idx.c_str(), &end, 10);
					if (!end || *end || i < 0) fail("bad index in " + token);
				}
				p.steps.push_back({ std::string(), i });
				return p;
			}

			// NFS-VltEd's words for values of this type: by the type's name (a
			// StringKey field named MODEL has a class of its own).
			const PropertyNames::Type* vltType(const sc::FieldPath& p, const ValueType& vt) const
			{
				if (!st_.names) return nullptr;
				if (vt.kind == TypeKind::StringKey && p.steps.empty() && Lower(p.field) == "model")
					if (const PropertyNames::Type* t = st_.names->find(vt.typeName + "(MODEL)")) return t;
				return st_.names->find(vt.typeName);
			}

			// The steps from a value of type `vt` to the bytes at `off`, where an
			// NFS-VltEd property is: a member (into nested structs), an element of a
			// vector or matrix, or a reference's class or collection.
			void stepTo(Collection& x, sc::FieldPath& p, const ValueType& vt, uint32_t off, RefPart& part, bool list,
						const std::string& word)
			{
				switch (vt.kind)
				{
					case TypeKind::RefSpec:
						if (off == 0) part = RefPart::Class;
						else if (off == 4) part = RefPart::Collection;
						else fail(word + " is a reference's pointer, which the game sets");
						return;
					case TypeKind::ClassRefSpec:
						// NFS-VltEd's Class is the collection key at 0, its Collection the pointer.
						if (off != 0) fail(word + " is a reference's pointer, which the game sets");
						return;
					case TypeKind::Vector2: case TypeKind::Vector3: case TypeKind::Vector4: case TypeKind::Quaternion:
					case TypeKind::FloatColour:
						p.steps.push_back({ std::string(), int64_t(off / 4) });
						return;
					case TypeKind::Matrix:
						p.steps.push_back({ std::string(), int64_t(off / 16) });
						p.steps.push_back({ std::string(), int64_t(off % 16 / 4) });
						return;
					case TypeKind::StringKey:
						if (off == 4) return; // its text (the hash follows it)
						fail(word + " is a StringKey's hash: set its text");
					case TypeKind::Struct:
					case TypeKind::Curve:
						if (!vt.st) break;
						for (const MemberDef& m : vt.st->members)
						{
							const uint32_t size = std::max<uint32_t>(m.elemSize * m.count, 1);
							if (off < m.offset || off >= m.offset + size) continue;
							if (m.isBitfield())
							{
								std::string held;
								for (const MemberDef& o : vt.st->members)
									if (o.offset == m.offset && o.isBitfield()) held += (held.empty() ? "" : ", ") + o.name;
								fail(word + " is the whole word holding " + held + ": set those by name");
							}
							p.steps.push_back({ m.name, -1 });
							uint32_t rest = off - m.offset;
							if (m.count > 1 && m.elemSize)
							{
								p.steps.push_back({ std::string(), int64_t(rest / m.elemSize) });
								rest %= m.elemSize;
							}
							if (list) return; // a list: its index and element come next
							stepTo(x, p, sc::TypeAt(db_, x, p), rest, part, false, word);
							return;
						}
						fail(word + " is at byte " + std::to_string(off) + ", where " + vt.st->name + " has no member");
					default:
						break;
				}
				if (off != 0) fail(word + " is at byte " + std::to_string(off) + " of a " + vt.typeName);
			}

			// The steps to an NFS-VltEd property: by our members' names where the
			// table gives them (they hold on every platform, where the PS2's 4-byte
			// bools move the offsets of the members after them), else by its offset.
			void stepToProperty(Collection& x, sc::FieldPath& p, const ValueType& vt, const PropertyNames::Property& pr, RefPart& part,
								const std::string& word)
			{
				if (pr.path.empty() || !vt.st)
				{
					stepTo(x, p, vt, pr.offset, part, pr.list, word);
					return;
				}
				auto member = [&](const StructDef* st, const PropertyNames::Step& step) -> const MemberDef*
				{
					const MemberDef* m = st ? st->member(step.member) : nullptr;
					if (!m && st)
						for (const MemberDef& mm : st->members)
							if (Lower(mm.name) == Lower(step.member)) m = &mm;
					if (!m)
						fail((pr.pathText() != word ? word + " is " + pr.pathText() + " in NFS-VltEd's " + std::string(InfoOf(db_.target().game).name) +
							  " words, and "
							  : std::string()) +
							 (st ? st->name + " has no member " + step.member : vt.typeName + " has no members") + " on " + db_.target().name());
					if (step.index >= int64_t(std::max<uint32_t>(m->count, 1))) fail(word + " is " + pr.pathText() + ", past the end of " + m->name);
					return m;
				};
				// Members of structs the scripts step into.
				ValueType cur = vt;
				size_t i = 0;
				for (; i < pr.path.size() && (cur.kind == TypeKind::Struct || cur.kind == TypeKind::Curve); ++i)
				{
					const MemberDef* m = member(cur.st, pr.path[i]);
					p.steps.push_back({ m->name, -1 });
					if (pr.path[i].index >= 0) p.steps.push_back({ std::string(), pr.path[i].index });
					if (i + 1 < pr.path.size() || pr.rest) cur = sc::TypeAt(db_, x, p);
				}
				if (i == pr.path.size())
				{
					if (pr.rest)
					{
						stepTo(x, p, cur, pr.rest, part, pr.list, word);
					}
					else if (pr.list && cur.kind != TypeKind::Curve)
					{
						// A list held in a struct of a pointer and its count
						// (DynamicSizeArray): the index after the word is the pointer's.
						const ValueType lt = sc::TypeAt(db_, x, p);
						if (lt.kind == TypeKind::Struct && lt.st)
							for (const MemberDef& m : lt.st->members)
								if (m.prim == MemberPrim::Ptr && !m.ptrCount.empty())
								{
									p.steps.push_back({ m.name, -1 });
									break;
								}
					}
					return;
				}
				// The rest is inside a value set whole: a reference's part, a
				// StringKey's text, a vector's component, else by the offset there.
				const std::string l = Lower(pr.path[i].member);
				switch (cur.kind)
				{
					case TypeKind::RefSpec:
						if (l == "mclasskey") part = RefPart::Class;
						else if (l == "mcollectionkey") part = RefPart::Collection;
						else fail(word + " is a reference's pointer, which the game sets");
						return;
					case TypeKind::StringKey:
						if (l != "mstring") fail(word + " is a StringKey's hash: set its text");
						return;
					case TypeKind::Vector2: case TypeKind::Vector3: case TypeKind::Vector4: case TypeKind::Quaternion:
					case TypeKind::FloatColour:
					{
						static const char* xyzw = "xyzw";
						static const char* rgba = "rgba";
						const char* f = l.size() == 1 ? std::strchr(xyzw, l[0]) : nullptr;
						const char* g = l.size() == 1 && !f ? std::strchr(rgba, l[0]) : nullptr;
						if (!f && !g) fail(word + " is " + pr.pathText() + ", which isn't a part of a " + cur.typeName);
						p.steps.push_back({ std::string(), f ? int64_t(f - xyzw) : int64_t(g - rgba) });
						return;
					}
					default:
					{
						uint32_t off = pr.rest;
						const StructDef* st = cur.st;
						for (; i < pr.path.size(); ++i)
						{
							const MemberDef* m = member(st, pr.path[i]);
							off += m->offset + uint32_t(std::max<int64_t>(pr.path[i].index, 0)) * m->elemSize;
							st = m->st;
						}
						stepTo(x, p, cur, off, part, pr.list, word);
						return;
					}
				}
			}

			// One property token of update_field: NFS-VltEd's word for a part of the
			// value, our schema's member, an index, or a part of a value the way
			// Attribulator names it.
			void property(Collection& x, sc::FieldPath& p, const std::string& token, RefPart& part)
			{
				if (!token.empty() && std::all_of(token.begin(), token.end(), [](char ch)
				{
					return std::isdigit(static_cast<unsigned char>(ch));
				}))
				{
					// An index into a list, unless the type names a property so (UpgradeDescription's 4).
					const ValueType vt = sc::TypeAt(db_, x, p);
					const PropertyNames::Type* t = vltType(p, vt);
					const PropertyNames::Property* pr = t && !t->single ? t->find(token) : nullptr;
					if (pr) stepToProperty(x, p, vt, *pr, part, token);
					else p.steps.push_back({ std::string(), std::strtoll(token.c_str(), nullptr, 10) });
					return;
				}
				const size_t lb = token.find('[');
				const std::string prop = token.substr(0, lb);
				std::string idx = lb == std::string::npos ? std::string() : token.substr(lb + 1, token.find(']', lb) - lb - 1);
				const std::string l = Lower(prop);
				const ValueType vt = sc::TypeAt(db_, x, p);
				if (const PropertyNames::Type* t = vltType(p, vt))
				{
					if (t->single) return; // NFS-VltEd ignores the word; update() steps to its one property
					const PropertyNames::Property* pr = t->find(token);
					if (!pr && !idx.empty()) pr = t->find(prop);
					else if (pr) idx.clear(); // M[1,1]: the index is part of the word
					if (pr)
					{
						stepToProperty(x, p, vt, *pr, part, token);
						if (!idx.empty()) p.steps.push_back({ std::string(), std::strtoll(idx.c_str(), nullptr, 10) });
						return;
					}
				}
				else if (st_.names && l == "data" && idx.empty())
				{
					part = RefPart::Data; // NFS-VltEd's raw class: the value's bytes
					return;
				}
				switch (vt.kind)
				{
					case TypeKind::RefSpec:
						// Group and Name: NFS-VltEd's words for a reference in a list.
						if (l == "collection" || l == "collectionkey" || l == "name") part = RefPart::Collection;
						else if (l == "class" || l == "classkey" || l == "group") part = RefPart::Class;
						else fail("a reference has Class and Collection, not " + prop);
						return;
					case TypeKind::Vector2: case TypeKind::Vector3: case TypeKind::Vector4: case TypeKind::Quaternion:
					case TypeKind::FloatColour:
					{
						static const char* xyzw = "xyzw";
						static const char* rgba = "rgba";
						int i = -1;
						if (l.size() == 1)
						{
							if (const char* f = std::strchr(xyzw, l[0])) i = int(f - xyzw);
							else if (const char* g = std::strchr(rgba, l[0])) i = int(g - rgba);
						}
						if (i < 0) fail(vt.typeName + " has X, Y, Z and W, not " + prop);
						p.steps.push_back({ std::string(), i });
						return;
					}
					case TypeKind::Matrix:
						// Data[row,column] or M[row,column], counted from 1.
						if ((l == "data" || l == "m") && idx.find(',') != std::string::npos)
						{
							const int r = std::atoi(idx.c_str()), col = std::atoi(idx.c_str() + idx.find(',') + 1);
							if (r < 1 || r > 4 || col < 1 || col > 4) fail("matrix rows and columns go from 1 to 4");
							p.steps.push_back({ std::string(), r - 1 });
							p.steps.push_back({ std::string(), col - 1 });
							return;
						}
						fail("a matrix element is M[row,column]");
					case TypeKind::Struct:
					case TypeKind::Curve:
					{
						std::string member = prop;
						if (vt.st)
						{
							// Our member names, without case, and without their m prefix (mKitNumber: KitNumber).
							const MemberDef* m = vt.st->member(prop);
							for (const MemberDef& mm : vt.st->members)
							{
								const std::string n = Lower(mm.name);
								if (!m && (n == l || (n.size() > 1 && n[0] == 'm' && std::isupper(static_cast<unsigned char>(mm.name[1])) &&
													  n.substr(1) == l)))
									m = &mm;
							}
							// Value: NFS-VltEd's word for a list element of one member (MiniRefSpec's key).
							if (!m && l == "value" && vt.st->members.size() == 1) m = &vt.st->members[0];
							if (!m)
							{
								std::string words;
								if (const PropertyNames::Type* t = vltType(p, vt))
									for (const auto& w : t->properties) words += (words.empty() ? "" : ", ") + w.word;
								fail(vt.st->name + " has no member " + prop + (words.empty() ? std::string() : " (NFS-VltEd's words: " + words + ")"));
							}
							member = m->name;
						}
						p.steps.push_back({ member, -1 });
						break;
					}
					default:
						// A simple value's one property.
						if (l == "value" || l == "string" || l == "text" || l == "hash" || l == "key") return;
						fail(vt.typeName + " has no property " + prop);
				}
				if (!idx.empty())
				{
					char* end = nullptr;
					const long long i = std::strtoll(idx.c_str(), &end, 10);
					if (!end || *end || i < 0) fail("bad index in " + token);
					p.steps.push_back({ std::string(), i });
				}
			}

			// A type NFS-VltEd sees as one value (a GCollectionKey, a single-member
			// struct): the property it sets, when ours is a struct.
			void single(Collection& x, sc::FieldPath& p, RefPart& part)
			{
				const ValueType vt = sc::TypeAt(db_, x, p);
				const PropertyNames::Type* t = vltType(p, vt);
				if (!t || !t->single || t->properties.empty() || part != RefPart::Whole) return;
				if (vt.kind == TypeKind::Struct) stepToProperty(x, p, vt, t->properties[0], part, t->properties[0].word);
			}

			// nfsms text as a value of the field's kind.
			sc::ValuePtr value(const std::string& t, const ValueType& vt, const Bytes& cur, RefPart part, Collection& x)
			{
				const Endian e = x.vault ? x.vault->raw.endian : Endian::Little;
				auto keyValue = [&](const std::string& s)
				{
					return IsHex(s) ? sc::MakeNumber(s) : s.empty() ? sc::MakeNumber("0") : sc::MakeString(s);
				};
				auto ref = [&](sc::ValuePtr a, sc::ValuePtr b)
				{
					auto v = std::make_shared<sc::Value>();
					v->kind = sc::Value::Kind::Binary;
					v->op = '/';
					v->items = { std::move(a), std::move(b) };
					return v;
				};
				if (part == RefPart::Data)
				{
					// Hex bytes, "00 00 80 3F", as many as the value has.
					std::string hex;
					for (char ch : t)
						if (!std::isspace(static_cast<unsigned char>(ch))) hex += ch;
					if (hex.size() % 2 || !std::all_of(hex.begin(), hex.end(), [](char ch)
					{
						return std::isxdigit(static_cast<unsigned char>(ch));
					}))
						fail("Data is the value's bytes in hex, like \"00 00 80 3F\"");
					if (hex.size() / 2 != vt.size)
						fail("Data needs " + std::to_string(vt.size) + " bytes, not " + std::to_string(hex.size() / 2));
					auto v = std::make_shared<sc::Value>();
					v->kind = sc::Value::Kind::Bytes;
					v->text = t;
					return v;
				}
				if (vt.kind == TypeKind::RefSpec)
				{
					if (part == RefPart::Whole && t.empty()) return sc::MakeWord("null");
					const RefSpecLayout rl = RefSpecAt(vt.size);
					const uint32_t ck = cur.size() >= rl.cls + rl.keySize ? LoadKey(db_, cur.data() + rl.cls, rl.keySize, e) : 0;
					const uint32_t k = cur.size() >= rl.col + rl.keySize ? LoadKey(db_, cur.data() + rl.col, rl.keySize, e) : 0;
					// The part that stays as it is, by name where it has one (64-bit keys need it).
					auto keep = [&](uint32_t key)
					{
						const std::string* n = db_.names().find(key);
						return n ? sc::MakeString(*n) : sc::MakeNumber(Hex32(key));
					};
					if (part == RefPart::Class) return ref(keyValue(t), k ? keep(k) : sc::MakeNumber("0"));
					// The collection alone, as these tools set it: the class stays.
					if (!ck && part == RefPart::Whole) fail("the reference has no class yet; set its Class first");
					const std::string* className = db_.names().find(ck); // by name, so messages read "vehicle/x"
					return ref(className ? sc::MakeString(*className) : sc::MakeNumber(Hex32(ck)), keyValue(t));
				}
				switch (vt.kind)
				{
					case TypeKind::Bool:
					{
						const std::string l = Lower(t);
						if (l == "true" || l == "1") return sc::MakeWord("true");
						if (l == "false" || l == "0") return sc::MakeWord("false");
						fail("a bool is true or false, not " + t);
					}
					case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16: case TypeKind::Int32:
					case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::UInt64: case TypeKind::Char: case TypeKind::Unknown:
						if (vt.kind == TypeKind::Unknown && vt.size != 4) break;
						if (IsHex(t) || IsNumber(t)) return sc::MakeNumber(t);
						// A name in a number field is its hash (Attribulator does this).
						return sc::MakeNumber(Hex32(StringHash32(t)));
					case TypeKind::Float:
					case TypeKind::Double:
						if (IsNumber(t) || IsHex(t)) return sc::MakeNumber(t);
						fail("not a number: " + t);
					case TypeKind::Enum: return IsHex(t) || IsNumber(t) ? sc::MakeNumber(t) : sc::MakeString(t);
					case TypeKind::Text:
					case TypeKind::StringKey: return sc::MakeString(t);
					case TypeKind::Key:
					case TypeKind::BinKey:
					case TypeKind::ClassRefSpec: return keyValue(t);
					default: break;
				}
				fail((vt.typeName.empty() ? std::string("this value") : vt.typeName) + " takes more than one value: name a property");
			}

			void update(const Command& c)
			{
				need(c, 4, 64);
				ClassDef& k = cls(c.args[0]);
				Collection& x = node(k, c.args[1]);
				if (!k.field(KeyOf(c.args[2].substr(0, c.args[2].find('[')))))
					fail("the class '" + c.args[0] + "' does not contain '" + c.args[2].substr(0, c.args[2].find('[')) + "'");
				sc::FieldPath p = fieldPath(x, c.args[2]);
				RefPart part = RefPart::Whole;
				for (size_t i = 3; i + 1 < c.args.size(); ++i) property(x, p, c.args[i], part);
				single(x, p, part);
				Bytes cur;
				const ValueType vt = sc::TypeAt(db_, x, p, &cur);
				sc::Statement s;
				s.kind = K::Set;
				s.path = p;
				s.value = value(c.args.back(), vt, cur, part, x);
				if (part == RefPart::Data && x.vault && x.vault->raw.endian == Endian::Big)
					message(sc::Message::Level::Warning, "Data sets the value's bytes as this platform's files hold them, big-endian; "
							"a script written for the PC gives them little-endian");
				amod(x, { s });
				if ((vt.kind == TypeKind::RefSpec || vt.kind == TypeKind::ClassRefSpec) && part != RefPart::Data)
					setRefs_.push_back({ x.classKey, x.key, p, line_, lineTop_, depth_, source_, path_, c.args.back() });
			}

		public:
			// WordsLeadTo: update's path, without setting anything.
			sc::FieldPath lead(Collection& x, const std::string& field, const std::vector<std::string>& words, std::string* partName)
			{
				sc::FieldPath p = fieldPath(x, field);
				RefPart part = RefPart::Whole;
				for (const std::string& w : words) property(x, p, w, part);
				single(x, p, part);
				sc::TypeAt(db_, x, p);
				if (partName)
					*partName = part == RefPart::Class ? "class" : part == RefPart::Collection ? "collection" : part == RefPart::Data ? "data" : "";
				return p;
			}

		private:
			// Value statements through the mod script engine.
			void amod(Collection& x, std::vector<sc::Statement> body)
			{
				sc::Script s;
				s.source = source_;
				sc::Statement e;
				e.kind = K::Edit;
				e.target.cls = Hex32(x.classKey);
				e.target.name = Hex32(x.key);
				e.body = std::move(body);
				s.statements.push_back(std::move(e));
				sc::ApplySettings st;
				st.fragment = true;
				st.strict = st_.strict;
				st.checkReferences = false; // checkReferences(), once the whole script ran
				const sc::ApplyReport r = sc::Apply(db_, s, st);
				for (const sc::Message& m : r.messages)
				{
					if (m.level == sc::Message::Level::Error) fail(m.text);
					message(m.level, m.text);
				}
				if (out_) emitEdit(x, s.statements[0].body);
				rep_.valuesSet += r.valuesSet;
				rep_.reverted += r.reverted;
				rep_.touched.insert(r.touched.begin(), r.touched.end());
				rep_.newNames.insert(rep_.newNames.end(), r.newNames.begin(), r.newNames.end());
			}

			Database& db_;
			const RunSettings& st_;
			sc::ApplyReport rep_;
			std::string source_, path_;
			int line_ = 0, lineTop_ = 0, depth_ = 0;
		};

	} // namespace

	script::Script ToModScript(Database& scratch, const Script& script, const RunSettings& settings, script::ApplyReport* report)
	{
		if (const std::string why = Incompatibility(scratch); !why.empty())
		{
			if (report)
			{
				*report = {};
				report->messages.push_back(sc::Message(sc::Message::Level::Error, {}, why));
			}
			return {};
		}
		// The setup's controls as options: a checkbox a bool, a radio group a
		// choice of its buttons' texts, asked with the text above it.
		Runner::Options opts;
		std::set<std::string> used;
		auto idFor = [&](const std::string& n)
		{
			std::string id;
			for (char ch : n) id += std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ? ch : '_';
			if (id.empty() || std::isdigit(static_cast<unsigned char>(id[0]))) id = "o_" + id;
			const std::string base = id;
			for (int i = 2; used.count(id); ++i) id = base + "_" + std::to_string(i);
			used.insert(id);
			return id;
		};
		std::string options, description, lastText;
		std::set<std::string> groupsDone;
		for (const SetupItem& item : script.setup)
		{
			if (item.kind == SetupItem::Kind::Text)
			{
				if (!description.empty() || !item.text.empty()) description += item.text + "\n";
				if (!item.text.empty()) lastText = item.text;
				continue;
			}
			if (item.kind == SetupItem::Kind::Checkbox)
			{
				const Control& c = script.controls[size_t(item.controls[0])];
				if (opts.boxes.count(c.name)) continue;
				const std::string id = idFor(c.name);
				opts.boxes[c.name] = id;
				options += "option " + id + " bool " + sc::Quote(c.label) + " default " + (c.checked ? "true" : "false") + "\n";
				continue;
			}
			const std::string group = script.controls[size_t(item.controls[0])].group;
			if (!groupsDone.insert(group).second) continue;
			std::vector<const Control*> buttons;
			for (const Control& c : script.controls)
				if (c.kind == Control::Kind::Radio && c.group == group) buttons.push_back(&c);
			// The buttons' texts, unless two are the same; nothing chosen at first, as in NFS-VltEd.
			std::set<std::string> labels;
			for (const Control* c : buttons) labels.insert(c->label);
			const bool byLabel = labels.size() == buttons.size() && !labels.count("(none)");
			std::string choices = "\"(none)\"";
			for (const Control* c : buttons)
			{
				const std::string text = byLabel ? c->label : c->name;
				opts.choices[{group, c->name}] = text;
				choices += ", " + sc::Quote(text);
			}
			const std::string id = idFor(group);
			opts.groups[group] = id;
			options += "option " + id + " choice " + sc::Quote(lastText.empty() ? group : lastText) + " choices [" + choices + "] default \"(none)\"\n";
		}
		while (!description.empty() && (description.back() == '\n' || description.back() == ' ')) description.pop_back();

		std::string stem = ToUtf8(FromUtf8(script.source).stem());
		std::string head = "attribulina 1\n";
		if (const std::string game = sc::GameId(scratch); !game.empty()) head += "game " + game + "\n";
		head += "title " + sc::Quote(stem) + "\n";
		if (!description.empty()) head += "description " + sc::Quote(description) + "\n";
		head += "# Converted from " + script.source + ", an nfsms script, by Attribulina.\n" + options;
		sc::Script out = sc::Parse(head, stem + ".amod");
		out.source = stem + ".amod";

		RunSettings st = settings;
		st.strict = false;
		st.doFiles = false;
		st.firstLine = st.lastLine = 0;
		std::vector<sc::Statement> body;
		Runner r(scratch, st);
		const sc::ApplyReport rep = r.convert(script, body, opts);
		for (sc::Statement& b : body) out.statements.push_back(std::move(b));
		if (report) *report = rep;
		return out;
	}

	script::ApplyReport Run(Database& db, const Script& script, const RunSettings& settings)
	{
		if (const std::string why = Incompatibility(db); !why.empty())
		{
			sc::ApplyReport r;
			r.messages.push_back(sc::Message(sc::Message::Level::Error, {}, why));
			return r;
		}
		Runner r(db, settings);
		return r.run(script);
	}

	Collection* CollectionOfLine(const Database& db, const Command& c, uint32_t* field)
	{
		if (field) *field = 0;
		static const char* const withNode[] = { "update_field", "update_collection", "add_field", "delete_field", "resize_field",
											   "resize_collection", "append_array", "delete_node", "rename_node", "move_node",
											   "change_vault", "copy_node", "copy_overwrite", "copy_fields" };
		static const char* const withField[] = { "update_field", "update_collection", "add_field", "delete_field", "resize_field",
												"resize_collection", "append_array" };
		bool has = false;
		for (const char* n : withNode) has |= c.name == n;
		if (c.name == "add_node" || c.name == "add_overwrite")
		{
			// The new node once it exists, else its parent.
			if (c.args.size() < 2) return nullptr;
			const ClassDef* k = db.findClass(KeyOf(c.args[0]));
			if (!k) return nullptr;
			if (Collection* x = db.findCollection(k->key, KeyOf(c.args.back()))) return x;
			return c.args.size() == 3 ? db.findCollection(k->key, KeyOf(c.args[1])) : nullptr;
		}
		if (!has || c.args.size() < 2) return nullptr;
		const ClassDef* k = db.findClass(KeyOf(c.args[0]));
		if (!k) return nullptr;
		Collection* x = db.findCollection(k->key, KeyOf(c.args[1]));
		bool hasField = false;
		for (const char* n : withField) hasField |= c.name == n;
		if (x && field && hasField && c.args.size() >= 3)
		{
			const std::string f = c.args[2].substr(0, c.args[2].find('['));
			if (k->field(KeyOf(f))) *field = KeyOf(f);
		}
		return x;
	}

	sc::FieldPath WordsLeadTo(Database& db, Collection& c, const std::string& field, const std::vector<std::string>& words,
							  const RunSettings& settings, std::string* part)
	{
		Runner r(db, settings);
		return r.lead(c, field, words, part);
	}

	// ------------------------------------------------------------- recording ---

	namespace
	{

		std::string Token(const std::string& s)
		{
			if (s.empty()) return "\"\"";
			if (s.find_first_of(" \t#") != std::string::npos) return "\"" + s + "\"";
			return s;
		}

		class Writer
		{
		public:
			Writer(const Database& db, const PropertyNames* names) : db_(db), names_(names)
			{
			}

			std::vector<std::string> out;

			void statements(const std::vector<sc::Statement>& list)
			{
				for (const sc::Statement& s : list) statement(s);
			}

		private:
			std::string nameOf(const std::string& n) const
			{
				// Names as written in the statement: a quoted 0x... is a name.
				if (n.size() >= 2 && n.front() == '"' && n.back() == '"') return n.substr(1, n.size() - 2);
				return n;
			}
			void statement(const sc::Statement& s)
			{
				switch (s.kind)
				{
					case K::Comment: out.push_back("#" + s.comment); break;
					case K::Edit:
						for (const sc::Statement& b : s.body) body(s.target, b);
						break;
					case K::Create:
					case K::Collection:
					{
						const std::string cls = nameOf(s.target.cls), n = nameOf(s.target.name);
						std::string parent = s.hasUnder && !s.under.root ? " " + Token(nameOf(s.under.name)) : std::string();
						if (s.kind == K::Create && s.hasFrom)
							out.push_back("copy_node " + Token(cls) + " " + Token(nameOf(s.from.name)) + parent + " " + Token(n));
						else
							out.push_back("add_node " + Token(cls) + parent + " " + Token(n));
						const ClassDef* k = db_.findClass(KeyOf(cls));
						if (!s.vault.empty())
						{
							// The vault, when it isn't the one add_node picks.
							const Collection* p = k && s.hasUnder && !s.under.root ? db_.findCollection(k->key, KeyOf(nameOf(s.under.name))) : nullptr;
							if (!p || !p->vault || p->vault->name != s.vault) out.push_back("change_vault " + Token(cls) + " " + Token(n) + " " + Token(s.vault));
						}
						// NFS-VltEd's new node has its layout fields only (a copy, its
						// source's own values too), and its update_field fails on a field
						// the node doesn't have: each other value is added first.
						newNode_ = true;
						added_.clear();
						if (s.kind == K::Create && s.hasFrom && k)
							if (const Collection* from = db_.findCollection(k->key, KeyOf(nameOf(s.from.name))))
								for (const Attribute& a : from->attributes) added_.insert(a.key);
						for (const sc::Statement& b : s.body) body(s.target, b);
						newNode_ = false;
						break;
					}
					case K::Delete: out.push_back("delete_node " + Token(nameOf(s.target.cls)) + " " + Token(nameOf(s.target.name))); break;
					case K::Rename:
						out.push_back("rename_node " + Token(nameOf(s.target.cls)) + " " + Token(nameOf(s.target.name)) + " " + Token(s.word));
						break;
					case K::Move:
						out.push_back("move_node " + Token(nameOf(s.target.cls)) + " " + Token(nameOf(s.target.name)) +
									  (s.under.root ? std::string() : " " + Token(nameOf(s.under.name))));
						break;
					case K::MoveToVault:
						out.push_back("change_vault " + Token(nameOf(s.target.cls)) + " " + Token(nameOf(s.target.name)) + " " + Token(s.vault));
						break;
					case K::CreateVault: out.push_back("# vault " + s.vault + " in " + s.word + " (change_vault makes it)"); break;
					case K::RenameVault: out.push_back("# rename vault " + s.vault + " to " + s.word + ": nfsms has no command that renames a vault"); break;
					case K::Class: out.push_back("# class " + nameOf(s.word) + ": nfsms has no command that makes or changes a class (a mod script's class statement does)"); break;
					case K::DeleteClass: out.push_back("# delete class " + nameOf(s.word) + ": nfsms has no command that deletes a class"); break;
					default: out.push_back("# a mod script statement nfsms has no command for"); break;
				}
			}

			void body(const sc::Target& t, const sc::Statement& b)
			{
				const std::string head = Token(nameOf(t.cls)) + " " + Token(nameOf(t.name));
				target_ = &t;
				switch (b.kind)
				{
					case K::Set:
					{
						if (b.op != '=')
						{
							out.push_back("# " + b.path.text() + " " + b.op + "= can't be written in nfsms");
							return;
						}
						// The field token takes an array's index; members (and a vector's
						// or matrix's components) follow as properties.
						std::string field = Token(nameOf(b.path.field));
						std::vector<std::string> props;
						size_t i = 0;
						const ClassDef* k = db_.findClass(KeyOf(nameOf(t.cls)));
						const FieldDef* f = k ? k->field(KeyOf(nameOf(b.path.field))) : nullptr;
						if (!b.path.steps.empty() && b.path.steps[0].member.empty() && (!f || f->isArray()))
						{
							field += "[" + std::to_string(b.path.steps[0].index) + "]";
							i = 1;
						}
						for (; i < b.path.steps.size(); ++i)
						{
							const auto& st = b.path.steps[i];
							if (st.member.empty())
							{
								if (props.empty()) props.push_back(std::to_string(st.index));
								else props.back() += "[" + std::to_string(st.index) + "]";
							}
							else
							{
								props.push_back(st.member);
							}
						}
						// NFS-VltEd reads a field the schema gives an enum (FieldDef::fieldEnum)
						// as the integer its type is: the value's number, not its name.
						const sc::Value* v = b.value.get();
						sc::ValuePtr number;
						if (f && f->fieldEnum && v->kind == sc::Value::Kind::Word)
							for (const auto& [n, val] : f->fieldEnum->values)
								if (n == v->text)
								{
									number = sc::MakeNumber(std::to_string(val));
									v = number.get();
									break;
								}
						const size_t at = out.size();
						value("update_field " + head, field, props, *v, b.path.steps.empty() || !b.path.steps[0].member.empty());
						if (f) addField(head, *f, b, at);
						break;
					}
					case K::Revert: out.push_back("delete_field " + head + " " + Token(nameOf(b.path.field))); break;
					case K::Resize:
					{
						const ClassDef* k = db_.findClass(KeyOf(nameOf(t.cls)));
						const FieldDef* f = k ? k->field(KeyOf(nameOf(b.path.field))) : nullptr;
						const size_t at = out.size();
						out.push_back("resize_field " + head + " " + Token(nameOf(b.path.field)) + " " + std::to_string(b.count));
						if (f) addField(head, *f, b, at);
						break;
					}
					case K::Comment: out.push_back("#" + b.comment); break;
					default: out.push_back("# not in nfsms: " + b.path.text()); break;
				}
			}

			// In a node add_node made: `add_field` before the lines from `at` that set
			// one of its values, the first time a line sets that field. Not for a
			// layout field (the node has it), nor when nfsms can't write the value
			// (only comments): the node keeps inheriting it.
			void addField(const std::string& head, const FieldDef& f, const sc::Statement& b, size_t at)
			{
				if (!newNode_ || f.inLayout() || added_.count(f.key)) return;
				if (std::none_of(out.begin() + std::ptrdiff_t(at), out.end(), [](const std::string& l) { return !l.empty() && l[0] != '#'; }))
					return;
				const std::string field = Token(nameOf(b.path.field));
				std::string line = "add_field " + head + " " + field;
				if (f.isArray())
				{
					// The array's length: as the statement gives it, else as the collection has it.
					int64_t count = -1;
					if (b.kind == K::Resize) count = b.count;
					else if (b.path.steps.empty() && b.value && b.value->kind == sc::Value::Kind::List) count = int64_t(b.value->items.size());
					if (count < 0 && target_)
						if (const ClassDef* k = db_.findClass(KeyOf(nameOf(target_->cls))))
							if (const Collection* x = db_.findCollection(k->key, KeyOf(nameOf(target_->name))))
								if (const Attribute* a = x->find(f.key)) count = a->count;
					if (count < 0 && !b.path.steps.empty() && b.path.steps[0].member.empty()) count = b.path.steps[0].index + 1;
					if (count < 0) count = 0;
					line += " " + std::to_string(count);
					// add_field gives it that length already.
					if (at < out.size() && out[at] == "resize_field " + head + " " + field + " " + std::to_string(count))
						out.erase(out.begin() + std::ptrdiff_t(at));
				}
				out.insert(out.begin() + std::ptrdiff_t(at), line);
				added_.insert(f.key);
			}

			// Our words after the field (members, Class, X, X[3]) in NFS-VltEd's: the
			// word its class has at the same byte, and a list as list, index, Value
			// (an update_collection line). Unchanged when the table has no word there.
			bool vltWords(const std::string& field, std::vector<std::string>& ps) const
			{
				if (!names_ || !target_) return false;
				const ClassDef* k = db_.findClass(KeyOf(nameOf(target_->cls)));
				Collection* x = k ? const_cast<Database&>(db_).findCollection(k->key, KeyOf(nameOf(target_->name))) : nullptr;
				if (!x) return false;
				Database& db = const_cast<Database&>(db_);
				sc::FieldPath p;
				const size_t lb = field.find('[');
				p.field = field.substr(0, lb);
				if (lb != std::string::npos) p.steps.push_back({ std::string(), std::strtoll(field.c_str() + lb + 1, nullptr, 10) });
				try
				{
					const ValueType top = sc::TypeAt(db, *x, p);
					const PropertyNames::Type* t = names_->find(top.typeName);
					if (!t) return false;
					if (t->single)
					{
						ps.clear(); // NFS-VltEd takes no word for it
						return false;
					}
					if (ps.empty())
					{
						// A class reference set whole is NFS-VltEd's Class, the word at 0.
						if (top.kind == TypeKind::ClassRefSpec)
							if (const PropertyNames::Property* pr = t->at(0)) ps = { pr->word };
						return false;
					}
					// Our members as the table's paths write them (a struct the table
					// has paths for: they hold on every platform), else the offset.
					const bool byPath = top.st && t->hasPaths();
					uint32_t off = 0, rest = 0;
					std::string path;
					auto word = [&](bool list) -> const PropertyNames::Property*
					{
						const PropertyNames::Property* pr = nullptr;
						if (!byPath) pr = t->at(off);
						else if (rest) pr = t->atPath(path + "+" + std::to_string(rest));
						else pr = t->atPath(path);
						// A word over several members has no path: by its offset.
						if (!pr && byPath)
							for (const PropertyNames::Property& q : t->properties)
								if (q.path.empty() && q.offset == off) pr = &q;
						return pr && pr->list == list ? pr : nullptr;
					};
					auto add = [&](const std::string& member, int64_t index)
					{
						path += (path.empty() ? "" : ".") + member + (index >= 0 ? "[" + std::to_string(index) + "]" : std::string());
					};
					ValueType vt = top;
					for (size_t i = 0; i < ps.size(); ++i)
					{
						std::string w = ps[i];
						int64_t index = -1;
						if (const size_t b = w.find('['); b != std::string::npos)
						{
							index = std::strtoll(w.c_str() + b + 1, nullptr, 10);
							w = w.substr(0, b);
						}
						const std::string l = Lower(w);
						if (vt.kind == TypeKind::RefSpec && (l == "class" || l == "collection"))
						{
							off += l == "class" ? 0 : 4;
							add(l == "class" ? "mClassKey" : "mCollectionKey", -1);
							if (i + 1 != ps.size()) return false;
							break;
						}
						if ((vt.kind == TypeKind::Vector2 || vt.kind == TypeKind::Vector3 || vt.kind == TypeKind::Vector4 ||
							 vt.kind == TypeKind::Quaternion || vt.kind == TypeKind::FloatColour) && w.size() == 1)
						{
							// X, Y, Z, W (or R, G, B, A), or the component's index.
							const char ch = char(std::tolower(static_cast<unsigned char>(w[0])));
							size_t at = std::string("xyzw").find(ch);
							if (at == std::string::npos) at = std::string("rgba").find(ch);
							if (at == std::string::npos && ch >= '0' && ch <= '3') at = size_t(ch - '0');
							if (at == std::string::npos) return false;
							const uint32_t k = uint32_t(at) * 4;
							off += k;
							rest += k;
							break;
						}
						if (vt.kind == TypeKind::Matrix && !w.empty() && std::isdigit(static_cast<unsigned char>(w[0])) && index >= 0)
						{
							// Row[column], from 0.
							off += uint32_t(std::strtoul(w.c_str(), nullptr, 10)) * 16 + uint32_t(index) * 4;
							break;
						}
						if (!vt.st) return false;
						const MemberDef* m = vt.st->member(w);
						if (!m) return false;
						off += m->offset;
						add(m->name, -1);
						if (const PropertyNames::Property* pr = word(true))
						{
							// A list: NFS-VltEd's update_collection with the index and the element's word.
							if (index < 0 || i + 1 != ps.size()) return false;
							ps = { pr->word, std::to_string(index), pr->elements.empty() ? std::string("Value") : pr->elements[0] };
							return true;
						}
						p.steps.push_back({ m->name, -1 });
						if (index >= 0)
						{
							off += uint32_t(index) * m->elemSize;
							path += "[" + std::to_string(index) + "]";
							p.steps.push_back({ std::string(), index });
						}
						vt = sc::TypeAt(db, *x, p);
					}
					const PropertyNames::Property* pr = word(false);
					if (!pr && byPath && !rest && !path.empty())
					{
						// A path that goes on into the value our member sets whole: a
						// StringKey's text, a key's one member (HAL_ID.hash).
						std::string inner;
						if (vt.kind == TypeKind::StringKey) inner = "mString";
						else if (vt.st && vt.st->members.size() == 1 && vt.st->members[0].offset == 0) inner = vt.st->members[0].name;
						if (const PropertyNames::Property* q = inner.empty() ? nullptr : t->atPath(path + "." + inner); q && !q->list) pr = q;
					}
					if (pr) ps = { pr->word };
				}
				catch (const std::exception&)
				{
				}
				return false;
			}

			// One value as update_field lines: compound values one line per part.
			void value(const std::string& head, const std::string& field, std::vector<std::string> props, const sc::Value& v, bool wholeField)
			{
				auto line = [&](std::vector<std::string> ps, const std::string& val)
				{
					std::string s = head + " " + field;
					if (vltWords(field, ps)) s = "update_collection" + s.substr(std::strlen("update_field"));
					for (const std::string& p : ps) s += " " + p;
					out.push_back(s + " " + Token(val));
				};
				using VK = sc::Value::Kind;
				switch (v.kind)
				{
					case VK::Number: case VK::Word: case VK::String:
						line(props, v.kind == VK::Word && v.text == "null" ? std::string() : v.text);
						return;
					case VK::Binary:
						if (v.op == '/')
						{
							// A reference: its two keys.
							std::string cls, col;
							std::function<void(const sc::Value&, bool)> flat = [&](const sc::Value& x, bool first)
							{
								if (x.kind == VK::Binary && x.op == '/')
								{
									flat(*x.items[0], first);
									flat(*x.items[1], false);
								}
								else if (first && cls.empty())
								{
									cls = x.text;
								}
								else
								{
									col += (col.empty() ? "" : "/") + x.text;
								}
							};
							flat(v, true);
							auto c1 = props, c2 = props;
							c1.push_back("Class");
							c2.push_back("Collection");
							line(c1, cls);
							line(c2, col);
							return;
						}
						break;
					case VK::Tuple:
					{
						static const char* xyzw[] = { "X", "Y", "Z", "W" };
						// A matrix: its rows, one line per element.
						if (v.items.size() == 4 && std::all_of(v.items.begin(), v.items.end(), [](const sc::ValuePtr& r)
						{
							return r->kind == VK::Tuple && r->items.size() == 4 &&
								std::all_of(r->items.begin(), r->items.end(), [](const sc::ValuePtr& e)
							{
								return e->kind == VK::Number;
							});
						}))
						{
							for (size_t r = 0; r < 4; ++r)
								for (size_t c = 0; c < 4; ++c)
								{
									auto ps = props;
									ps.push_back(std::to_string(r) + "[" + std::to_string(c) + "]");
									line(ps, v.items[r]->items[c]->text);
								}
							return;
						}
						if (v.items.size() <= 4 && std::all_of(v.items.begin(), v.items.end(), [](const sc::ValuePtr& i)
						{
							return i->kind == VK::Number;
						}))
						{
							for (size_t i = 0; i < v.items.size(); ++i)
							{
								auto ps = props;
								ps.push_back(xyzw[i]);
								line(ps, v.items[i]->text);
							}
							return;
						}
						break;
					}
					case VK::List:
					{
						// A whole array: its size, then each element.
						if (wholeField && props.empty() && field.find('[') == std::string::npos)
							out.push_back("resize_field " + head.substr(std::strlen("update_field ")) + " " + field + " " + std::to_string(v.items.size()));
						for (size_t i = 0; i < v.items.size(); ++i)
						{
							if (props.empty() && field.find('[') == std::string::npos)
							{
								value(head, field + "[" + std::to_string(i) + "]", props, *v.items[i], false);
							}
							else
							{
								auto ps = props;
								if (ps.empty()) ps.push_back(std::to_string(i));
								else ps.back() += "[" + std::to_string(i) + "]";
								value(head, field, ps, *v.items[i], false);
							}
						}
						return;
					}
					case VK::Struct:
						for (const auto& [m, mv] : v.members)
						{
							auto ps = props;
							ps.push_back(m);
							value(head, field, ps, *mv, false);
						}
						return;
					default: break;
				}
				out.push_back("# " + field + ": " + (v.kind == VK::Lua ? std::string("Lua handlers") : v.kind == VK::Bytes ? std::string("raw bytes") : sc::PrintValue(v)) +
							  " can't be written in nfsms");
			}

			const Database& db_;
			const PropertyNames* names_;
			const sc::Target* target_ = nullptr;
			// In the body of a node add_node or copy_node makes, and the fields it
			// has besides its layout's (added, or copied from the source).
			bool newNode_ = false;
			std::set<uint32_t> added_;
		};

	} // namespace

	std::vector<std::string> FromStatements(const Database& db, const std::vector<script::Statement>& statements,
											const PropertyNames* names)
	{
		Writer w(db, names);
		w.statements(statements);
		return w.out;
	}

} // namespace attrib::nfsms
