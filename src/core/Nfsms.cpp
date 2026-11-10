// nfsms: reading NFS-VltEd / Attribulator mod scripts and running them
// (Nfsms.h, doc/nfsms.md). Value commands become mod script statements and go
// through script::Apply; the others call the edit API the way those tools do.

#include "Nfsms.h"

#include "Edit.h"
#include "Game.h"
#include "Curve.h"
#include "Hash.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
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

		// What NFS-VltEd's char.IsWhiteSpace finds in a name (the ASCII ones).
		bool HasWhiteSpace(const std::string& t)
		{
			return t.find_first_of(" \t\r\n\v\f") != std::string::npos;
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
		// A file or folder a script names under `folder`. Scripts are written on
		// Windows, where names match in any case (Install.nfsms names
		// rfr_Challenge.nfsms for rfr_challenge.nfsms), so a part that isn't there
		// as written is the entry whose name differs only in case, when there is one.
		fs::path InFolder(const fs::path& folder, const std::string& written)
		{
			fs::path at = folder;
			std::error_code ec;
			for (const fs::path& part : Written(written))
			{
				fs::path next = at / part;
				if (part != "." && part != ".." && !fs::exists(next, ec) && fs::is_directory(at, ec))
				{
					const std::string want = Lower(ToUtf8(part));
					for (const fs::directory_entry& e : fs::directory_iterator(at, ec))
						if (Lower(ToUtf8(e.path().filename())) == want)
						{
							next = e.path();
							break;
						}
				}
				at = std::move(next);
			}
			return at;
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
			Whole, Class, Collection, Data,
			// NFS-VltEd compatible: a StringKey's hash or text alone (MODEL's Collision
			// and Visual, Most Wanted's Collision64), and a RefSpec's pointer word (a
			// list element's Value).
			Hash, Text, Pointer, Hash64
		};

		// NFS-VltEd's own type of a word (PropertyNames::Property::vltType): its size
		// in the file, 0 when it isn't a single value.
		uint32_t VltSize(const std::string& t)
		{
			const std::string f = t.substr(0, t.find_first_of("/>"));
			if (f == "u8" || f == "i8" || f == "bool8") return 1;
			if (f == "u16" || f == "i16") return 2;
			if (f == "u32" || f == "i32" || f == "f32" || f == "bool32") return 4;
			if (f == "u64" || f == "i64" || f == "f64") return 8;
			return 0;
		}
		// NFS-VltEd's `version` line (A/ _0001.cs): .NET's Version.TryParse (two to
		// four whole numbers), refused when newer than its own 4.6.0.0. "" when it
		// reads, else why not.
		std::string VersionProblem(const std::string& v)
		{
			std::vector<long long> parts;
			size_t b = 0;
			for (;;)
			{
				const size_t e = v.find('.', b);
				const std::string part = v.substr(b, e == std::string::npos ? std::string::npos : e - b);
				if (part.empty() || part.size() > 9 || !std::all_of(part.begin(), part.end(), [](char ch)
				{
					return std::isdigit(static_cast<unsigned char>(ch));
				}))
					return "Unable to parse version: " + v;
				parts.push_back(std::atoll(part.c_str()));
				if (e == std::string::npos) break;
				b = e + 1;
			}
			if (parts.size() < 2 || parts.size() > 4) return "Unable to parse version: " + v;
			// A part left out is -1 to .NET, below any number.
			const long long mine[4] = { 4, 6, 0, 0 };
			for (size_t i = 0; i < 4; ++i)
			{
				const long long theirs = i < parts.size() ? parts[i] : -1;
				if (theirs != mine[i])
				{
					if (theirs > mine[i]) return "NFS-VltEd v" + std::to_string(parts[0]) + "." + std::to_string(parts[1]) + " or higher is required";
					break;
				}
			}
			return {};
		}
		// NFS-VltEd's `game` line (A/_001A_0007.cs): its codes as written, one of
		// them this database's game.
		std::string GameProblem(const std::vector<std::string>& codes, const std::string& mine)
		{
			static const char* const known[] = { "MW", "C", "PS", "UC", "W", "WB" };
			for (const std::string& g : codes)
				if (std::find(std::begin(known), std::end(known), g) == std::end(known)) return "The parameter is not valid: " + g;
			if (std::find(codes.begin(), codes.end(), mine) == codes.end())
			{
				std::string list;
				for (const std::string& g : codes) list += (list.empty() ? "" : ", ") + g;
				return "This mod supports for the following game(s): " + (list.empty() ? std::string("(none)") : list);
			}
			return {};
		}

		// Words NFS-VltEd reads as keys or bin keys: names, which scripts keep.
		bool IsKeyWord(const std::string& vltType)
		{
			const size_t slash = vltType.find('/');
			const std::string how = slash == std::string::npos ? std::string() : vltType.substr(slash + 1);
			return how == "key" || how == "binkey" || how == "key64";
		}
		// Whether NFS-VltEd's word reads the bytes of our value of kind `ours` and
		// `size` otherwise: another width, or a number where we have a float (or
		// the other way round; PresetRidePaint's 6 is a u32 over our mSaturation).
		bool VltReadsOtherwise(const std::string& t, TypeKind ours, uint32_t size)
		{
			const uint32_t n = VltSize(t);
			if (!n) return false;
			if (n != size) return true;
			const std::string f = t.substr(0, t.find_first_of("/>"));
			const bool vltFloat = f == "f32" || f == "f64";
			const bool ourFloat = ours == TypeKind::Float || ours == TypeKind::Double;
			const bool ourNumber = ours == TypeKind::Bool || ours == TypeKind::Int8 || ours == TypeKind::UInt8 || ours == TypeKind::Int16 ||
								   ours == TypeKind::UInt16 || ours == TypeKind::Int32 || ours == TypeKind::UInt32 || ours == TypeKind::Int64 ||
								   ours == TypeKind::UInt64 || ours == TypeKind::Enum || ours == TypeKind::Key || ours == TypeKind::BinKey;
			return vltFloat ? !ourFloat && ourNumber : ourFloat;
		}

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
		return ToUtf8(InFolder(FromUtf8(folder), written));
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
		// A file saved with a byte order mark: .NET's readers drop it.
		if (line.substr(0, 3) == "\xEF\xBB\xBF") i = 3;
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
			c.at.push_back(uint32_t(i));
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
			c.at.push_back(uint32_t(i));
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

	const PropertyNames::Property* PropertyNames::Type::find(std::string_view word, bool anyCase) const
	{
		for (const Property& p : properties)
			if (p.word == word) return &p;
		if (!anyCase) return nullptr;
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
				if (in >> k && k.size() > 1 && k[0] == ':')
				{
					// NFS-VltEd's own type at the offset: u32, i32/hex, u32/key, ...
					p.vltType = k.substr(1);
					if (!(in >> k)) k.clear();
				}
				if (!k.empty() && k != "list")
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

		// The scalar members of a struct, through nested structs and inline arrays
		// (VinylLayer's transform and colors[4]), at their offsets in it.
		struct Leaf
		{
			std::vector<sc::FieldPath::Step> steps;
			const MemberDef* m = nullptr;
			uint32_t offset = 0;
		};
		void Leaves(const StructDef& st, uint32_t base, std::vector<sc::FieldPath::Step>& steps, std::vector<Leaf>& out)
		{
			for (const MemberDef& m : st.members)
			{
				if (m.prim == MemberPrim::Ptr || m.isBitfield()) continue;
				const bool inner = m.st && TypeOfMember(m).kind == TypeKind::Struct;
				if (!inner && (!m.elemSize || m.elemSize > 8)) continue;
				for (uint32_t i = 0; i < std::max<uint32_t>(m.count, 1); ++i)
				{
					steps.push_back({ m.name, -1 });
					if (m.count > 1) steps.push_back({ std::string(), int64_t(i) });
					const uint32_t at = base + m.offset + i * m.elemSize;
					if (inner) Leaves(*m.st, at, steps, out);
					else out.push_back({ steps, &m, at });
					steps.resize(steps.size() - (m.count > 1 ? 2 : 1));
				}
			}
		}

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
				// NFS-VltEd's installer reads the whole script first and runs none of it
				// when one of these is wrong.
				if (st_.vltEd && !st_.firstLine)
					if (std::optional<sc::Message> why = refusal(s))
					{
						rep_.messages.push_back(*why);
						rep_.messages.push_back(sc::Message(sc::Message::Level::Info, why->pos,
							"NFS-VltEd's installer runs none of a script with this mistake, so nothing ran"));
						return rep_;
					}
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
			// What makes NFS-VltEd's installer refuse a script (A/_0003.cs, then
			// A/_0012_0007.cs), the first one: a version or game line it can't take,
			// then in the script as its included scripts make it, a ui_control that
			// doesn't read or repeats a name, a ui_option naming no control above it,
			// or one closing a block another opened.
			std::optional<sc::Message> refusal(const Script& top)
			{
				struct Line
				{
					const Command* c;
					int topLine;
					std::string file;
				};
				std::vector<Line> lines;
				std::vector<std::unique_ptr<Script>> held; // the included scripts read
				std::function<void(const Script&, int, int)> walk = [&](const Script& sc, int depth, int topLine)
				{
					for (const Command& c : sc.commands)
					{
						const int at = depth ? topLine : c.line;
						if (c.name == "script" && c.args.size() == 1 && depth < 16)
						{
							const fs::path p = InFolder(FromUtf8(sc.folder), c.args[0]);
							std::string text;
							try
							{
								if (st_.readFile) text = st_.readFile(ToUtf8(p));
								else
								{
									std::ifstream in(p, std::ios::binary);
									if (!in) continue; // the line fails when it runs
									std::ostringstream ss;
									ss << in.rdbuf();
									text = ss.str();
								}
							}
							catch (const std::exception&)
							{
								continue;
							}
							auto sub = std::make_unique<Script>(Parse(text, c.args[0], ToUtf8(p.parent_path())));
							sub->path = ToUtf8(p);
							held.push_back(std::move(sub));
							walk(*held.back(), depth + 1, at);
							continue;
						}
						lines.push_back({ &c, at, depth ? sc.path : std::string() });
					}
				};
				walk(top, 0, 0);
				auto refuse = [&](const Line& l, const std::string& why)
				{
					return sc::Message(sc::Message::Level::Error, sc::Pos{ l.topLine, 1 }, why, l.file, l.file.empty() ? 0 : l.c->line);
				};
				const std::string mine = GameCode(db_);
				for (const Line& l : lines)
				{
					const Command& c = *l.c;
					if (c.name == "version" && c.args.size() == 1)
						if (const std::string why = VersionProblem(c.args[0]); !why.empty()) return refuse(l, why);
					if (c.name == "game" && !mine.empty())
						if (const std::string why = GameProblem(c.args, mine); !why.empty()) return refuse(l, why);
				}
				std::set<std::string> controls;
				std::optional<std::string> open;
				for (const Line& l : lines)
				{
					const Command& c = *l.c;
					if (c.name == "ui_control" && c.args.size() == 4)
					{
						if (!c.error.empty()) return refuse(l, c.error);
						const std::string key = Lower(c.args[0]) == "checkbox" ? c.args[1] : c.args[1] + c.args[2];
						if (!controls.insert(key).second) return refuse(l, "Duplicate control name has been found: " + key);
					}
					else if (c.name == "ui_option" && (c.args.size() == 1 || c.args.size() == 2))
					{
						const std::string key = c.args.size() == 2 ? c.args[0] + c.args[1] : c.args[0];
						if (!controls.count(key)) return refuse(l, "The control with the specified name wasn't found: " + key);
						if (!open) open = key;
						else if (*open != key) return refuse(l, "Missing closing ui_option: " + key);
						else open.reset();
					}
				}
				return std::nullopt;
			}

			struct SpareWrite
			{
				std::string field;              // the field token, field[i]
				std::vector<std::string> words; // after it, the value last
				std::optional<sc::Statement> set; // or a value to put back (a shrunk layout array's)
			};
			// Curves as NFS-VltEd holds them: three lists it resizes freely, while the file
			// keeps the point count each was read with. Per collection and curve path.
			struct CurveShadow
			{
				std::vector<float> lists[3]; // X, Y, Y2
				uint32_t count = 0;          // what the file keeps
				bool noted = false;
			};

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
				if (st_.vltEd && (n == "append_array" || n == "add_overwrite" || n == "copy_overwrite"))
				{
					// Attribulator's commands: NFS-VltEd's pattern doesn't know them, so the
					// line is a comment there.
					note(n + " is Attribulator's: NFS-VltEd reads the line as a comment, so it does nothing");
					emitComment(n + ": skipped, as NFS-VltEd skips it");
					return;
				}
				if (n == "version")
				{
					need(c, 1, 1);
					// NFS-VltEd runs scripts for 4.6 and older, and refuses newer ones.
					if (const std::string why = VersionProblem(c.args[0]); !why.empty())
					{
						if (st_.vltEd) fail(why);
						warn(why + "; read as 4.6");
					}
				}
				else if (n == "game")
				{
					const std::string mine = GameCode(db_);
					if (st_.vltEd && !mine.empty())
					{
						if (const std::string why = GameProblem(c.args, mine); !why.empty()) fail(why);
						return;
					}
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
					const fs::path p = InFolder(FromUtf8(s.folder), c.args[0]);
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
						fs::create_directories(InFolder(FromUtf8(st_.targetFolder), c.args[0]), ec);
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
					if (c.args[2].empty() || HasWhiteSpace(c.args[2])) fail("a node name can't be empty or hold white space");
					if (find(k, c.args[2])) fail("there is already a node called '" + c.args[2] + "'");
					// A hash is a key, as NFS-VltEd reads it (0x and up to 8 hex digits).
					const bool hash = IsHex(c.args[2]);
					const bool known = hash || db_.names().find(StringHash32(c.args[2])) != nullptr;
					sc::Statement r;
					r.kind = K::Rename;
					r.target = targetOf(x);
					r.word = c.args[2];
					emit(std::move(r));
					if (hash) CollectionEditor(db_).rename(x, KeyOf(c.args[2]));
					else CollectionEditor(db_).rename(x, c.args[2]);
					if (!known) rep_.newNames.push_back(c.args[2]);
					const std::string* shown = hash ? db_.names().find(x.key) : nullptr;
					setCollectionName(x, shown ? *shown : c.args[2]);
					touch(x.vault);
					++rep_.renamed;
				}
				else if (n == "move_node")
				{
					need(c, st_.vltEd ? 3 : 2, 3); // NFS-VltEd always names the parent
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
					if (st_.vltEd)
					{
						requireOwn(x, *f, c.args[2]);
						char* end = nullptr;
						const long long want = std::strtoll(c.args[3].c_str(), &end, 10);
						if (c.args[3].empty() || !end || *end) fail("Value cannot be parsed: " + c.args[3] + " isn't a number");
						resizeCompat(k, x, *f, c.args[2], want);
					}
					else
					{
						sc::Statement r;
						r.kind = K::Resize;
						r.path.field = c.args[2];
						r.count = uint32_t(std::strtoul(c.args[3].c_str(), nullptr, 10));
						amod(x, { r });
					}
				}
				else if (n == "resize_collection")
				{
					need(c, 4, st_.vltEd ? 5 : 64); // NFS-VltEd: one word at most
					ClassDef& k = cls(c.args[0]);
					Collection& x = node(k, c.args[1]);
					sc::FieldPath p = fieldPath(x, c.args[2]);
					const FieldDef* f = k.field(KeyOf(p.field));
					if (f) requireOwn(x, *f, p.field);
					RefPart part = RefPart::Whole;
					elementWords_ = inElement_ = nullptr;
					for (size_t i = 3; i + 1 < c.args.size(); ++i) property(x, p, c.args[i], part);
					// NFS-VltEd resizes a curve's list without the point count the file keeps.
					if (st_.vltEd && f && curvePoint(x, p, nullptr, std::strtoll(c.args.back().c_str(), nullptr, 10))) return;
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
					if (st_.vltEd && !x.find(f->key)) fail("The node '" + c.args[1] + "' does not contain '" + c.args[2] + "' field.");
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
					if (st_.vltEd) fail("The command is not supported (NFS-VltEd's message; it doesn't run convert_field)");
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
					forgetCompat(*it);
					ed.remove(**it);
					++rep_.deleted;
				}
			}

			// A class with a CollectionName field keeps the name there too (Attribulator does this).
			void setCollectionName(Collection& c, const std::string& n)
			{
				const FieldDef* f = c.cls ? c.cls->field(StringHash32("CollectionName")) : nullptr;
				if (!f) return;
				std::string text = n;
				if (st_.vltEd)
				{
					// NFS-VltEd sets it in the layout only, to the node's name as it
					// shows it: the dictionary's, else the key in lower-case hex.
					if (!f->inLayout()) return;
					if (const std::string* known = db_.names().find(c.key)) text = *known;
					else
					{
						char b[16];
						std::snprintf(b, sizeof(b), "0x%08x", c.key);
						text = b;
					}
				}
				sc::Statement s;
				s.kind = K::Set;
				s.path.field = "CollectionName";
				s.value = sc::MakeString(text);
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
				if (n.empty() || HasWhiteSpace(n)) fail("a node name can't be empty or hold white space");
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
				if (st_.vltEd) layoutFromDefault(k, x);
				setCollectionName(x, n);
				touch(vault);
				++rep_.created;
			}

			// NFS-VltEd's add_node starts the layout as the class's default collection has it.
			void layoutFromDefault(ClassDef& k, Collection& x)
			{
				const Collection* def = db_.findCollection(k.key, StringHash32("default"));
				if (!def || def == &x) return;
				std::vector<uint32_t> copied;
				for (Attribute& a : x.attributes)
					if (a.inLayout)
						if (const Attribute* d = def->find(a.key))
						{
							a = *d;
							DetachCopy(a);
							copied.push_back(a.key);
						}
				if (out_ && !copied.empty())
				{
					std::vector<sc::Statement> body;
					for (uint32_t key : copied)
						for (sc::Statement& v : sc::ExportValue(db_, x, key).body) body.push_back(std::move(v));
					emitEdit(x, std::move(body));
				}
			}
			// What NFS-VltEd holds past our model goes with a copy: slots past an array's
			// count, a curve's lists. `fields`: only those (copy_fields), else all.
			void copyHeld(const Collection& from, const Collection& to, const std::vector<uint32_t>* fields = nullptr)
			{
				auto wanted = [&](uint32_t key)
				{
					return !fields || std::find(fields->begin(), fields->end(), key) != fields->end();
				};
				std::vector<std::pair<uint32_t, std::map<uint32_t, std::vector<SpareWrite>>>> slots;
				for (const auto& [key, held] : spare_)
					if (key.first == &from && wanted(key.second)) slots.push_back({ key.second, held });
				for (auto& [field, held] : slots) spare_[{ &to, field }] = std::move(held);
				std::vector<std::pair<std::string, CurveShadow>> lists;
				for (const auto& [key, sh] : curves_)
					if (key.first == &from && (!fields || wanted(KeyOf(key.second.substr(0, key.second.find_first_of("[."))))))
						lists.push_back({ key.second, sh });
				for (auto& [path, sh] : lists) curves_[{ &to, path }] = std::move(sh);
			}

			void copyNode(const Command& c, bool overwrite)
			{
				need(c, 3, 4);
				ClassDef& k = cls(c.args[0]);
				Collection& src = node(k, c.args[1]);
				Collection* parent = c.args.size() == 4 ? &node(k, c.args[2]) : nullptr;
				const std::string& n = c.args.back();
				if (n.empty() || HasWhiteSpace(n)) fail("a node name can't be empty or hold white space");
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
				if (st_.vltEd) copyHeld(src, x);
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
				const std::string& vn = c.args[2];
				if (st_.vltEd)
				{
					// NFS-VltEd moves gameplay nodes only, and never into the vaults of
					// the other files (A/_0013_0007.cs).
					if (vn.empty()) fail("Name cannot be empty");
					if (HasWhiteSpace(vn)) fail("Vault name cannot contain white space");
				}
				ClassDef& k = cls(c.args[0]);
				if (st_.vltEd && name(k.key) != "gameplay") fail("Change vault name for node of the class '" + c.args[0] + "' is not supported.");
				Collection& x = node(k, c.args[1]);
				if (st_.vltEd)
				{
					const std::string l = Lower(vn);
					if (l == "db" || l == "frontend" || l == "cars" || l == "commerce")
						fail("The vault name cannot be changed to one of the following: db, frontend, cars, commerce");
				}
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
				if (x.find(f->key))
				{
					if (st_.vltEd) fail("The node '" + c.args[1] + "' already contains the '" + c.args[2] + "' field.");
					return; // there already
				}
				if (st_.vltEd && c.args.size() == 4)
				{
					const std::string& n = c.args[3];
					const size_t from = !n.empty() && n[0] == '+' ? 1 : 0;
					if (from >= n.size() || !std::all_of(n.begin() + from, n.end(), [](char ch)
					{
						return std::isdigit(static_cast<unsigned char>(ch));
					}))
						fail("Value cannot be parsed: " + n + " isn't a count");
				}
				const uint32_t count = c.args.size() == 4 ? uint32_t(std::strtoul(c.args[3].c_str(), nullptr, 10)) : 0;
				if (f->isArray() && count > 0xFFFF) fail("'" + c.args[2] + "' holds at most 65535 elements");
				if (st_.vltEd && addFieldFromDefault(k, x, *f, count, c.args[2])) return;
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

			// NFS-VltEd's add_field: a copy of the class's default collection's own
			// value. An array gets `count` slots, the first from default's, and default's
			// count (the slots past it are held, not read). False when default has no
			// value of its own there: then zeros, as without the switch.
			bool addFieldFromDefault(ClassDef& k, Collection& x, const FieldDef& f, uint32_t count, const std::string& field)
			{
				const Collection* def = db_.findCollection(k.key, StringHash32("default"));
				const Attribute* dv = def && def != &x ? def->find(f.key) : nullptr;
				if (!dv) return false;
				Attribute& a = OverrideAttribute(db_, x, f);
				a = *dv;
				DetachCopy(a);
				if (a.isArray)
				{
					if (dv->count > count) // only `count` slots: default's first
						while (a.count > count) RemoveElement(a, a.count - 1);
					a.capacity = uint16_t(std::max<uint32_t>(count, a.count));
				}
				touch(x.vault);
				++rep_.valuesSet;
				if (out_) emitEdit(x, sc::ExportValue(db_, x, f.key).body);
				note(field + " starts as " + db_.names().format(k.key) + "/default's, as in NFS-VltEd" +
					 (a.isArray && a.capacity > a.count ? ": " + std::to_string(a.count) + " of " + std::to_string(a.capacity) + " slots are read" : std::string()));
				return true;
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
				// At the end of whatever the array holds (an amod append), so a script
				// converted from it adds the element on any database.
				sc::Statement s;
				s.kind = K::Append;
				s.path.field = c.args[2];
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
				// As NFS-VltEd (A/_0011_0007.cs): base copies the layout fields;
				// of the source's own other values, overwrite replaces the ones the
				// target has and optional adds the ones it lacks. Inherited values
				// aren't copied. The flags are case-sensitive there.
				bool base = false, optional = false, overwrite = false;
				std::stringstream ss(c.args[3]);
				for (std::string o; std::getline(ss, o, '|');)
				{
					if (!st_.vltEd) o = Lower(o);
					base |= o == "base";
					optional |= o == "optional";
					overwrite |= o == "overwrite";
				}
				if (!base && !optional && !overwrite) fail("copy_fields needs base, optional or overwrite (or several, joined with |)");
				std::vector<uint32_t> copied;
				for (const Attribute& a : src.attributes)
				{
					Attribute* there = dst.find(a.key);
					if (a.inLayout ? !base : there ? !overwrite : !optional) continue;
					Attribute copy = a;
					DetachCopy(copy);
					if (there) *there = std::move(copy);
					else dst.attributes.push_back(std::move(copy));
					++rep_.valuesSet;
					copied.push_back(a.key);
				}
				// The target keeps its own name in CollectionName, as NFS-VltEd sets it back.
				if (st_.vltEd) copyHeld(src, dst, &copied);
				if (const FieldDef* cn = k.field(StringHash32("CollectionName")); base && cn && cn->inLayout())
					setCollectionName(dst, db_.names().format(dst.key));
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
				const fs::path from = InFolder(FromUtf8(s.folder), c.args[0]);
				std::error_code ec;
				if (!fs::exists(from, ec)) fail("file does not exist: " + c.args[0]);
				if (!SafeRelative(c.args[1])) fail("file_copy needs a destination inside the game's folder: " + c.args[1]);
				if (!st_.doFiles || st_.targetFolder.empty()) return;
				const fs::path to = InFolder(FromUtf8(st_.targetFolder), c.args[1]);
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
					if (st_.vltEd) fail("NFS-VltEd can't read [^] (Attribulator's): give the index");
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
						else if (off == 8 && st_.vltEd) part = RefPart::Pointer;
						else fail(word + " is a reference's pointer, which the game sets" + (off == 8 ? " (NFS-VltEd compatible runs set it)" : ""));
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
					{
						// NFS-VltEd's MODEL words set a hash or the text alone.
						const StringKeyLayout kl = StringKeyAt(vt.size);
						if (st_.vltEd) part = off == kl.text ? RefPart::Text : kl.hash64 && off < kl.hash32 ? RefPart::Hash64 : RefPart::Hash;
						if (off == kl.text || st_.vltEd) return; // its text (the hash follows it)
						fail(word + " is a StringKey's hash: set its text");
					}
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
						else if (st_.vltEd) part = RefPart::Pointer; // NFS-VltEd writes the pointer word too
						else fail(word + " is a reference's pointer, which the game sets (NFS-VltEd compatible runs set it)");
						return;
					case TypeKind::StringKey:
						if (st_.vltEd)
						{
							part = l == "mstring" ? RefPart::Text : l == "mhash64" ? RefPart::Hash64 : RefPart::Hash;
							return;
						}
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
					if (st_.vltEd && t && t->single && vt.size) return; // NFS-VltEd ignores the word
					const PropertyNames::Property* pr = t && !t->single ? t->find(token, !st_.vltEd) : nullptr;
					if (pr)
					{
						noteWord(*pr, p, vt);
						stepToProperty(x, p, vt, *pr, part, token);
						elementWords_ = pr->list ? &pr->elements : nullptr;
					}
					else
					{
						// NFS-VltEd takes a number as an index only after a list's word.
						if (st_.vltEd && t && vt.size) fail(NotFound(*t, token));
						p.steps.push_back({ std::string(), std::strtoll(token.c_str(), nullptr, 10) });
						inElement_ = elementWords_;
						elementWords_ = nullptr;
					}
					return;
				}
				const size_t lb = token.find('[');
				const std::string prop = token.substr(0, lb);
				std::string idx = lb == std::string::npos ? std::string() : token.substr(lb + 1, token.find(']', lb) - lb - 1);
				const std::string l = Lower(prop);
				const ValueType vt = sc::TypeAt(db_, x, p);
				// An element of a list: NFS-VltEd matches its word exactly among the
				// element's words (Value; Group, Name, Value).
				const std::vector<std::string>* element = inElement_;
				inElement_ = nullptr;
				if (st_.vltEd && element && !element->empty() && std::find(element->begin(), element->end(), token) == element->end())
				{
					std::string words;
					for (const std::string& w : *element) words += (words.empty() ? "" : ", ") + w;
					fail("Value '" + token + "' cannot be found (the element's words: " + words + ")");
				}
				if (const PropertyNames::Type* t = vltType(p, vt))
				{
					if (t->single) return; // NFS-VltEd ignores the word; update() steps to its one property
					const PropertyNames::Property* pr = t->find(token, !st_.vltEd);
					if (!pr && !idx.empty() && !st_.vltEd) pr = t->find(prop);
					else if (pr) idx.clear(); // M[1,1]: the index is part of the word
					if (pr)
					{
						noteWord(*pr, p, vt);
						stepToProperty(x, p, vt, *pr, part, token);
						elementWords_ = pr->list ? &pr->elements : nullptr;
						if (!idx.empty()) p.steps.push_back({ std::string(), std::strtoll(idx.c_str(), nullptr, 10) });
						return;
					}
					// NFS-VltEd knows only its own words, as written.
					if (st_.vltEd && !element) fail(NotFound(*t, token));
				}
				else if (st_.names && (st_.vltEd ? prop == "Data" : l == "data") && idx.empty())
				{
					part = RefPart::Data; // NFS-VltEd's raw class: the value's bytes
					return;
				}
				else if (st_.vltEd && st_.names && !element)
				{
					// A type NFS-VltEd has no class for: its raw class has Data alone.
					fail("Value '" + token + "' cannot be found: NFS-VltEd has no words for " + vt.typeName + " but Data, its bytes in hex");
				}
				switch (vt.kind)
				{
					case TypeKind::RefSpec:
						// Group and Name: NFS-VltEd's words for a reference in a list.
						if (l == "collection" || l == "collectionkey" || l == "name") part = RefPart::Collection;
						else if (l == "class" || l == "classkey" || l == "group") part = RefPart::Class;
						// NFS-VltEd's list element writes its third word too, the pointer the game sets.
						else if (l == "value" && st_.vltEd) part = RefPart::Pointer;
						else fail("a reference has Class and Collection, not " + prop + (l == "value" ? " (NFS-VltEd compatible runs set its pointer word)" : ""));
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
				if (vt.kind == TypeKind::Struct)
				{
					noteWord(t->properties[0], p, vt);
					stepToProperty(x, p, vt, t->properties[0], part, t->properties[0].word);
				}
			}
			// NFS-VltEd's message for a word its class doesn't have, with the words it has.
			static std::string NotFound(const PropertyNames::Type& t, const std::string& token)
			{
				std::string words;
				for (const auto& w : t.properties) words += (words.empty() ? "" : ", ") + w.word;
				return "Value '" + token + "' cannot be found (NFS-VltEd's words: " + words + ")";
			}
			void noteWord(const PropertyNames::Property& pr, const sc::FieldPath& p, const ValueType& vt)
			{
				lastProp_ = &pr;
				lastBase_ = p;
				lastBaseType_ = vt;
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
					if (st_.vltEd)
					{
						// NFS-VltEd's raw class: tokens split on spaces, each one or two
						// hex digits in any case, exactly as many as the value's bytes.
						std::istringstream in(t);
						std::string tok;
						size_t n = 0;
						while (in >> tok)
						{
							if (tok.size() > 2 || !std::all_of(tok.begin(), tok.end(), [](char ch)
							{
								return std::isxdigit(static_cast<unsigned char>(ch));
							}))
								fail("Value cannot be parsed: " + tok + " isn't a byte in hex");
							hex += tok.size() == 1 ? "0" + tok : tok;
							++n;
						}
						if (n != vt.size) fail("Length does not match: Data needs " + std::to_string(vt.size) + " bytes, not " + std::to_string(n));
					}
					else
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
					if (st_.vltEd)
					{
						v->text.clear();
						for (size_t i = 0; i + 1 < hex.size(); i += 2) v->text += hex.substr(i, 2) + " ";
					}
					return v;
				}
				if (vt.kind == TypeKind::RefSpec && part == RefPart::Pointer)
				{
					// NFS-VltEd writes the word after the keys as given: a u32, or an i32
					// in some types (FETuningSlider's 3).
					char* end = nullptr;
					const long long n = IsHex(t) ? (long long)std::strtoull(t.c_str() + 2, &end, 16) : std::strtoll(t.c_str(), &end, 10);
					if (t.empty() || !end || *end || n > 0xFFFFFFFFll || n < INT32_MIN) fail("Value cannot be parsed: " + t + " isn't a 32-bit number");
					if (vt.size != 12 || cur.size() < 12) fail("this reference has no 32-bit pointer word after its keys");
					Bytes b = cur;
					store32(b.data() + 8, uint32_t(n), e);
					std::string hex;
					for (uint8_t ch : b)
					{
						char buf[4];
						std::snprintf(buf, sizeof(buf), "%02X ", ch);
						hex += buf;
					}
					auto v = std::make_shared<sc::Value>();
					v->kind = sc::Value::Kind::Bytes;
					v->text = hex;
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
					{
						// NFS-VltEd reads 1,000.5 (en-US: the comma groups thousands).
						std::string n = t;
						if (st_.vltEd) n.erase(std::remove(n.begin(), n.end(), ','), n.end());
						if (IsNumber(n) || IsHex(n)) return sc::MakeNumber(n);
						fail("not a number: " + t);
					}
					case TypeKind::Enum: return IsHex(t) || IsNumber(t) ? sc::MakeNumber(t) : sc::MakeString(t);
					case TypeKind::Text:
					case TypeKind::StringKey: return sc::MakeString(t);
					case TypeKind::BinKey:
						// NFS-VltEd reads "" as 0xFFFFFFFF and a decimal number in a bin key as the key itself.
						if (st_.vltEd && t.empty()) return sc::MakeNumber("0xFFFFFFFF");
						if (st_.vltEd && !t.empty() && std::all_of(t.begin(), t.end(), [](char ch)
						{
							return std::isdigit(static_cast<unsigned char>(ch));
						}) && std::strtoull(t.c_str(), nullptr, 10) <= 0xFFFFFFFFull)
							return sc::MakeNumber(t);
						return keyValue(t);
					case TypeKind::Key:
					case TypeKind::ClassRefSpec: return keyValue(t);
					default: break;
				}
				fail((vt.typeName.empty() ? std::string("this value") : vt.typeName) + " takes more than one value: name a property");
			}

			void update(const Command& c)
			{
				// NFS-VltEd: update_field has one word at most, update_collection a list's
				// word (or none), an index and the element's word.
				if (st_.vltEd) need(c, c.name == "update_collection" ? 6 : 4, c.name == "update_collection" ? 7 : 5);
				else need(c, 4, 64);
				ClassDef& k = cls(c.args[0]);
				Collection& x = node(k, c.args[1]);
				updateIn(k, x, c.args[2], std::vector<std::string>(c.args.begin() + 3, c.args.end()), c.name == "update_collection");
			}

			// update_field in a node: the field token, then the words after it with the value last.
			void updateIn(ClassDef& k, Collection& x, const std::string& fieldToken, const std::vector<std::string>& rest, bool collection = false)
			{
				const std::string fieldName = fieldToken.substr(0, fieldToken.find('['));
				const FieldDef* f = k.field(KeyOf(fieldName));
				if (!f) fail("the class '" + db_.names().format(k.key) + "' does not contain '" + fieldName + "'");
				requireOwn(x, *f, fieldName);
				sc::FieldPath p = fieldPath(x, fieldToken);
				if (st_.vltEd)
				{
					// NFS-VltEd sets element 0 of an array given no index, and ignores an
					// index on a field that isn't an array.
					if (f->isArray() && p.steps.empty()) p.steps.push_back({ std::string(), 0 });
					else if (!f->isArray()) p.steps.clear();
				}
				const std::string& text = rest.back();
				if (st_.vltEd && spareWrite(x, *f, p, fieldToken, rest)) return;
				RefPart part = RefPart::Whole;
				elementWords_ = inElement_ = nullptr;
				size_t first = 0;
				if (st_.vltEd && collection)
				{
					// NFS-VltEd's update_collection: a list's word, the index and the
					// element's word. Without the list's word, the element's class must
					// have one property, and it a list.
					const ValueType vt0 = sc::TypeAt(db_, x, p);
					const PropertyNames::Type* t0 = vltType(p, vt0);
					const std::string w = rest.size() == 4 ? rest[0] : std::string();
					const PropertyNames::Property* list = !t0 ? nullptr
						: rest.size() == 4 ? t0->find(w, false)
						: t0->single && !t0->properties.empty() ? &t0->properties[0] : nullptr;
					if (!list) fail(t0 && !w.empty() ? NotFound(*t0, w) : "Value '" + w + "' is not a collection.");
					if (!list->list) fail("Value '" + w + "' is not a collection.");
					lastProp_ = nullptr;
					noteWord(*list, p, vt0);
					stepToProperty(x, p, vt0, *list, part, list->word);
					elementWords_ = &list->elements;
					first = rest.size() == 4 ? 1 : 0;
				}
				for (size_t i = first; i + 1 < rest.size(); ++i)
				{
					lastProp_ = nullptr;
					property(x, p, rest[i], part);
				}
				if (rest.size() == 1) lastProp_ = nullptr;
				single(x, p, part);
				if (st_.vltEd)
				{
					// Its class for a reference has two properties: no word finds neither.
					if (rest.size() == 1 && part == RefPart::Whole && sc::TypeAt(db_, x, p).kind == TypeKind::RefSpec)
						fail("Value '' cannot be found: a reference takes Class or Collection");
					if (curvePoint(x, p, &text)) return;
					if (part == RefPart::Hash || part == RefPart::Hash64 || part == RefPart::Text)
					{
						stringKeyPart(x, *f, p, part, text);
						return;
					}
					if (part == RefPart::Whole && lastProp_ && wideWrite(x, text)) return;
					if (part == RefPart::Whole && !lastProp_ && oneValue(x, p, text)) return;
				}
				Bytes cur;
				const ValueType vt = sc::TypeAt(db_, x, p, &cur);
				sc::Statement s;
				s.kind = K::Set;
				s.path = p;
				s.value = value(text, vt, cur, part, x);
				if (part == RefPart::Data && x.vault && x.vault->raw.endian == Endian::Big)
					message(sc::Message::Level::Warning, "Data sets the value's bytes as this platform's files hold them, big-endian; "
							"a script written for the PC gives them little-endian");
				amod(x, { s });
				if ((vt.kind == TypeKind::RefSpec || vt.kind == TypeKind::ClassRefSpec) && part != RefPart::Data && part != RefPart::Pointer)
					setRefs_.push_back({ x.classKey, x.key, p, line_, lineTop_, depth_, source_, path_, text });
			}

			// ---- NFS-VltEd compatible runs

			// A value command on an array slot past the count but held (below the
			// capacity): NFS-VltEd writes the slot and the game doesn't read it, until a
			// resize_field brings it in. Kept for then.
			bool spareWrite(Collection& x, const FieldDef& f, const sc::FieldPath& p, const std::string& fieldToken,
							const std::vector<std::string>& rest)
			{
				if (!f.isArray() || p.steps.empty() || !p.steps[0].member.empty()) return false;
				const Attribute* a = x.find(f.key);
				if (!a) return false;
				const int64_t i = p.steps[0].index;
				if (i < int64_t(a->count) || i >= int64_t(SlotsHeld(*a))) return false;
				auto& slots = spare_[{ &x, f.key }];
				if (slots.empty())
					note(fieldToken.substr(0, fieldToken.find('[')) + " holds " + std::to_string(a->count) + " of its " +
						 std::to_string(SlotsHeld(*a)) + " slots: NFS-VltEd keeps values past them for a resize_field that brings them in");
				slots[uint32_t(i)].push_back({ fieldToken, rest, std::nullopt });
				return true;
			}

			// The slots [from, to) of an array a resize_field brought in: the values
			// written there before.
			void replaySpare(ClassDef& k, Collection& x, const FieldDef& f, uint32_t from, uint32_t to)
			{
				const auto it = spare_.find({ &x, f.key });
				if (it == spare_.end()) return;
				auto& slots = it->second;
				for (auto sl = slots.lower_bound(from); sl != slots.end() && sl->first < to;)
				{
					const std::vector<SpareWrite> writes = std::move(sl->second);
					sl = slots.erase(sl);
					for (const SpareWrite& w : writes)
					{
						if (w.set) amod(x, { *w.set });
						else updateIn(k, x, w.field, w.words);
					}
				}
				if (slots.empty()) spare_.erase(it);
			}

			// NFS-VltEd's resize_field: a layout array's count moves within its slots,
			// which keep their values; an entry array is cut or grows with zeros, and
			// its capacity follows.
			void resizeCompat(ClassDef& k, Collection& x, const FieldDef& f, const std::string& field, long long n)
			{
				Attribute* a = x.find(f.key);
				if (!a) fail("Field '" + field + "' does not exist in the node '" + db_.names().format(x.key) + "'");
				if (n < 0) fail("Array size must be greater than 0.");
				const uint32_t old = a->count;
				if (a->inLayout)
				{
					const uint32_t held = SlotsHeld(*a);
					if (uint64_t(n) > held)
						fail("Size of the array '" + field + "' cannot be changed to " + std::to_string(n) + ". The correct value is from 0 to " +
							 std::to_string(held));
					if (uint32_t(n) < old)
					{
						// What the shrink takes out stays in the slots for a later growth.
						const sc::Statement whole = sc::ExportValue(db_, x, f.key);
						const sc::Value* list = whole.body.empty() ? nullptr : whole.body[0].value.get();
						if (list && list->kind == sc::Value::Kind::List)
							for (uint32_t i = uint32_t(n); i < old && i < list->items.size(); ++i)
							{
								sc::Statement set;
								set.kind = K::Set;
								set.path.field = field;
								set.path.steps.push_back({ std::string(), int64_t(i) });
								set.value = list->items[i];
								auto& w = spare_[{ &x, f.key }][i];
								w.insert(w.begin(), SpareWrite{ field, {}, set });
							}
					}
				}
				else if (n > 32767)
				{
					fail("Value cannot be parsed: " + std::to_string(n) + " doesn't fit NFS-VltEd's array size (an Int16)");
				}
				sc::Statement r;
				r.kind = K::Resize;
				r.path.field = field;
				r.count = uint32_t(n);
				amod(x, { r });
				if (uint32_t(n) > old) replaySpare(k, x, f, old, uint32_t(n));
				else if (!a->inLayout)
				{
					// Cut: the slots past it go.
					const auto it = spare_.find({ &x, f.key });
					if (it != spare_.end()) it->second.erase(it->second.lower_bound(uint32_t(n)), it->second.end());
				}
			}

			// A path to one of a curve's lists (X, Y, Y2), with or without an index:
			// the curve's path, which list, and the index (-1 for none).
			bool curveList(Collection& x, const sc::FieldPath& p, sc::FieldPath& curve, int& list, int64_t& index)
			{
				size_t n = p.steps.size();
				index = -1;
				if (n && p.steps[n - 1].member.empty())
				{
					index = p.steps[n - 1].index;
					--n;
				}
				if (!n || p.steps[n - 1].member.empty()) return false;
				const std::string m = Lower(p.steps[n - 1].member);
				list = m == "x" ? 0 : m == "y" ? 1 : m == "y2" ? 2 : -1;
				if (list < 0) return false;
				curve = p;
				curve.steps.resize(n - 1);
				return sc::TypeAt(db_, x, curve).kind == TypeKind::Curve;
			}
			// The curve at `curve` as the collection holds it now: a curve field, an
			// element of one, or a member of a struct (AI::GlueCurve's Easy).
			CurveValue curveAt(Collection& x, const sc::FieldPath& curve)
			{
				const ValueType vt = sc::TypeAt(db_, x, curve);
				uint32_t off = 0;
				const DataBlock* b = sc::BlockAt(db_, x, curve, &off);
				if (!b || uint64_t(off) + vt.size > b->bytes.size()) return CurveValue{};
				return ReadCurve(*b, off, x.vault ? x.vault->raw.endian : Endian::Little, vt.size);
			}
			// The curve as NFS-VltEd holds it, read the first time a line touches it.
			CurveShadow& shadowOf(Collection& x, const sc::FieldPath& curve)
			{
				auto [it, made] = curves_.try_emplace({ &x, curve.text() });
				if (!made) return it->second;
				CurveValue c;
				try
				{
					c = curveAt(x, curve);
				}
				catch (...)
				{
					curves_.erase(it);
					throw;
				}
				it->second.lists[0] = c.x;
				it->second.lists[1] = c.y;
				it->second.lists[2] = c.y2;
				it->second.count = uint32_t(c.x.size());
				return it->second;
			}
			// A curve point set (with `text`), or a list resized (`size`): NFS-VltEd
			// changes its list alone, never works out Y2, and saves each list's count as
			// read, so the game reads the first count values from where each list starts
			// in the lists it wrote one after another.
			bool curvePoint(Collection& x, const sc::FieldPath& p, const std::string* text, int64_t size = -1)
			{
				sc::FieldPath curve;
				int list = 0;
				int64_t index = -1;
				if (!curveList(x, p, curve, list, index)) return false;
				CurveShadow& sh = shadowOf(x, curve);
				std::vector<float>& l = sh.lists[list];
				if (text)
				{
					if (index < 0) fail("a curve list's element needs its index");
					if (uint64_t(index) >= l.size()) fail("The array index '" + std::to_string(index) + "' out of bounds.");
					std::string t = *text;
					t.erase(std::remove(t.begin(), t.end(), ','), t.end());
					char* end = nullptr;
					const float v = std::strtof(t.c_str(), &end);
					if (t.empty() || !end || *end) fail("Value cannot be parsed: " + *text + " isn't a number");
					l[size_t(index)] = v;
				}
				else
				{
					if (size < 0 || size > 65536) fail("the size must be from 0 to 65536");
					l.resize(size_t(size), 0.0f);
				}
				// What the game reads from what NFS-VltEd writes.
				std::vector<float> block;
				for (const auto& part : sh.lists) block.insert(block.end(), part.begin(), part.end());
				const CurveValue now = curveAt(x, curve);
				const std::vector<float>* nowLists[3] = { &now.x, &now.y, &now.y2 };
				std::vector<sc::Statement> sets;
				size_t start = 0;
				bool past = false;
				for (int i = 0; i < 3; ++i)
				{
					std::vector<sc::ValuePtr> items;
					for (uint32_t j = 0; j < sh.count; ++j)
					{
						float v = 0;
						if (start + j < block.size()) v = block[start + j];
						else
						{
							past = true; // past what NFS-VltEd writes: what follows in the file
							if (j < nowLists[i]->size()) v = (*nowLists[i])[j];
						}
						items.push_back(sc::MakeNumber(sc::NumberText(v, true)));
					}
					start += sh.lists[i].size();
					sc::Statement set;
					set.kind = K::Set;
					set.path = curve;
					set.path.steps.push_back({ i == 0 ? "X" : i == 1 ? "Y" : "Y2", -1 });
					set.value = sc::MakeList(std::move(items));
					sets.push_back(std::move(set));
				}
				if (!sh.noted && (sh.lists[0].size() != sh.count || sh.lists[1].size() != sh.count || sh.lists[2].size() != sh.count))
				{
					sh.noted = true;
					note(curve.text() + "'s lists are resized, and NFS-VltEd keeps the " + std::to_string(sh.count) +
						 " points the file has: the game reads each list's " + std::to_string(sh.count) +
						 " values from where it starts in the lists NFS-VltEd writes one after another" +
						 (past ? "; the last ones would come from what follows in the file, kept as they were here" : ""));
				}
				amod(x, sets);
				return true;
			}

			// MODEL's Collision (the hash alone) and Visual (the text alone).
			void stringKeyPart(Collection& x, const FieldDef& f, const sc::FieldPath& p, RefPart part, const std::string& text)
			{
				const Attribute* a = x.resolve(f.key);
				if (!a || !p.steps.empty() || (a->elemSize != 8 && a->elemSize != 16))
					fail("NFS-VltEd compatible runs set a StringKey field's hash or text alone");
				const Endian e = x.vault ? x.vault->raw.endian : Endian::Little;
				const StringKeyLayout kl = StringKeyAt(a->elemSize);
				uint32_t hash = load32(a->data.bytes.data() + kl.hash32, e);
				uint64_t hash64 = kl.hash64 ? load64(a->data.bytes.data(), e) : 0;
				const PtrSlot* ts = a->data.ptrAt(kl.text);
				std::string t = ts && ts->isString ? ts->text : std::string();
				if (part == RefPart::Hash)
				{
					// A key: a name hashed, 0x and up to 8 hex digits as the number.
					hash = KeyOf(text);
				}
				else if (part == RefPart::Hash64)
				{
					// Most Wanted's 64-bit hash: a name hashed with StringHash64, 0x hex the number.
					const Bytes b = vltBytes("u64/key64", text);
					hash64 = load64(b.data(), Endian::Little);
				}
				else
				{
					t = text;
				}
				auto v = std::make_shared<sc::Value>();
				v->kind = sc::Value::Kind::Tuple;
				v->items = { sc::MakeString(t), sc::MakeNumber(Hex32(hash)) };
				if (kl.hash64)
				{
					char b[24];
					std::snprintf(b, sizeof(b), "0x%016llX", static_cast<unsigned long long>(hash64));
					v->items.push_back(sc::MakeNumber(b));
				}
				sc::Statement s;
				s.kind = K::Set;
				s.path = p;
				s.value = v;
				amod(x, { s });
			}

			// NFS-VltEd's word as a number of its own type, its bytes as the PC files
			// hold them (little-endian), the text read as NFS-VltEd reads it: a plain
			// word through .NET's parsing under en-US (Int32.Parse and the like:
			// decimal digits and a sign, nothing else; Single.Parse: digits, commas
			// grouping thousands, an exponent, Infinity and NaN, no hex;
			// Boolean.Parse: true or false), the others through its converters
			// (A/_000C_0014.cs maps them): /hex (A/_0016_0001.cs) 0x and hex digits
			// or a decimal number; /hexdigits (A/_001F_0001.cs) hex digits, 0x or
			// not; /key a name hashed (0x hex the number, "" 0); /binkey a decimal
			// number, else "" 0xFFFFFFFF, 0x hex or bStringHash; /key64 StringHash64.
			Bytes vltBytes(const std::string& type, const std::string& text)
			{
				const uint32_t size = VltSize(type);
				const std::string base = type.substr(0, type.find_first_of("/>"));
				const std::string how = type.find('/') != std::string::npos ? type.substr(type.find('/') + 1) : std::string();
				Bytes b(size, 0);
				auto put = [&](uint64_t u)
				{
					for (uint32_t i = 0; i < size; ++i) b[i] = uint8_t(u >> (8 * i));
				};
				auto bad = [&](const std::string& why) -> void
				{
					fail("Value cannot be parsed: " + text + why);
				};
				std::string t = text;
				const auto ws = [](char ch)
				{
					return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
				};
				while (!t.empty() && ws(t.front())) t.erase(t.begin());
				while (!t.empty() && ws(t.back())) t.pop_back();
				const auto digitsFrom = [](const std::string& u, size_t from)
				{
					return from < u.size() && std::all_of(u.begin() + from, u.end(), [](char ch)
					{
						return std::isdigit(static_cast<unsigned char>(ch));
					});
				};
				const auto hexFrom = [](const std::string& u, size_t from)
				{
					return from < u.size() && std::all_of(u.begin() + from, u.end(), [](char ch)
					{
						return std::isxdigit(static_cast<unsigned char>(ch));
					});
				};
				const auto hexPrefix = [](const std::string& u)
				{
					return u.size() >= 2 && u[0] == '0' && (u[1] == 'x' || u[1] == 'X');
				};
				// Hex digits as a number of at most `bits` (leading zeros are fine).
				const auto hexValue = [&](const std::string& u, size_t from, uint32_t bits, uint64_t& out)
				{
					if (!hexFrom(u, from)) return false;
					size_t i = from;
					while (i + 1 < u.size() && u[i] == '0') ++i;
					if ((u.size() - i) * 4 > bits) return false;
					out = std::strtoull(u.c_str() + i, nullptr, 16);
					return true;
				};
				// UInt32.TryParse: digits with a sign; -0 is 0.
				const auto unsignedValue = [&](const std::string& u, uint64_t limit, uint64_t& out)
				{
					const size_t from = !u.empty() && (u[0] == '+' || u[0] == '-') ? 1 : 0;
					if (!digitsFrom(u, from)) return false;
					errno = 0;
					const unsigned long long n = std::strtoull(u.c_str() + from, nullptr, 10);
					if (errno == ERANGE || n > limit || (u[0] == '-' && n != 0)) return false;
					out = n;
					return true;
				};

				if (how == "key")
				{
					put(t.empty() ? 0 : KeyOf(t));
					return b;
				}
				if (how == "key64")
				{
					uint64_t v = 0;
					if (hexPrefix(t) && hexValue(t, 2, 64, v)) put(v);
					else put(t.empty() ? 0 : StringHash64(t));
					return b;
				}
				if (how == "binkey")
				{
					uint64_t v = 0;
					if (unsignedValue(t, 0xFFFFFFFFull, v)) put(v);
					else if (t.empty()) put(0xFFFFFFFFull);
					else if (hexPrefix(t) && hexValue(t, 2, 32, v)) put(v);
					else put(BinHash(t));
					return b;
				}
				if (how == "hex")
				{
					uint64_t v = 0;
					if (hexPrefix(t) ? !hexValue(t, 2, 32, v) : !unsignedValue(t, 0xFFFFFFFFull, v)) bad(" isn't a number (0x and hex digits, or decimal)");
					put(v);
					return b;
				}
				if (how == "hexdigits")
				{
					uint64_t v = 0;
					const std::string digits = hexPrefix(t) ? t.substr(2) : t;
					if (!hexValue(digits, 0, 32, v)) bad(" isn't hex digits");
					put(v);
					return b;
				}
				if (base == "f32" || base == "f64" || type.find('>') != std::string::npos)
				{
					double d = 0;
					if (t == "Infinity") d = HUGE_VAL;
					else if (t == "-Infinity") d = -HUGE_VAL;
					else if (t == "NaN") d = std::nan("");
					else
					{
						// [sign] digits (commas among them) [. digits] [e [sign] digits]
						size_t i = t.empty() || (t[0] != '+' && t[0] != '-') ? 0 : 1;
						bool any = false;
						while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == ',')) any |= t[i++] != ',';
						if (i < t.size() && t[i] == '.')
							for (++i; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i) any = true;
						if (any && i < t.size() && (t[i] == 'e' || t[i] == 'E'))
						{
							++i;
							if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
							const size_t e0 = i;
							while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
							if (i == e0) any = false;
						}
						if (!any || i != t.size()) bad(" isn't a number");
						std::string n = t;
						n.erase(std::remove(n.begin(), n.end(), ','), n.end());
						d = std::strtod(n.c_str(), nullptr);
						// .NET Framework's Single.Parse and Double.Parse refuse what doesn't fit.
						if (std::isinf(d) || (base == "f32" && std::fabs(d) > double(FLT_MAX))) bad(" is too large");
					}
					if (base == "f32")
					{
						const float f = float(d);
						uint32_t u;
						std::memcpy(&u, &f, 4);
						put(u);
					}
					else if (base == "f64")
					{
						uint64_t u;
						std::memcpy(&u, &d, 8);
						put(u);
					}
					else
					{
						put(uint64_t(int64_t(d))); // a float NFS-VltEd shows, stored as an integer
					}
					return b;
				}
				if (base == "bool8" || base == "bool32")
				{
					const std::string l = Lower(t);
					if (l != "true" && l != "false") bad(": a bool is true or false");
					put(l == "true");
					return b;
				}
				// Int32.Parse and the like: [sign] digits, in range.
				const bool sign = base[0] == 'i';
				const uint32_t bits = size * 8;
				const size_t from = !t.empty() && (t[0] == '+' || t[0] == '-') ? 1 : 0;
				if (!digitsFrom(t, from)) bad(" isn't a whole number");
				errno = 0;
				const unsigned long long mag = std::strtoull(t.c_str() + from, nullptr, 10);
				const bool negative = t[0] == '-';
				const unsigned long long hi = sign ? (1ull << (bits - 1)) - (negative ? 0 : 1) : bits >= 64 ? ~0ull : (1ull << bits) - 1;
				if (errno == ERANGE || mag > hi || (!sign && negative && mag != 0)) bad(" doesn't fit NFS-VltEd's " + base);
				put(negative ? uint64_t(0) - uint64_t(mag) : uint64_t(mag));
				return b;
			}

			// Our value of kind `mt` holding the bits `v` (its `ms` bytes as a
			// little-endian number), as a script value; null for a kind that isn't a
			// number.
			static sc::ValuePtr BitsValue(const ValueType& mt, uint32_t ms, uint64_t v, Endian e)
			{
				if (mt.kind == TypeKind::Bool && v <= 1) return sc::MakeWord(v ? "true" : "false");
				if (mt.kind == TypeKind::Float && ms == 4)
				{
					const uint32_t u = uint32_t(v);
					float fl;
					std::memcpy(&fl, &u, 4);
					return sc::MakeNumber(sc::NumberText(fl, true));
				}
				if (mt.kind == TypeKind::Double && ms == 8)
				{
					double d;
					std::memcpy(&d, &v, 8);
					return sc::MakeNumber(sc::NumberText(d));
				}
				// Enums are read as signed numbers of their size.
				if (mt.kind == TypeKind::Int8 || mt.kind == TypeKind::Int16 || mt.kind == TypeKind::Int32 || mt.kind == TypeKind::Int64 ||
					mt.kind == TypeKind::Enum)
				{
					const int64_t sv = ms == 8 ? int64_t(v) : int64_t(v << (64 - 8 * ms)) >> (64 - 8 * ms);
					return sc::MakeNumber(std::to_string(sv));
				}
				if (mt.kind == TypeKind::Bool)
				{
					// A bool holding 2 is a byte all the same: as NFS-VltEd writes it.
					auto bytes = std::make_shared<sc::Value>();
					bytes->kind = sc::Value::Kind::Bytes;
					for (uint32_t i = 0; i < ms; ++i)
					{
						char buf[4];
						std::snprintf(buf, sizeof(buf), "%02X ", unsigned((v >> (8 * (e == Endian::Big ? ms - 1 - i : i))) & 0xFF));
						bytes->text += buf;
					}
					return bytes;
				}
				// A key's number in hex (a bare decimal would be a name to hash).
				if (mt.kind == TypeKind::Key || mt.kind == TypeKind::BinKey)
				{
					if (ms != 8) return sc::MakeNumber(Hex32(uint32_t(v)));
					char b[24];
					std::snprintf(b, sizeof(b), "0x%016llX", static_cast<unsigned long long>(v));
					return sc::MakeNumber(b);
				}
				if (IsIntegerKind(mt.kind) || mt.kind == TypeKind::Unknown) return sc::MakeNumber(std::to_string(v));
				return nullptr;
			}

			// A one-value type (an enum, a float, a GCollectionKey: NFS-VltEd's class
			// for it has one property): the text read as NFS-VltEd reads that
			// property, then set in our value.
			bool oneValue(Collection& x, const sc::FieldPath& p, const std::string& text)
			{
				Bytes cur;
				const ValueType vt = sc::TypeAt(db_, x, p, &cur);
				const PropertyNames::Type* t = vltType(p, vt);
				if (!t || !t->single || t->properties.empty()) return false;
				const PropertyNames::Property& pr = t->properties[0];
				const uint32_t size = VltSize(pr.vltType);
				// Keys go the usual way, which keeps their names; strings and lists too.
				if (!size || pr.list || pr.offset || IsKeyWord(pr.vltType)) return false;
				const Bytes now = vltBytes(pr.vltType, text);
				uint64_t v = 0;
				for (uint32_t i = 0; i < size; ++i) v |= uint64_t(now[i]) << (8 * i);
				const Endian e = x.vault ? x.vault->raw.endian : Endian::Little;
				sc::ValuePtr val;
				if (vt.size == size)
				{
					val = BitsValue(vt, size, v, e);
				}
				else
				{
					// Another width: the number NFS-VltEd read, which our value must hold.
					const std::string base = pr.vltType.substr(0, pr.vltType.find_first_of("/>"));
					if (base == "f32" || base == "f64")
					{
						double d;
						if (base == "f32")
						{
							const uint32_t u = uint32_t(v);
							float f;
							std::memcpy(&f, &u, 4);
							d = f;
						}
						else
						{
							std::memcpy(&d, &v, 8);
						}
						val = sc::MakeNumber(sc::NumberText(d));
					}
					else if (base[0] == 'i')
					{
						const int64_t sv = size == 8 ? int64_t(v) : int64_t(v << (64 - 8 * size)) >> (64 - 8 * size);
						val = sc::MakeNumber(std::to_string(sv));
					}
					else if (base == "bool8" || base == "bool32")
					{
						val = sc::MakeWord(v ? "true" : "false");
					}
					else
					{
						val = sc::MakeNumber(std::to_string(v));
					}
				}
				if (!val) return false;
				sc::Statement s;
				s.kind = K::Set;
				s.path = p;
				s.value = val;
				amod(x, { s });
				return true;
			}
			// A word NFS-VltEd writes with another width than our member there has
			// (FEPartData's 4 is an int32 over tier, drift, drag and grip): its bytes
			// over our members by offset, each set to what its bytes then say.
			// A word NFS-VltEd writes with another width than our member there has
			// (FEPartData's 4 is an int32 over tier, drift, drag and grip): its bytes
			// over our members by offset, each set to what its bytes then say.
			bool wideWrite(Collection& x, const std::string& text)
			{
				const PropertyNames::Property& pr = *lastProp_;
				const uint32_t size = VltSize(pr.vltType);
				const StructDef* st = lastBaseType_.st;
				if (!size || pr.list || !st) return false;
				std::vector<Leaf> leaves;
				std::vector<sc::FieldPath::Step> steps;
				Leaves(*st, 0, steps, leaves);
				for (const Leaf& l : leaves)
					if (l.offset == pr.offset && !VltReadsOtherwise(pr.vltType, TypeOfMember(*l.m).kind, l.m->elemSize) &&
						IsKeyWord(pr.vltType))
						return false; // our member is the key: the usual way keeps its name
				Bytes cur;
				sc::TypeAt(db_, x, lastBase_, &cur);
				if (cur.size() < pr.offset + size) return false;
				const Bytes now = vltBytes(pr.vltType, text);
				const Endian e = x.vault ? x.vault->raw.endian : Endian::Little;
				if (db_.target().platform == Platform::PS2 || db_.target().platform == Platform::GameCube)
					warn(pr.word + " is NFS-VltEd's " + pr.vltType + " at byte " + std::to_string(pr.offset) +
						 " of the PC's layout, set here over the members at those bytes on " + db_.target().name());
				std::vector<sc::Statement> sets;
				for (const Leaf& l : leaves)
				{
					const MemberDef& m = *l.m;
					const uint32_t ms = m.elemSize;
					const uint32_t b0 = std::max(l.offset, pr.offset), b1 = std::min(l.offset + ms, pr.offset + size);
					if (b0 >= b1) continue;
					// The member as the PC holds it, the word's bytes over it.
					uint64_t v = 0;
					for (uint32_t i = 0; i < ms; ++i)
						v |= uint64_t(cur[l.offset + (e == Endian::Big ? ms - 1 - i : i)]) << (8 * i);
					for (uint32_t bpos = b0; bpos < b1; ++bpos)
					{
						const uint32_t sh = 8 * (bpos - l.offset);
						v = (v & ~(uint64_t(0xFF) << sh)) | (uint64_t(now[bpos - pr.offset]) << sh);
					}
					const sc::ValuePtr val = BitsValue(TypeOfMember(m), ms, v, e);
					if (!val) continue;
					sc::Statement s;
					s.kind = K::Set;
					s.path = lastBase_;
					s.path.steps.insert(s.path.steps.end(), l.steps.begin(), l.steps.end());
					s.value = val;
					sets.push_back(std::move(s));
				}
				if (sets.empty()) return false;
				amod(x, sets);
				return true;
			}
			static bool IsIntegerKind(TypeKind k)
			{
				return k == TypeKind::UInt8 || k == TypeKind::UInt16 || k == TypeKind::UInt32 || k == TypeKind::UInt64 || k == TypeKind::Char ||
					k == TypeKind::Int8 || k == TypeKind::Int16 || k == TypeKind::Int32 || k == TypeKind::Int64;
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

			// ---- NFS-VltEd compatible runs (st_.vltEd): what NFS-VltEd holds that our model doesn't.
			// An array there holds every slot of its capacity (mAlloc), and a value command may
			// write a slot past the count; resize_field moves the count over them. Those writes
			// are kept here (an nfsms line's words, or a value a layout array's shrink left in
			// its slot) and made when a resize brings the slot in: per collection and field,
			// by slot, in order.
			std::map<std::pair<const Collection*, uint32_t>, std::map<uint32_t, std::vector<SpareWrite>>> spare_;
			std::map<std::pair<const Collection*, std::string>, CurveShadow> curves_;
			// The last NFS-VltEd word update() stepped through: the value it is in, and the word.
			const PropertyNames::Property* lastProp_ = nullptr;
			// A list's element words after its word (elementWords_), then after its index (inElement_).
			const std::vector<std::string>* elementWords_ = nullptr;
			const std::vector<std::string>* inElement_ = nullptr;
			sc::FieldPath lastBase_;
			ValueType lastBaseType_;

			void forgetCompat(const Collection* c)
			{
				for (auto it = spare_.begin(); it != spare_.end();) it = it->first.first == c ? spare_.erase(it) : std::next(it);
				for (auto it = curves_.begin(); it != curves_.end();) it = it->first.first == c ? curves_.erase(it) : std::next(it);
			}
			// NFS-VltEd's value commands need a field the node has: its layout's, or its own.
			void requireOwn(const Collection& x, const FieldDef& f, const std::string& field)
			{
				if (st_.vltEd && !f.inLayout() && !x.find(f.key))
					fail("Field '" + field + "' does not exist in the node '" + db_.names().format(x.key) +
						 "': NFS-VltEd changes only a node's own fields (add_field it first)");
			}
			// How many slots NFS-VltEd holds for the array: a layout array its layout's, an entry
			// array its capacity.
			static uint32_t SlotsHeld(const Attribute& a)
			{
				return a.inLayout ? MaxElements(a, false) : std::max<uint32_t>(a.capacity, a.count);
			}
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

	LineTarget TargetOfLine(Database& db, const Script& script, const Command& c, size_t column, const RunSettings& settings)
	{
		LineTarget t;
		const std::vector<std::string>& a = c.args;
		// The parameter under the caret: the last one starting at or before it
		// (-1: the command itself).
		int on = -1;
		for (size_t i = 0; i < a.size() && i < c.at.size(); ++i)
			if (c.at[i] <= column) on = int(i);
		const std::string& n = c.name;
		if (n.empty()) return t;
		if (n == "script")
		{
			if (a.empty()) t.why = "the script line names no file";
			else t.file = ScriptFile(script.folder, a[0]);
			return t;
		}
		if (n == "ui_option")
		{
			if (a.empty()) t.why = "the ui_option line names no control";
			for (const Control& k : script.controls)
			{
				const bool is = a.size() == 2 ? k.kind == Control::Kind::Radio && k.group == a[0] && k.name == a[1]
					: a.size() == 1 && k.kind == Control::Kind::Checkbox && k.name == a[0];
				if (is)
				{
					t.controlLine = k.line;
					return t;
				}
			}
			if (!a.empty()) t.why = "no ui_control " + a[0] + (a.size() > 1 ? " " + a[1] : "") + " in this script";
			return t;
		}

		// Which parameters name nodes, which one the line changes, and where the
		// field and its words are.
		std::vector<int> nodes;
		int main = -1, fallback = -1, field = -1, wordsEnd = -1; // words: field + 1 .. wordsEnd - 1
		const int count = int(a.size());
		if (n == "update_field" || n == "update_collection" || n == "resize_collection")
		{
			nodes = { 1 };
			main = 1;
			field = 2;
			wordsEnd = count - 1;
		}
		else if (n == "add_field" || n == "delete_field" || n == "resize_field" || n == "append_array")
		{
			nodes = { 1 };
			main = 1;
			field = 2;
			wordsEnd = 3;
		}
		else if (n == "delete_node" || n == "change_vault")
		{
			nodes = { 1 };
			main = 1;
		}
		else if (n == "rename_node")
		{
			// The node, or by its new name once the line has run.
			nodes = { 1, 2 };
			main = 1;
			fallback = 2;
		}
		else if (n == "move_node")
		{
			nodes = { 1, 2 };
			main = 1;
		}
		else if (n == "add_node" || n == "add_overwrite")
		{
			// The new node once it exists, else its parent.
			nodes = count >= 3 ? std::vector<int>{ 1, 2 } : std::vector<int>{ 1 };
			main = count >= 3 ? 2 : 1;
			fallback = count >= 3 ? 1 : -1;
		}
		else if (n == "copy_node" || n == "copy_overwrite")
		{
			nodes = count >= 4 ? std::vector<int>{ 1, 2, 3 } : std::vector<int>{ 1, 2 };
			main = count >= 4 ? 3 : 2;
			fallback = 1;
		}
		else if (n == "copy_fields")
		{
			nodes = { 1, 2 };
			main = 2;
		}
		else
		{
			if (n == "delete_nodes") t.why = "delete_nodes names a class, not a node";
			return t;
		}
		if (count < 2)
		{
			t.why = n + " needs a class and a node";
			return t;
		}
		const ClassDef* cls = db.findClass(KeyOf(a[0]));
		if (!cls)
		{
			t.why = "no class " + a[0];
			return t;
		}
		auto find = [&](int i) -> Collection*
		{
			return i >= 0 && i < count ? db.findCollection(cls->key, KeyOf(a[size_t(i)])) : nullptr;
		};
		const bool onNode = std::find(nodes.begin(), nodes.end(), on) != nodes.end();
		const int which = onNode ? on : main;
		t.collection = find(which);
		if (!t.collection && which == main) t.collection = find(fallback);
		if (!t.collection)
		{
			t.why = which < count ? "no collection " + a[0] + "/" + a[size_t(which)] : n + " names no node";
			return t;
		}
		if (onNode || field < 0 || field >= count || t.collection != find(main)) return t;

		// The value: as far as the word under the caret, else every word, else
		// the field alone.
		auto words = [&](int end)
		{
			std::vector<std::string> w;
			for (int i = field + 1; i < end && i < count; ++i) w.push_back(a[size_t(i)]);
			return w;
		};
		std::vector<std::vector<std::string>> tries;
		if (on == field) tries.push_back({});
		else if (on > field && on < wordsEnd) tries.push_back(words(on + 1));
		tries.push_back(words(wordsEnd));
		for (const std::vector<std::string>& w : tries)
		{
			try
			{
				t.path = WordsLeadTo(db, *t.collection, a[size_t(field)], w, settings);
				t.hasPath = true;
				return t;
			}
			catch (const std::exception&)
			{
			}
		}
		const std::string f = a[size_t(field)].substr(0, a[size_t(field)].find('['));
		if (cls->field(KeyOf(f)))
		{
			t.path.field = f;
			t.hasPath = true;
		}
		return t;
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
			Writer(const Database& db, const PropertyNames* names, bool vltEd, const Database* before)
				: db_(db), names_(names), vltEd_(vltEd), before_(before)
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
					{
						// Fields it only inherited before need add_field first in NFS-VltEd.
						added_.clear();
						const ClassDef* k = before_ ? before_->findClass(KeyOf(nameOf(s.target.cls))) : nullptr;
						const Collection* was = k ? before_->findCollection(k->key, KeyOf(nameOf(s.target.name))) : nullptr;
						newNode_ = was != nullptr;
						if (was)
							for (const Attribute& a : was->attributes) added_.insert(a.key);
						for (const sc::Statement& b : s.body) body(s.target, b);
						newNode_ = false;
						break;
					}
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
						// NFS-VltEd never works a curve's Y2 out: the whole curve as it is now.
						if (vltEd_ && !inCurve_)
							if (const std::optional<sc::FieldPath> root = curveRoot(t, b.path))
								if (sc::ValuePtr whole = curveNow(t, *root))
								{
									sc::Statement w = b;
									w.path = *root;
									w.value = whole;
									w.was.reset();
									inCurve_ = true;
									body(t, w);
									inCurve_ = false;
									target_ = &t;
									break;
								}
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
							// From the end: [^] is the last element; others by the count now.
							int64_t index = b.path.steps[0].index;
							if ((index < -1 || (index == -1 && vltEd_)) && f) index += int64_t(lengthNow(t, *f)); // NFS-VltEd has no [^]
							field += index == -1 ? std::string("[^]") : "[" + std::to_string(index) + "]";
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
					case K::Remove:
					case K::Insert:
					{
						// nfsms can't take elements out or put them in the middle: the
						// array as it is now, once.
						const ClassDef* k = db_.findClass(KeyOf(nameOf(t.cls)));
						const FieldDef* f = k ? k->field(KeyOf(nameOf(b.path.field))) : nullptr;
						const Collection* x = k ? db_.findCollection(k->key, KeyOf(nameOf(t.name))) : nullptr;
						const bool fieldArray = b.path.steps.empty() || (b.path.steps.size() == 1 && b.path.steps[0].member.empty());
						const char* what = b.kind == K::Remove ? "remove " : "insert ";
						if (!f || !x || !fieldArray)
						{
							out.push_back("# not in nfsms: " + std::string(what) + b.path.text());
							break;
						}
						if (!whole_.insert({ x, f->key }).second) break;
						out.push_back("# " + std::string(what) + b.path.text() + (b.value ? " = " + sc::PrintValue(*b.value) : std::string()) +
									  ": nfsms has no command for it, so the whole array as it is now");
						for (const sc::Statement& e : sc::ExportValue(db_, *x, f->key).body) body(t, e);
						break;
					}
					case K::Resize:
					{
						const ClassDef* k = db_.findClass(KeyOf(nameOf(t.cls)));
						const FieldDef* f = k ? k->field(KeyOf(nameOf(b.path.field))) : nullptr;
						if (!b.path.steps.empty())
						{
							out.push_back("# resize " + b.path.text() + ": nfsms resizes a field's array (resize_collection a list in it)");
							break;
						}
						// `resize ... by n`: the length it has now.
						const uint32_t n = b.by ? (f ? lengthNow(t, *f) : 0) : b.count;
						if (b.by && !f)
						{
							out.push_back("# resize " + b.path.text() + " by " + std::to_string(b.number) + ": no such field");
							break;
						}
						const size_t at = out.size();
						out.push_back("resize_field " + head + " " + Token(nameOf(b.path.field)) + " " + std::to_string(n));
						if (f) addField(head, *f, b, at);
						break;
					}
					case K::Append:
					{
						const ClassDef* k = db_.findClass(KeyOf(nameOf(t.cls)));
						const FieldDef* f = k ? k->field(KeyOf(nameOf(b.path.field))) : nullptr;
						const std::string field = Token(nameOf(b.path.field));
						if (!b.path.steps.empty() || !f)
						{
							out.push_back("# append " + b.path.text() + ": nfsms appends to a field's array only");
							break;
						}
						if (b.ifMissing) out.push_back("# append " + b.path.text() + " if missing: nfsms adds it even where the array holds it");
						std::vector<sc::ValuePtr> items;
						if (!b.value) items.push_back(nullptr);
						else if (b.value->kind == sc::Value::Kind::List) items = b.value->items;
						else items.push_back(b.value);
						if (vltEd_)
						{
							// NFS-VltEd has no append_array: the length it has now, then each new element.
							const uint32_t n = lengthNow(t, *f);
							out.push_back("resize_field " + head + " " + field + " " + std::to_string(n));
							for (size_t i = 0; i < items.size(); ++i)
								if (items[i] && n >= items.size())
									value("update_field " + head, field + "[" + std::to_string(n - items.size() + i) + "]", {}, *items[i], false);
							break;
						}
						for (const sc::ValuePtr& v : items)
						{
							// Attribulator's append_array, then the new last element's parts.
							if (v && v->kind == sc::Value::Kind::Number)
							{
								out.push_back("append_array " + head + " " + field + " " + Token(v->text));
								continue;
							}
							out.push_back("append_array " + head + " " + field);
							if (v) value("update_field " + head, field + "[^]", {}, *v, false);
						}
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
					const std::string resize = "resize_field " + head + " " + field + " " + std::to_string(count);
					const bool resized = at < out.size() && out[at] == resize;
					// add_field gives it that length already; NFS-VltEd's gives it the class's
					// default collection's count, so there the resize stays.
					if (resized && !vltEd_) out.erase(out.begin() + std::ptrdiff_t(at));
					if (!resized && vltEd_) out.insert(out.begin() + std::ptrdiff_t(at), resize);
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
					Bytes topBytes;
					const ValueType top = sc::TypeAt(db, *x, p, &topBytes);
					const PropertyNames::Type* t = names_->find(top.typeName);
					if (!t) return false;
					if (t->single)
					{
						ps.clear(); // NFS-VltEd takes no word for it
						// Its one property as NFS-VltEd reads it: an enum as its number.
						const PropertyNames::Property* pr = t->properties.empty() ? nullptr : &t->properties[0];
						if (vltEd_ && pr && !pr->list && !pr->offset && VltSize(pr->vltType) && !IsKeyWord(pr->vltType))
						{
							const Endian e = x->vault ? x->vault->raw.endian : Endian::Little;
							if (VltSize(pr->vltType) == top.size) wideValue_ = wordNow(*pr, top, topBytes, e);
							else if (top.kind == TypeKind::Enum && top.size <= 8 && topBytes.size() >= top.size)
							{
								uint64_t u = 0;
								for (uint32_t i = 0; i < top.size; ++i)
									u |= uint64_t(topBytes[e == Endian::Big ? top.size - 1 - i : i]) << (8 * i);
								const int64_t sv = top.size == 8 ? int64_t(u) : int64_t(u << (64 - 8 * top.size)) >> (64 - 8 * top.size);
								wideValue_ = std::to_string(sv);
							}
						}
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
					// NFS-VltEd has no word of its own for this member (VinylLayer's
					// transform, a padding byte): the word over its bytes, written whole.
					if (!pr && vltEd_)
						for (const PropertyNames::Property& q : t->properties)
							if (!q.list && VltSize(q.vltType) && off >= q.offset && off < q.offset + VltSize(q.vltType))
							{
								pr = &q;
								break;
							}
					if (pr) ps = { pr->word };
					// The word as NFS-VltEd reads it, from the value's bytes now: wider than
					// our member (FEPartData's 4 over tier, drift, drag and grip), or a
					// number where we have a name (an enum member).
					if (pr && vltEd_ && !pr->list && VltSize(pr->vltType) && (VltReadsOtherwise(pr->vltType, vt.kind, vt.size) || !IsKeyWord(pr->vltType)))
						wideValue_ = wordNow(*pr, top, topBytes, x->vault ? x->vault->raw.endian : Endian::Little);
				}
				catch (const std::exception&)
				{
				}
				return false;
			}

			// An NFS-VltEd word's value in a value's bytes, as NFS-VltEd shows it (its
			// type, the PC's little-endian bytes).
			std::optional<std::string> wordNow(const PropertyNames::Property& pr, const ValueType& top, const Bytes& b, Endian e) const
			{
				const uint32_t size = VltSize(pr.vltType);
				if (b.size() < pr.offset + size) return std::nullopt;
				Bytes le(b.begin() + pr.offset, b.begin() + pr.offset + size);
				if (e == Endian::Big)
				{
					// Each member's bytes the PC's way round.
					if (!top.st) return std::nullopt;
					std::vector<Leaf> leaves;
					std::vector<sc::FieldPath::Step> steps;
					Leaves(*top.st, 0, steps, leaves);
					for (const Leaf& l : leaves)
					{
						const uint32_t ms = l.m->elemSize;
						for (uint32_t i = 0; i < ms; ++i)
						{
							const uint32_t at = l.offset + i;
							if (at >= pr.offset && at < pr.offset + size && l.offset + ms - 1 - i < b.size()) le[at - pr.offset] = b[l.offset + ms - 1 - i];
						}
					}
				}
				uint64_t u = 0;
				for (uint32_t i = 0; i < size; ++i) u |= uint64_t(le[i]) << (8 * i);
				const std::string base = pr.vltType.substr(0, pr.vltType.find_first_of("/>"));
				const std::string how = pr.vltType.find('/') != std::string::npos ? pr.vltType.substr(pr.vltType.find('/') + 1) : std::string();
				if (base == "f32")
				{
					const uint32_t w = uint32_t(u);
					float f;
					std::memcpy(&f, &w, 4);
					return sc::NumberText(f, true);
				}
				if (base == "f64")
				{
					double d;
					std::memcpy(&d, &u, 8);
					return sc::NumberText(d);
				}
				if (base == "bool8" || base == "bool32") return std::string(u ? "true" : "false");
				if (how == "key")
				{
					const std::string* n = db_.names().find(uint32_t(u));
					return n ? *n : Hex32(uint32_t(u));
				}
				if ((how == "hex" || how == "hexdigits") && size == 4) return Hex32(uint32_t(u));
				if (base[0] == 'i' && size < 8) return std::to_string(int64_t(u << (64 - 8 * size)) >> (64 - 8 * size));
				return std::to_string(u);
			}

			// The curve at `path` of the target as it is now, Y2 included (NFS-VltEd
			// never works Y2 out), or null when the path isn't a curve.
			sc::ValuePtr curveNow(const sc::Target& t, const sc::FieldPath& path) const
			{
				const ClassDef* k = db_.findClass(KeyOf(nameOf(t.cls)));
				Collection* x = k ? const_cast<Database&>(db_).findCollection(k->key, KeyOf(nameOf(t.name))) : nullptr;
				if (!x) return nullptr;
				sc::FieldPath p = path;
				p.field = nameOf(path.field);
				CurveValue c;
				try
				{
					// A curve field, an element of one, or a struct's member (AI::GlueCurve's Easy).
					Database& db = const_cast<Database&>(db_);
					const ValueType vt = sc::TypeAt(db, *x, p);
					uint32_t off = 0;
					const DataBlock* b = vt.kind == TypeKind::Curve ? sc::BlockAt(db, *x, p, &off) : nullptr;
					if (!b || uint64_t(off) + vt.size > b->bytes.size()) return nullptr;
					c = ReadCurve(*b, off, x->vault ? x->vault->raw.endian : Endian::Little, vt.size);
				}
				catch (const std::exception&)
				{
					return nullptr;
				}
				auto list = [](const std::vector<float>& v)
				{
					std::vector<sc::ValuePtr> items;
					for (float f : v) items.push_back(sc::MakeNumber(sc::NumberText(f, true)));
					return sc::MakeList(std::move(items));
				};
				auto v = std::make_shared<sc::Value>();
				v->kind = sc::Value::Kind::Struct;
				v->members = { { "xMin", sc::MakeNumber(sc::NumberText(c.xMin, true)) }, { "xMax", sc::MakeNumber(sc::NumberText(c.xMax, true)) },
							   { "yMin", sc::MakeNumber(sc::NumberText(c.yMin, true)) }, { "yMax", sc::MakeNumber(sc::NumberText(c.yMax, true)) },
							   { "X", list(c.x) }, { "Y", list(c.y) }, { "Y2", list(c.y2) } };
				return v;
			}

			// One value as update_field lines: compound values one line per part.
			void value(const std::string& head, const std::string& field, std::vector<std::string> props, const sc::Value& v, bool wholeField)
			{
				auto line = [&](std::vector<std::string> ps, const std::string& val)
				{
					std::string s = head + " " + field;
					wideValue_.reset();
					if (vltWords(field, ps)) s = "update_collection" + s.substr(std::strlen("update_field"));
					for (const std::string& p : ps) s += " " + p;
					const std::string& text = wideValue_ ? *wideValue_ : val;
					// NFS-VltEd cuts a line at its first # and has no escape for a quote.
					if (vltEd_ && text.find_first_of("#\"") != std::string::npos)
						out.push_back("# " + s + ": NFS-VltEd can't read a value holding # or \", " + text);
					else
						out.push_back(s + " " + Token(text));
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
			const bool vltEd_; // NFS-VltEd's form (FromStatements)
			const Database* before_; // the database the statements change (FromStatements)
			const sc::Target* target_ = nullptr;
			// The value an NFS-VltEd word wider than our member has now (vltWords, vltEd_).
			mutable std::optional<std::string> wideValue_;
			bool inCurve_ = false;

			// The curve a path leads into (or is), as a path.
			std::optional<sc::FieldPath> curveRoot(const sc::Target& t, const sc::FieldPath& path) const
			{
				const ClassDef* k = db_.findClass(KeyOf(nameOf(t.cls)));
				Collection* x = k ? const_cast<Database&>(db_).findCollection(k->key, KeyOf(nameOf(t.name))) : nullptr;
				if (!x) return std::nullopt;
				for (size_t n = path.steps.size() + 1; n-- > 0;)
				{
					sc::FieldPath p = path;
					p.field = nameOf(path.field);
					p.steps.resize(n);
					try
					{
						if (sc::TypeAt(const_cast<Database&>(db_), *x, p).kind == TypeKind::Curve) return p;
					}
					catch (const std::exception&)
					{
					}
				}
				return std::nullopt;
			}
			// In the body of a node add_node or copy_node makes, and the fields it
			// has besides its layout's (added, or copied from the source).
			bool newNode_ = false;
			std::set<uint32_t> added_;
			std::set<std::pair<const Collection*, uint32_t>> whole_; // arrays written whole for a `remove`

			// The length of the target's array in the field now (0 without a value).
			uint32_t lengthNow(const sc::Target& t, const FieldDef& f) const
			{
				const ClassDef* k = db_.findClass(KeyOf(nameOf(t.cls)));
				const Collection* x = k ? db_.findCollection(k->key, KeyOf(nameOf(t.name))) : nullptr;
				const Attribute* a = x ? x->resolve(f.key) : nullptr;
				return a ? a->count : 0;
			}
		};

	} // namespace

	std::vector<std::string> FromStatements(const Database& db, const std::vector<script::Statement>& statements,
											const PropertyNames* names, bool vltEd, const Database* before)
	{
		Writer w(db, names, vltEd, before);
		w.statements(statements);
		// A word over several members comes once for each of them: one line.
		std::vector<std::string> out;
		for (std::string& l : w.out)
			if (out.empty() || out.back() != l || l.empty() || l[0] == '#') out.push_back(std::move(l));
		return out;
	}

} // namespace attrib::nfsms
