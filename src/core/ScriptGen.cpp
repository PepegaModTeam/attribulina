// Mod scripts: making them from a database (Script.h, doc/scripts.md). The
// values are written so that applying them gives back the same bytes:
// floats in the fewest digits that read back to the same float, Lua handlers
// as source only when compiling it reproduces the stored blob.

#include "Script.h"

#include "Compression.h"
#include "Curve.h"
#include "Gameplay.h"
#include "TypeLayouts.h"
#include "Hash.h"
#include "lua/Lua.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace attrib::script
{

	namespace
	{

		using K = Statement::Kind;

		std::string Hex32(uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "0x%08X", v);
			return b;
		}

		std::string FloatText(float f)
		{
			return NumberText(double(f), true);
		}
		std::string DoubleText(double d)
		{
			return NumberText(d);
		}

		bool LooksLikeNumber(const std::string& s)
		{
			if (s.empty()) return false;
			char* end = nullptr;
			std::strtod(s.c_str(), &end);
			return (end && !*end) || (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
		}

		// A key's name as a value: a bare word when it reads back as the same name.
		ValuePtr NameValue(const std::string& name)
		{
			if (IsBareWord(name) && !LooksLikeNumber(name)) return MakeWord(name);
			return MakeString(name);
		}

		ValuePtr Tuple(std::vector<ValuePtr> items)
		{
			return MakeList(std::move(items), Value::Kind::Tuple);
		}

		ValuePtr Binary(char op, ValuePtr a, ValuePtr b)
		{
			auto v = std::make_shared<Value>();
			v->kind = Value::Kind::Binary;
			v->op = op;
			v->items = { std::move(a), std::move(b) };
			return v;
		}

		ValuePtr BytesValue(const uint8_t* p, size_t n)
		{
			auto v = std::make_shared<Value>();
			v->kind = Value::Kind::Bytes;
			v->text = HexBytes(p, n);
			return v;
		}

		bool IsCountMember(const StructDef& st, const MemberDef& m)
		{
			for (const MemberDef& o : st.members)
				if (o.prim == MemberPrim::Ptr && o.ptrCount == m.name && o.ptrTarget != "void") return true;
			return false;
		}

		bool SameBlockDeep(const DataBlock& a, const DataBlock& b, int depth = 0)
		{
			if (a.bytes != b.bytes || a.ptrs.size() != b.ptrs.size()) return false;
			if (depth > 32) return true;
			for (const PtrSlot& p : a.ptrs)
			{
				const PtrSlot* q = b.ptrAt(p.offset);
				if (!q || q->isString != p.isString || q->text != p.text || bool(q->data) != bool(p.data)) return false;
				if (p.data && !SameBlockDeep(*p.data, *q->data, depth + 1)) return false;
			}
			return true;
		}

		bool SameRegion(const DataBlock& a, uint32_t aoff, const DataBlock& b, uint32_t boff, uint32_t size)
		{
			if (uint64_t(aoff) + size > a.bytes.size() || uint64_t(boff) + size > b.bytes.size()) return false;
			if (std::memcmp(a.bytes.data() + aoff, b.bytes.data() + boff, size) != 0) return false;
			auto inside = [](const DataBlock& x, uint32_t off, uint32_t size)
			{
				std::vector<const PtrSlot*> r;
				for (const PtrSlot& p : x.ptrs)
					if (p.offset >= off && p.offset < off + size) r.push_back(&p);
				return r;
			};
			const auto pa = inside(a, aoff, size), pb = inside(b, boff, size);
			if (pa.size() != pb.size()) return false;
			for (const PtrSlot* p : pa)
			{
				const PtrSlot* q = b.ptrAt(p->offset - aoff + boff);
				if (!q || q->isString != p->isString || q->text != p->text || bool(q->data) != bool(p->data)) return false;
				if (p->data && !SameBlockDeep(*p->data, *q->data)) return false;
			}
			return true;
		}

		bool SameAttribute(const Attribute& a, const Attribute& b)
		{
			return a.count == b.count && a.isArray == b.isArray && a.elemSize == b.elemSize && SameBlockDeep(a.data, b.data);
		}

		class Gen
		{
		public:
			explicit Gen(const Database& db) : db_(db)
			{
			}

			// The whole value at `off` of `b`.
			ValuePtr value(const DataBlock& b, uint32_t off, const ValueType& vt, Endian e, int depth = 0)
			{
				if (uint64_t(off) + std::max<uint32_t>(vt.size, 1) > b.bytes.size()) return MakeWord("null");
				const uint8_t* p = b.bytes.data() + off;
				const KeyNames& names = db_.names();
				switch (vt.kind)
				{
					case TypeKind::Bool: return MakeWord(ReadInteger(b, off, vt, e) ? "true" : "false");
					case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16: case TypeKind::Int32:
					case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::Char: return MakeNumber(std::to_string(ReadInteger(b, off, vt, e)));
					case TypeKind::UInt64: return MakeNumber(std::to_string(uint64_t(ReadInteger(b, off, vt, e))));
					case TypeKind::Float:
					{
						const float f = loadf32(p, e);
						if (std::isnan(f)) return BytesValue(p, 4);
						return MakeNumber(FloatText(f));
					}
					case TypeKind::Double:
					{
						const uint64_t u = load64(p, e);
						double d;
						std::memcpy(&d, &u, 8);
						if (std::isnan(d)) return BytesValue(p, 8);
						return MakeNumber(DoubleText(d));
					}
					case TypeKind::Enum:
					{
						const int64_t v = ReadInteger(b, off, vt, e);
						if (vt.en)
							if (const std::string* n = vt.en->nameOf(v)) return MakeWord(*n);
						return MakeNumber(std::to_string(v));
					}
					case TypeKind::Text:
					{
						const PtrSlot* s = b.ptrAt(off);
						return s && s->isString ? MakeString(s->text) : MakeWord("null");
					}
					case TypeKind::StringKey:
					{
						const StringKeyLayout sk = StringKeyAt(vt.size);
						const uint32_t h = load32(p + sk.hash32, e);
						const uint64_t h64 = sk.hash64 ? load64(p, e) : 0;
						const PtrSlot* s = b.ptrAt(off + sk.text);
						if (!s && !h && !h64) return MakeWord("null");
						const std::string text = s ? s->text : std::string();
						const bool own = h == (text.empty() ? 0u : StringHash32(text)) && (!sk.hash64 || h64 == StringHash64(text));
						if (own) return MakeString(text);
						if (!sk.hash64) return Tuple({ MakeString(text), MakeNumber(Hex32(h)) });
						char buf[24];
						std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(h64));
						return Tuple({ MakeString(text), MakeNumber(Hex32(h)), MakeNumber(buf) });
					}
					case TypeKind::RefSpec:
					{
						const RefSpecLayout rl = RefSpecAt(vt.size);
						const uint32_t ck = LoadKey(db_, p + rl.cls, rl.keySize, e), k = LoadKey(db_, p + rl.col, rl.keySize, e);
						if (!ck && !k) return MakeWord("null");
						// A path name (gameplay) as bare segments when it can be.
						if (const std::string* n = names.find(k); n && n->find('/') != std::string::npos)
						{
							ValuePtr chain = key(names, ck);
							bool bare = true;
							size_t b = 0;
							for (;;)
							{
								const size_t e = n->find('/', b);
								const std::string seg = n->substr(b, e == std::string::npos ? std::string::npos : e - b);
								if (seg.empty() || !IsBareWord(seg)) bare = false;
								if (!bare) break;
								chain = Binary('/', chain, LooksLikeNumber(seg) ? MakeNumber(seg) : MakeWord(seg));
								if (e == std::string::npos) break;
								b = e + 1;
							}
							if (bare) return chain;
						}
						return Binary('/', key(names, ck), key(names, k));
					}
					case TypeKind::Key:
					case TypeKind::BinKey:
					case TypeKind::ClassRefSpec:
					{ // its collection, the class being the type's
						const uint32_t k = vt.kind == TypeKind::Key ? LoadKey(db_, p, vt.size, e) : load32(p, e);
						if (!k) return MakeNumber("0");
						return key(vt.kind == TypeKind::BinKey ? db_.binNames() : names, k);
					}
					case TypeKind::Colour:
					{
						std::vector<ValuePtr> items;
						for (int i = 0; i < 4; ++i) items.push_back(MakeNumber(std::to_string(p[i])));
						return Tuple(std::move(items));
					}
					case TypeKind::FloatColour: case TypeKind::Vector2: case TypeKind::Vector3: case TypeKind::Vector4:
					case TypeKind::Quaternion:
					{
						const uint32_t n = vt.kind == TypeKind::Vector2 ? 2 : vt.kind == TypeKind::Vector3 ? 3 : 4;
						return floats(p, n, e);
					}
					case TypeKind::Matrix:
					{
						std::vector<ValuePtr> rows;
						for (int r = 0; r < 4; ++r) rows.push_back(floats(p + r * 16, 4, e));
						return Tuple(std::move(rows));
					}
					case TypeKind::Blob: return blob(b, off, e);
					case TypeKind::Curve: return curve(b, off, e, vt.size);
					case TypeKind::Struct:
						if (ExpandsIntoMembers(vt) && depth < 32) return structValue(b, off, vt, e, depth);
						return BytesValue(p, vt.size);
					case TypeKind::Unknown:
						if (vt.size == 4 && !b.ptrAt(off) && vt.hint == "hex") return MakeNumber(Hex32(load32(p, e)));
						if (vt.size == 4 && !b.ptrAt(off)) return MakeNumber(std::to_string(load32(p, e)));
						return BytesValue(p, vt.size);
				}
				return BytesValue(p, vt.size);
			}

			// An attribute's whole value: a list for an array.
			ValuePtr attribute(const Attribute& a, const std::string& fieldName, Endian e)
			{
				ValueType vt = TypeOfAttribute(a, fieldName);
				if (!a.isArray) return value(a.data, 0, vt, e);
				std::vector<ValuePtr> items;
				for (uint32_t i = 0; i < a.count; ++i) items.push_back(value(a.data, i * a.elemSize, vt, e));
				return MakeList(std::move(items));
			}

			// Sets for what differs between two values of the same type.
			void diff(std::vector<Statement>& out, const FieldPath& path, const DataBlock& ob, uint32_t ooff, const DataBlock& nb,
					  uint32_t noff, const ValueType& vt, Endian e, bool was, int depth = 0)
			{
				const uint32_t size = std::max<uint32_t>(vt.size, 1);
				if (SameRegion(ob, ooff, nb, noff, size)) return;
				if (ExpandsIntoMembers(vt) && depth < 32 && uint64_t(ooff) + size <= ob.bytes.size() &&
					uint64_t(noff) + size <= nb.bytes.size())
				{
					for (const MemberDef& m : vt.st->members)
					{
						if (IsCountMember(*vt.st, m)) continue;
						FieldPath sub = path;
						sub.steps.push_back({ m.name, -1 });
						const ValueType mt = TypeOfMember(m);
						if (m.prim == MemberPrim::Ptr)
						{
							if (m.ptrTarget.empty() || m.ptrTarget == "void") continue;
							const ValueType et = TypeOfNamed(db_, m.ptrTarget);
							if (!et.size) continue;
							const MemberDef* cm = m.ptrCount.empty() ? nullptr : vt.st->member(m.ptrCount);
							const PtrSlot* os = ob.ptrAt(ooff + m.offset);
							const PtrSlot* ns = nb.ptrAt(noff + m.offset);
							const uint32_t on = count(ob, ooff, os, cm, et, e), nn = count(nb, noff, ns, cm, et, e);
							if (!cm)
							{
								if (on && nn) diff(out, sub, *os->data, 0, *ns->data, 0, et, e, was, depth + 1);
								else if (on != nn) set(out, sub, nn ? value(*ns->data, 0, et, e) : MakeWord("null"), nullptr);
								continue;
							}
							if (on != nn)
							{
								set(out, sub, pointerList(nb, noff, m, cm, et, e), nullptr);
								continue;
							}
							for (uint32_t i = 0; i < nn; ++i)
							{
								FieldPath el = sub;
								el.steps.push_back({ std::string(), int64_t(i) });
								diff(out, el, *os->data, i * et.size, *ns->data, i * et.size, et, e, was, depth + 1);
							}
						}
						else if (m.count > 1 && mt.kind == TypeKind::Float && (m.count == 3 || m.count == 4))
						{
							ValueType v = mt;
							v.kind = m.count == 4 ? TypeKind::Vector4 : TypeKind::Vector3;
							v.size = 4 * m.count;
							diff(out, sub, ob, ooff + m.offset, nb, noff + m.offset, v, e, was, depth + 1);
						}
						else if (m.count > 1)
						{
							for (uint32_t i = 0; i < m.count; ++i)
							{
								FieldPath el = sub;
								el.steps.push_back({ std::string(), int64_t(i) });
								diff(out, el, ob, ooff + m.offset + i * m.elemSize, nb, noff + m.offset + i * m.elemSize, mt, e, was, depth + 1);
							}
						}
						else
						{
							diff(out, sub, ob, ooff + m.offset, nb, noff + m.offset, mt, e, was, depth + 1);
						}
					}
					return;
				}
				// A leaf (or a curve, a blob: whole). `was` only on small values.
				ValuePtr now = value(nb, noff, vt, e);
				ValuePtr old;
				if (was && vt.kind != TypeKind::Curve && vt.kind != TypeKind::Blob && vt.kind != TypeKind::Matrix) old = value(ob, ooff, vt, e);
				set(out, path, now, old);
			}

			void diffAttribute(std::vector<Statement>& out, const Attribute& o, const Attribute& n, const std::string& fieldName, Endian e,
							   bool was)
			{
				if (SameAttribute(o, n)) return;
				FieldPath path;
				path.field = fieldName;
				const ValueType vt = TypeOfAttribute(n, fieldName);
				if (!n.isArray)
				{
					diff(out, path, o.data, 0, n.data, 0, vt, e, was);
					return;
				}
				if (o.count != n.count || o.elemSize != n.elemSize || !o.isArray)
				{
					set(out, path, attribute(n, fieldName, e), nullptr);
					return;
				}
				for (uint32_t i = 0; i < n.count; ++i)
				{
					FieldPath el = path;
					el.steps.push_back({ std::string(), int64_t(i) });
					diff(out, el, o.data, i * o.elemSize, n.data, i * n.elemSize, vt, e, was);
				}
			}

			static void set(std::vector<Statement>& out, const FieldPath& path, ValuePtr v, ValuePtr was)
			{
				Statement s;
				s.kind = K::Set;
				s.path = path;
				s.value = std::move(v);
				s.was = std::move(was);
				out.push_back(std::move(s));
			}

			std::string name(uint32_t k) const
			{
				return db_.names().format(k);
			}

		private:
			ValuePtr key(const KeyNames& names, uint32_t k)
			{
				if (const std::string* n = names.find(k)) return NameValue(*n);
				return MakeNumber(Hex32(k));
			}

			ValuePtr floats(const uint8_t* p, uint32_t n, Endian e)
			{
				std::vector<ValuePtr> items;
				for (uint32_t i = 0; i < n; ++i)
				{
					const float f = loadf32(p + i * 4, e);
					items.push_back(MakeNumber(std::isnan(f) ? "nan" : FloatText(f)));
				}
				return Tuple(std::move(items));
			}

			uint32_t count(const DataBlock& b, uint32_t off, const PtrSlot* s, const MemberDef* cm, const ValueType& et, Endian e)
			{
				if (!s || !s->data) return 0;
				if (!cm) return 1;
				const uint32_t n = uint32_t(ReadInteger(b, off + cm->offset, TypeOfMember(*cm), e));
				return std::min<uint32_t>(n, uint32_t(s->data->bytes.size() / et.size));
			}

			ValuePtr pointerList(const DataBlock& b, uint32_t off, const MemberDef& m, const MemberDef* cm, const ValueType& et, Endian e,
								 int depth = 0)
			{
				const PtrSlot* s = b.ptrAt(off + m.offset);
				const uint32_t n = count(b, off, s, cm, et, e);
				std::vector<ValuePtr> items;
				for (uint32_t i = 0; i < n; ++i) items.push_back(value(*s->data, i * et.size, et, e, depth + 1));
				return MakeList(std::move(items));
			}

			ValuePtr structValue(const DataBlock& b, uint32_t off, const ValueType& vt, Endian e, int depth)
			{
				auto v = std::make_shared<Value>();
				v->kind = Value::Kind::Struct;
				for (const MemberDef& m : vt.st->members)
				{
					if (IsCountMember(*vt.st, m)) continue;
					const ValueType mt = TypeOfMember(m);
					ValuePtr mv;
					if (m.prim == MemberPrim::Ptr)
					{
						if (m.ptrTarget.empty() || m.ptrTarget == "void") continue;
						const ValueType et = TypeOfNamed(db_, m.ptrTarget);
						if (!et.size) continue;
						const MemberDef* cm = m.ptrCount.empty() ? nullptr : vt.st->member(m.ptrCount);
						if (cm)
						{
							mv = pointerList(b, off, m, cm, et, e, depth);
						}
						else
						{
							const PtrSlot* s = b.ptrAt(off + m.offset);
							mv = s && s->data ? value(*s->data, 0, et, e, depth + 1) : MakeWord("null");
						}
					}
					else if (m.count > 1 && mt.kind == TypeKind::Float && (m.count == 3 || m.count == 4))
					{
						mv = floats(b.bytes.data() + off + m.offset, m.count, e);
					}
					else if (m.count > 1)
					{
						std::vector<ValuePtr> items;
						for (uint32_t i = 0; i < m.count; ++i) items.push_back(value(b, off + m.offset + i * m.elemSize, mt, e, depth + 1));
						mv = MakeList(std::move(items));
					}
					else
					{
						mv = value(b, off + m.offset, mt, e, depth + 1);
					}
					v->members.emplace_back(m.name, mv);
				}
				return v;
			}

			ValuePtr curve(const DataBlock& b, uint32_t off, Endian e, uint32_t size)
			{
				const CurveValue c = ReadCurve(b, off, e, size);
				auto v = std::make_shared<Value>();
				v->kind = Value::Kind::Struct;
				v->members.emplace_back("xMin", MakeNumber(FloatText(c.xMin)));
				v->members.emplace_back("xMax", MakeNumber(FloatText(c.xMax)));
				v->members.emplace_back("yMin", MakeNumber(FloatText(c.yMin)));
				v->members.emplace_back("yMax", MakeNumber(FloatText(c.yMax)));
				if (CurveAt(size).graphScale) v->members.emplace_back("graphScale", MakeNumber(FloatText(c.graphScale)));
				auto list = [&](const std::vector<float>& f)
				{
					std::vector<ValuePtr> items;
					for (float x : f) items.push_back(MakeNumber(FloatText(x)));
					return MakeList(std::move(items));
				};
				v->members.emplace_back("X", list(c.x));
				v->members.emplace_back("Y", list(c.y));
				// Applying a curve recomputes Y2; stored data that differs keeps its own.
				if (c.y2 != NaturalY2(c.x, c.y)) v->members.emplace_back("Y2", list(c.y2));
				return v;
			}

			ValuePtr blob(const DataBlock& b, uint32_t off, Endian e)
			{
				const uint32_t size = load32(b.bytes.data() + off, e);
				const PtrSlot* s = b.ptrAt(off + 4);
				if (!s || !s->data || !size) return MakeWord("null");
				// All of the data: retail Carbon's mSize can be short of it (TypeLayouts.h).
				const Bytes& data = s->data->bytes;
				// A compressed Lua handler: its source, when that compiles back to
				// exactly these bytes.
				try
				{
					if (ParseLzHeader(data))
					{
						const Bytes chunk = Decompress(data);
						if (const auto version = lua::DetectVersion(chunk))
						{
							const lua::DecompileResult d = lua::Decompile(chunk);
							if (d.exact)
							{
								const lua::CompileResult r = lua::Compile(d.source, *version);
								if (r.ok() && CompressBest(r.chunk) == data)
								{
									auto v = std::make_shared<Value>();
									v->kind = Value::Kind::Lua;
									v->text = d.source;
									return v;
								}
							}
						}
					}
				}
				catch (const std::exception&)
				{
				}
				return BytesValue(data.data(), data.size());
			}

			const Database& db_;
		};

		std::string FileName(const std::string& path)
		{
			const size_t slash = path.find_last_of("/\\");
			return slash == std::string::npos ? path : path.substr(slash + 1);
		}

		std::string Lower(std::string s)
		{
			for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		Target TargetOf(const Database& db, uint32_t cls, uint32_t key)
		{
			Target t;
			t.cls = db.names().format(cls);
			t.name = db.names().format(key);
			// A real name that looks like a hash stays a name.
			if (db.names().find(key) && t.name.size() > 2 && t.name[0] == '0' && (t.name[1] == 'x' || t.name[1] == 'X'))
				t.name = "\"" + t.name + "\"";
			return t;
		}

		Target TargetOf(const Database& db, const Collection& c)
		{
			return TargetOf(db, c.classKey, c.key);
		}

		uint64_t Id(uint32_t cls, uint32_t key)
		{
			return (uint64_t(cls) << 32) | key;
		}

		// Parents before children, roots in class order.
		std::vector<const Collection*> ParentsFirst(const std::vector<const Collection*>& list)
		{
			std::unordered_set<const Collection*> in(list.begin(), list.end()), done;
			std::vector<const Collection*> out;
			std::function<void(const Collection*)> add = [&](const Collection* c)
			{
				if (done.count(c)) return;
				if (c->parent && in.count(c->parent)) add(c->parent);
				done.insert(c);
				out.push_back(c);
			};
			for (const Collection* c : list) add(c);
			return out;
		}

		// A value whose field its class doesn't define (World's coop_pursuit_1 event):
		// the reader keeps it as read, and a script can't name it.
		Statement UndefinedField(const std::string& field)
		{
			Statement s;
			s.kind = K::Comment;
			s.comment = " " + field + ": its class doesn't define this field; the value is kept as it is";
			return s;
		}

		Statement CollectionStatement(const Database& db, Gen& gen, const Collection& c, const ExportSettings& st)
		{
			Statement s;
			s.kind = st.editOnly ? K::Edit : K::Collection;
			s.target = TargetOf(db, c);
			if (!st.editOnly)
			{
				s.hasUnder = true;
				if (c.parent) s.under = TargetOf(db, *c.parent);
				else if (c.parentKey) s.under = TargetOf(db, c.classKey, c.parentKey);
				else s.under.root = true;
				if (c.vault) s.vault = c.vault->name;
				s.exact = st.exact && st.fields.empty();
			}
			const Endian e = c.vault ? c.vault->raw.endian : Endian::Little;
			for (const Attribute& a : c.attributes)
			{
				if (!st.fields.empty() && !st.fields.count(a.key)) continue;
				if (!a.def)
				{
					s.body.push_back(UndefinedField(gen.name(a.key)));
					continue;
				}
				FieldPath p;
				p.field = gen.name(a.key);
				Gen::set(s.body, p, gen.attribute(a, p.field, e), nullptr);
			}
			return s;
		}

		// An `edit` of `c` with what differs from `o`.
		Statement EditOf(const Database& cur, Gen& gen, const Collection& o, const Collection& c, bool was)
		{
			const KeyNames& names = cur.names();
			Statement ed;
			ed.kind = K::Edit;
			ed.target = TargetOf(cur, c);
			const Endian e = c.vault ? c.vault->raw.endian : Endian::Little;
			for (const Attribute& a : c.attributes)
			{
				const std::string fieldName = names.format(a.key);
				if (!a.def) continue; // kept as read (UndefinedField)
				const Attribute* b = o.find(a.key);
				// A field whose class changed its type is set whole.
				if (b && !(b->type && a.type && b->type->hash == a.type->hash && b->isArray == a.isArray && b->elemSize == a.elemSize)) b = nullptr;
				if (b)
				{
					gen.diffAttribute(ed.body, *b, a, fieldName, e, was);
				}
				else
				{
					FieldPath p;
					p.field = fieldName;
					Gen::set(ed.body, p, gen.attribute(a, fieldName, e), nullptr);
				}
			}
			for (const Attribute& b : o.attributes)
				if (!b.inLayout && b.def && !c.find(b.key) && c.cls && c.cls->field(b.key))
				{
					Statement r;
					r.kind = K::Revert;
					r.path.field = names.format(b.key);
					ed.body.push_back(std::move(r));
				}
			return ed;
		}

	} // namespace

	Statement DiffValues(const Database& base, const Collection& before, const Database& cur, const Collection& now, bool recordWas)
	{
		(void)base;
		Gen gen(cur);
		return EditOf(cur, gen, before, now, recordWas);
	}

	Script NewScript(const Database& db, std::vector<Statement> statements)
	{
		Script sc;
		Statement h;
		h.kind = K::Header;
		h.number = kVersion;
		sc.statements.push_back(h);
		const std::string game = GameId(db);
		if (!game.empty()) sc.setMeta("game", game);
		for (Statement& s : statements) sc.statements.push_back(std::move(s));
		return sc;
	}

	Statement CreateVaultStatement(const Vault& vault)
	{
		Statement s;
		s.kind = K::CreateVault;
		s.vault = vault.name;
		if (vault.pack) s.word = FileName(vault.pack->path);
		return s;
	}

	std::vector<Statement> Export(const Database& db, const std::vector<const Collection*>& collections, const ExportSettings& settings)
	{
		Gen gen(db);
		std::vector<Statement> out;
		const std::vector<const Collection*> list = ParentsFirst(collections);
		if (settings.vaults && !settings.editOnly)
		{
			std::vector<const Vault*> vaults;
			for (const Collection* c : list)
				if (c->vault && c->vault->pack && !c->vault->isDatabase && std::find(vaults.begin(), vaults.end(), c->vault) == vaults.end())
					vaults.push_back(c->vault);
			for (const Vault* v : vaults) out.push_back(CreateVaultStatement(*v));
		}
		for (const Collection* c : list)
		{
			Statement s = CollectionStatement(db, gen, *c, settings);
			if (settings.editOnly && s.body.empty()) continue;
			out.push_back(std::move(s));
		}
		return out;
	}

	std::vector<Statement> ExportGameplay(const Database& db, const GameplayModel& m, const std::vector<const Collection*>& elements)
	{
		ExportSettings st;
		st.vaults = true;
		std::vector<Statement> out = Export(db, elements, st);
		// The game finds an element through its owner's Children: an owner left
		// out keeps its whole list (as a diff writes a list that grew).
		const std::unordered_set<const Collection*> in(elements.begin(), elements.end());
		std::vector<const Collection*> owners;
		for (const Collection* c : elements)
		{
			const Collection* o = m.owner(c);
			if (!o || in.count(o) || std::find(owners.begin(), owners.end(), o) != owners.end()) continue;
			const std::vector<Collection*> listed = m.listedChildren(o);
			if (std::find(listed.begin(), listed.end(), c) != listed.end()) owners.push_back(o);
		}
		for (const Collection* o : owners)
		{
			Statement s = ExportValue(db, *o, StringHash32(gameplay::kChildren));
			if (!s.body.empty()) out.push_back(std::move(s));
		}
		return out;
	}

	Statement ExportValue(const Database& db, const Collection& c, uint32_t field)
	{
		Gen gen(db);
		Statement s;
		s.kind = K::Edit;
		s.target = TargetOf(db, c);
		const Collection* owner = nullptr;
		if (const Attribute* a = c.resolve(field, &owner))
		{
			const Endian e = owner && owner->vault ? owner->vault->raw.endian : Endian::Little;
			FieldPath p;
			p.field = gen.name(field);
			Gen::set(s.body, p, gen.attribute(*a, p.field, e), nullptr);
		}
		return s;
	}

	namespace
	{
		// A key as scripts write a name: the name (quoted when it looks like a
		// hash), or 0x... when it has none.
		std::string NameText(const Database& db, uint32_t key)
		{
			const std::string* n = db.names().find(key);
			if (!n) return Hex32(key);
			// Only a name that reads as a 32-bit hash needs them ("0x" and 16 hex
			// digits, a 64-bit key's name, is a name as it is).
			bool hash = n->size() > 2 && n->size() <= 10 && (*n)[0] == '0' && ((*n)[1] == 'x' || (*n)[1] == 'X');
			for (size_t i = 2; hash && i < n->size(); ++i) hash = std::isxdigit(static_cast<unsigned char>((*n)[i])) != 0;
			return hash ? "\"" + *n + "\"" : *n;
		}

		bool SameClass(const ClassDef& a, const ClassDef& b)
		{
			if (a.fields.size() != b.fields.size() || a.layoutSize != b.layoutSize || a.layoutCount != b.layoutCount ||
				a.layoutKeyShift != b.layoutKeyShift || a.staticSize != b.staticSize || a.collectionReserve != b.collectionReserve)
				return false;
			for (size_t i = 0; i < a.fields.size(); ++i)
			{
				const FieldDef& x = a.fields[i];
				const FieldDef& y = b.fields[i];
				if (x.key != y.key || x.typeHash != y.typeHash || x.offset != y.offset || x.size != y.size || x.maxCount != y.maxCount ||
					x.flags != y.flags || x.alignment != y.alignment)
					return false;
			}
			return SameBlockDeep(a.staticData, b.staticData);
		}
	} // namespace

	Statement ClassStatement(const Database& db, const ClassDef& cls, bool values)
	{
		const ClassDesign d = DesignOf(db, cls);
		Gen gen(db);
		Statement s;
		s.kind = K::Class;
		s.word = NameText(db, cls.key);
		if (cls.vault) s.vault = cls.vault->name;
		if (d.collectionReserve) s.settings.emplace_back("reserve", *d.collectionReserve);
		if (d.layoutSize) s.settings.emplace_back("layoutsize", *d.layoutSize);
		if (d.layoutCount) s.settings.emplace_back("layoutcount", *d.layoutCount);
		if (d.layoutKeyShift) s.settings.emplace_back("keyshift", *d.layoutKeyShift);
		if (d.staticSize) s.settings.emplace_back("staticsize", *d.staticSize);
		const Endian e = cls.vault ? cls.vault->raw.endian : Endian::Little;
		for (const FieldDesign& fd : d.fields)
		{
			Statement f;
			f.kind = K::ClassField;
			f.word = FieldStorageName(fd.storage);
			f.path.field = NameText(db, fd.key);
			const TypeInfo* t = db.typeByHash(fd.typeHash);
			f.text = t ? t->name : Hex32(fd.typeHash);
			f.isArray = fd.array;
			f.count = fd.count;
			f.unsearchable = !fd.searchable;
			if (fd.offset) f.settings.emplace_back("at", *fd.offset);
			if (fd.alignment) f.settings.emplace_back("align", *fd.alignment);
			if (fd.flags) f.settings.emplace_back("flags", *fd.flags);
			if (fd.maxCount) f.settings.emplace_back("max", *fd.maxCount);
			if (fd.size) f.settings.emplace_back("size", *fd.size);
			const FieldDef* def = cls.field(fd.key);
			// Index tables follow the class's collections (UpdateIndexTables).
			if (values && def && def->isStatic() && !(t && t->name == kIndexTableType) && uint32_t(def->offset) + def->size <= cls.staticData.bytes.size())
				f.value = gen.value(cls.staticData, def->offset, TypeOfType(def->type, def->size, gen.name(def->key)), e);
			s.body.push_back(std::move(f));
		}
		// Static bytes no static field covers (World's gameplay keeps 8 nonzero ones).
		if (values && cls.staticSize)
		{
			Bytes block = cls.staticData.bytes;
			block.resize(cls.staticSize, 0);
			bool loose = false;
			for (uint32_t i = 0; i < block.size() && !loose; ++i)
			{
				if (!block[i]) continue;
				bool covered = false;
				for (const FieldDef& f : cls.fields)
					covered |= f.isStatic() && i >= f.offset && i < uint32_t(f.offset) + f.size;
				loose = !covered;
			}
			if (loose)
			{
				Statement b;
				b.kind = K::ClassField;
				b.word = "bytes";
				b.value = BytesValue(block.data(), block.size());
				s.body.push_back(std::move(b));
			}
		}
		return s;
	}

	Script Diff(const Database& base, const Database& cur, const DiffSettings& settings)
	{
		Gen gen(cur);
		const KeyNames& names = cur.names();
		auto wanted = [&](uint32_t cls)
		{
			return settings.classes.empty() || settings.classes.count(cls);
		};

		// Only packs open on both sides are compared: a pack the other side
		// didn't load isn't deleted or new.
		std::unordered_set<std::string> oldFiles, newFiles;
		for (const auto& pk : base.packs()) oldFiles.insert(Lower(FileName(pk->path)));
		for (const auto& pk : cur.packs()) newFiles.insert(Lower(FileName(pk->path)));
		// World's event vaults are files of their own. When both sides loaded the
		// gc.vaults folder (each has one), a file only one side has was made or
		// deleted, with its vault and collections.
		auto hasEventFiles = [](const Database& d)
		{
			return std::any_of(d.packs().begin(), d.packs().end(), [](const auto& pk)
			{
				return pk->format == PackFormat::VaultFile;
			});
		};
		std::vector<const LoadedPack*> newEventFiles, goneEventFiles;
		if (hasEventFiles(base) && hasEventFiles(cur))
		{
			for (const auto& pk : cur.packs())
				if (pk->format == PackFormat::VaultFile && !oldFiles.count(Lower(FileName(pk->path)))) newEventFiles.push_back(pk.get());
			for (const auto& pk : base.packs())
				if (pk->format == PackFormat::VaultFile && !newFiles.count(Lower(FileName(pk->path)))) goneEventFiles.push_back(pk.get());
			for (const LoadedPack* pk : newEventFiles) oldFiles.insert(Lower(FileName(pk->path)));
			for (const LoadedPack* pk : goneEventFiles) newFiles.insert(Lower(FileName(pk->path)));
		}
		auto inPack = [](const Collection* c, const std::unordered_set<std::string>& files)
		{
			return c->vault && c->vault->pack && files.count(Lower(FileName(c->vault->pack->path)));
		};
		std::unordered_map<uint64_t, const Collection*> oldById, newById;
		std::vector<const Collection*> oldAll, newAll;
		for (const auto& cls : base.classes())
			if (wanted(cls->key))
				for (const Collection* c : cls->collections)
					if (inPack(c, newFiles))
					{
						oldById[Id(c->classKey, c->key)] = c;
						oldAll.push_back(c);
					}
		for (const auto& cls : cur.classes())
			if (wanted(cls->key))
				for (const Collection* c : cls->collections)
					if (inPack(c, oldFiles))
					{
						newById[Id(c->classKey, c->key)] = c;
						newAll.push_back(c);
					}

		std::vector<const Collection*> created, deleted;
		for (const Collection* c : newAll)
			if (!oldById.count(Id(c->classKey, c->key))) created.push_back(c);
		// A deleted class's collections go with it (`delete class`).
		for (const Collection* c : oldAll)
			if (!newById.count(Id(c->classKey, c->key)) && cur.findClass(c->classKey)) deleted.push_back(c);

		// A deleted and a created collection with the same values, vault and
		// parent are a rename.
		auto sameValues = [](const Collection& a, const Collection& b)
		{
			if (a.attributes.size() != b.attributes.size() || !SameBlockDeep(a.layout, b.layout)) return false;
			for (const Attribute& x : a.attributes)
			{
				const Attribute* y = b.find(x.key);
				if (!y || !SameAttribute(x, *y)) return false;
			}
			return true;
		};
		// Old key -> new key of renamed collections, by class (class << 32 | key).
		std::unordered_map<uint64_t, uint32_t> renamedKey;
		auto renamedParent = [&](const Collection& c)
		{
			auto it = renamedKey.find(Id(c.classKey, c.parentKey));
			return it == renamedKey.end() ? c.parentKey : it->second;
		};
		std::vector<std::pair<const Collection*, const Collection*>> renames;
		for (auto it = deleted.begin(); it != deleted.end();)
		{
			const Collection* d = *it;
			auto match = std::find_if(created.begin(), created.end(), [&](const Collection* c)
			{
				if (c->classKey != d->classKey || !c->vault || !d->vault || c->vault->name != d->vault->name) return false;
				return c->parentKey == renamedParent(*d) && sameValues(*d, *c) && cur.names().find(c->key);
			});
			if (match == created.end())
			{
				++it;
				continue;
			}
			renames.emplace_back(d, *match);
			renamedKey[Id(d->classKey, d->key)] = (*match)->key;
			created.erase(match);
			it = deleted.erase(it);
		}

		std::vector<Statement> out;
		auto comment = [&](const std::string& text)
		{
			Statement s;
			s.kind = K::Comment;
			s.comment = " " + text;
			out.push_back(std::move(s));
		};

		// New and renamed vaults, in packs open in both, and new event vault
		// files. A vault only the base has and one only the current has, in the
		// same pack and holding the same collections, are a rename.
		std::vector<Statement> vaults, vaultRenames;
		std::unordered_map<std::string, std::string> renamedVault; // old name -> new
		std::unordered_set<std::string> keptVaults;                 // in both under another name
		auto idsOf = [](const Vault& v)
		{
			std::set<uint64_t> ids;
			for (const Collection* c : v.collections) ids.insert(Id(c->classKey, c->key));
			return ids;
		};
		for (const auto& pk : cur.packs())
		{
			const LoadedPack* old = nullptr;
			for (const auto& o : base.packs())
				if (Lower(FileName(o->path)) == Lower(FileName(pk->path))) old = o.get();
			const bool newFile = std::find(newEventFiles.begin(), newEventFiles.end(), pk.get()) != newEventFiles.end();
			if (!old && !newFile) continue;
			std::vector<const Vault*> gone;
			if (old)
				for (const auto& ov : old->vaults)
				{
					bool there = false;
					for (const auto& v : pk->vaults) there |= ov->name == v->name;
					if (!there && !ov->collections.empty()) gone.push_back(ov.get());
				}
			for (const auto& v : pk->vaults)
			{
				bool there = false;
				if (old)
					for (const auto& ov : old->vaults) there |= ov->name == v->name;
				if (there) continue;
				const std::set<uint64_t> ids = idsOf(*v);
				auto was = std::find_if(gone.begin(), gone.end(), [&](const Vault* ov)
				{
					return idsOf(*ov) == ids;
				});
				if (was != gone.end())
				{
					Statement s;
					s.kind = K::RenameVault;
					s.vault = (*was)->name;
					s.word = v->name;
					renamedVault[(*was)->name] = v->name;
					keptVaults.insert((*was)->name);
					gone.erase(was);
					vaultRenames.push_back(std::move(s));
					continue;
				}
				vaults.push_back(CreateVaultStatement(*v));
			}
		}
		if (!vaults.empty())
		{
			comment("New vaults");
			for (Statement& s : vaults) out.push_back(std::move(s));
		}
		if (!vaultRenames.empty())
		{
			comment("Renamed vaults");
			for (Statement& s : vaultRenames) out.push_back(std::move(s));
		}
		auto vaultNow = [&](const std::string& name)
		{
			auto it = renamedVault.find(name);
			return it == renamedVault.end() ? name : it->second;
		};

		// Classes made or changed, in packs open in both, before their collections.
		auto classInPack = [](const ClassDef& c, const std::unordered_set<std::string>& files)
		{
			return c.vault && c.vault->pack && files.count(Lower(FileName(c.vault->pack->path)));
		};
		std::vector<Statement> classes, dropClasses;
		for (const auto& c : cur.classes())
		{
			if (!wanted(c->key) || !classInPack(*c, oldFiles)) continue;
			const ClassDef* o = base.findClass(c->key);
			if (o && SameClass(*o, *c)) continue;
			classes.push_back(ClassStatement(cur, *c, true));
		}
		for (const auto& o : base.classes())
		{
			if (!wanted(o->key) || !classInPack(*o, newFiles) || cur.findClass(o->key)) continue;
			Statement s;
			s.kind = K::DeleteClass;
			s.word = NameText(base, o->key);
			dropClasses.push_back(std::move(s));
		}
		if (!classes.empty()) comment("New and changed classes");
		for (Statement& s : classes) out.push_back(std::move(s));

		if (!renames.empty()) comment("Renamed");
		for (const auto& [d, c] : renames)
		{
			Statement s;
			s.kind = K::Rename;
			s.target = TargetOf(cur, d->classKey, d->key);
			s.word = names.format(c->key);
			out.push_back(std::move(s));
		}

		const std::vector<const Collection*> made = ParentsFirst(created);
		if (!made.empty()) comment("New collections");
		for (const Collection* c : made) out.push_back(CollectionStatement(cur, gen, *c, {}));

		// Existing collections: parent, vault, values.
		std::vector<Statement> moves, edits;
		for (const Collection* c : newAll)
		{
			const Collection* o = nullptr;
			if (auto it = oldById.find(Id(c->classKey, c->key)); it != oldById.end()) o = it->second;
			for (const auto& [d, n] : renames)
				if (n == c) o = d;
			if (!o) continue;
			if (renamedParent(*o) != c->parentKey)
			{
				Statement s;
				s.kind = K::Move;
				s.target = TargetOf(cur, *c);
				s.hasUnder = true;
				if (c->parentKey) s.under = TargetOf(cur, c->classKey, c->parentKey);
				else s.under.root = true;
				moves.push_back(std::move(s));
			}
			if (c->vault && o->vault && c->vault->name != vaultNow(o->vault->name))
			{
				Statement s;
				s.kind = K::MoveToVault;
				s.target = TargetOf(cur, *c);
				s.vault = c->vault->name;
				moves.push_back(std::move(s));
			}
			if (!settings.values) continue;
			Statement ed = EditOf(cur, gen, *o, *c, settings.recordWas);
			if (!ed.body.empty()) edits.push_back(std::move(ed));
		}
		if (!moves.empty()) comment("Moved");
		for (Statement& s : moves) out.push_back(std::move(s));
		if (!edits.empty()) comment("Changed values");
		for (Statement& s : edits) out.push_back(std::move(s));

		// Deleted: children first.
		std::vector<const Collection*> gone = ParentsFirst(deleted);
		std::reverse(gone.begin(), gone.end());
		if (!gone.empty()) comment("Deleted");
		for (const Collection* d : gone)
		{
			Statement s;
			s.kind = K::Delete;
			s.target = TargetOf(cur, d->classKey, d->key);
			out.push_back(std::move(s));
		}

		if (!dropClasses.empty()) comment("Deleted classes, with their collections");
		for (Statement& s : dropClasses) out.push_back(std::move(s));

		std::vector<Statement> dropVaults;
		for (const auto& o : base.packs())
		{
			const LoadedPack* pk = nullptr;
			for (const auto& p : cur.packs())
				if (Lower(FileName(o->path)) == Lower(FileName(p->path))) pk = p.get();
			const bool goneFile = std::find(goneEventFiles.begin(), goneEventFiles.end(), o.get()) != goneEventFiles.end();
			if (!pk && !goneFile) continue;
			for (const auto& ov : o->vaults)
			{
				bool there = keptVaults.count(ov->name) > 0;
				if (pk)
					for (const auto& v : pk->vaults) there |= ov->name == v->name;
				if (there) continue;
				Statement s;
				s.kind = K::DeleteVault;
				s.vault = ov->name;
				dropVaults.push_back(std::move(s));
			}
		}
		if (!dropVaults.empty()) comment("Deleted vaults");
		for (Statement& s : dropVaults) out.push_back(std::move(s));

		return NewScript(cur, std::move(out));
	}

	void Merge(Script& script, const std::vector<Statement>& statements)
	{
		for (const Statement& st : statements)
		{
			Statement* into = nullptr;
			if (st.kind == K::Edit || st.kind == K::Collection)
				for (Statement& s : script.statements)
					if (s.kind == st.kind && s.target.text() == st.target.text()) into = &s;
			if (st.kind == K::CreateVault &&
				std::any_of(script.statements.begin(), script.statements.end(), [&](const Statement& s)
				{
					return s.kind == K::CreateVault && Lower(s.vault) == Lower(st.vault);
				}))
				continue;
			if (!into)
			{
				script.statements.push_back(st);
				continue;
			}
			for (const Statement& b : st.body)
			{
				bool replaced = false;
				if (b.kind == K::Set && b.op == '=')
					for (Statement& x : into->body)
						if (x.kind == K::Set && x.op == '=' && x.path.text() == b.path.text())
						{
							ValuePtr firstWas = x.was;
							x = b;
							if (firstWas) x.was = firstWas; // what it was before the first change
							replaced = true;
						}
				if (!replaced) into->body.push_back(b);
			}
		}
	}

} // namespace attrib::script
