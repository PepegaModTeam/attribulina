#include "Schema.h"
#include "Hash.h"

#include <charconv>

namespace attrib
{

	namespace
	{

		struct Prim
		{
			const char* name;
			MemberPrim prim;
			uint32_t size;
		};
		constexpr Prim kPrims[] = {
			{"bool", MemberPrim::Bool, 1},     {"int8", MemberPrim::Int8, 1},     {"uint8", MemberPrim::UInt8, 1},
			{"int16", MemberPrim::Int16, 2},   {"uint16", MemberPrim::UInt16, 2}, {"int32", MemberPrim::Int32, 4},
			{"uint32", MemberPrim::UInt32, 4}, {"int64", MemberPrim::Int64, 8},   {"uint64", MemberPrim::UInt64, 8},
			{"float", MemberPrim::Float, 4},   {"double", MemberPrim::Double, 8}, {"char", MemberPrim::Char, 1},
			{"bool32", MemberPrim::Bool32, 4}, {"cstr", MemberPrim::CStr, 4},     {"ptr", MemberPrim::Ptr, 4},
		};

		std::vector<std::string_view> Split(std::string_view line)
		{
			std::vector<std::string_view> out;
			size_t i = 0;
			while (i < line.size())
			{
				while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
				if (i >= line.size()) break;
				size_t j = i;
				if (line[i] == '"')
				{
					j = line.find('"', i + 1);
					if (j == std::string_view::npos) j = line.size();
					out.push_back(line.substr(i + 1, j - i - 1));
					i = j + 1;
					continue;
				}
				while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
				out.push_back(line.substr(i, j - i));
				i = j;
			}
			return out;
		}

		bool ParseInt(std::string_view s, int64_t& out)
		{
			int base = 10;
			bool neg = false;
			if (!s.empty() && s[0] == '-')
			{
				neg = true; s.remove_prefix(1);
			}
			if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			{
				base = 16; s.remove_prefix(2);
			}
			uint64_t v = 0;
			auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
			if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
			out = neg ? -int64_t(v) : int64_t(v);
			return true;
		}

	} // namespace

	uint32_t PrimSize(MemberPrim p)
	{
		for (const Prim& k : kPrims)
			if (k.prim == p) return k.size;
		return 0;
	}

	const char* PrimName(MemberPrim p)
	{
		for (const Prim& k : kPrims)
			if (k.prim == p) return k.name;
		return "?";
	}

	const std::string* EnumDef::nameOf(int64_t v) const
	{
		for (const auto& [n, val] : values)
			if (val == v) return &n;
		return nullptr;
	}

	const MemberDef* StructDef::member(std::string_view n) const
	{
		for (const auto& m : members)
			if (m.name == n) return &m;
		return nullptr;
	}

	const StructDef* TypeSchema::findStruct(std::string_view name) const
	{
		auto it = structByName_.find(std::string(name));
		return it == structByName_.end() ? nullptr : it->second;
	}

	const EnumDef* TypeSchema::findEnum(std::string_view name) const
	{
		auto it = enumByName_.find(std::string(name));
		return it == enumByName_.end() ? nullptr : it->second;
	}

	const EnumDef* TypeSchema::fieldEnum(uint32_t classKey, uint32_t fieldKey) const
	{
		auto it = fieldEnums_.find(uint64_t(classKey) << 32 | fieldKey);
		return it == fieldEnums_.end() ? nullptr : it->second;
	}

