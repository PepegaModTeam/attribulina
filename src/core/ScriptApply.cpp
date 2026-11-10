// Mod scripts: applying them to a database (Script.h, doc/scripts.md).

#include "Script.h"

#include "ClassList.h"
#include "Compression.h"
#include "Gameplay.h"
#include "Curve.h"
#include "Edit.h"
#include "Hash.h"
#include "Writer.h"
#include "lua/Lua.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace attrib::script
{

	namespace
	{

		using K = Statement::Kind;

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

		// * and ? wildcards.
		bool Glob(std::string_view pat, std::string_view s)
		{
			size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
			while (i < s.size())
			{
				if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i]))
				{
					++p;
					++i;
				}
				else if (p < pat.size() && pat[p] == '*')
				{
					star = p++;
					mark = i;
				}
				else if (star != std::string_view::npos)
				{
					p = star + 1;
					i = ++mark;
				}
				else
				{
					return false;
				}
			}
			while (p < pat.size() && pat[p] == '*') ++p;
			return p == pat.size();
		}

		// A number while it's worked out; integer literals stay exact.
		struct Num
		{
			double d = 0;
			bool isInt = false;
			uint64_t bits = 0;    // the integer's two's complement bits
			bool unsignedBig = false; // above INT64_MAX
			std::string literal;  // as written, when it is one (floats read it with strtof)
			int64_t i() const
			{
				return int64_t(bits);
			}
		};

		// 0x and up to 16 hex digits: a number (a name as a key holds at most 8, IsHex).
		bool IsHexNumber(std::string_view s)
		{
			if (s.size() < 3 || s.size() > 18 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
			for (size_t i = 2; i < s.size(); ++i)
				if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
			return true;
		}

		bool ParseNum(const std::string& text, Num& out)
		{
			out = Num{};
			out.literal = text;
			if (IsHexNumber(text))
			{
				out.bits = std::strtoull(text.c_str() + 2, nullptr, 16);
				out.isInt = true;
				out.unsignedBig = out.bits > uint64_t(INT64_MAX);
				out.d = double(out.bits);
				return true;
			}
			if (text.find_first_of(".eEnN") == std::string::npos)
			{
				errno = 0;
				char* end = nullptr;
				const long long v = std::strtoll(text.c_str(), &end, 10);
				if (end && !*end && errno == 0)
				{
					out.isInt = true;
					out.bits = uint64_t(v);
					out.d = v == 0 && text[0] == '-' ? -0.0 : double(v);
					return true;
				}
				errno = 0;
				const unsigned long long u = text[0] == '-' ? 0 : std::strtoull(text.c_str(), &end, 10);
				if (text[0] != '-' && end && !*end && errno == 0)
				{
					out.isInt = true;
					out.bits = u;
					out.unsignedBig = true;
					out.d = double(u);
					return true;
				}
			}
			char* end = nullptr;
			out.d = std::strtod(text.c_str(), &end);
			return end && !*end && !text.empty();
		}

		bool SameBlock(const DataBlock& a, const DataBlock& b, int depth = 0)
		{
			if (a.bytes != b.bytes || a.ptrs.size() != b.ptrs.size()) return false;
			if (depth > 32) return true;
			for (const PtrSlot& p : a.ptrs)
			{
				const PtrSlot* q = b.ptrAt(p.offset);
				if (!q || q->isString != p.isString || q->text != p.text || bool(q->data) != bool(p.data)) return false;
				if (p.data && p.data != q->data && !SameBlock(*p.data, *q->data, depth + 1)) return false;
			}
			return true;
		}

		bool SameAttribute(const Attribute& a, const Attribute& b)
		{
			return a.count == b.count && a.capacity == b.capacity && a.isArray == b.isArray && SameBlock(a.data, b.data);
		}

		// The bytes and pointers in [off, off + size) of two blocks.
		bool SameRegion(const DataBlock& a, const DataBlock& b, uint32_t off, uint32_t size)
		{
			if (uint64_t(off) + size > a.bytes.size() || uint64_t(off) + size > b.bytes.size()) return false;
			if (std::memcmp(a.bytes.data() + off, b.bytes.data() + off, size) != 0) return false;
			for (const DataBlock* x : { &a, &b })
				for (const PtrSlot& p : x->ptrs)
				{
					if (p.offset < off || p.offset >= off + size) continue;
					const PtrSlot* q = (x == &a ? b : a).ptrAt(p.offset);
					if (!q || q->isString != p.isString || q->text != p.text || bool(q->data) != bool(p.data)) return false;
					if (p.data && p.data != q->data && !SameBlock(*p.data, *q->data)) return false;
				}
			return true;
		}

		bool IsCountMember(const StructDef& st, const MemberDef& m)
		{
			for (const MemberDef& o : st.members)
				if (o.prim == MemberPrim::Ptr && o.ptrCount == m.name && o.ptrTarget != "void") return true;
			return false;
		}

		const MemberDef* FindMember(const StructDef& st, const std::string& name)
		{
			if (const MemberDef* m = st.member(name)) return m;
			const std::string l = Lower(name);
			for (const MemberDef& m : st.members)
				if (Lower(m.name) == l) return &m;
			return nullptr;
		}

		bool IsVectorKind(TypeKind k)
		{
			return k == TypeKind::Vector2 || k == TypeKind::Vector3 || k == TypeKind::Vector4 || k == TypeKind::Quaternion ||
				k == TypeKind::FloatColour;
		}
		uint32_t VectorLength(TypeKind k)
		{
			switch (k)
			{
				case TypeKind::Vector2: return 2;
				case TypeKind::Vector3: return 3;
				default: return 4;
			}
		}

		const char* const kCurveFloats[] = { "xMin", "xMax", "yMin", "yMax" };

		// Where a path leads inside an attribute.
		struct Place
		{
			enum class Kind : uint8_t
			{
				Value,       // one value at block+off
				AttrArray,   // the attribute's elements
				InlineArray, // count elements at block+off
				PtrArray,    // what the pointer member at block+off+member->offset leads to
				CurvePart,   // X, Y or Y2 of the curve at block+off (one element with index >= 0)
			} kind = Kind::Value;
			DataBlock* block = nullptr;
			uint32_t off = 0;
			ValueType vt; // the value, or the elements of an array
			Attribute* attr = nullptr;
			uint32_t count = 0;
			const MemberDef* member = nullptr;
			const MemberDef* countMember = nullptr; // null: a pointer to one value
			ValueType countType;
			int curvePart = -1; // 0 X, 1 Y, 2 Y2
			uint32_t curveSize = 44; // the curve's (Curve.h CurveAt)
			int64_t index = -1;
			std::string where;
		};

		// An index past the end while a condition reads (Applier::softRange_).
		struct OutOfRange
		{
		};

		class Applier
		{
		public:
			Applier(Database& db, const Script& sc, const ApplySettings& st) : db_(db), sc_(sc), st_(st)
			{
			}

			ApplyReport run()
			{
				try
				{
					opts_ = ResolveOptions(sc_, st_);
					checkHeader();
					top(sc_.statements);
					for (const Ref& r : refs_)
						if (st_.checkReferences && !db_.findCollection(r.cls, r.key))
						{
							pos_ = r.pos;
							file_ = r.file;
							at_ = r.at;
							warn("no collection " + r.text + " for the reference " + r.field + " (set anyway)");
						}
					file_.clear();
					rep_.ok = true;
				}
				catch (const ScriptError& e)
				{
					// file_ is still the included script it stopped in.
					if (!e.file.empty()) rep_.messages.push_back(Message::Error(e));
					else message(Message::Level::Error, e.pos.line ? e.pos : pos_, e.message);
				}
				catch (const std::exception& e)
				{
					message(Message::Level::Error, pos_, e.what());
				}
				return rep_;
			}

		private:
			[[noreturn]] void fail(Pos p, const std::string& m) const
			{
				throw ScriptError(p, m, file_.empty() ? sc_.source : file_);
			}
			[[noreturn]] void fail(const std::string& m) const
			{
				fail(pos_, m);
			}
			// In an included script, at the include line, naming the file and its line.
			void message(Message::Level l, Pos p, std::string t)
			{
				if (file_.empty()) rep_.messages.push_back({ l, p, std::move(t) });
				else rep_.messages.push_back({ l, at_, std::move(t), file_, p.line });
			}
			void warn(const std::string& t)
			{
				if (st_.strict) fail(t + " (strict)");
				message(Message::Level::Warning, pos_, t);
			}
			void info(const std::string& t)
			{
				message(Message::Level::Info, pos_, t);
			}

			KeyNames& names()
			{
				return db_.names();
			}
			std::string label(const Collection& c)
			{
				return names().format(c.classKey) + "/" + names().format(c.key);
			}

			// ---------------------------------------------------------- names ---

			// A name or 0x...; a quoted "0x..." is a name.
			static uint32_t KeyOf(const std::string& text, bool bin = false)
			{
				if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
				{
					const std::string inner = text.substr(1, text.size() - 2);
					return bin ? BinHash(inner) : StringHash32(inner);
				}
				if (IsHex(text)) return uint32_t(std::strtoul(text.c_str() + 2, nullptr, 16));
				return bin ? BinHash(text) : StringHash32(text);
			}
			static std::string NameOf(const std::string& text)
			{
				if (text.size() >= 2 && text.front() == '"' && text.back() == '"') return text.substr(1, text.size() - 2);
				return IsHex(text) ? std::string() : text;
			}
			// Remembers a name the script used for something that exists.
			void learn(const std::string& text, bool bin = false)
			{
				const std::string n = NameOf(text);
				if (n.empty()) return;
				KeyNames& dict = bin ? db_.binNames() : names();
				if (dict.find(dict.hash(n))) return;
				dict.add(n);
				if (std::find(rep_.newNames.begin(), rep_.newNames.end(), n) == rep_.newNames.end()) rep_.newNames.push_back(n);
			}

			ClassDef& classOf(const Target& t)
			{
				ClassDef* c = db_.findClass(KeyOf(t.cls));
				if (!c) fail(t.pos, "no class " + t.cls);
				learn(t.cls);
				return *c;
			}
			Collection* find(const Target& t)
			{
				ClassDef& cls = classOf(t);
				Collection* c = db_.findCollection(cls.key, KeyOf(t.name));
				if (c) learn(t.name);
				return c;
			}
			Collection& required(const Target& t)
			{
				Collection* c = find(t);
				if (!c) fail(t.pos, "no collection " + t.text());
				return *c;
			}
			std::vector<Collection*> matching(const Target& t)
			{
				if (t.name.find_first_of("*?") == std::string::npos)
				{
					std::vector<Collection*> one;
					one.push_back(&required(t));
					return one;
				}
				ClassDef& cls = classOf(t);
				const std::string pat = Lower(t.name);
				std::vector<Collection*> out;
				for (Collection* c : cls.collections)
					if (Glob(pat, Lower(names().format(c->key)))) out.push_back(c);
				return out;
			}
			// The vault of that name, else one whose name differs only in case.
			Vault* findVault(const std::string& text)
			{
				const std::string n = NameOf(text).empty() ? text : NameOf(text);
				Vault* loose = nullptr;
				for (const auto& pk : db_.packs())
					for (const auto& v : pk->vaults)
					{
						if (v->name == n) return v.get();
						if (!loose && Lower(v->name) == Lower(n)) loose = v.get();
					}
				return loose;
			}
			Vault& vaultNamed(const std::string& text)
			{
				Vault* v = findVault(text);
				if (!v) fail("no vault " + (NameOf(text).empty() ? text : NameOf(text)));
				return *v;
			}
			Vault& defaultVault(ClassDef& cls, Collection* parent, Collection* like)
			{
				Vault* v = parent ? parent->vault : like ? like->vault : nullptr;
				if (!v && !cls.collections.empty()) v = cls.collections.front()->vault;
				if (!v) v = cls.vault;
				if (!v) fail("say which vault it goes in: in vault <name>");
				return *v;
			}
			void touch(Vault* v)
			{
				if (!v || !v->pack) return;
				v->pack->dirty = true;
				rep_.touched.insert(v->pack);
			}

			// -------------------------------------------------------- options ---

			ValuePtr optionValue(const std::string& id, Pos p)
			{
				auto it = opts_.find(id);
				if (it == opts_.end()) fail(p, "no option $" + id + " (declare it with option " + id + " ...)");
				return it->second;
			}
			// The value with $option replaced (one level: options hold literals).
			const Value& resolve(const Value& v)
			{
				if (v.kind == Value::Kind::Option) return *optionValue(v.text, v.pos);
				return v;
			}
			static bool Truthy(const Value& v)
			{
				if (v.kind == Value::Kind::Word) return v.text == "true";
				if (v.kind == Value::Kind::Number)
				{
					Num n;
					return ParseNum(v.text, n) && n.d != 0;
				}
				return !v.text.empty();
			}
			// `col`: the collection whose block the `if` is in (null at the top).
			bool test(const Condition& c, Collection* col)
			{
				using CK = Condition::Kind;
				switch (c.kind)
				{
					case CK::Option: return Truthy(*optionValue(c.option, pos_));
					case CK::Not: return !test(c.items[0], col);
					case CK::And: return test(c.items[0], col) && test(c.items[1], col);
					case CK::Or: return test(c.items[0], col) || test(c.items[1], col);
					case CK::ExistsClass: return db_.findClass(KeyOf(c.word)) != nullptr;
					case CK::ExistsVault: return findVault(c.word) != nullptr;
					case CK::ExistsField:
					{
						const ClassDef* k = db_.findClass(KeyOf(c.word));
						return k && k->field(KeyOf(c.name));
					}
					case CK::ExistsCollection:
					{
						const ClassDef* k = db_.findClass(KeyOf(c.target.cls));
						if (!k) return false;
						if (c.target.name.find_first_of("*?") == std::string::npos) return db_.findCollection(k->key, KeyOf(c.target.name)) != nullptr;
						const std::string pat = Lower(c.target.name);
						for (const Collection* x : k->collections)
							if (Glob(pat, Lower(names().format(x->key)))) return true;
						return false;
					}
					case CK::Own:
					{
						const ClassDef* k = col ? col->cls : nullptr;
						const FieldDef* f = k ? k->field(KeyOf(c.path.field)) : nullptr;
						return f && col->find(f->key);
					}
					case CK::Has:
					{
						Attribute zero;
						return readPlace(*col, c.path, zero, false).has_value();
					}
					default: break;
				}
				if (!c.hasPath)
				{
					// $option compared with a value.
					const Value& a = *optionValue(c.option, pos_);
					const Value& b = resolve(*c.value);
					Num x, y;
					const bool numbers = ParseNum(a.text, x) && ParseNum(b.text, y);
					if (c.kind == CK::Equal || c.kind == CK::NotEqual)
					{
						const bool eq = a.text == b.text || (numbers && x.d == y.d);
						return c.kind == CK::Equal ? eq : !eq;
					}
					if (!numbers) fail(c.value->pos, "<, >, <= and >= compare numbers: $" + c.option + " is " + PrintValue(a));
					return Compare(c.kind, x.d, y.d);
				}
				Attribute zero;
				const std::optional<Place> pl = readPlace(*col, c.path, zero, true);
				if (c.count)
				{
					const double n = pl ? double(lengthAt(*pl, c.path.pos)) : 0;
					return Compare(c.kind, n, number(*c.value).d);
				}
				if (!pl) return c.kind == CK::NotEqual; // nothing there
				if (c.kind == CK::Contains)
				{
					const uint32_t n = lengthAt(*pl, c.path.pos);
					for (uint32_t i = 0; i < n; ++i)
						if (elementIs(*pl, i, *c.value)) return true;
					return false;
				}
				if (pl->kind != Place::Kind::Value)
					fail(c.path.pos, c.path.text() + " is an array: compare an element (" + c.path.text() + "[0]), its count, or test contains");
				if (c.kind == CK::Equal || c.kind == CK::NotEqual)
				{
					const bool eq = holds(*pl->block, pl->off, pl->vt, *c.value);
					return c.kind == CK::Equal ? eq : !eq;
				}
				return Compare(c.kind, numberAt(*pl, c.path), number(*c.value).d);
			}
			static bool Compare(Condition::Kind k, double a, double b)
			{
				switch (k)
				{
					case Condition::Kind::Equal: return a == b;
					case Condition::Kind::NotEqual: return a != b;
					case Condition::Kind::Less: return a < b;
					case Condition::Kind::LessEqual: return a <= b;
					case Condition::Kind::Greater: return a > b;
					case Condition::Kind::GreaterEqual: return a >= b;
					default: return false;
				}
			}

			// Where `path` leads in the collection's value of the field, own or
			// inherited, without writing; nothing when it has no value there (no
			// value of the field, an index past the end). `known`: a field the
			// class doesn't have is an error, else nothing too.
			std::optional<Place> readPlace(Collection& c, const FieldPath& path, Attribute& zero, bool known)
			{
				(void)zero;
				const FieldDef* f = c.cls ? c.cls->field(KeyOf(path.field)) : nullptr;
				if (!f)
				{
					if (known) fail(path.pos, names().format(c.classKey) + " has no field " + path.field);
					return std::nullopt;
				}
				endian_ = c.vault ? c.vault->raw.endian : Endian::Little;
				Attribute* a = const_cast<Attribute*>(c.resolve(f->key));
				if (!a) return std::nullopt;
				const bool outer = softRange_;
				softRange_ = true;
				try
				{
					Place pl = locate(*a, path, names().format(f->key), false, path.steps.size());
					softRange_ = outer;
					return pl;
				}
				catch (const OutOfRange&)
				{
					softRange_ = outer;
					return std::nullopt;
				}
				catch (...)
				{
					softRange_ = outer;
					throw;
				}
			}

			// The length of the array a place is.
			uint32_t lengthAt(const Place& pl, Pos p)
			{
				switch (pl.kind)
				{
					case Place::Kind::AttrArray: return pl.attr->count;
					case Place::Kind::InlineArray: return pl.count;
					case Place::Kind::PtrArray:
						if (!pl.countMember) fail(p, pl.where + " points at one value, not an array");
						return pointerCount(pl);
					case Place::Kind::CurvePart:
						if (pl.index < 0)
						{
							const CurveValue cv = ReadCurve(*pl.block, pl.off, endian_, pl.curveSize);
							return uint32_t((pl.curvePart == 0 ? cv.x : pl.curvePart == 1 ? cv.y : cv.y2).size());
						}
						break;
					default: break;
				}
				fail(p, pl.where + " isn't an array");
			}

			// Element i of the array at `pl`: its block, offset and type.
			Place elementAt(const Place& pl, uint32_t i)
			{
				Place e;
				e.vt = pl.vt;
				e.where = pl.where + "[" + std::to_string(i) + "]";
				switch (pl.kind)
				{
					case Place::Kind::AttrArray:
						e.block = &pl.attr->data;
						e.off = i * pl.attr->elemSize;
						break;
					case Place::Kind::InlineArray:
						e.block = pl.block;
						e.off = pl.off + i * pl.vt.size;
						break;
					case Place::Kind::PtrArray:
					{
						const PtrSlot* sl = pl.block->ptrAt(pl.off + pl.member->offset);
						e.block = sl ? sl->data.get() : nullptr;
						e.off = i * pl.vt.size;
						break;
					}
					default: break;
				}
				return e;
			}

			// The value at block+off is `v` (typed by vt), as setting it there
			// would leave it: nothing is set, and no name or reference is noted.
			bool holds(const DataBlock& b, uint32_t off, const ValueType& vt, const Value& v)
			{
				const size_t refs = refs_.size(), learned = rep_.newNames.size();
				DataBlock copy = b;
				assignValue(copy, off, vt, v);
				refs_.resize(refs);
				rep_.newNames.resize(learned);
				return SameRegion(copy, b, off, std::max<uint32_t>(vt.size, 1));
			}
			bool elementIs(const Place& pl, uint32_t i, const Value& v)
			{
				const Place e = elementAt(pl, i);
				return e.block && holds(*e.block, e.off, e.vt, v);
			}
			bool holdsZero(const Place& pl, uint32_t i)
			{
				const Place e = elementAt(pl, i);
				if (!e.block) return false;
				for (uint32_t k = 0; k < e.vt.size; ++k)
					if (e.off + k >= e.block->bytes.size() || e.block->bytes[e.off + k]) return false;
				for (const PtrSlot& p : e.block->ptrs)
					if (p.offset >= e.off && p.offset < e.off + e.vt.size) return false;
				return true;
			}

			double numberAt(const Place& pl, const FieldPath& path)
			{
				const ValueType& vt = pl.vt;
				const uint8_t* p = pl.block->bytes.data() + pl.off;
				switch (vt.kind)
				{
					case TypeKind::Bool: case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16:
					case TypeKind::Int32: case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::Char: case TypeKind::Enum:
						return double(ReadInteger(*pl.block, pl.off, vt, endian_));
					case TypeKind::UInt64: return double(uint64_t(ReadInteger(*pl.block, pl.off, vt, endian_)));
					case TypeKind::Float: return loadf32(p, endian_);
					case TypeKind::Double:
					{
						const uint64_t u = load64(p, endian_);
						double d;
						std::memcpy(&d, &u, 8);
						return d;
					}
					default: fail(path.pos, "<, >, <= and >= compare numbers; " + path.text() + " is " + vt.typeName);
				}
			}

			// -------------------------------------------------------- numbers ---

			Num number(const Value& v0)
			{
				const Value& v = resolve(v0);
				Num n;
				switch (v.kind)
				{
					case Value::Kind::Number:
						if (!ParseNum(v.text, n)) fail(v.pos, "bad number " + v.text);
						return n;
					case Value::Kind::Word:
						if (v.text == "true" || v.text == "false")
						{
							n.isInt = true;
							n.bits = v.text == "true";
							n.d = double(n.bits);
							return n;
						}
						fail(v.pos, "expected a number, found " + v.text);
					case Value::Kind::Negate:
					{
						Num a = number(*v.items[0]);
						if (a.isInt && !a.unsignedBig && a.i() != INT64_MIN)
						{
							a.bits = uint64_t(-a.i());
						}
						else
						{
							a.isInt = false;
						}
						a.d = -a.d;
						a.literal.clear();
						return a;
					}
					case Value::Kind::Binary:
					{
						const Num a = number(*v.items[0]), b = number(*v.items[1]);
						if (v.op == '/' && b.d == 0) fail(v.pos, "division by zero");
						n.d = v.op == '+' ? a.d + b.d : v.op == '-' ? a.d - b.d : v.op == '*' ? a.d * b.d : a.d / b.d;
						// Integers stay exact while the result can't overflow (below 2^53).
						if (a.isInt && b.isInt && !a.unsignedBig && !b.unsignedBig && v.op != '/' && std::fabs(n.d) < 9.0e15)
						{
							const int64_t x = a.i(), y = b.i();
							n.isInt = true;
							n.bits = uint64_t(v.op == '+' ? x + y : v.op == '-' ? x - y : x * y);
						}
						return n;
					}
					default: fail(v.pos, "expected a number");
				}
			}

			// Integer for a field of `vt`, checked against its size and sign.
			int64_t integerFor(const Num& n, const ValueType& vt, Pos p, bool round)
			{
				int64_t v;
				bool big = false;
				if (n.isInt)
				{
					v = n.i();
					big = n.unsignedBig;
				}
				else
				{
					if (!std::isfinite(n.d)) fail(p, "not a finite number");
					const double r = std::nearbyint(n.d);
					if (r != n.d && !round) fail(p, vt.name + " takes whole numbers, not " + n.literal);
					if (std::fabs(r) >= 9.2e18) fail(p, "number out of range");
					v = int64_t(r);
				}
				uint32_t bits = vt.size * 8;
				bool isSigned = IsSignedKind(vt.kind) || vt.kind == TypeKind::Char;
				if (vt.member && vt.member->isBitfield())
				{
					bits = vt.member->bitSize;
					isSigned = false;
				}
				if (bits >= 64)
				{
					if (isSigned && big) fail(p, "number out of range for " + vt.typeName);
					if (!isSigned && v < 0 && !big) fail(p, "negative number for unsigned " + vt.typeName);
					return v;
				}
				const int64_t lo = isSigned ? -(int64_t(1) << (bits - 1)) : 0;
				const int64_t hi = isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
				if (big || v < lo || v > hi)
					fail(p, (n.literal.empty() ? std::to_string(v) : n.literal) + " is out of range for " +
						 (vt.typeName.empty() ? std::string("this field") : vt.typeName) + " (" + std::to_string(lo) + " to " +
						 std::to_string(hi) + ")");
				return v;
			}
			static float FloatOf(const Num& n)
			{
				if (!n.literal.empty() && !n.isInt) return std::strtof(n.literal.c_str(), nullptr);
				return float(n.d);
			}

			// ---------------------------------------------------------- places ---

			// The nested block a pointer slot leads to, made (or unshared) for writing.
			DataBlock& nestedForWrite(DataBlock& b, uint32_t slotOff, uint32_t align)
			{
				PtrSlot* s = b.ptrAt(slotOff);
				if (!s)
				{
					PtrSlot n;
					n.offset = slotOff;
					b.ptrs.push_back(std::move(n));
					s = &b.ptrs.back();
				}
				s->isString = false;
				s->text.clear();
				if (!s->data)
				{
					s->data = std::make_shared<DataBlock>();
					s->data->align = align;
				}
				else if (s->data.use_count() > 1)
				{
					// Shared with another pointer (FEPartData's camera arrays): copy on write.
					s->data = std::make_shared<DataBlock>(*s->data);
				}
				s->seq = kNewPtr;
				return *s->data;
			}

			uint32_t pointerCount(const Place& pl)
			{
				const PtrSlot* s = pl.block->ptrAt(pl.off + pl.member->offset);
				if (!pl.countMember) return s && s->data ? 1u : 0u;
				uint32_t n = uint32_t(ReadInteger(*pl.block, pl.off + pl.countMember->offset, pl.countType, endian_));
				if (s && s->data && pl.vt.size) n = std::min<uint32_t>(n, uint32_t(s->data->bytes.size() / pl.vt.size));
				else n = 0;
				return n;
			}

			Place start(Attribute& a, const std::string& fieldName)
			{
				Place pl;
				pl.vt = TypeOfAttribute(a, fieldName);
				pl.block = &a.data;
				pl.where = fieldName;
				if (a.isArray)
				{
					pl.kind = Place::Kind::AttrArray;
					pl.attr = &a;
					pl.vt.size = a.elemSize;
				}
				return pl;
			}

			Place step(const Place& pl, const FieldPath::Step& st, bool write, Pos p)
			{
				Place out;
				out.block = pl.block;
				if (st.member.empty())
				{
					int64_t i = st.index;
					out.where = pl.where + "[" + std::to_string(i) + "]";
					// A negative index counts from the end: [-1] is the last element.
					auto range = [&](uint32_t n)
					{
						if (i < 0) i += int64_t(n);
						if (i < 0 || uint64_t(i) >= n)
						{
							if (softRange_) throw OutOfRange{};
							fail(p, out.where + " is past the end (" + pl.where + " has " + std::to_string(n) + " element" +
								 (n == 1 ? "" : "s") + "; resize or insert first)");
						}
					};
					switch (pl.kind)
					{
						case Place::Kind::AttrArray:
							range(pl.attr->count);
							out.off = uint32_t(i) * pl.attr->elemSize;
							out.vt = pl.vt;
							return out;
						case Place::Kind::InlineArray:
							range(pl.count);
							out.off = pl.off + uint32_t(i) * pl.vt.size;
							out.vt = pl.vt;
							return out;
						case Place::Kind::PtrArray:
						{
							if (!pl.countMember) fail(p, pl.where + " points at one value; it has no elements to index");
							range(pointerCount(pl));
							const uint32_t slot = pl.off + pl.member->offset;
							out.block = write ? &nestedForWrite(*pl.block, slot, pl.vt.size == 1 ? 1 : 4) : pl.block->ptrAt(slot)->data.get();
							out.off = uint32_t(i) * pl.vt.size;
							out.vt = pl.vt;
							return out;
						}
						case Place::Kind::CurvePart:
							if (pl.index >= 0) fail(p, out.where + ": one index is enough");
							if (i < 0) fail(p, out.where + ": a curve's points are counted from the start");
							out = pl;
							out.index = i;
							out.where = pl.where + "[" + std::to_string(i) + "]";
							return out;
						case Place::Kind::Value:
							if (IsVectorKind(pl.vt.kind))
							{
								range(VectorLength(pl.vt.kind));
								out.off = pl.off + uint32_t(i) * 4;
								out.vt.kind = TypeKind::Float;
								out.vt.size = 4;
								out.vt.typeName = "float";
								out.vt.name = pl.vt.name;
								return out;
							}
							if (pl.vt.kind == TypeKind::Matrix)
							{
								range(4);
								out.off = pl.off + uint32_t(i) * 16;
								out.vt.kind = TypeKind::Vector4;
								out.vt.size = 16;
								out.vt.typeName = "row";
								return out;
							}
							fail(p, pl.where + " isn't an array");
					}
				}
				out.where = pl.where + "." + st.member;
				if (pl.kind == Place::Kind::PtrArray && !pl.countMember)
				{
					// A pointer to one value: its members.
					const uint32_t slot = pl.off + pl.member->offset;
					Place inner;
					inner.where = pl.where;
					inner.vt = pl.vt;
					if (write)
					{
						DataBlock& nb = nestedForWrite(*pl.block, slot, 4);
						if (nb.bytes.size() < pl.vt.size) nb.bytes.resize(pl.vt.size, 0);
						inner.block = &nb;
					}
					else
					{
						const PtrSlot* s = pl.block->ptrAt(slot);
						if (!s || !s->data) fail(p, pl.where + " is null");
						inner.block = s->data.get();
					}
					return step(inner, st, write, p);
				}
				if (pl.kind != Place::Kind::Value) fail(p, pl.where + " is an array: give an index before ." + st.member);
				if (pl.vt.kind == TypeKind::Curve)
				{
					const bool scale = CurveAt(pl.vt.size).graphScale;
					for (int k = 0; k < (scale ? 5 : 4); ++k)
						if (Lower(st.member) == Lower(k < 4 ? kCurveFloats[k] : "graphScale"))
						{
							out.off = pl.off + uint32_t(k) * 4;
							out.vt.kind = TypeKind::Float;
							out.vt.size = 4;
							out.vt.typeName = "float";
							return out;
						}
					const std::string m = Lower(st.member);
					if (m == "x" || m == "y" || m == "y2")
					{
						out.kind = Place::Kind::CurvePart;
						out.off = pl.off;
						out.curvePart = m == "x" ? 0 : m == "y" ? 1 : 2;
						out.curveSize = pl.vt.size;
						return out;
					}
					fail(p, scale ? "a curve has xMin, xMax, yMin, yMax, graphScale, X, Y and Y2" : "a curve has xMin, xMax, yMin, yMax, X, Y and Y2");
				}
				if (!ExpandsIntoMembers(pl.vt)) fail(p, pl.where + " (" + pl.vt.typeName + ") has no members");
				const MemberDef* m = FindMember(*pl.vt.st, st.member);
				if (!m) fail(p, pl.vt.st->name + " has no member " + st.member);
				if (IsCountMember(*pl.vt.st, *m)) fail(p, m->name + " is the length of an array: resize the array instead");
				const ValueType mt = TypeOfMember(*m);
				out.where = pl.where + "." + m->name;
				if (m->prim == MemberPrim::Ptr)
				{
					if (m->ptrTarget.empty() || m->ptrTarget == "void") fail(p, out.where + " is set by the game at runtime");
					out.kind = Place::Kind::PtrArray;
					out.off = pl.off;
					out.member = m;
					out.vt = TypeOfNamed(db_, m->ptrTarget);
					if (!out.vt.size) fail(p, "no size for " + m->ptrTarget);
					out.countMember = m->ptrCount.empty() ? nullptr : pl.vt.st->member(m->ptrCount);
					if (out.countMember) out.countType = TypeOfMember(*out.countMember);
					return out;
				}
				if (m->count > 1 && mt.kind == TypeKind::Float && (m->count == 3 || m->count == 4))
				{
					out.off = pl.off + m->offset;
					out.vt = mt;
					out.vt.kind = m->count == 4 ? TypeKind::Vector4 : TypeKind::Vector3;
					out.vt.size = 4 * m->count;
					return out;
				}
				if (m->count > 1)
				{
					out.kind = Place::Kind::InlineArray;
					out.off = pl.off + m->offset;
					out.vt = mt;
					out.count = m->count;
					return out;
				}
				out.off = pl.off + m->offset;
				out.vt = mt;
				return out;
			}

			Place locate(Attribute& a, const FieldPath& path, const std::string& fieldName, bool write, size_t steps)
			{
				Place pl = start(a, fieldName);
				for (size_t i = 0; i < steps; ++i) pl = step(pl, path.steps[i], write, path.pos);
				return pl;
			}

			// --------------------------------------------------------- writing ---

			std::vector<ValuePtr> listOf(const Value& v0, const char* what)
			{
				const Value& v = resolve(v0);
				if (v.kind == Value::Kind::Word && v.text == "null") return {};
				if (v.kind != Value::Kind::List) fail(v.pos, std::string(what) + " takes a list: [a, b, ...]");
				return v.items;
			}

			void assign(const Place& pl, const Value& v)
			{
				switch (pl.kind)
				{
					case Place::Kind::Value: assignValue(*pl.block, pl.off, pl.vt, v); return;
					case Place::Kind::AttrArray:
					{
						Attribute& a = *pl.attr;
						const std::vector<ValuePtr> items = listOf(v, pl.where.c_str());
						resizeAttribute(a, uint32_t(items.size()), v.pos, pl.where);
						for (size_t i = 0; i < items.size(); ++i) assignValue(a.data, uint32_t(i) * a.elemSize, pl.vt, *items[i]);
						return;
					}
					case Place::Kind::InlineArray:
					{
						const std::vector<ValuePtr> items = listOf(v, pl.where.c_str());
						if (items.size() != pl.count)
							fail(v.pos, pl.where + " has " + std::to_string(pl.count) + " elements, the list " + std::to_string(items.size()));
						for (size_t i = 0; i < items.size(); ++i) assignValue(*pl.block, pl.off + uint32_t(i) * pl.vt.size, pl.vt, *items[i]);
						return;
					}
					case Place::Kind::PtrArray:
					{
						const uint32_t slot = pl.off + pl.member->offset;
						const Value& r = resolve(v);
						if (!pl.countMember)
						{
							if (r.kind == Value::Kind::Word && r.text == "null")
							{
								DropSlot(*pl.block, slot);
								return;
							}
							DataBlock& nb = nestedForWrite(*pl.block, slot, 4);
							if (nb.bytes.size() < pl.vt.size) nb.bytes.resize(pl.vt.size, 0);
							assignValue(nb, 0, pl.vt, r);
							return;
						}
						// Resized only when the length changes: retail FEPartData has
						// empty arrays that still point into a block shared with others.
						const std::vector<ValuePtr> items = listOf(r, pl.where.c_str());
						if (items.size() != pointerCount(pl)) resizePointer(pl, uint32_t(items.size()), v.pos);
						if (items.empty()) return;
						DataBlock& nb = nestedForWrite(*pl.block, slot, pl.vt.size == 1 ? 1 : 4);
						for (size_t i = 0; i < items.size(); ++i) assignValue(nb, uint32_t(i) * pl.vt.size, pl.vt, *items[i]);
						return;
					}
					case Place::Kind::CurvePart:
					{
						CurveValue c = ReadCurve(*pl.block, pl.off, endian_, pl.curveSize);
						std::vector<float>* part = pl.curvePart == 0 ? &c.x : pl.curvePart == 1 ? &c.y : &c.y2;
						if (pl.index >= 0)
						{
							if (uint64_t(pl.index) >= part->size()) fail(v.pos, pl.where + " is past the end of the curve");
							(*part)[size_t(pl.index)] = FloatOf(number(v));
						}
						else
						{
							std::vector<float> vals;
							for (const ValuePtr& i : listOf(v, pl.where.c_str())) vals.push_back(FloatOf(number(*i)));
							*part = std::move(vals);
						}
						writeCurve(*pl.block, pl.off, c, pl.curvePart == 2, v.pos, pl.curveSize);
						return;
					}
				}
			}

			void writeCurve(DataBlock& b, uint32_t off, CurveValue& c, bool y2Given, Pos p, uint32_t size)
			{
				if (c.x.size() != c.y.size())
					fail(p, "a curve needs as many X as Y values (" + std::to_string(c.x.size()) + " and " + std::to_string(c.y.size()) +
						 "): set both in one value, { X = [...], Y = [...] }");
				if (!y2Given) c.y2 = NaturalY2(c.x, c.y);
				else if (c.y2.size() != c.x.size()) fail(p, "Y2 needs one value per point");
				WriteCurve(b, off, c, endian_, size);
			}

			void resizeAttribute(Attribute& a, uint32_t n, Pos p, const std::string& where)
			{
				if (n > MaxElements(a, true))
					fail(p, where + " holds at most " + std::to_string(MaxElements(a, true)) + " elements" + (a.inLayout ? " (its layout space)" : ""));
				if (n != a.count) warnOverLimit(a, n, where);
				while (a.count < n) InsertElement(a, a.count);
				while (a.count > n) RemoveElement(a, a.count - 1);
			}

			// An entry array may go past its definition's count (the game reads its
			// own count); that is a warning, so strict stops there.
			void warnOverLimit(const Attribute& a, uint32_t n, const std::string& where)
			{
				if (std::string w = ElementLimitWarning(a, n, where); !w.empty()) warn(w);
			}

			void resizePointer(const Place& pl, uint32_t n, Pos p)
			{
				if (!pl.countMember) fail(p, pl.where + " points at one value");
				const uint64_t maxN = pl.countType.size == 1 ? 0xFF : 0xFFFF;
				if (n > maxN) fail(p, pl.where + " holds at most " + std::to_string(maxN) + " elements");
				const uint32_t slot = pl.off + pl.member->offset;
				if (n == 0)
				{
					DropSlot(*pl.block, slot); // an empty array is a null pointer, as in retail data
				}
				else
				{
					DataBlock& nb = nestedForWrite(*pl.block, slot, pl.vt.size == 1 ? 1 : 4);
					nb.bytes.resize(size_t(n) * pl.vt.size, 0);
					auto& ps = nb.ptrs;
					ps.erase(std::remove_if(ps.begin(), ps.end(), [&](const PtrSlot& s)
					{
						return s.offset >= n * pl.vt.size;
					}), ps.end());
				}
				WriteInteger(*pl.block, pl.off + pl.countMember->offset, pl.countType, n, endian_);
			}

			void assignBytes(DataBlock& b, uint32_t off, const ValueType& vt, const Value& v)
			{
				std::vector<uint8_t> bytes;
				if (!ParseHexBytes(v.text, bytes)) fail(v.pos, "bytes takes hex: bytes \"0a 1b 2c\"");
				if (vt.kind == TypeKind::Blob)
				{
					setBlob(b, off, Bytes(bytes.begin(), bytes.end()));
					return;
				}
				if (bytes.size() != vt.size)
					fail(v.pos, vt.typeName + " is " + std::to_string(vt.size) + " bytes, not " + std::to_string(bytes.size()));
				if (everyPlatform_ && vt.size > 1 && !warnedBytes_)
				{
					// The PC's bytes are the other way round on the 360, the PS3, the GameCube and the Wii.
					warnedBytes_ = true;
					const Pos was = pos_;
					pos_ = v.pos;
					warn("the script is for every platform, and a bytes value keeps the byte order it was written in");
					pos_ = was;
				}
				for (const PtrSlot& s : b.ptrs)
					if (s.offset >= off && s.offset < off + vt.size) fail(v.pos, "it holds pointers: set its members instead of its bytes");
				std::memcpy(b.bytes.data() + off, bytes.data(), bytes.size());
			}

			void setBlob(DataBlock& b, uint32_t off, const Bytes& data)
			{
				// The data it holds already: nothing changes, not even an mSize short of it (TypeLayouts.h).
				if (const PtrSlot* s = b.ptrAt(off + 4); s && s->data && !data.empty() && s->data->bytes == data) return;
				if (data.empty())
				{
					DropSlot(b, off + 4);
				}
				else
				{
					DataBlock& nb = nestedForWrite(b, off + 4, 1);
					nb.bytes = data;
					nb.ptrs.clear();
				}
				store32(b.bytes.data() + off, uint32_t(data.size()), endian_);
			}

			void assignValue(DataBlock& b, uint32_t off, const ValueType& vt, const Value& v0)
			{
				const Value& v = resolve(v0);
				if (uint64_t(off) + std::max<uint32_t>(vt.size, 1) > b.bytes.size()) fail(v.pos, "the value lies outside its data");
				if (v.kind == Value::Kind::Bytes)
				{
					assignBytes(b, off, vt, v);
					return;
				}
				uint8_t* p = b.bytes.data() + off;
				const bool isNull = v.kind == Value::Kind::Word && v.text == "null";
				auto tuple = [&](size_t n) -> const std::vector<ValuePtr>&
				{
					if (v.kind != Value::Kind::Tuple || v.items.size() != n)
						fail(v.pos, (vt.name.empty() ? vt.typeName : vt.name) + " takes " + std::to_string(n) + " values: (" +
							 std::string(n == 2 ? "x, y" : n == 3 ? "x, y, z" : "a, b, c, d") + ")");
					return v.items;
				};
				auto text = [&]() -> std::string
				{
					if (v.kind == Value::Kind::String || v.kind == Value::Kind::Word || v.kind == Value::Kind::Number) return v.text;
					fail(v.pos, "expected a name or a string");
				};
				switch (vt.kind)
				{
					case TypeKind::Bool:
					{
						bool on;
						if (v.kind == Value::Kind::Word && (v.text == "true" || v.text == "false")) on = v.text == "true";
						else
						{
							const Num n = number(v);
							if (n.d != 0 && n.d != 1) fail(v.pos, "a bool is true or false");
							on = n.d != 0;
						}
						WriteInteger(b, off, vt, on ? 1 : 0, endian_);
						return;
					}
					case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16: case TypeKind::Int32:
					case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::UInt64: case TypeKind::Char:
						WriteInteger(b, off, vt, integerFor(number(v), vt, v.pos, false), endian_);
						return;
					case TypeKind::Float: storef32(p, FloatOf(number(v)), endian_); return;
					case TypeKind::Double:
					{
						const double d = number(v).d;
						uint64_t u;
						std::memcpy(&u, &d, 8);
						store64(p, u, endian_);
						return;
					}
					case TypeKind::Enum:
					{
						if (v.kind == Value::Kind::Word || v.kind == Value::Kind::String)
						{
							const EnumDef* en = vt.en;
							if (!en) fail(v.pos, "no names for this enum's values: use a number");
							const std::pair<std::string, int64_t>* hit = nullptr;
							// Older schemas spelled some values as IDA does: GRace::Boss::kRaceBoss_None.
							const std::string prefix = en->name + "::";
							const std::string name = v.text.compare(0, prefix.size(), prefix) == 0 ? v.text.substr(prefix.size()) : v.text;
							for (const auto& e : en->values)
								if (!hit && e.first == name) hit = &e;
							if (!hit)
								for (const auto& e : en->values)
									if (Lower(e.first) == Lower(name)) hit = &e;
							if (!hit) fail(v.pos, en->name + " has no value " + v.text);
							WriteInteger(b, off, vt, hit->second, endian_);
						}
						else
						{
							Num n = number(v);
							ValueType it = vt;
							it.kind = vt.size == 1 ? TypeKind::Int8 : vt.size == 2 ? TypeKind::Int16 : vt.size == 8 ? TypeKind::Int64 : TypeKind::Int32;
							WriteInteger(b, off, vt, integerFor(n, it, v.pos, false), endian_);
						}
						return;
					}
					case TypeKind::Text:
						if (isNull) DropSlot(b, off);
						else if (v.kind == Value::Kind::String) EnsureStringSlot(b, off).text = v.text;
						else fail(v.pos, "text takes a string in quotes, or null");
						return;
					case TypeKind::StringKey:
					{
						const StringKeyLayout sk = StringKeyAt(vt.size);
						if (isNull)
						{
							store32(p + sk.hash32, 0, endian_);
							if (sk.hash64) store64(p, 0, endian_);
							DropSlot(b, off + sk.text);
							return;
						}
						std::string s;
						uint32_t hash;
						uint64_t hash64;
						if (v.kind == Value::Kind::Tuple)
						{
							// ("text", 0xHASH) or, 16 bytes, ("text", 0xHASH, 0xHASH64): stored
							// hashes that aren't the text's own.
							const auto& it = tuple(sk.hash64 ? 3 : 2);
							const Value& t = resolve(*it[0]);
							if (t.kind != Value::Kind::String) fail(t.pos, "a string key's text goes in quotes");
							s = t.text;
							const Num h = number(*it[1]);
							if (!h.isInt || h.bits > 0xFFFFFFFFull) fail(it[1]->pos, "the hash is a 32-bit 0x... value");
							hash = uint32_t(h.bits);
							hash64 = 0;
							if (sk.hash64)
							{
								const Num h64 = number(*it[2]);
								if (!h64.isInt) fail(it[2]->pos, "the second hash is a 64-bit 0x... value");
								hash64 = h64.bits;
							}
						}
						else if (v.kind == Value::Kind::String)
						{
							s = v.text;
							hash = s.empty() ? 0 : StringHash32(s);
							hash64 = StringHash64(s); // 0 for ""
						}
						else
						{
							fail(v.pos, sk.hash64 ? "a string key takes a string in quotes, (\"text\", 0xHASH, 0xHASH64) or null"
								 : "a string key takes a string in quotes, (\"text\", 0xHASH) or null");
						}
						EnsureStringSlot(b, off + sk.text).text = s;
						store32(p + sk.hash32, hash, endian_);
						if (sk.hash64) store64(p, hash64, endian_);
						if (!s.empty()) learn(s);
						return;
					}
					case TypeKind::RefSpec:
					{
						uint32_t ck = 0, k = 0;
						if (!isNull)
						{
							std::vector<const Value*> parts;
							std::function<void(const Value&)> flatten = [&](const Value& x)
							{
								const Value& r = resolve(x);
								if (r.kind == Value::Kind::Binary && r.op == '/')
								{
									flatten(*r.items[0]);
									flatten(*r.items[1]);
								}
								else
								{
									parts.push_back(&r);
								}
							};
							flatten(v);
							std::string whole;
							if (parts.size() == 1 && parts[0]->kind == Value::Kind::String)
							{
								// "class/collection" in quotes.
								whole = parts[0]->text;
								const size_t slash = whole.find('/');
								if (slash == std::string::npos) fail(v.pos, "a reference is class/collection, or null");
								ck = KeyOf(whole.substr(0, slash));
								k = KeyOf(whole.substr(slash + 1));
							}
							else
							{
								if (parts.size() < 2) fail(v.pos, "a reference is class/collection, or null");
								for (const Value* x : parts)
									if (x->kind != Value::Kind::Word && x->kind != Value::Kind::String && x->kind != Value::Kind::Number)
										fail(x->pos, "a reference is class/collection, or null");
								const std::string cls = parts[0]->kind == Value::Kind::String && IsHex(parts[0]->text) ? "\"" + parts[0]->text + "\"" : parts[0]->text;
								std::string col;
								for (size_t i = 1; i < parts.size(); ++i) col += (i > 1 ? "/" : "") + parts[i]->text;
								if (parts.size() == 2 && parts[1]->kind == Value::Kind::String && IsHex(col)) col = "\"" + col + "\"";
								ck = KeyOf(cls);
								k = KeyOf(col);
								whole = parts[0]->text + "/" + col;
							}
							// A new reference is checked at the end (the script may make
							// its collection further down). Retail data has some that
							// lead nowhere; those aren't the script's doing.
							const RefSpecLayout rl = RefSpecAt(vt.size);
							const bool same = LoadKey(db_, p + rl.cls, rl.keySize, endian_) == ck && LoadKey(db_, p + rl.col, rl.keySize, endian_) == k;
							if (k && !same && !db_.findCollection(ck, k)) refs_.push_back({ pos_, ck, k, whole, vt.name, file_, at_ });
							// The early Most Wanted demos store StringHash64: the names are needed.
							if (rl.keySize == 8)
							{
								const size_t slash = whole.find('/');
								learn(whole.substr(0, slash));
								if (slash != std::string::npos) learn(whole.substr(slash + 1));
							}
						}
						const RefSpecLayout rl = RefSpecAt(vt.size);
						if (!StoreKey(db_, p + rl.cls, rl.keySize, ck, endian_) || !StoreKey(db_, p + rl.col, rl.keySize, k, endian_))
							fail(v.pos, "this reference holds 64-bit keys (StringHash64): give the class and collection by name");
						return;
					}
					case TypeKind::Key:
					case TypeKind::BinKey:
					case TypeKind::ClassRefSpec:
					{ // its collection's name; the pointer after it stays 0
// 0 and 0x... are values; a word or a string is a name ("" is 0).
						const bool bin = vt.kind == TypeKind::BinKey;
						const std::string t = text();
						uint32_t k = 0;
						if (isNull || t.empty() || (v.kind == Value::Kind::Number && t == "0")) k = 0;
						else if (v.kind == Value::Kind::Number && IsHex(t)) k = uint32_t(std::strtoul(t.c_str() + 2, nullptr, 16));
						else
						{
							k = bin ? BinHash(t) : StringHash32(t);
							learn(t, bin);
						}
						if (vt.kind == TypeKind::ClassRefSpec)
						{
							// Checked at the end like a reference (the script may make it further down).
							const uint32_t cls = ClassRefSpecClass(vt.typeName);
							if (k && k != load32(p, endian_) && !db_.findCollection(cls, k))
								refs_.push_back({ pos_, cls, k, db_.names().format(cls) + "/" + t, vt.name, file_, at_ });
						}
						if (vt.kind == TypeKind::Key && vt.size == 8)
						{
							// An early Most Wanted demo's StringHash64 key.
							if (!StoreKey(db_, p, 8, k, endian_)) fail(v.pos, vt.name + " holds a 64-bit key (StringHash64): give it by name");
							return;
						}
						store32(p, k, endian_);
						return;
					}
					case TypeKind::Colour:
					{
						const auto& it = tuple(4);
						for (int i = 0; i < 4; ++i)
						{
							ValueType u8;
							u8.kind = TypeKind::UInt8;
							u8.size = 1;
							u8.typeName = "colour byte";
							p[i] = uint8_t(integerFor(number(*it[size_t(i)]), u8, it[size_t(i)]->pos, false));
						}
						return;
					}
					case TypeKind::FloatColour: case TypeKind::Vector2: case TypeKind::Vector3: case TypeKind::Vector4:
					case TypeKind::Quaternion:
					{
						const uint32_t n = VectorLength(vt.kind);
						const auto& it = tuple(n);
						for (uint32_t i = 0; i < n; ++i) storef32(p + i * 4, FloatOf(number(*it[i])), endian_);
						return;
					}
					case TypeKind::Matrix:
					{
						if (v.kind != Value::Kind::Tuple || (v.items.size() != 4 && v.items.size() != 16))
							fail(v.pos, "a matrix takes four rows ((a, b, c, d), ...) or 16 numbers");
						std::vector<const Value*> nums;
						for (const ValuePtr& r : v.items)
						{
							const Value& row = resolve(*r);
							if (v.items.size() == 16) nums.push_back(&row);
							else if (row.kind == Value::Kind::Tuple && row.items.size() == 4)
								for (const ValuePtr& x : row.items) nums.push_back(x.get());
							else fail(row.pos, "a matrix row has four numbers");
						}
						for (size_t i = 0; i < 16; ++i) storef32(p + i * 4, FloatOf(number(*nums[i])), endian_);
						return;
					}
					case TypeKind::Blob:
					{
						if (isNull)
						{
							setBlob(b, off, {});
							return;
						}
						if (v.kind != Value::Kind::Lua) fail(v.pos, "a blob takes bytes \"...\", lua \"...\" or null");
						// The version of the chunk there, else the game's (Gameplay.h LuaVersionOf).
						lua::Version version = LuaVersionOf(db_);
						try
						{
							if (const PtrSlot* s = b.ptrAt(off + 4); s && s->data && ParseLzHeader(s->data->bytes))
								version = VersionOfChunk(db_, Decompress(s->data->bytes));
						}
						catch (const std::exception&)
						{
						}
						const lua::CompileResult r = lua::Compile(v.text, version);
						if (!r.ok()) fail(v.pos, "Lua: " + r.error);
						setBlob(b, off, CompressBest(r.chunk));
						return;
					}
					case TypeKind::Curve:
					{
						if (v.kind != Value::Kind::Struct) fail(v.pos, "a curve takes { xMin = .., xMax = .., yMin = .., yMax = .., X = [...], Y = [...] }");
						CurveValue c = ReadCurve(b, off, endian_, vt.size);
						const bool scale = CurveAt(vt.size).graphScale;
						bool y2 = false;
						for (const auto& [name, mv] : v.members)
						{
							const std::string m = Lower(name);
							float* f = m == "xmin" ? &c.xMin : m == "xmax" ? &c.xMax : m == "ymin" ? &c.yMin : m == "ymax" ? &c.yMax
								: m == "graphscale" && scale ? &c.graphScale : nullptr;
							if (f)
							{
								*f = FloatOf(number(*mv));
								continue;
							}
							std::vector<float>* part = m == "x" ? &c.x : m == "y" ? &c.y : m == "y2" ? &c.y2 : nullptr;
							if (!part) fail(mv->pos, scale ? "a curve has xMin, xMax, yMin, yMax, graphScale, X, Y and Y2" : "a curve has xMin, xMax, yMin, yMax, X, Y and Y2");
							std::vector<float> vals;
							for (const ValuePtr& i : listOf(*mv, name.c_str())) vals.push_back(FloatOf(number(*i)));
							*part = std::move(vals);
							y2 |= m == "y2";
						}
						writeCurve(b, off, c, y2, v.pos, vt.size);
						return;
					}
					case TypeKind::Struct:
						if (ExpandsIntoMembers(vt))
						{
							if (v.kind != Value::Kind::Struct) fail(v.pos, vt.st->name + " takes { member = value, ... }");
							Place self;
							self.block = &b;
							self.off = off;
							self.vt = vt;
							self.where = vt.name;
							for (const auto& [name, mv] : v.members) assign(step(self, { name, -1 }, true, mv->pos), *mv);
							return;
						}
						fail(v.pos, "no layout for " + vt.typeName + ": use bytes \"...\"");
					case TypeKind::Unknown:
						if (vt.size == 4)
						{
							ValueType u = vt;
							const Num n = number(v);
							u.kind = n.isInt && (n.bits >> 31) && !(n.i() < 0) ? TypeKind::UInt32 : TypeKind::Int32;
							WriteInteger(b, off, u, integerFor(n, u, v.pos, false), endian_);
							return;
						}
						fail(v.pos, vt.typeName + " has no layout: use bytes \"...\"");
				}
			}

			// ------------------------------------------------------- statements ---

			void checkHeader()
			{
				const Statement* first = nullptr;
				for (const Statement& s : sc_.statements)
					if (s.kind != K::Comment)
					{
						first = &s;
						break;
					}
				const int need = FormatNeeded(sc_.statements);
				if (!st_.fragment && (!first || first->kind != K::Header))
					message(Message::Level::Warning, {}, "no \"attribulina " + std::to_string(need) + "\" line at the top; reading it as format " +
							std::to_string(need));
				else if (first && first->kind == K::Header && first->number < need)
					message(Message::Level::Warning, first->pos, "this script uses format " + std::to_string(need) + " statements (append, conditions on the "
							"database, ...) but its first line says " + std::to_string(first->number) + ": make it \"attribulina " +
							std::to_string(need) + "\" so that older versions of Attribulina say they can't read it");
				checkGame(sc_);
			}
			// The `game` line names the game and platform the script was made for.
			// Another one is a warning (an error when strict): the values still apply,
			// typed by the fields they go into, but the script wasn't made for these
			// files.
			void checkGame(const Script& sc)
			{
				const Statement* line = nullptr;
				for (const Statement& s : sc.statements)
					if (s.kind == K::Meta && s.word == "game") line = &s;
				if (!line) return;
				pos_ = line->pos;
				const std::optional<ScriptTarget> t = ParseScriptTarget(line->text);
				if (!t)
				{
					warn("unknown game " + Quote(line->text) +
						 ": it names a game and a platform, such as prostreet_pc or prostreet_ps2, or prostreet_all for every platform");
					return;
				}
				everyPlatform_ = t->allPlatforms();
				const attrib::Target& here = db_.target();
				if (here.known() && !t->fits(here)) warn("the script is for " + t->name() + ", and this database is " + here.name());
			}

			// The included script's statements, here: it shares the options, and the
			// references it sets are checked with the rest at the end.
			void include(const Statement& s)
			{
				if (!s.included) fail(s.pos, "include " + Quote(s.text) + " wasn't read");
				const std::string outerFile = file_;
				const Pos outerAt = at_;
				if (file_.empty()) at_ = s.pos;
				file_ = s.included->source;
				const bool outerEvery = everyPlatform_;
				everyPlatform_ = false;
				checkGame(*s.included);
				top(s.included->statements);
				everyPlatform_ = outerEvery;
				file_ = outerFile;
				at_ = outerAt;
			}

			void top(const std::vector<Statement>& list)
			{
				for (const Statement& s : list)
				{
					pos_ = s.pos;
					switch (s.kind)
					{
						case K::Comment: case K::Header: case K::Meta: case K::Option: break;
						case K::If:
							if (test(s.cond, nullptr)) top(s.body);
							else top(s.elseBody);
							break;
						case K::Edit:
						{
							const std::vector<Collection*> cols = matching(s.target);
							if (cols.empty()) warn("edit " + s.target.text() + " matches no collection");
							for (Collection* c : cols) block(*c, s.body, nullptr);
							break;
						}
						case K::Create: create(s); break;
						case K::Collection: upsert(s); break;
						case K::Delete: remove(s); break;
						case K::Rename: rename(s); break;
						case K::Move:
						{
							Collection& c = required(s.target);
							Collection* parent = s.under.root ? nullptr : &required(s.under);
							move(c, parent);
							break;
						}
						case K::MoveToVault:
						{
							Collection& c = required(s.target);
							Vault& v = vaultNamed(s.vault);
							if (c.vault != &v)
							{
								touch(c.vault);
								CollectionEditor(db_).moveToVault(c, v);
								touch(&v);
								++rep_.moved;
							}
							break;
						}
						case K::CreateVault: createVault(s); break;
						case K::DeleteVault: deleteVault(s); break;
						case K::RenameVault: renameVault(s); break;
						case K::Class: classStatement(s); break;
						case K::DeleteClass: deleteClass(s); break;
						case K::Echo: echo(s, nullptr); break;
						case K::Include: include(s); break;
						default: fail(s.pos, "this goes inside an edit, create or collection block");
					}
				}
			}

			void move(Collection& c, Collection* parent)
			{
				if (parent == c.parent && (parent || !c.parentKey)) return;
				if (parent && parent->cls != c.cls) fail(label(*parent) + " is another class than " + label(c));
				if (!CollectionEditor(db_).move(c, parent)) fail("can't put " + label(c) + " under " + label(*parent) + ": it is under it");
				touch(c.vault);
				++rep_.moved;
			}

			Collection& make(ClassDef& cls, const Target& t, Vault& vault, Collection* parent)
			{
				CollectionEditor ed(db_);
				const std::string name = NameOf(t.name);
				if (name.empty())
				{
					Collection& c = ed.create(vault, cls, KeyOf(t.name), parent);
					warn("made " + t.text() + " without a name, so it has no export id");
					return c;
				}
				if (name.find_first_of("*?") != std::string::npos) fail(t.pos, "a new collection's name can't hold * or ?");
				const bool known = names().find(StringHash32(name)) != nullptr;
				Collection& c = ed.create(vault, cls, name, parent);
				if (!known) rep_.newNames.push_back(name);
				return c;
			}

			// A CollectionName field that held the old name follows the new one.
			void followName(Collection& c, const std::string& oldName)
			{
				Attribute* a = c.find(StringHash32("CollectionName"));
				if (!a || !a->type || a->type->kind != TypeKind::Text || a->isArray) return;
				PtrSlot* s = a->data.ptrAt(0);
				if (!s || !s->isString || s->text != oldName) return;
				s->text = names().format(c.key);
			}

			void copyValues(const Collection& src, Collection& dst)
			{
				dst.layout = src.layout;
				DetachCopy(dst.layout);
				dst.attributes = src.attributes;
				for (Attribute& a : dst.attributes) DetachCopy(a);
			}

			void create(const Statement& s)
			{
				ClassDef& cls = classOf(s.target);
				if (db_.findCollection(cls.key, KeyOf(s.target.name)))
					fail(s.target.pos, s.target.text() + " exists already (a collection statement makes it or updates it)");
				Collection* src = s.hasFrom ? &required(s.from) : nullptr;
				if (src && src->cls != &cls) fail(s.from.pos, s.from.text() + " is another class");
				Collection* parent = s.hasUnder ? (s.under.root ? nullptr : &required(s.under)) : src ? src->parent : nullptr;
				if (parent && parent->cls != &cls) fail(s.under.pos, s.under.text() + " is another class");
				Vault& vault = s.vault.empty() ? defaultVault(cls, parent, src) : vaultNamed(s.vault);
				Collection& c = make(cls, s.target, vault, parent);
				if (src)
				{
					copyValues(*src, c);
					followName(c, names().format(src->key));
				}
				touch(&vault);
				++rep_.created;
				block(c, s.body, nullptr);
			}

			void upsert(const Statement& s)
			{
				ClassDef& cls = classOf(s.target);
				Collection* c = find(s.target);
				Collection* parent = s.hasUnder ? (s.under.root ? nullptr : &required(s.under)) : c ? c->parent : nullptr;
				if (parent && parent->cls != &cls) fail(s.under.pos, s.under.text() + " is another class");
				if (!c)
				{
					Vault& vault = s.vault.empty() ? defaultVault(cls, parent, nullptr) : vaultNamed(s.vault);
					c = &make(cls, s.target, vault, parent);
					touch(&vault);
					++rep_.created;
				}
				else
				{
					if (s.hasUnder) move(*c, parent);
					if (!s.vault.empty())
					{
						Vault& v = vaultNamed(s.vault);
						if (c->vault != &v)
						{
							touch(c->vault);
							CollectionEditor(db_).moveToVault(*c, v);
							touch(&v);
							++rep_.moved;
						}
					}
				}
				std::set<uint32_t> assigned;
				block(*c, s.body, &assigned);
				if (s.exact)
				{
					std::vector<uint32_t> drop;
					for (const Attribute& a : c->attributes)
						if (!a.inLayout && a.def && !assigned.count(a.key)) drop.push_back(a.key);
					for (uint32_t k : drop)
						if (RevertAttribute(*c, k))
						{
							++rep_.reverted;
							touch(c->vault);
						}
				}
			}

			void remove(const Statement& s)
			{
				Collection* c = find(s.target);
				if (!c)
				{
					info(s.target.text() + " is gone already");
					return;
				}
				std::vector<Collection*> all;
				std::function<void(Collection*)> walk = [&](Collection* x)
				{
					all.push_back(x);
					for (Collection* k : x->children) walk(k);
				};
				walk(c);
				if (all.size() > 1 && !s.withChildren)
					fail(s.target.text() + " has " + std::to_string(all.size() - 1) + " collection(s) under it: delete them first, or say "
						 "delete " + s.target.text() + " with children");
				CollectionEditor ed(db_);
				for (auto it = all.rbegin(); it != all.rend(); ++it)
				{
					Collection& x = **it;
					if (st_.beforeRemove) st_.beforeRemove(x);
					touch(x.vault);
					ed.remove(x);
					++rep_.deleted;
				}
			}

			void rename(const Statement& s)
			{
				// A 0x... word is a key (a name not known, as an nfsms rename_node to a hash).
				const std::string newName = NameOf(s.word);
				const uint32_t newKey = newName.empty() ? KeyOf(s.word) : StringHash32(newName);
				Collection* c = find(s.target);
				if (!c)
				{
					const ClassDef& cls = classOf(s.target);
					if (db_.findCollection(cls.key, newKey))
					{
						info(s.target.text() + " is called " + s.word + " already");
						return;
					}
					fail(s.target.pos, "no collection " + s.target.text());
				}
				const bool known = newName.empty() || names().find(newKey) != nullptr;
				const std::string oldName = names().format(c->key);
				if (newName.empty()) CollectionEditor(db_).rename(*c, newKey);
				else CollectionEditor(db_).rename(*c, newName);
				followName(*c, oldName);
				if (!known) rep_.newNames.push_back(newName);
				touch(c->vault);
				for (Collection* k : c->children) touch(k->vault);
				++rep_.renamed;
			}

			Vault& databaseVault()
			{
				for (const auto& pk : db_.packs())
					for (const auto& v : pk->vaults)
						if (v->isDatabase) return *v;
				fail("no pack with the database (attributes.bin) is open: say which vault the class goes in: in vault <name>");
			}

			void classStatement(const Statement& s)
			{
				ClassDesign d;
				try
				{
					d = DesignFromStatement(db_, s);
				}
				catch (const ScriptError& e)
				{
					fail(e.pos, e.message);
				}
				learn(s.word);
				for (const Statement& f : s.body)
					if (f.kind == K::ClassField && f.word != "bytes") learn(f.path.field);
				ClassDef* cls = db_.findClass(d.key);
				ClassEditor ed(db_);
				try
				{
					if (!cls)
					{
						Vault& v = s.vault.empty() ? databaseVault() : vaultNamed(s.vault);
						cls = &ed.create(v, d);
						++rep_.classesCreated;
					}
					else
					{
						if (!s.vault.empty() && &vaultNamed(s.vault) != cls->vault)
							warn("class " + s.word + " is defined in vault " + (cls->vault ? cls->vault->name : std::string("?")) + "; it stays there");
						if (IsGameClass(db_, cls->key))
						{
							// The game's classes stay as they are; a statement of one as it is changes nothing.
							ClassDef laid;
							laid.collections = cls->collections;
							LayOutClass(db_, d, laid);
							laid.collectionReserve = cls->collectionReserve;
							bool same = laid.fields.size() == cls->fields.size() && laid.layoutSize == cls->layoutSize &&
								laid.layoutCount == cls->layoutCount && laid.layoutKeyShift == cls->layoutKeyShift && laid.staticSize == cls->staticSize;
							for (size_t i = 0; same && i < laid.fields.size(); ++i)
							{
								const FieldDef& a = laid.fields[i];
								const FieldDef& b = cls->fields[i];
								same = a.key == b.key && a.typeHash == b.typeHash && a.offset == b.offset && a.size == b.size &&
									a.maxCount == b.maxCount && a.flags == b.flags && a.alignment == b.alignment;
							}
							if (!same)
								fail(s.word + " is a class of the game: its compiled code reads it as it is, so it can't be redefined "
									 "(only classes made for a mod can)");
						}
						else if (ed.redesign(*cls, d))
						{
							++rep_.classesChanged;
						}
					}
				}
				catch (const ScriptError&)
				{
					throw;
				}
				catch (const std::exception& e)
				{
					fail(e.what());
				}
				for (LoadedPack* p : ed.touched()) rep_.touched.insert(p);
				// Static values: the block as stored first, then each field's.
				endian_ = cls->vault ? cls->vault->raw.endian : Endian::Little;
				for (const Statement& f : s.body)
				{
					if (f.kind != K::ClassField || f.word != "bytes" || !f.value) continue;
					pos_ = f.pos;
					std::vector<uint8_t> bytes;
					if (f.value->kind != Value::Kind::Bytes || !ParseHexBytes(f.value->text, bytes))
						fail(f.pos, "a class's bytes are hex: bytes \"0a 1b 2c\"");
					if (bytes.size() > cls->staticSize)
						fail(f.pos, std::to_string(bytes.size()) + " bytes don't fit the class's " + std::to_string(cls->staticSize) + " static bytes");
					if (cls->staticData.bytes.size() < cls->staticSize) cls->staticData.bytes.resize(cls->staticSize, 0);
					if (!std::equal(bytes.begin(), bytes.end(), cls->staticData.bytes.begin()))
					{
						std::copy(bytes.begin(), bytes.end(), cls->staticData.bytes.begin());
						++rep_.valuesSet;
						touch(cls->vault);
					}
				}
				for (const Statement& f : s.body)
				{
					if (f.kind != K::ClassField || f.word == "bytes" || !f.value) continue;
					pos_ = f.pos;
					const FieldDef* def = cls->field(KeyOf(f.path.field));
					if (!def || !def->isStatic()) fail(f.pos, "only a static field takes a value here: collections hold the others");
					if (cls->staticData.bytes.size() < uint32_t(def->offset) + def->size) cls->staticData.bytes.resize(uint32_t(def->offset) + def->size, 0);
					const DataBlock before = cls->staticData;
					assignValue(cls->staticData, def->offset, TypeOfStatic(*def, names().format(def->key)), *f.value);
					if (cls->staticData.bytes != before.bytes || cls->staticData.ptrs.size() != before.ptrs.size())
					{
						++rep_.valuesSet;
						touch(cls->vault);
					}
				}
				pos_ = s.pos;
			}

			void deleteClass(const Statement& s)
			{
				ClassDef* cls = db_.findClass(KeyOf(s.word));
				if (!cls)
				{
					info("class " + s.word + " is gone already");
					return;
				}
				ClassEditor ed(db_);
				const size_t n = cls->collections.size();
				ed.remove(*cls, st_.beforeRemove);
				for (LoadedPack* p : ed.touched()) rep_.touched.insert(p);
				rep_.deleted += n;
				++rep_.classesDeleted;
			}

			void createVault(const Statement& s)
			{
				const std::string file = Lower(NameOf(s.word).empty() ? s.word : NameOf(s.word));
				LoadedPack* pack = nullptr;
				for (const auto& pk : db_.packs())
				{
					std::string fn = pk->path;
					const size_t slash = fn.find_last_of("/\\");
					if (slash != std::string::npos) fn = fn.substr(slash + 1);
					if (Lower(fn) == file || Lower(pk->path) == file) pack = pk.get();
				}
				const std::string name = NameOf(s.vault).empty() ? s.vault : NameOf(s.vault);
				// In another file too: `in vault` puts collections in it all the same.
				if (const Vault* v = findVault(s.vault))
				{
					std::string where;
					if (v->pack && v->pack != pack)
					{
						where = v->pack->path;
						if (const size_t slash = where.find_last_of("/\\"); slash != std::string::npos) where = where.substr(slash + 1);
						where = " in " + where;
					}
					info("vault " + v->name + " exists already" + where);
					return;
				}
				if (!pack)
				{
					// World's event vaults are files of their own: `create vault x in
					// x.bin` makes one in gc.vaults.
					if (db_.target().game != Game::World) fail("no pack " + s.word + " is open");
					if (file.size() < 5 || file.substr(file.size() - 4) != ".bin") fail("an event vault file's name ends in .bin, not " + s.word);
					const Vault* like = EventVaultModel(db_);
					if (!like) fail("no vault is open to copy the header from");
					const std::string written = NameOf(s.word).empty() ? s.word : NameOf(s.word);
					const std::string path = NewEventVaultPath(db_, written.substr(0, written.size() - 4));
					LoadedPack& made = CreateEventVaultFile(db_, path, name, *like);
					rep_.touched.insert(&made);
					return;
				}
				if (const std::string p = VaultNameProblem(db_, *pack, name); !p.empty()) fail(p);
				if (pack->vaults.empty()) fail(s.word + " has no vault to copy the header from");
				CreateVault(*pack, name, *pack->vaults.front());
				pack->dirty = true;
				rep_.touched.insert(pack);
			}

			void deleteVault(const Statement& s)
			{
				const std::string name = NameOf(s.vault).empty() ? s.vault : NameOf(s.vault);
				for (const auto& pk : db_.packs())
					if (Vault* v = FindVault(*pk, name))
					{
						LoadedPack* pack = pk.get();
						if (pack->format == PackFormat::VaultFile)
						{
							// An event vault is its file: the file goes when the database is saved.
							const std::string path = pack->path;
							if (!db_.removePack(*pack)) fail("vault " + name + " isn't empty");
							rep_.touched.erase(pack);
							rep_.removedFiles.push_back(path);
							return;
						}
						if (!RemoveVault(*pack, *v)) fail("vault " + name + " isn't empty");
						pack->dirty = true;
						rep_.touched.insert(pack);
						return;
					}
				info("vault " + name + " is gone already");
			}

			void renameVault(const Statement& s)
			{
				const std::string name = NameOf(s.vault).empty() ? s.vault : NameOf(s.vault);
				const std::string to = NameOf(s.word).empty() ? s.word : NameOf(s.word);
				Vault* v = FindVault(db_, name);
				if (!v)
				{
					if (FindVault(db_, to))
					{
						info("vault " + name + " is called " + to + " already");
						return;
					}
					fail("no vault " + name + " is open");
				}
				try
				{
					for (Collection* c : RenameVault(db_, *v, to))
						if (c->vault && c->vault->pack)
						{
							c->vault->pack->dirty = true;
							rep_.touched.insert(c->vault->pack);
						}
				}
				catch (const std::runtime_error& e)
				{
					fail(e.what());
				}
				v->pack->dirty = true;
				rep_.touched.insert(v->pack);
			}

			// `echo`: its values joined by spaces. In a block, a word naming one of
			// the collection's fields prints that field's value.
			void echo(const Statement& s, Collection* c)
			{
				std::string text;
				for (const ValuePtr& item : s.value->items)
				{
					const Value& v = resolve(*item);
					std::string part;
					switch (v.kind)
					{
						case Value::Kind::String: case Value::Kind::Number: part = v.text; break;
						case Value::Kind::Word:
							part = v.text;
							if (c && c->cls)
								if (const FieldDef* f = c->cls->field(KeyOf(v.text)))
								{
									const Statement e = ExportValue(db_, *c, f->key);
									part = e.body.empty() ? std::string("(not set)") : PrintValue(*e.body[0].value);
								}
							break;
						case Value::Kind::Binary: case Value::Kind::Negate:
						{
							const Num n = number(v);
							char b[40];
							if (n.isInt) std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(n.i()));
							else std::snprintf(b, sizeof(b), "%.9g", n.d);
							part = b;
							break;
						}
						default: part = PrintValue(v); break;
					}
					text += (text.empty() ? "" : " ") + part;
				}
				if (s.word == "error") fail(text);
				message(s.word == "warning" ? Message::Level::Warning : Message::Level::Echo, pos_, text);
			}

			// Value statements inside edit/create/collection. `assigned` collects
			// the fields set (for `exact`).
			void block(Collection& c, const std::vector<Statement>& body, std::set<uint32_t>* assigned)
			{
				endian_ = c.vault ? c.vault->raw.endian : Endian::Little;
				for (const Statement& s : body)
				{
					pos_ = s.pos;
					switch (s.kind)
					{
						case K::Comment: break;
						case K::If:
							if (test(s.cond, &c)) block(c, s.body, assigned);
							else block(c, s.elseBody, assigned);
							break;
						case K::Set: case K::Resize: case K::Insert: case K::Remove: case K::Append: valueStatement(c, s, assigned); break;
						case K::Echo: echo(s, &c); break;
						case K::Revert:
						{
							const FieldDef& f = fieldOf(c, s.path);
							if (f.inLayout()) fail(s.path.pos, s.path.field + " is a layout field: every collection has its own");
							if (RevertAttribute(c, f.key))
							{
								++rep_.reverted;
								touch(c.vault);
							}
							break;
						}
						default: fail(s.pos, "this can't go inside a collection's block");
					}
				}
			}

			const FieldDef& fieldOf(Collection& c, const FieldPath& p)
			{
				const FieldDef* f = c.cls ? c.cls->field(KeyOf(p.field)) : nullptr;
				if (!f) fail(p.pos, names().format(c.classKey) + " has no field " + p.field);
				learn(p.field);
				return *f;
			}

			void valueStatement(Collection& c, const Statement& s, std::set<uint32_t>* assigned)
			{
				const FieldDef& f = fieldOf(c, s.path);
				if (assigned) assigned->insert(f.key);
				const std::string fieldName = names().format(f.key);
				// What it was, to tell a change from a no-op.
				const Attribute* before = c.find(f.key);
				const bool wasOwn = before != nullptr;
				const Attribute snapshot = wasOwn ? *before : Attribute{};
				Attribute& a = OverrideAttribute(db_, c, f);
				const size_t steps = s.path.steps.size();
				switch (s.kind)
				{
					case K::Set:
					{
						const Place pl = locate(a, s.path, fieldName, true, steps);
						if (s.was) checkWas(pl, *s.was, c, s.path);
						if (s.op == '=') assign(pl, *s.value);
						else compound(pl, s.op, *s.value);
						break;
					}
					case K::Resize:
					{
						const Place pl = locate(a, s.path, fieldName, true, steps);
						if (pl.kind != Place::Kind::AttrArray && pl.kind != Place::Kind::PtrArray) fail(s.path.pos, pl.where + " has a fixed size");
						uint32_t n = s.count;
						if (s.by)
						{
							const int64_t want = int64_t(lengthAt(pl, s.path.pos)) + s.number;
							if (want < 0)
								fail(s.pos, pl.where + " has " + std::to_string(lengthAt(pl, s.path.pos)) + " elements: it can't lose " +
									 std::to_string(-s.number));
							n = uint32_t(want);
						}
						if (pl.kind == Place::Kind::AttrArray) resizeAttribute(a, n, s.pos, pl.where);
						else resizePointer(pl, n, s.pos);
						break;
					}
					case K::Remove:
						if (s.value)
						{
							// Every element that is the value; none is a note.
							const Place pl = locate(a, s.path, fieldName, true, steps);
							if (pl.kind != Place::Kind::AttrArray && (pl.kind != Place::Kind::PtrArray || !pl.countMember))
								fail(s.path.pos, pl.where + " isn't an array that can lose elements");
							size_t removed = 0;
							for (int64_t i = int64_t(lengthAt(pl, s.path.pos)) - 1; i >= 0; --i)
								if (elementIs(pl, uint32_t(i), *s.value))
								{
									Statement one = s;
									one.kind = K::Remove;
									insertOrRemove(a, pl, i, one);
									++removed;
								}
							if (!removed) info(label(c) + " " + s.path.text() + " holds no " + PrintValue(*s.value));
							break;
						}
						[[fallthrough]];
					case K::Insert:
					{
						const Place pl = locate(a, s.path, fieldName, true, steps - 1);
						int64_t at = s.path.steps.back().index;
						// Counted from the end: insert [-1] goes before the last element.
						if (at < 0 && (pl.kind == Place::Kind::AttrArray || (pl.kind == Place::Kind::PtrArray && pl.countMember)))
							at += int64_t(lengthAt(pl, s.path.pos));
						insertOrRemove(a, pl, at, s);
						break;
					}
					case K::Append:
					{
						// At the end; a list adds each of its items, `if missing` only those
						// the array doesn't hold yet.
						const Place pl = locate(a, s.path, fieldName, true, steps);
						if (pl.kind != Place::Kind::AttrArray && (pl.kind != Place::Kind::PtrArray || !pl.countMember))
							fail(s.path.pos, pl.where + " isn't an array that can grow");
						std::vector<ValuePtr> items;
						if (!s.value) items.push_back(nullptr);
						else if (const Value& r = resolve(*s.value); r.kind == Value::Kind::List) items = r.items;
						else items.push_back(s.value);
						for (const ValuePtr& v : items)
						{
							if (s.ifMissing)
							{
								bool there = false;
								const uint32_t n = lengthAt(pl, s.path.pos);
								for (uint32_t i = 0; i < n && !there; ++i)
									there = v ? elementIs(pl, i, *v) : holdsZero(pl, i);
								if (there) continue;
							}
							Statement one = s;
							one.kind = K::Insert;
							one.value = v;
							insertOrRemove(a, pl, int64_t(lengthAt(pl, s.path.pos)), one);
						}
						break;
					}
					default: break;
				}
				// Unchanged: put the original back, with its pointer order and shared
				// blocks, so the pack still writes back byte for byte.
				Attribute* now = c.find(f.key);
				if (wasOwn && now && SameAttribute(snapshot, *now))
				{
					*now = snapshot;
				}
				else
				{
					++rep_.valuesSet;
					touch(c.vault);
					if (std::getenv("ATTRIB_SCRIPT_TRACE"))
						std::fprintf(stderr, "script: line %d sets %s %s (%s)\n", s.pos.line, label(c).c_str(), s.path.text().c_str(),
									 !wasOwn ? "was inherited" : !now ? "gone" : snapshot.count != now->count ? "count" :
									 snapshot.capacity != now->capacity ? "capacity" : snapshot.data.bytes != now->data.bytes ? "bytes" : "pointers");
				}
			}

			void insertOrRemove(Attribute& a, const Place& pl, int64_t at, const Statement& s)
			{
				const bool ins = s.kind == K::Insert;
				if (pl.kind == Place::Kind::AttrArray)
				{
					if (at < 0 || uint64_t(at) > a.count - (ins ? 0 : 1) || (!ins && a.count == 0))
						fail(s.path.pos, s.path.text() + " is past the end (" + std::to_string(a.count) + " elements)");
					if (ins)
					{
						if (a.count + 1 > MaxElements(a, true))
							fail(s.pos, pl.where + " holds at most " + std::to_string(MaxElements(a, true)) + " elements" +
								 (a.inLayout ? " (its layout space)" : ""));
						warnOverLimit(a, a.count + 1, pl.where);
						InsertElement(a, uint32_t(at));
						if (s.value) assignValue(a.data, uint32_t(at) * a.elemSize, pl.vt, *s.value);
					}
					else
					{
						RemoveElement(a, uint32_t(at));
					}
					return;
				}
				if (pl.kind != Place::Kind::PtrArray || !pl.countMember) fail(s.path.pos, pl.where + " has a fixed size");
				const uint32_t n = pointerCount(pl);
				if (at < 0 || uint64_t(at) > n - (ins ? 0 : 1) || (!ins && n == 0))
					fail(s.path.pos, s.path.text() + " is past the end (" + std::to_string(n) + " elements)");
				const uint32_t es = pl.vt.size, pos = uint32_t(at) * es;
				if (ins)
				{
					resizePointer(pl, n + 1, s.pos);
					DataBlock& nb = nestedForWrite(*pl.block, pl.off + pl.member->offset, es == 1 ? 1 : 4);
					// Shift the tail up one element.
					std::memmove(nb.bytes.data() + pos + es, nb.bytes.data() + pos, size_t(n - uint32_t(at)) * es);
					std::memset(nb.bytes.data() + pos, 0, es);
					for (PtrSlot& p : nb.ptrs)
						if (p.offset >= pos) p.offset += es;
					if (s.value) assignValue(nb, pos, pl.vt, *s.value);
				}
				else
				{
					DataBlock& nb = nestedForWrite(*pl.block, pl.off + pl.member->offset, es == 1 ? 1 : 4);
					auto& ps = nb.ptrs;
					ps.erase(std::remove_if(ps.begin(), ps.end(), [&](const PtrSlot& p)
					{
						return p.offset >= pos && p.offset < pos + es;
					}), ps.end());
					for (PtrSlot& p : ps)
						if (p.offset >= pos + es) p.offset -= es;
					nb.bytes.erase(nb.bytes.begin() + pos, nb.bytes.begin() + pos + es);
					nb.bytes.resize(size_t(n) * es); // resizePointer trims it to n - 1
					resizePointer(pl, n - 1, s.pos);
				}
			}

			void compound(const Place& pl, char op, const Value& v)
			{
				if (pl.kind != Place::Kind::Value) fail(pl.where + " isn't a number");
				const ValueType& vt = pl.vt;
				const uint8_t* p = pl.block->bytes.data() + pl.off;
				double cur;
				switch (vt.kind)
				{
					case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16: case TypeKind::Int32:
					case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::UInt64: case TypeKind::Char:
						cur = double(ReadInteger(*pl.block, pl.off, vt, endian_));
						break;
					case TypeKind::Float: cur = loadf32(p, endian_); break;
					case TypeKind::Double:
					{
						const uint64_t u = load64(p, endian_);
						std::memcpy(&cur, &u, 8);
						break;
					}
					default: fail(std::string(1, op) + "= works on numbers; " + pl.where + " is " + vt.typeName);
				}
				const Num n = number(v);
				if (op == '/' && n.d == 0) fail("division by zero");
				Num r;
				r.d = op == '+' ? cur + n.d : op == '-' ? cur - n.d : op == '*' ? cur * n.d : cur / n.d;
				if (vt.kind == TypeKind::Float) storef32(pl.block->bytes.data() + pl.off, float(r.d), endian_);
				else if (vt.kind == TypeKind::Double)
				{
					uint64_t u;
					std::memcpy(&u, &r.d, 8);
					store64(pl.block->bytes.data() + pl.off, u, endian_);
				}
				else
				{
					WriteInteger(*pl.block, pl.off, vt, integerFor(r, vt, v.pos, true), endian_); // rounded
				}
			}

			// `was`: the value before the change, as the author's database had it.
			void checkWas(const Place& pl, const Value& was, const Collection& c, const FieldPath& path)
			{
				if (pl.kind != Place::Kind::Value) fail(was.pos, "was checks a single value; give an index");
				DataBlock copy = *pl.block;
				assignValue(copy, pl.off, pl.vt, was);
				if (!SameRegion(copy, *pl.block, pl.off, std::max<uint32_t>(pl.vt.size, 1)))
					warn(label(c) + " " + path.text() + " was " + PrintValue(was) + " in the script's database, not here");
			}

		public:
			// TypeAt(): walks the path without writing, through the own value or
			// the inherited one (or a zero value of the field's type).
			ValueType typeAt(Collection& c, const FieldPath& path, Bytes* current)
			{
				pos_ = path.pos;
				endian_ = c.vault ? c.vault->raw.endian : Endian::Little;
				const FieldDef& f = fieldOf(c, path);
				const Collection* owner = nullptr;
				Attribute* a = const_cast<Attribute*>(c.resolve(f.key, &owner));
				Attribute zero;
				if (!a)
				{
					zero.key = f.key;
					zero.def = &f;
					zero.type = f.type;
					zero.isArray = f.isArray();
					zero.elemSize = f.type ? f.type->size : f.size;
					zero.count = 0;
					a = &zero;
				}
				const Place pl = locate(*a, path, names().format(f.key), false, path.steps.size());
				if (pl.kind == Place::Kind::Value)
				{
					if (current && pl.block && uint64_t(pl.off) + pl.vt.size <= pl.block->bytes.size())
						current->assign(pl.block->bytes.begin() + pl.off, pl.block->bytes.begin() + pl.off + pl.vt.size);
					return pl.vt;
				}
				if (pl.kind == Place::Kind::CurvePart && pl.index >= 0)
				{
					// One point of a curve's X, Y or Y2.
					ValueType point;
					point.kind = TypeKind::Float;
					point.size = 4;
					point.typeName = "float";
					return point;
				}
				ValueType vt = pl.vt;
				vt.typeName = "array of " + vt.typeName; // an array: Value kind can't be set from one word
				vt.kind = TypeKind::Unknown;
				vt.size = 0;
				return vt;
			}
			// BlockAt(): the block and offset of the value at `path` (own or
			// inherited), or null when the collection has no value there.
			const DataBlock* blockAt(Collection& c, const FieldPath& path, uint32_t& off)
			{
				pos_ = path.pos;
				endian_ = c.vault ? c.vault->raw.endian : Endian::Little;
				const FieldDef& f = fieldOf(c, path);
				Attribute* a = const_cast<Attribute*>(c.resolve(f.key));
				if (!a) return nullptr;
				const Place pl = locate(*a, path, names().format(f.key), false, path.steps.size());
				if (pl.kind != Place::Kind::Value || !pl.block) return nullptr;
				off = pl.off;
				return pl.block;
			}

		private:
			Database& db_;
			const Script& sc_;
			const ApplySettings& st_;
			ApplyReport rep_;
			std::map<std::string, ValuePtr> opts_;
			struct Ref
			{
				Pos pos;
				uint32_t cls, key;
				std::string text, field;
				std::string file; // set in an included script: file_ and at_ then
				Pos at;
			};
			std::vector<Ref> refs_; // references to collections that weren't there when set
			Pos pos_;
			// Running an included script: its path, and the include line in sc_ that
			// led to it ("" for sc_ itself).
			std::string file_;
			Pos at_;
			Endian endian_ = Endian::Little;
			bool softRange_ = false;     // a condition reading: an index past the end throws OutOfRange
			bool everyPlatform_ = false; // the script running names <game>_all
			bool warnedBytes_ = false;
		};

		ValuePtr OptionFromText(const OptionDef& o, const std::string& text, Pos p, const std::string& source)
		{
			auto bad = [&](const std::string& why) -> ScriptError
			{
				return ScriptError(p, "option " + o.id + ": " + why, source);
			};
			switch (o.type)
			{
				case OptionDef::Type::Bool:
				{
					const std::string t = Lower(text);
					if (t == "true" || t == "1" || t == "yes" || t == "on") return MakeWord("true");
					if (t == "false" || t == "0" || t == "no" || t == "off") return MakeWord("false");
					throw bad("expected true or false, not " + text);
				}
				case OptionDef::Type::Number:
				case OptionDef::Type::Integer:
				{
					Num n;
					if (!ParseNum(text, n)) throw bad("expected a number, not " + text);
					if (o.type == OptionDef::Type::Integer && !n.isInt && n.d != std::nearbyint(n.d)) throw bad("expected a whole number, not " + text);
					if (o.min && n.d < *o.min) throw bad(text + " is below the minimum " + std::to_string(*o.min));
					if (o.max && n.d > *o.max) throw bad(text + " is above the maximum " + std::to_string(*o.max));
					return MakeNumber(text);
				}
				case OptionDef::Type::Choice:
				{
					for (const std::string& c : o.choices)
						if (c == text) return MakeString(c);
					for (const std::string& c : o.choices)
						if (Lower(c) == Lower(text)) return MakeString(c);
					std::string list;
					for (const std::string& c : o.choices) list += (list.empty() ? "" : ", ") + c;
					throw bad("expected one of " + list + ", not " + text);
				}
				case OptionDef::Type::Text: return MakeString(text);
			}
			throw bad("unknown type");
		}

		// The script that declares `o`, `sc` or one it includes; `at` is then the
		// include line in `sc` that leads to it.
		const Script* Declaring(const Script& sc, const OptionDef* o, Pos* at)
		{
			std::function<const Script* (const std::vector<Statement>&)> walk = [&](const std::vector<Statement>& list) -> const Script*
			{
				for (const Statement& s : list)
				{
					if (s.kind == Statement::Kind::Option && &s.option == o) return &sc;
					if (s.kind == Statement::Kind::If)
					{
						if (const Script* r = walk(s.body)) return r;
						if (const Script* r = walk(s.elseBody)) return r;
					}
					if (s.kind == Statement::Kind::Include && s.included)
						if (const Script* r = Declaring(*s.included, o, nullptr))
						{
							if (at) *at = s.pos;
							return r;
						}
				}
				return nullptr;
			};
			return walk(sc.statements);
		}

	} // namespace

	std::map<std::string, ValuePtr> ResolveOptions(const Script& script, const ApplySettings& settings)
	{
		// A mistake in an included script's option names that file.
		auto located = [&](const OptionDef& o, const ScriptError& e)
		{
			Pos at;
			const Script* in = Declaring(script, &o, &at);
			return !in || in == &script ? e : ScriptError(at, e.message, in->source, e.pos);
		};
		auto read = [&](const OptionDef& o, const std::string& text, Pos p)
		{
			try
			{
				return OptionFromText(o, text, p, script.source);
			}
			catch (const ScriptError& e)
			{
				throw located(o, e);
			}
		};
		std::map<std::string, const OptionDef*> defs;
		for (const OptionDef* o : script.options())
		{
			if (defs.count(o->id)) throw located(*o, ScriptError(o->pos, "option " + o->id + " is declared twice", script.source));
			defs[o->id] = o;
		}
		for (const auto& [id, text] : settings.options)
			if (!defs.count(id)) throw ScriptError({}, "the script has no option " + id, script.source);
		std::map<std::string, ValuePtr> out;
		for (const auto& [id, o] : defs)
		{
			auto it = settings.options.find(id);
			if (it != settings.options.end())
			{
				out[id] = read(*o, it->second, o->pos);
			}
			else if (o->defaultValue)
			{
				const Value& d = *o->defaultValue;
				if (d.kind != Value::Kind::Number && d.kind != Value::Kind::Word && d.kind != Value::Kind::String)
					throw located(*o, ScriptError(d.pos, "option " + id + ": the default is a plain value", script.source));
				out[id] = read(*o, d.text, d.pos);
			}
			else
			{
				switch (o->type)
				{
					case OptionDef::Type::Bool: out[id] = MakeWord("false"); break;
					case OptionDef::Type::Number:
					case OptionDef::Type::Integer: out[id] = MakeNumber(o->min ? std::to_string(int64_t(*o->min)) : "0"); break;
					case OptionDef::Type::Choice: out[id] = MakeString(o->choices.front()); break;
					case OptionDef::Type::Text: out[id] = MakeString(""); break;
				}
			}
		}
		return out;
	}

	ClassDesign DesignFromStatement(const Database& db, const Statement& s)
	{
		auto keyOf = [](const std::string& text) -> uint32_t
		{
			if (text.size() >= 2 && text.front() == '"' && text.back() == '"') return StringHash32(text.substr(1, text.size() - 2));
			if (IsHex(text)) return uint32_t(std::strtoul(text.c_str() + 2, nullptr, 16));
			return StringHash32(text);
		};
		auto nameOf = [](const std::string& text) -> std::string
		{
			if (text.size() >= 2 && text.front() == '"' && text.back() == '"') return text.substr(1, text.size() - 2);
			return IsHex(text) ? std::string() : text;
		};
		auto ranged = [](const Statement& st, const std::string& key, int64_t v, int64_t most) -> int64_t
		{
			if (v > most) throw ScriptError(st.pos, key + " " + std::to_string(v) + " is more than " + std::to_string(most));
			return v;
		};
		ClassDesign d;
		d.name = nameOf(s.word);
		d.key = keyOf(s.word);
		for (const auto& [k, v] : s.settings)
		{
			if (k == "reserve") d.collectionReserve = uint32_t(ranged(s, k, v, UINT32_MAX));
			else if (k == "layoutsize") d.layoutSize = uint32_t(ranged(s, k, v, UINT32_MAX));
			else if (k == "staticsize") d.staticSize = uint32_t(ranged(s, k, v, UINT32_MAX));
			else if (k == "layoutcount") d.layoutCount = uint16_t(ranged(s, k, v, 0xFFFF));
			else if (k == "keyshift") d.layoutKeyShift = uint16_t(ranged(s, k, v, 0xFFFF));
		}
		for (const Statement& f : s.body)
		{
			if (f.kind != K::ClassField || f.word == "bytes") continue;
			FieldDesign fd;
			fd.name = nameOf(f.path.field);
			fd.key = keyOf(f.path.field);
			fd.storage = f.word == "layout" ? FieldStorage::Layout : f.word == "static" ? FieldStorage::Static : FieldStorage::Entry;
			fd.array = f.isArray;
			fd.count = uint16_t(ranged(f, "a count", f.count, 0xFFFF));
			fd.searchable = !f.unsearchable;
			for (const auto& [k, v] : f.settings)
			{
				if (k == "at") fd.offset = uint16_t(ranged(f, k, v, 0xFFFF));
				else if (k == "align") fd.alignment = uint8_t(ranged(f, k, v, 7));
				else if (k == "flags") fd.flags = uint8_t(ranged(f, k, v, 0xFF));
				else if (k == "max") fd.maxCount = uint16_t(ranged(f, k, v, 0xFFFF));
				else if (k == "size") fd.size = uint16_t(ranged(f, k, v, 0xFFFF));
			}
			const TypeInfo* t = FindTypeByName(db, f.text);
			if (!t && !fd.size)
				throw ScriptError(f.path.pos, "the type table has no type " + f.text + " (a field's type is one of the types the game's files list)");
			fd.typeHash = t ? t->hash : StringHash32(f.text);
			d.fields.push_back(std::move(fd));
		}
		return d;
	}

	ValueType TypeAt(Database& db, Collection& c, const FieldPath& path, Bytes* current)
	{
		static const Script none;
		static const ApplySettings defaults;
		Applier a(db, none, defaults);
		return a.typeAt(c, path, current);
	}

	const DataBlock* BlockAt(Database& db, Collection& c, const FieldPath& path, uint32_t* offset)
	{
		static const Script none;
		static const ApplySettings defaults;
		Applier a(db, none, defaults);
		uint32_t off = 0;
		const DataBlock* b = a.blockAt(c, path, off);
		if (offset) *offset = off;
		return b;
	}

	uint32_t NameKey(const std::string& written)
	{
		if (written.size() >= 2 && written.front() == '"' && written.back() == '"') return StringHash32(written.substr(1, written.size() - 2));
		if (IsHex(written)) return uint32_t(std::strtoul(written.c_str() + 2, nullptr, 16));
		return StringHash32(written);
	}

	std::vector<Collection*> TargetCollections(const Database& db, const Target& t)
	{
		std::vector<Collection*> out;
		const ClassDef* cls = db.findClass(NameKey(t.cls));
		if (!cls || t.root) return out;
		if (t.name.find_first_of("*?") == std::string::npos)
		{
			if (Collection* c = db.findCollection(cls->key, NameKey(t.name))) out.push_back(c);
			return out;
		}
		const std::string pat = Lower(t.name);
		for (Collection* c : cls->collections)
			if (Glob(pat, Lower(db.names().format(c->key)))) out.push_back(c);
		return out;
	}

	ApplyReport Apply(Database& db, const Script& script, const ApplySettings& settings)
	{
		if (HasUnreadIncludes(script))
		{
			Script read = script;
			try
			{
				LoadIncludes(read);
			}
			catch (const ScriptError& e)
			{
				ApplyReport r;
				r.messages.push_back(Message::Error(e));
				return r;
			}
			Applier a(db, read, settings);
			return a.run();
		}
		Applier a(db, script, settings);
		return a.run();
	}

	std::string ApplyReport::summary() const
	{
		std::string s;
		auto add = [&](size_t n, const char* one, const char* many)
		{
			if (!n) return;
			s += (s.empty() ? "" : ", ") + std::to_string(n) + " " + (n == 1 ? one : many);
		};
		add(valuesSet, "value set", "values set");
		add(reverted, "value reverted", "values reverted");
		add(created, "collection created", "collections created");
		add(renamed, "collection renamed", "collections renamed");
		add(moved, "collection moved", "collections moved");
		add(deleted, "collection deleted", "collections deleted");
		add(classesCreated, "class created", "classes created");
		add(classesChanged, "class changed", "classes changed");
		add(classesDeleted, "class deleted", "classes deleted");
		return s.empty() ? "no changes" : s;
	}

	size_t ApplyReport::warnings() const
	{
		size_t n = 0;
		for (const Message& m : messages) n += m.level == Message::Level::Warning;
		return n;
	}

	size_t ApplyReport::errors() const
	{
		size_t n = 0;
		for (const Message& m : messages) n += m.level == Message::Level::Error;
		return n;
	}

	std::unique_ptr<Database> CloneDatabase(const Database& db)
	{
		auto out = std::make_unique<Database>();
		out->setTarget(db.target());
		if (db.sharedTypeSchema()) out->addTypeSchema(db.sharedTypeSchema());
		for (const auto& l : db.classLists()) out->addClassList(l);
		out->names() = db.names();
		out->binNames() = db.binNames();
		for (const auto& pk : db.packs())
		{
			const Bytes bytes = WritePack(db, *pk);
			LoadedPack& np = out->loadPack(pk->path, bytes);
			np.compression = pk->compression;
			np.dirty = pk->dirty;
		}
		return out;
	}

} // namespace attrib::script
