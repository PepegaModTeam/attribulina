#include "Search.h"

#include "core/Hash.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace app
{

	using namespace attrib;

	namespace
	{

		// ------------------------------------------------------------------ text ---

		// ASCII only, like ToLower in the "C" locale, and much faster than std::tolower.
		char LowerChar(char c)
		{
			return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
		}
		bool IsDigit(char c)
		{
			return c >= '0' && c <= '9';
		}

		std::string LowerAscii(std::string s)
		{
			for (char& c : s) c = LowerChar(c);
			return s;
		}

		// The needles below are already lowercase.
		bool EqualsCI(std::string_view s, std::string_view lowerNeedle)
		{
			if (s.size() != lowerNeedle.size()) return false;
			for (size_t i = 0; i < s.size(); ++i)
				if (LowerChar(s[i]) != lowerNeedle[i]) return false;
			return true;
		}

		bool ContainsCI(std::string_view s, std::string_view lowerNeedle)
		{
			if (lowerNeedle.empty()) return true;
			if (s.size() < lowerNeedle.size()) return false;
			const char first = lowerNeedle[0];
			for (size_t i = 0, last = s.size() - lowerNeedle.size(); i <= last; ++i)
			{
				if (LowerChar(s[i]) != first) continue;
				size_t k = 1;
				while (k < lowerNeedle.size() && LowerChar(s[i + k]) == lowerNeedle[k]) ++k;
				if (k == lowerNeedle.size()) return true;
			}
			return false;
		}

		// The order CollectionIndex sorts names in (ToLower, then bytes).
		bool LessCI(std::string_view a, std::string_view b)
		{
			const size_t n = std::min(a.size(), b.size());
			for (size_t i = 0; i < n; ++i)
			{
				const unsigned char ca = static_cast<unsigned char>(LowerChar(a[i]));
				const unsigned char cb = static_cast<unsigned char>(LowerChar(b[i]));
				if (ca != cb) return ca < cb;
			}
			return a.size() < b.size();
		}

		// Case-insensitive with digit runs compared as numbers, so "Children[2]"
		// comes before "Children[10]".
		bool NaturalLess(std::string_view a, std::string_view b)
		{
			size_t i = 0, j = 0;
			while (i < a.size() && j < b.size())
			{
				if (IsDigit(a[i]) && IsDigit(b[j]))
				{
					size_t ei = i, ej = j;
					while (ei < a.size() && IsDigit(a[ei])) ++ei;
					while (ej < b.size() && IsDigit(b[ej])) ++ej;
					while (i + 1 < ei && a[i] == '0') ++i;
					while (j + 1 < ej && b[j] == '0') ++j;
					if (ei - i != ej - j) return ei - i < ej - j;
					if (const int c = a.substr(i, ei - i).compare(b.substr(j, ej - j))) return c < 0;
					i = ei;
					j = ej;
					continue;
				}
				const char ca = LowerChar(a[i]), cb = LowerChar(b[j]);
				if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb);
				++i;
				++j;
			}
			return a.size() - i < b.size() - j;
		}

		std::string Trim(const std::string& s)
		{
			size_t b = 0, e = s.size();
			while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
			while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
			return s.substr(b, e - b);
		}

		// "0x" followed by 1 to 8 hex digits and nothing else.
		bool ParseHex(std::string_view s, uint32_t& out)
		{
			if (s.size() < 3 || s.size() > 10 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
			uint32_t v = 0;
			for (size_t i = 2; i < s.size(); ++i)
			{
				const char c = LowerChar(s[i]);
				if (IsDigit(c)) v = v * 16 + uint32_t(c - '0');
				else if (c >= 'a' && c <= 'f') v = v * 16 + uint32_t(c - 'a' + 10);
				else return false;
			}
			out = v;
			return true;
		}

		// A decimal number taking the whole string. Hex is left to ParseHex (strtod
		// would read "0x10" as 16 and match floats with it).
		bool ParseNumber(const std::string& s, double& out)
		{
			if (s.empty() || s.find_first_of("xX") != std::string::npos) return false;
			char* end = nullptr;
			const double v = std::strtod(s.c_str(), &end);
			if (!end || *end != 0 || !std::isfinite(v)) return false;
			out = v;
			return true;
		}

		// What KeyNames::format gives, without allocating when the name is known.
		std::string_view NameOf(const KeyNames& names, uint32_t key, char(&buf)[16])
		{
			if (const std::string* n = names.find(key)) return *n;
			std::snprintf(buf, sizeof(buf), "0x%08X", key);
			return buf;
		}

		// Equal within float precision, and to a number typed from the 6 significant
		// digits the inspector shows.
		bool Near(double v, double q)
		{
			const double d = std::fabs(v - q);
			return d <= 1e-6 || d <= 1e-5 * std::max(std::fabs(v), std::fabs(q));
		}

		// ------------------------------------------------------------------ walk ---

		// One step below a field: an element index or a struct member.
		struct Step
		{
			const MemberDef* member = nullptr; // null: index
			uint32_t index = 0;
		};

		// How the inspector lays out one struct member (App::drawMemberRows).
		struct MemberPlan
		{
			enum class Kind : uint8_t
			{
				Value, InlineArray, Pointer
			} kind = Kind::Value;
			const MemberDef* m = nullptr;
			ValueType type; // the member's type, an inline array's element, or what a pointer leads to
			const MemberDef* countMember = nullptr; // pointer arrays: the member holding the count
			ValueType countType;
		};
		struct StructPlan
		{
			std::vector<MemberPlan> members;
		};

		// A hit before it is formatted: the value is summarized only for hits that
		// make it into the result.
		struct Pending
		{
			uint32_t field = 0;
			std::string where;
			const DataBlock* block = nullptr;
			uint32_t off = 0;
			const ValueType* type = nullptr; // null: `value` is already set
			std::string value;
		};

		// Walks a collection's own values the way the inspector expands them and
		// calls a matcher on every leaf. Types are worked out once per type, not per
		// value.
		class Walker
		{
		public:
			explicit Walker(const Database& db) : db_(db)
			{
			}

			// The field name only steers how a value is shown (colours), so one
			// ValueType serves every field of a type, but for a field the schema
			// gives an enum of its own (FieldDef::fieldEnum).
			const ValueType& attributeType(const Attribute& a)
			{
				auto& slot = a.def && a.def->fieldEnum ? byField_[a.def] : a.type ? byType_[a.type] : bySize_[a.elemSize];
				if (!slot)
				{
					static const std::string kNoName;
					slot = std::make_unique<ValueType>(TypeOfAttribute(a, kNoName));
				}
				return *slot;
			}

			// match(block, offset, type, endian) is true for a hit.
			template <class Match>
			void walk(const Collection& c, Match& match, std::vector<Pending>& out)
			{
				endian_ = c.vault ? c.vault->raw.endian : Endian::Little;
				out_ = &out;
				for (const Attribute& a : c.attributes)
				{
					const ValueType& vt = attributeType(a);
					field_ = a.key;
					if (!a.isArray)
					{
						value(a.data, 0, vt, match, 0);
						continue;
					}
					for (uint32_t i = 0; i < a.count; ++i)
					{
						const uint64_t off = uint64_t(i) * a.elemSize;
						if (off >= a.data.bytes.size()) break;
						steps_.push_back({ nullptr, i });
						value(a.data, uint32_t(off), vt, match, 0);
						steps_.pop_back();
					}
				}
				out_ = nullptr;
			}

			// One value of any type (a struct's members, what its pointers lead
			// to), its paths starting at `path`, with the inspector's row labels.
			template <class Match>
			void walkValue(const DataBlock& b, uint32_t off, const ValueType& vt, Endian e, const std::string& path, Match& match,
				std::vector<Pending>& out)
			{
				endian_ = e;
				out_ = &out;
				prefix_ = &path;
				indexOne_ = true;
				field_ = 0;
				value(b, off, vt, match, 0);
				out_ = nullptr;
				prefix_ = nullptr;
				indexOne_ = false;
			}

		private:
			template <class Match>
			void value(const DataBlock& b, uint32_t off, const ValueType& vt, Match& match, int depth)
			{
				if (uint64_t(off) + std::max<uint32_t>(vt.size, 1) > b.bytes.size()) return; // shown as "?"
				if (!ExpandsIntoMembers(vt))
				{
					if (match(b, off, vt, endian_))
					{
						Pending p;
						p.field = field_;
						p.where = where();
						p.block = &b;
						p.off = off;
						p.type = &vt;
						out_->push_back(std::move(p));
					}
					return;
				}
				// Nested blocks are trees in practice; the guard is for a block that
				// points back at itself.
				if (depth > 32) return;
				for (const MemberPlan& mp : plan(*vt.st).members)
				{
					const MemberDef& m = *mp.m;
					steps_.push_back({ &m, 0 });
					switch (mp.kind)
					{
						case MemberPlan::Kind::Value: value(b, off + m.offset, mp.type, match, depth + 1); break;
						case MemberPlan::Kind::InlineArray:
							for (uint32_t i = 0; i < m.count; ++i)
							{
								steps_.push_back({ nullptr, i });
								value(b, off + m.offset + i * m.elemSize, mp.type, match, depth + 1);
								steps_.pop_back();
							}
							break;
						case MemberPlan::Kind::Pointer:
						{
							const PtrSlot* s = b.ptrAt(off + m.offset);
							if (!s || !s->data || !mp.type.size) break;
							const DataBlock& nb = *s->data;
							uint32_t n = mp.countMember ? uint32_t(ReadInteger(b, off + mp.countMember->offset, mp.countType, endian_)) : 1u;
							n = std::min<uint32_t>(n, uint32_t(nb.bytes.size() / mp.type.size));
							for (uint32_t i = 0; i < n; ++i)
							{
								// A single pointed-to value has no index in its path.
								if (mp.countMember || indexOne_) steps_.push_back({ nullptr, i });
								value(nb, i * mp.type.size, mp.type, match, depth + 1);
								if (mp.countMember || indexOne_) steps_.pop_back();
							}
							break;
						}
					}
					steps_.pop_back();
				}
			}

			// The rows App::drawMemberRows draws for a struct, in its order.
			const StructPlan& plan(const StructDef& st)
			{
				auto& slot = plans_[&st];
				if (slot) return *slot;
				slot = std::make_unique<StructPlan>();
				for (const MemberDef& m : st.members)
				{
					// A count that sizes a pointer array is shown on the array's row.
					bool isCount = false;
					for (const MemberDef& o : st.members)
						isCount |= o.prim == MemberPrim::Ptr && o.ptrCount == m.name && o.ptrTarget != "void";
					if (isCount) continue;
					MemberPlan mp;
					mp.m = &m;
					mp.type = TypeOfMember(m);
					if (m.prim == MemberPrim::Ptr)
					{
						// Pointers the game sets at runtime have nothing to search.
						if (m.ptrTarget.empty() || m.ptrTarget == "void") continue;
						mp.kind = MemberPlan::Kind::Pointer;
						mp.type = TypeOfNamed(db_, m.ptrTarget);
						mp.countMember = m.ptrCount.empty() ? nullptr : st.member(m.ptrCount);
						if (mp.countMember) mp.countType = TypeOfMember(*mp.countMember);
					}
					else if (m.count > 1 && mp.type.kind == TypeKind::Float && (m.count == 3 || m.count == 4))
					{
						// float[3] / float[4] is one vector row.
						mp.type.kind = m.count == 4 ? TypeKind::Vector4 : TypeKind::Vector3;
						mp.type.size = 4 * m.count;
					}
					else if (m.count > 1)
					{
						mp.kind = MemberPlan::Kind::InlineArray;
					}
					slot->members.push_back(std::move(mp));
				}
				return *slot;
			}

			std::string where() const
			{
				std::string s = prefix_ ? *prefix_ : db_.names().format(field_);
				for (const Step& st : steps_)
				{
					if (st.member)
					{
						s += '.';
						s += st.member->name;
					}
					else
					{
						s += '[';
						s += std::to_string(st.index);
						s += ']';
					}
				}
				return s;
			}

			const Database& db_;
			std::unordered_map<const TypeInfo*, std::unique_ptr<ValueType>> byType_;
			std::unordered_map<const FieldDef*, std::unique_ptr<ValueType>> byField_;
			std::unordered_map<uint32_t, std::unique_ptr<ValueType>> bySize_;
			std::unordered_map<const StructDef*, std::unique_ptr<StructPlan>> plans_;
			std::vector<Step> steps_;
			std::vector<Pending>* out_ = nullptr;
			uint32_t field_ = 0;
			Endian endian_ = Endian::Little;
			const std::string* prefix_ = nullptr; // walkValue: the path of the value walked
			bool indexOne_ = false;               // "[0]" for a pointer to one value, as the inspector's row reads
		};

		// -------------------------------------------------------------- matchers ---

		// Values mode: the query read every way a value could be written.
		class ValueMatch
		{
		public:
			ValueMatch(const Database& db, const std::string& query, bool whole, SearchHash hexHash)
				: db_(db), whole_(whole), hexHash_(hexHash)
			{
				lower_ = LowerAscii(query);
				hasHex_ = ParseHex(query, hex_);
				// "0xBDFC" is part of a name shown as its hash ("0xBDFC670F").
				hexText_ = lower_.size() > 2 && lower_[0] == '0' && lower_[1] == 'x';
				double num = 0;
				if (ParseNumber(query, num))
				{
					hasNum_ = true;
					num_ = num;
					if (num == std::trunc(num) && std::fabs(num) < 9.2e18)
					{
						hasInt_ = true;
						int_ = int64_t(num);
					}
				}
				hash_ = StringHash32(query);
				binHash_ = BinHash(query);
				isTrue_ = lower_ == "true";
				isFalse_ = lower_ == "false";
				hasSlash_ = lower_.find('/') != std::string::npos;
			}

			bool operator()(const DataBlock& b, uint32_t off, const ValueType& vt, Endian e)
			{
				const uint8_t* p = b.bytes.data() + off;
				switch (vt.kind)
				{
					case TypeKind::Bool: return ReadInteger(b, off, vt, e) ? isTrue_ : isFalse_;
					case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16:
					case TypeKind::Int32: case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::UInt64: case TypeKind::Char:
						return integer(ReadInteger(b, off, vt, e), vt);
					case TypeKind::Float: return hasNum_ && Near(loadf32(p, e), num_);
					case TypeKind::Double:
					{
						if (!hasNum_) return false;
						const uint64_t u = load64(p, e);
						double d;
						std::memcpy(&d, &u, 8);
						return Near(d, num_);
					}
					case TypeKind::Vector2: return floats(p, 2, e);
					case TypeKind::Vector3: return floats(p, 3, e);
					case TypeKind::Vector4: case TypeKind::Quaternion: case TypeKind::FloatColour: return floats(p, 4, e);
					case TypeKind::Matrix: return floats(p, 16, e);
					case TypeKind::Text:
					{
						const PtrSlot* s = b.ptrAt(off);
						return s && text(s->text);
					}
					case TypeKind::StringKey:
					{
						// The stored hash, which can differ from the text's in retail data.
						const StringKeyLayout sk = StringKeyAt(vt.size);
						const uint32_t h = load32(p + sk.hash32, e);
						if (h == hash_ || (hasHex_ && h == hex_)) return true;
						const PtrSlot* s = b.ptrAt(off + sk.text);
						return s && text(s->text);
					}
					case TypeKind::Key: return key(db_.names(), LoadKey(db_, p, vt.size, e), hash_);
					case TypeKind::ClassRefSpec: return key(db_.names(), load32(p, e), hash_);
					case TypeKind::BinKey: return key(db_.binNames(), load32(p, e), binHash_);
					case TypeKind::RefSpec:
					{
						const RefSpecLayout rl = RefSpecAt(vt.size);
						const uint32_t ck = LoadKey(db_, p + rl.cls, rl.keySize, e), k = LoadKey(db_, p + rl.col, rl.keySize, e);
						if (!ck && !k) return false; // "(none)"
						return key(db_.names(), k, hash_) || refText(ck, k);
					}
					case TypeKind::Enum:
					{
						const int64_t v = ReadInteger(b, off, vt, e);
						if (hasInt_ && v == int_) return true;
						if (vt.en)
							if (const std::string* n = vt.en->nameOf(v)) return plain(*n);
						return false;
					}
					case TypeKind::Colour:
					{
						// 0xAARRGGBB as the inspector shows the bytes (A, R, G, B), or
						// the u32 they make in the vault's byte order.
						if (!hasHex_) return false;
						const uint32_t argb = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
						return argb == hex_ || load32(p, e) == hex_;
					}
					case TypeKind::Unknown:
					{
						// Edited as an int32 when it is 4 bytes and not a pointer.
						if (vt.size != 4 || b.ptrAt(off)) return false;
						const uint32_t u = load32(p, e);
						if ((hasHex_ && u == hex_) || (hasInt_ && int64_t(int32_t(u)) == int_)) return true;
						return vt.hint == "hex" && shownHex(u); // shown as 0x...
					}
					case TypeKind::Blob: case TypeKind::Curve: case TypeKind::Struct: return false;
				}
				return false;
			}

			// A collection's own name, as a key value is matched.
			bool ownName(const Collection& c) const
			{
				return key(db_.names(), c.key, hash_);
			}

			// Its parent, when the query names it whole: the name, its hash, or a
			// 0x... the name hashes to. Every child of a collection whose name
			// merely contains the text would bury the other hits.
			bool parentNamed(const Collection& c) const
			{
				const uint32_t v = c.parentKey;
				if (!v) return false;
				if (v == hash_ || (hasHex_ && v == hex_)) return true;
				const std::string* n = db_.names().find(v);
				return n && (EqualsCI(*n, lower_) || named(*n));
			}

		private:
			// Text as shown: it contains (or is) the query.
			bool plain(std::string_view s) const
			{
				return whole_ ? EqualsCI(s, lower_) : ContainsCI(s, lower_);
			}

			// A 0x... query is also the hash of the text it finds.
			bool named(std::string_view s) const
			{
				return hasHex_ && (hexHash_ == SearchHash::BinHash ? BinHash(s) : StringHash32(s)) == hex_;
			}

			bool text(std::string_view s) const
			{
				return plain(s) || named(s);
			}

			// A hash shown as "0x" and 8 hex digits, matched as text.
			bool shownHex(uint32_t v) const
			{
				if (!hexText_) return false;
				char b[16];
				std::snprintf(b, sizeof(b), "0x%08X", v);
				return plain(b);
			}

			// "class/collection" as FormatValue writes it, built only when the query
			// could span the '/'.
			bool refText(uint32_t ck, uint32_t k)
			{
				char cb[16], kb[16];
				const std::string_view cn = NameOf(db_.names(), ck, cb);
				const std::string_view kn = k ? NameOf(db_.names(), k, kb) : std::string_view("(none)");
				if (!hasSlash_) return !whole_ && (ContainsCI(cn, lower_) || ContainsCI(kn, lower_));
				const std::string_view q = lower_;
				if (whole_)
					return q.size() == cn.size() + 1 + kn.size() && q[cn.size()] == '/' && EqualsCI(cn, q.substr(0, cn.size())) &&
					EqualsCI(kn, q.substr(cn.size() + 1));
				scratch_.assign(cn);
				scratch_ += '/';
				scratch_ += kn;
				return ContainsCI(scratch_, lower_);
			}

			// A hashed name: its value, the hash of the text (names the dictionary
			// lacks), the dictionary's name for it (or a name a 0x... is the hash
			// of), or the 0x... it is shown as when no name is known.
			bool key(const KeyNames& dict, uint32_t v, uint32_t textHash) const
			{
				if (v == textHash || (hasHex_ && v == hex_)) return true;
				if (const std::string* n = dict.find(v)) return text(*n);
				return shownHex(v);
			}

			bool integer(int64_t v, const ValueType& vt) const
			{
				if (hasInt_ && v == int_) return true;
				if (!hasHex_) return false;
				// Compare the stored bits, so 0xFF finds an int8 -1.
				uint64_t raw = uint64_t(v);
				if (vt.size < 8 && !(vt.member && vt.member->isBitfield())) raw &= (1ull << (vt.size * 8)) - 1;
				return raw == hex_;
			}

			bool floats(const uint8_t* p, int n, Endian e) const
			{
				if (!hasNum_) return false;
				for (int i = 0; i < n; ++i)
					if (Near(loadf32(p + i * 4, e), num_)) return true;
				return false;
			}

			const Database& db_;
			bool whole_;
			SearchHash hexHash_;
			std::string lower_;
			bool hasHex_ = false, hexText_ = false, hasNum_ = false, hasInt_ = false, isTrue_ = false, isFalse_ = false, hasSlash_ = false;
			uint32_t hex_ = 0, hash_ = 0, binHash_ = 0;
			double num_ = 0;
			int64_t int_ = 0;
			std::string scratch_;
		};

		// References mode: values that point at a collection (or a class).
		class RefMatch
		{
		public:
			RefMatch(const Database& db, uint32_t cls, uint32_t key) : db_(db), cls_(cls), key_(key)
			{
				if (const std::string* n = key ? db.names().find(key) : nullptr)
				{
					hasName_ = true;
					lowerName_ = LowerAscii(*n);
					binHash_ = BinHash(*n);
				}
			}

			bool operator()(const DataBlock& b, uint32_t off, const ValueType& vt, Endian e) const
			{
				const uint8_t* p = b.bytes.data() + off;
				switch (vt.kind)
				{
					case TypeKind::RefSpec:
					{
						const RefSpecLayout rl = RefSpecAt(vt.size);
						const uint32_t ck = LoadKey(db_, p + rl.cls, rl.keySize, e);
						if (!key_) return ck == cls_;
						// A reference without a class still names the collection.
						return LoadKey(db_, p + rl.col, rl.keySize, e) == key_ && (ck == cls_ || ck == 0);
					}
					case TypeKind::Key: return key_ && LoadKey(db_, p, vt.size, e) == key_;
					case TypeKind::ClassRefSpec: return key_ ? load32(p, e) == key_ && ClassRefSpecClass(vt.typeName) == cls_ : ClassRefSpecClass(vt.typeName) == cls_;
					case TypeKind::StringKey: return key_ && load32(p + StringKeyAt(vt.size).hash32, e) == key_;
					case TypeKind::BinKey: return hasName_ && load32(p, e) == binHash_;
					case TypeKind::Text:
					{
						// Gameplay keeps element paths in some Text fields (TrackRecordHolder).
						if (!hasName_) return false;
						const PtrSlot* s = b.ptrAt(off);
						return s && EqualsCI(s->text, lowerName_);
					}
					default: return false;
				}
			}

		private:
			const Database& db_;
			uint32_t cls_, key_;
			bool hasName_ = false;
			std::string lowerName_;
			uint32_t binHash_ = 0;
		};

		// ----------------------------------------------------------------- order ---

		// A collection's hits, before they go into the result.
		struct Group
		{
			const Collection* c = nullptr;
			std::string name; // collection name, for sorting
			std::vector<Pending> hits;
		};

		// Classes in the order hits are reported, by name like the Explorer.
		std::vector<const ClassDef*> SortedClasses(const Database& db, uint32_t inClass)
		{
			std::vector<std::pair<std::string, const ClassDef*>> v;
			for (const auto& cls : db.classes())
				if (!inClass || cls->key == inClass) v.emplace_back(db.names().format(cls->key), cls.get());
			std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b)
			{
				return LessCI(a.first, b.first);
			});
			std::vector<const ClassDef*> out;
			for (const auto& e : v) out.push_back(e.second);
			return out;
		}

		// Adds one class's hits to the result, by collection name and then by where,
		// up to the limit. Classes are searched one at a time so only collections
		// with hits are named and sorted, and a search that reaches its limit stops
		// at the end of a class. False when the limit cut hits off.
		bool Collect(const Database& db, std::vector<Group>& groups, const SearchQuery& q, SearchResult& r)
		{
			for (Group& g : groups) g.name = db.names().format(g.c->key);
			std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b)
			{
				return LessCI(a.name, b.name);
			});
			for (Group& g : groups)
			{
				const Collection& c = *g.c;
				const Endian e = c.vault ? c.vault->raw.endian : Endian::Little;
				std::stable_sort(g.hits.begin(), g.hits.end(), [](const Pending& a, const Pending& b)
				{
					return NaturalLess(a.where, b.where);
				});
				for (Pending& p : g.hits)
				{
					if (r.hits.size() >= q.limit)
					{
						r.truncated = true;
						break;
					}
					SearchHit h;
					h.classKey = c.classKey;
					h.key = c.key;
					h.field = p.field;
					h.where = std::move(p.where);
					h.value = p.type ? FormatValue(db, *p.block, p.off, *p.type, e) : std::move(p.value);
					r.hits.push_back(std::move(h));
				}
				if (r.truncated) break;
			}
			groups.clear();
			return !r.truncated;
		}

		std::string ElementCount(uint32_t n)
		{
			return std::to_string(n) + (n == 1 ? " element" : " elements");
		}

		void SearchFields(const Database& db, const SearchQuery& q, const std::string& text, SearchResult& r)
		{
			const KeyNames& names = db.names();
			const std::string lower = LowerAscii(text);
			// 0x...: the field whose key it is, or whose name it is the bStringHash of.
			uint32_t hex = 0;
			const bool isHex = ParseHex(text, hex);
			auto hashed = [&](const FieldDef& f)
			{
				if (!isHex) return false;
				if (f.key == hex) return true;
				const std::string* n = q.hexHash == SearchHash::BinHash ? names.find(f.key) : nullptr;
				return n && BinHash(*n) == hex;
			};
			Walker types(db);
			std::vector<Group> groups;
			bool more = true;
			for (const ClassDef* cls : SortedClasses(db, q.inClass))
			{
				// This class's matching fields, by name.
				std::vector<std::pair<std::string, const FieldDef*>> fields;
				for (const FieldDef& f : cls->fields)
				{
					std::string n = names.format(f.key);
					if ((q.wholeValue ? EqualsCI(n, lower) : ContainsCI(n, lower)) || hashed(f)) fields.emplace_back(std::move(n), &f);
				}
				if (fields.empty()) continue;
				std::stable_sort(fields.begin(), fields.end(), [](const auto& a, const auto& b)
				{
					return NaturalLess(a.first, b.first);
				});
				for (const auto& f : fields) r.fields.emplace_back(cls->key, f.second->key);
				if (!more) continue; // past the limit: only the definitions are still wanted
				for (const Collection* c : cls->collections)
				{
					++r.collectionsSearched;
					Group g;
					for (const auto& [fieldName, f] : fields)
					{
						const Attribute* a = c->find(f->key);
						if (!a) continue;
						Pending p;
						p.field = f->key;
						p.where = fieldName;
						if (a->isArray)
						{
							p.value = ElementCount(a->count);
						}
						else
						{
							p.block = &a->data;
							p.type = &types.attributeType(*a);
						}
						g.hits.push_back(std::move(p));
					}
					if (g.hits.empty()) continue;
					g.c = c;
					groups.push_back(std::move(g));
				}
				more = Collect(db, groups, q, r);
			}
		}

	} // namespace

	SearchResult RunSearch(const Database& db, const SearchQuery& q)
	{
		SearchResult r;
		const KeyNames& names = db.names();
		if (q.inClass && !db.findClass(q.inClass))
		{
			r.error = "No class " + names.format(q.inClass) + " in the database";
			return r;
		}
		const std::string text = Trim(q.text);

		if (q.mode == SearchMode::Fields)
		{
			if (text.empty())
			{
				r.error = "Type something to search for";
				return r;
			}
			SearchFields(db, q, text, r);
			return r;
		}

		const Collection* target = nullptr;
		if (q.mode == SearchMode::References)
		{
			if (!q.refClass)
			{
				r.error = "Pick a collection or class to find references to";
				return r;
			}
			if (!db.findClass(q.refClass))
			{
				r.error = "No class " + names.format(q.refClass) + " in the database";
				return r;
			}
			if (q.refKey)
			{
				target = db.findCollection(q.refClass, q.refKey);
				if (!target)
				{
					r.error = "No collection " + names.format(q.refKey) + " in class " + names.format(q.refClass);
					return r;
				}
			}
		}
		else if (text.empty())
		{
			r.error = "Type something to search for";
			return r;
		}

		Walker walker(db);
		std::vector<Group> groups;
		auto run = [&](auto& match)
		{
			for (const ClassDef* cls : SortedClasses(db, q.inClass))
			{
				for (const Collection* c : cls->collections)
				{
					++r.collectionsSearched;
					Group g;
					walker.walk(*c, match, g.hits);
					if constexpr (std::is_same_v<std::decay_t<decltype(match)>, ValueMatch>)
					{
						// The collection's own name and its parent are values of it too.
						if (match.ownName(*c))
						{
							Pending p;
							p.where = "(name)";
							p.value = names.format(c->key);
							g.hits.push_back(std::move(p));
						}
						if (match.parentNamed(*c))
						{
							Pending p;
							p.where = "parent";
							p.value = names.format(c->parentKey);
							g.hits.push_back(std::move(p));
						}
					}
					// Text equal to the target's own name inside the target is
					// its name (gameplay's Name and CollectionName), not a reference.
					if (target && c == target)
						std::erase_if(g.hits, [](const Pending& p)
					{
						return p.type && p.type->kind == TypeKind::Text;
					});
					// Collections that inherit from the target (an element's
					// template, a template's base).
					if (target && c->parent == target)
					{
						Pending p;
						p.where = "parent";
						p.value = names.format(target->key);
						g.hits.push_back(std::move(p));
					}
					if (g.hits.empty()) continue;
					g.c = c;
					groups.push_back(std::move(g));
				}
				if (!Collect(db, groups, q, r)) return;
			}
		};
		if (q.mode == SearchMode::References)
		{
			RefMatch match(db, q.refClass, q.refKey);
			run(match);
		}
		else
		{
			ValueMatch match(db, text, q.wholeValue, q.hexHash);
			run(match);
		}
		return r;
	}

	std::vector<ValueLine> ValueLines(const Database& db, const DataBlock& b, uint32_t off, const ValueType& vt, Endian e,
		const std::string& path)
	{
		Walker walker(db);
		auto every = [](const DataBlock&, uint32_t, const ValueType&, Endian) { return true; };
		std::vector<Pending> found;
		walker.walkValue(b, off, vt, e, path, every, found);
		std::vector<ValueLine> out;
		out.reserve(found.size());
		for (Pending& p : found) out.push_back({ std::move(p.where), FormatValueInFull(db, *p.block, p.off, *p.type, e) });
		return out;
	}

	bool ParseReferenceTarget(const Database& db, const std::string& text, uint32_t& cls, uint32_t& key)
	{
		// A name, or 0x... for a hash.
		auto parse = [](const std::string& s, uint32_t& out)
		{
			if (s.empty()) return false;
			if (!ParseHex(s, out)) out = StringHash32(s);
			return true;
		};
		const std::string t = Trim(text);
		const size_t slash = t.find('/');
		uint32_t c = 0, k = 0;
		if (!parse(t.substr(0, slash), c) || !db.findClass(c)) return false;
		// Collection names can hold more '/' (gameplay paths).
		if (slash != std::string::npos && (!parse(t.substr(slash + 1), k) || !db.findCollection(c, k))) return false;
		cls = c;
		key = k;
		return true;
	}

} // namespace app