	std::shared_ptr<TypeSchema> TypeSchema::Load(const std::string& path)
	{
		const Bytes data = ReadFile(path);
		return Parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), path);
	}

	std::shared_ptr<TypeSchema> TypeSchema::Parse(std::string_view text, const std::string& source)
	{
		auto s = std::make_shared<TypeSchema>();
		s->source = source;
		StructDef* curStruct = nullptr;
		EnumDef* curEnum = nullptr;
		size_t lineNo = 0;
		struct FieldLine
		{
			std::string cls, field, en;
			size_t line;
		};
		std::vector<FieldLine> fieldLines; // resolved once every enum is read
		auto fail = [&](const std::string& msg) -> FormatError
		{
			return FormatError(source + ":" + std::to_string(lineNo) + ": " + msg);
		};
		auto num = [&](std::string_view t, const char* what)
		{
			int64_t v;
			if (!ParseInt(t, v)) throw fail(std::string("bad ") + what + " '" + std::string(t) + "'");
			return v;
		};

		for (size_t pos = 0; pos <= text.size();)
		{
			size_t end = text.find('\n', pos);
			if (end == std::string_view::npos) end = text.size();
			std::string_view line = text.substr(pos, end - pos);
			pos = end + 1;
			++lineNo;
			if (const size_t hash = line.find('#'); hash != std::string_view::npos && line.find('"') == std::string_view::npos)
				line = line.substr(0, hash);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
			const auto tok = Split(line);
			if (tok.empty())
			{
				if (end == text.size()) break;
				continue;
			}

			if (tok[0] == "end")
			{
				if (!curStruct && !curEnum) throw fail("'end' without a struct or enum");
				curStruct = nullptr;
				curEnum = nullptr;
			}
			else if (curEnum)
			{
				if (tok.size() < 2) throw fail("enum value needs a name and a number");
				curEnum->values.emplace_back(std::string(tok[0]), num(tok[1], "enum value"));
			}
			else if (curStruct)
			{
				if (tok.size() < 3) throw fail("member needs a name, a type and an offset");
				MemberDef m;
				m.name = std::string(tok[0]);
				m.typeName = std::string(tok[1]);
				m.offset = uint32_t(num(tok[2], "offset"));
				for (const Prim& k : kPrims)
					if (tok[1] == k.name) m.prim = k.prim;
				for (size_t i = 3; i < tok.size(); ++i)
				{
					const std::string_view t = tok[i];
					const size_t eq = t.find('=');
					if (eq == std::string_view::npos) throw fail("unexpected '" + std::string(t) + "'");
					const std::string_view k = t.substr(0, eq), v = t.substr(eq + 1);
					if (k == "n") m.count = uint32_t(num(v, "count"));
					else if (k == "bits")
					{
						const size_t colon = v.find(':');
						if (colon == std::string_view::npos) throw fail("bits= needs offset:size");
						m.bitOffset = uint8_t(num(v.substr(0, colon), "bit offset"));
						m.bitSize = uint8_t(num(v.substr(colon + 1), "bit size"));
					}
					else if (k == "ptr") m.ptrTarget = std::string(v);
					else if (k == "count") m.ptrCount = std::string(v);
					else if (k == "hint") m.hint = std::string(v);
					// Unknown attributes are ignored so newer files still load.
				}
				curStruct->members.push_back(std::move(m));
			}
			else if (tok[0] == "schema")
			{
				size_t i = 1;
				if (i < tok.size() && GameById(tok[i]) != Game::Unknown) s->game = GameById(tok[i++]);
				if (i < tok.size()) s->title = std::string(tok[i++]);
				for (; i < tok.size(); ++i)
				{
					const Platform p = PlatformById(tok[i]);
					if (p == Platform::Unknown) throw fail("unknown platform '" + std::string(tok[i]) + "'");
					s->platforms.push_back(p);
				}
			}
			else if (tok[0] == "enum")
			{
				if (tok.size() < 3) throw fail("enum needs a name and a size");
				auto e = std::make_unique<EnumDef>();
				e->name = std::string(tok[1]);
				e->size = uint32_t(num(tok[2], "size"));
				curEnum = e.get();
				s->enumByName_[e->name] = e.get();
				s->enums_.push_back(std::move(e));
			}
			else if (tok[0] == "field")
			{
				if (tok.size() != 4) throw fail("field needs a class, a field and an enum");
				fieldLines.push_back({ std::string(tok[1]), std::string(tok[2]), std::string(tok[3]), lineNo });
			}
			else if (tok[0] == "struct")
			{
				if (tok.size() < 4) throw fail("struct needs a name, a size and an alignment");
				auto st = std::make_unique<StructDef>();
				st->name = std::string(tok[1]);
				st->size = uint32_t(num(tok[2], "size"));
				st->align = uint32_t(num(tok[3], "alignment"));
				for (size_t i = 4; i < tok.size(); ++i)
					if (tok[i].rfind("hint=", 0) == 0) st->hint = std::string(tok[i].substr(5));
				curStruct = st.get();
				s->structByName_[st->name] = st.get();
				s->structs_.push_back(std::move(st));
			}
			else
			{
				throw fail("unexpected '" + std::string(tok[0]) + "'");
			}
			if (end == text.size()) break;
		}
		if (curStruct || curEnum) throw fail("missing 'end'");
		if (s->game == Game::Unknown)
		{
			// An older header: the game from the file name, <game>_<platform>.txt.
			const std::string stem = source.substr(source.find_last_of("/\\") + 1);
			s->game = GameById(stem.substr(0, stem.find('_')));
		}

		for (const FieldLine& f : fieldLines)
		{
			const EnumDef* e = s->findEnum(f.en);
			if (!e) throw FormatError(source + ":" + std::to_string(f.line) + ": " + f.cls + "." + f.field + " names unknown enum " + f.en);
			s->fieldEnums_[uint64_t(StringHash32(f.cls)) << 32 | StringHash32(f.field)] = e;
		}

		// Resolve member types now that every name is known.
		for (auto& st : s->structs_)
		{
			for (MemberDef& m : st->members)
			{
				if (m.prim != MemberPrim::None)
				{
					m.elemSize = PrimSize(m.prim);
					if (m.prim == MemberPrim::Bool || m.prim == MemberPrim::Bool32) s->boolSize = m.elemSize;
				}
				else if (const StructDef* t = s->findStruct(m.typeName))
				{
					m.st = t;
					m.elemSize = t->size;
				}
				else if (const EnumDef* e = s->findEnum(m.typeName))
				{
					m.en = e;
					m.elemSize = e->size;
				}
				else
				{
					throw FormatError(source + ": " + st->name + "." + m.name + " has unknown type " + m.typeName);
				}
				if (m.offset + m.elemSize * m.count > st->size && !m.isBitfield())
					throw FormatError(source + ": " + st->name + "." + m.name + " runs past the end of the struct");
			}
		}
		return s;
	}

} // namespace attrib
