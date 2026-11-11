#include "Database.h"

#include "Compression.h"
#include "Game.h"
#include "Hash.h"
#include "TypeLayouts.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <unordered_set>

namespace attrib
{

	// ---------------------------------------------------------------- types ---

	TypeKind ClassifyType(const std::string& n, uint32_t size)
	{
		if (n.rfind("Attrib::Gen::ClassRefSpec_", 0) == 0) return size == 8 ? TypeKind::ClassRefSpec : TypeKind::Unknown;
		static const std::unordered_map<std::string, TypeKind> kKinds = {
			{"EA::Reflection::Bool", TypeKind::Bool},
			{"EA::Reflection::Int8", TypeKind::Int8},
			{"EA::Reflection::UInt8", TypeKind::UInt8},
			{"EA::Reflection::Int16", TypeKind::Int16},
			{"EA::Reflection::UInt16", TypeKind::UInt16},
			{"EA::Reflection::Int32", TypeKind::Int32},
			{"EA::Reflection::UInt32", TypeKind::UInt32},
			{"EA::Reflection::Int64", TypeKind::Int64},
			{"EA::Reflection::UInt64", TypeKind::UInt64},
			{"EA::Reflection::Float", TypeKind::Float},
			{"EA::Reflection::Double", TypeKind::Double},
			{"EA::Reflection::Char", TypeKind::Char},
			{"EA::Reflection::Text", TypeKind::Text},
			{"Attrib::StringKey", TypeKind::StringKey},
			{"Attrib::RefSpec", TypeKind::RefSpec},
			{"Attrib::Key", TypeKind::Key},
			{"UCrc32", TypeKind::Key},
			{"GCollectionKey", TypeKind::Key},
			{"DefinitionKey", TypeKind::Key},
			{"type_bStringHash", TypeKind::BinKey},
			{"Attrib::Types::Colour", TypeKind::Colour},
			{"Attrib::Types::FloatColour", TypeKind::FloatColour},
			{"Attrib::Types::ShaderColour", TypeKind::FloatColour},
			{"Attrib::Types::Vector2", TypeKind::Vector2},
			{"Attrib::Types::Vector3", TypeKind::Vector3},
			{"Attrib::Types::Vector4", TypeKind::Vector4},
			{"Attrib::Types::Quaternion", TypeKind::Quaternion},
			{"Attrib::Types::Matrix", TypeKind::Matrix},
			{"Attrib::Blob", TypeKind::Blob},
			{"Curve", TypeKind::Curve},
		};
		static const std::unordered_map<TypeKind, uint32_t> kSizes = {
			{TypeKind::Bool, 1}, {TypeKind::Int8, 1}, {TypeKind::UInt8, 1}, {TypeKind::Char, 1},
			{TypeKind::Int16, 2}, {TypeKind::UInt16, 2}, {TypeKind::Int32, 4}, {TypeKind::UInt32, 4},
			{TypeKind::Float, 4}, {TypeKind::Text, 4}, {TypeKind::Key, 4}, {TypeKind::BinKey, 4},
			{TypeKind::Colour, 4}, {TypeKind::Int64, 8}, {TypeKind::UInt64, 8}, {TypeKind::Double, 8},
			{TypeKind::StringKey, 8}, {TypeKind::Vector2, 8}, {TypeKind::Blob, 8}, {TypeKind::RefSpec, 12},
			{TypeKind::Vector3, 12}, {TypeKind::Vector4, 16}, {TypeKind::Quaternion, 16},
			{TypeKind::FloatColour, 16}, {TypeKind::Matrix, 64}, {TypeKind::Curve, 44},
		};
		auto it = kKinds.find(n);
		if (it == kKinds.end())
			return TypeKind::Unknown;
		// A game with a different layout for the same name falls back to bytes.
		// (PS2 and GameCube compilers make a bool 4 bytes; the early Most Wanted
		// demos' keys are 8, their RefSpec 24: Values.h RefSpecAt.)
		auto sz = kSizes.find(it->second);
		const bool wide = (it->second == TypeKind::Bool && size == 4) || (it->second == TypeKind::Key && size == 8) ||
			(it->second == TypeKind::RefSpec && size == 24) || (it->second == TypeKind::Curve && size == 48);
		if (sz != kSizes.end() && sz->second != size && !wide)
			return TypeKind::Unknown;
		return it->second;
	}

	const char* TypeKindName(TypeKind k)
	{
		switch (k)
		{
			case TypeKind::Unknown: return "bytes";
			case TypeKind::Bool: return "bool";
			case TypeKind::Int8: return "int8";
			case TypeKind::UInt8: return "uint8";
			case TypeKind::Int16: return "int16";
			case TypeKind::UInt16: return "uint16";
			case TypeKind::Int32: return "int32";
			case TypeKind::UInt32: return "uint32";
			case TypeKind::Int64: return "int64";
			case TypeKind::UInt64: return "uint64";
			case TypeKind::Float: return "float";
			case TypeKind::Double: return "double";
			case TypeKind::Char: return "char";
			case TypeKind::Text: return "text";
			case TypeKind::StringKey: return "string key";
			case TypeKind::RefSpec: return "reference";
			case TypeKind::Key: return "key";
			case TypeKind::BinKey: return "bin key";
			case TypeKind::Colour: return "colour";
			case TypeKind::FloatColour: return "float colour";
			case TypeKind::Vector2: return "vector2";
			case TypeKind::Vector3: return "vector3";
			case TypeKind::Vector4: return "vector4";
			case TypeKind::Quaternion: return "quaternion";
			case TypeKind::Matrix: return "matrix";
			case TypeKind::Blob: return "blob";
			case TypeKind::Curve: return "curve";
			case TypeKind::Struct: return "struct";
			case TypeKind::Enum: return "enum";
			case TypeKind::ClassRefSpec: return "class reference";
		}
		return "?";
	}

	const PointerLayout* TypeInfo::pointers() const
	{
		if (const PointerLayout* p = PointerLayoutFor(name, size)) return p;
		return derivedPointers.get();
	}

	uint32_t LayoutArrayHeader(const FieldDef& f)
	{
		if (!f.isArray()) return 0;
		// Element types aligned to 16 (vectors, matrices) start 16 bytes in.
		return (1u << f.alignment) > 8 ? 16u : 8u;
	}

	uint16_t ArrayPad(const FieldDef& f)
	{
		return (1u << f.alignment) > 8 ? 0x8000 : 0;
	}

	uint32_t LayoutExtent(const FieldDef& f, uint16_t capacity)
	{
		return f.isArray() ? LayoutArrayHeader(f) + uint32_t(capacity) * f.size : f.size;
	}

	uint32_t LayoutOffset(const FieldDef& f)
	{
		if (!f.isArray() && f.size == 4 && f.type && f.type->kind == TypeKind::Bool && f.offset % 4) return (f.offset + 3u) & ~3u;
		return f.offset;
	}

	uint32_t LayoutBytes(const ClassDef& cls)
	{
		uint32_t n = cls.layoutSize;
		for (const auto& f : cls.fields)
			if (f.inLayout() && !f.isArray()) n = std::max(n, LayoutOffset(f) + f.size);
		return n;
	}

	// ----------------------------------------------------------- small helpers ---

	PtrSlot* DataBlock::ptrAt(uint32_t offset)
	{
		for (auto& p : ptrs)
			if (p.offset == offset) return &p;
		return nullptr;
	}
	const PtrSlot* DataBlock::ptrAt(uint32_t offset) const
	{
		for (auto& p : ptrs)
			if (p.offset == offset) return &p;
		return nullptr;
	}

	const FieldDef* ClassDef::field(uint32_t fieldKey) const
	{
		for (const auto& f : fields)
			if (f.key == fieldKey) return &f;
		return nullptr;
	}

	Attribute* Collection::find(uint32_t fieldKey)
	{
		for (auto& a : attributes)
			if (a.key == fieldKey) return &a;
		return nullptr;
	}
	const Attribute* Collection::find(uint32_t fieldKey) const
	{
		for (auto& a : attributes)
			if (a.key == fieldKey) return &a;
		return nullptr;
	}
	const Attribute* Collection::resolve(uint32_t fieldKey, const Collection** owner) const
	{
		int guard = 0;
		for (const Collection* c = this; c && guard < 256; c = c->parent, ++guard)
		{
			if (const Attribute* a = c->find(fieldKey))
			{
				if (owner) *owner = c;
				return a;
			}
			if (cls && cls->field(fieldKey) && cls->field(fieldKey)->inLayout())
				break; // layout fields live in every collection's own layout
		}
		if (owner) *owner = nullptr;
		return nullptr;
	}

	namespace
	{
		uint64_t CollectionId(uint32_t classKey, uint32_t key)
		{
			return (uint64_t(classKey) << 32) | key;
		}
	} // namespace

	// ---------------------------------------------------------- BlockReader ---

	namespace detail
	{

		// Reads bin blocks and attaches their pointers by type (TypeLayouts.h),
		// sharing nested blocks between pointers that target the same address.
		class BlockReader
		{
		public:
			BlockReader(const Database& db, const RawVault& raw, Vault& vault) : db_(db), raw_(raw), vault_(vault), r_(raw.binReader())
			{
				PtrSeq seq = 0;
				int block = -1;
				for (const PtrRef& p : raw.pointers)
				{
					if (p.type == 2) block = p.index;
					else if (p.type == 3 || p.type == 1) (block == 0 ? vltSeq_ : binSeq_)[p.fixupOffset] = seq;
					++seq;
				}
			}

			DataBlock read(uint32_t offset, uint32_t size) const
			{
				DataBlock b;
				auto s = r_.slice(offset, size);
				b.bytes.assign(s.begin(), s.end());
				return b;
			}

			// Attaches the pointers of one element of `type` that sits at `elemOffset`
			// in `b`, where `b` starts at bin offset `blockStart`.
			void attach(DataBlock& b, uint32_t blockStart, uint32_t elemOffset, const TypeInfo* type)
			{
				if (!type) return;
				if (trace_) traced_[blockStart + elemOffset] = type;
				attach(b, blockStart, elemOffset, type->pointers());
			}

			void attach(DataBlock& b, uint32_t blockStart, uint32_t elemOffset, const PointerLayout* pl)
			{
				if (!pl) return;
				for (uint32_t s : pl->unfollowed) null(b, blockStart, elemOffset + s);
				for (uint32_t s : pl->strings)
				{
					const uint32_t loc = blockStart + elemOffset + s;
					const uint32_t* target = raw_.binPtr(loc);
					if (!target)
					{
						null(b, blockStart, elemOffset + s);
						continue;
					}
					consumed_.insert(loc);
					b.ptrs.push_back(stringSlot(elemOffset + s, *target, seqOf(binSeq_, loc)));
					store32(b.bytes.data() + elemOffset + s, 0, raw_.endian);
				}
				for (const NestedField& n : pl->nested)
				{
					const uint32_t loc = blockStart + elemOffset + n.offset;
					const uint32_t* target = raw_.binPtr(loc);
					if (!target)
					{
						null(b, blockStart, elemOffset + n.offset);
						continue;
					}
					consumed_.insert(loc);
					PtrSlot slot;
					slot.offset = elemOffset + n.offset;
					slot.seq = seqOf(binSeq_, loc);
					uint32_t count = NestedElementCount(n, b.bytes.data() + elemOffset, raw_.endian);
					if (n.count == CountSource::PairSum)
						for (const PtrSlot& p : b.ptrs)
							if (p.offset == elemOffset + n.countOffset && p.data)
								for (size_t i = 0; i + 8 <= p.data->bytes.size(); i += 8) count += load32(p.data->bytes.data() + i + 4, raw_.endian);
					slot.data = nested(*target, n, count);
					b.ptrs.push_back(std::move(slot));
					store32(b.bytes.data() + elemOffset + n.offset, 0, raw_.endian);
				}
			}

			PtrSlot stringSlot(uint32_t rel, uint32_t target, PtrSeq seq)
			{
				PtrSlot p;
				p.offset = rel;
				p.seq = seq;
				p.isString = true;
				if (!raw_.inStrE(target))
				{
					warn("a string pointer targets bin 0x" + ByteReader::Hex(target) + ", outside the string table");
					p.text = r_.cstr(target);
					return p;
				}
				auto it = strings_.find(target);
				if (it == strings_.end())
					it = strings_.emplace(target, r_.cstr(target)).first;
				p.text = it->second;
				return p;
			}

			PtrSeq vltSeq(uint32_t loc) const
			{
				return seqOf(vltSeq_, loc);
			}

			// A null pointer in the bin at `rel` in `b`, listed in PtrN (Most Wanted's layout).
			void null(DataBlock& b, uint32_t blockStart, uint32_t rel)
			{
				const uint32_t loc = blockStart + rel;
				if (!raw_.binNulls.count(loc) || !nullsSeen_.insert(loc).second) return;
				b.nulls.push_back({ rel, seqOf(binSeq_, loc) });
			}
			// A null pointer in the vlt that a load data structure explains.
			void vltNull(uint32_t loc)
			{
				vltNullsSeen_.insert(loc);
			}

			// Called once the vault is read: every bin pointer must be explained.
			void finish()
			{
				// Every null listed must be a pointer the layouts know, or the writer can't list it again.
				size_t nulls = 0;
				for (uint32_t loc : raw_.binNulls)
				{
					if (nullsSeen_.count(loc)) continue;
					++nulls;
					if (trace_) std::fprintf(stderr, "%s: null pointer at bin 0x%X\n", vault_.name.c_str(), loc);
				}
				for (uint32_t loc : raw_.vltNulls)
				{
					if (vltNullsSeen_.count(loc)) continue;
					++nulls;
					if (trace_) std::fprintf(stderr, "%s: null pointer at vlt 0x%X\n", vault_.name.c_str(), loc);
				}
				if (nulls)
					warn(std::to_string(nulls) + " null pointer(s) in PtrN aren't where the type layouts put pointers; saving would drop them");
				size_t missing = 0;
				for (auto& [loc, t] : raw_.binFixups)
				{
					if (consumed_.count(loc)) continue;
					++missing;
					if (!trace_) continue;
					// ATTRIB_POINTER_TRACE=1: the value each one sits in, to find the type that needs a layout.
					auto it = traced_.upper_bound(loc);
					if (it != traced_.begin() && loc < (--it)->first + it->second->size)
						std::fprintf(stderr, "%s: pointer at bin 0x%X: %s +%u (%u bytes), to %s 0x%X\n", vault_.name.c_str(), loc,
									 it->second->name.c_str(), loc - it->first, it->second->size, raw_.inStrE(t) ? "a string at" : "bin", t);
					else
						std::fprintf(stderr, "%s: pointer at bin 0x%X, in no value read, to %s 0x%X\n", vault_.name.c_str(), loc,
									 raw_.inStrE(t) ? "a string at" : "bin", t);
				}
				if (missing)
					warn(std::to_string(missing) + " pointer(s) in the bin aren't covered by the type layouts; "
						 "saving would drop them");
				for (auto& [off, text] : strings_)
					vault_.stringOrder.push_back(text);
			}

		private:
			static PtrSeq seqOf(const std::unordered_map<uint32_t, PtrSeq>& m, uint32_t loc)
			{
				auto it = m.find(loc);
				return it == m.end() ? kNewPtr : it->second;
			}

			std::shared_ptr<DataBlock> nested(uint32_t target, const NestedField& n, uint32_t count)
			{
				auto it = nested_.find(target);
				if (it != nested_.end())
					return it->second;
				uint32_t size = count * n.elemSize;
				if (n.toLzEnd && target < raw_.bin.size())
					if (auto h = ParseLzHeader(ByteSpan(raw_.bin.data() + target, raw_.bin.size() - target)))
					{
						const uint64_t end = h->kind == LzKind::Jdlz ? h->compressedSize : uint64_t(h->headerSize) + h->compressedSize;
						if (end > size && target + end <= raw_.bin.size()) size = uint32_t(end);
					}
				auto block = std::make_shared<DataBlock>(read(target, size));
				// Most Wanted puts every compiled Lua block on 16 bytes.
				block->align = n.toLzEnd && raw_.mostWantedOwn() ? 16 : n.align;
				nested_[target] = block;
				// The element type's pointers: the hand table's, else the type table's
				// (a RefSpec's Collection*, which Most Wanted's layout lists as null).
				const PointerLayout* pl = PointerLayoutFor(n.elemType, n.elemSize);
				if (!pl)
					if (const TypeInfo* t = db_.typeByHash(StringHash32(n.elemType)); t && t->size == n.elemSize) pl = t->pointers();
				if (pl)
					for (uint32_t k = 0; k < count; ++k)
						attach(*block, target, k * n.elemSize, pl);
				return block;
			}

			void warn(std::string msg)
			{
				vault_.warnings.push_back(std::move(msg));
				vault_.exactlyWritable = false;
			}

			const Database& db_;
			const RawVault& raw_;
			Vault& vault_;
			ByteReader r_;
			std::unordered_map<uint32_t, PtrSeq> vltSeq_, binSeq_;
			std::unordered_set<uint32_t> nullsSeen_, vltNullsSeen_;
			std::unordered_set<uint32_t> consumed_;
			std::map<uint32_t, std::string> strings_; // by offset: StrE order
			std::unordered_map<uint32_t, std::shared_ptr<DataBlock>> nested_;
			const bool trace_ = std::getenv("ATTRIB_POINTER_TRACE") != nullptr;
			std::map<uint32_t, const TypeInfo*> traced_; // bin offset of each value read, when tracing
		};

	} // namespace detail

	using detail::BlockReader;

	// ------------------------------------------------------------- Database ---

	Database::Database() = default;
	Database::~Database() = default;

	void Database::addTypeSchema(std::shared_ptr<const TypeSchema> schema)
	{
		if (schema) schemaCandidates_.push_back(std::move(schema));
	}

	void Database::applyTypeSchema()
	{
		// The schema with the most attribute types at the right size wins; on a
		// tie, the one whose bools are as wide as the database's (the PS2's are 4
		// bytes, and FEPartData is all that tells its layouts from the PC's).
		uint32_t boolSize = 0;
		for (const auto& t : types_)
			if (t.name == "EA::Reflection::Bool") boolSize = t.size;
		// With a target, only its game's schemas: another game's could read a
		// type of the same size with other members.
		std::vector<std::shared_ptr<const TypeSchema>> pool;
		if (target_.known())
		{
			for (const auto& c : schemaCandidates_)
				if (c->game == target_.game &&
					std::find(c->platforms.begin(), c->platforms.end(), target_.platform) != c->platforms.end())
					pool.push_back(c);
			if (pool.empty())
				for (const auto& c : schemaCandidates_)
					if (c->game == target_.game) pool.push_back(c);
		}
		else
		{
			pool = schemaCandidates_;
		}
		size_t best = 0;
		for (const auto& c : pool)
		{
			size_t score = 0;
			for (const auto& t : types_)
			{
				const StructDef* st = c->findStruct(t.name);
				const EnumDef* en = c->findEnum(t.name);
				score += (st && st->size == t.size) || (en && en->size == t.size);
			}
			score = score * 2 + (c->boolSize == boolSize);
			if (score > best)
			{
				best = score;
				typeSchema_ = c;
			}
		}
		if (!typeSchema_) return;
		for (auto& t : types_)
		{
			const StructDef* st = typeSchema_->findStruct(t.name);
			const EnumDef* en = typeSchema_->findEnum(t.name);
			if (st && st->size == t.size)
			{
				t.schemaStruct = st;
				if (t.kind == TypeKind::Unknown) t.kind = TypeKind::Struct;
				if (!PointerLayoutFor(t.name, t.size))
				{
					PointerLayout pl = DerivePointerLayout(*st, *typeSchema_);
					if (!pl.strings.empty() || !pl.nested.empty() || !pl.unfollowed.empty())
						t.derivedPointers = std::make_shared<const PointerLayout>(std::move(pl));
				}
			}
			else if (en && en->size == t.size)
			{
				t.schemaEnum = en;
				if (t.kind == TypeKind::Unknown) t.kind = TypeKind::Enum;
			}
		}
		for (auto& c : classes_)
			for (FieldDef& f : c->fields) f.fieldEnum = fieldEnumFor(c->key, f);
	}

	const EnumDef* Database::fieldEnumFor(uint32_t classKey, const FieldDef& f) const
	{
		const EnumDef* en = typeSchema_ ? typeSchema_->fieldEnum(classKey, f.key) : nullptr;
		if (!en || !f.type || f.type->size != en->size) return nullptr;
		switch (f.type->kind)
		{
			case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16:
			case TypeKind::Int32: case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::UInt64: return en;
			default: return nullptr;
		}
	}

	const TypeInfo* Database::typeByHash(uint32_t hash) const
	{
		auto it = typeByHash_.find(hash);
		return it == typeByHash_.end() ? nullptr : it->second;
	}

	ClassDef* Database::findClass(uint32_t key) const
	{
		auto it = classByKey_.find(key);
		return it == classByKey_.end() ? nullptr : it->second;
	}

	Collection* Database::findCollection(uint32_t classKey, uint32_t key) const
	{
		auto it = collectionByKey_.find(CollectionId(classKey, key));
		return it == collectionByKey_.end() ? nullptr : it->second;
	}

	std::string Key64Name(uint64_t key)
	{
		char b[24];
		std::snprintf(b, sizeof(b), "0x%016llX", static_cast<unsigned long long>(key));
		return b;
	}

	std::optional<uint64_t> ParseKey64Name(std::string_view n)
	{
		if (n.size() != 18 || n[0] != '0' || (n[1] != 'x' && n[1] != 'X')) return std::nullopt;
		uint64_t v = 0;
		for (size_t i = 2; i < n.size(); ++i)
		{
			const char c = n[i];
			const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (d < 0) return std::nullopt;
			v = v << 4 | uint64_t(d);
		}
		return v;
	}

	uint32_t Database::keyFrom64(uint64_t key) const
	{
		if (!key) return 0;
		if (auto it = from64_.find(key); it != from64_.end()) return it->second;
		const std::string* known = names_.find64(key);
		const std::string name = known ? *known : Key64Name(key);
		if (!known) names_.add(name);
		const uint32_t k = StringHash32(name);
		if (auto it = to64_.find(k); it != to64_.end() && it->second != key)
			throw FormatError("the 64-bit keys " + Key64Name(it->second) + " and " + Key64Name(key) + " both stand for " + names_.format(k));
		from64_[key] = k;
		to64_[k] = key;
		return k;
	}

	std::optional<uint64_t> Database::findKey64(uint32_t key) const
	{
		if (!key) return 0;
		if (auto it = to64_.find(key); it != to64_.end()) return it->second;
		const std::string* n = names_.find(key);
		if (!n) return std::nullopt;
		if (const auto k = ParseKey64Name(*n)) return *k;
		return StringHash64(*n);
	}

	uint64_t Database::keyTo64(uint32_t key) const
	{
		if (const auto k = findKey64(key)) return *k;
		throw FormatError("no 64-bit key for " + names_.format(key) + ": its name isn't known");
	}

	uint64_t Database::exportId64(uint32_t classKey, const std::string& name) const
	{
		return StringHash64(names_.format(classKey) + "/" + name);
	}

	PackSummary DescribePack(ByteSpan fileData)
	{
		PackSummary out;
		PackFile pf = ReadPack(fileData);
		for (PackVault& pv : pf.vaults)
		{
			out.vaults.push_back(pv.name);
			const RawVault raw = ParseRawVault(pv.name, std::move(pv.bin), std::move(pv.vlt));
			for (const auto& e : raw.exports)
				out.hasDatabase |= e.type == ExportType::DatabaseLoadData;
		}
		return out;
	}

	LoadedPack& Database::loadPack(const std::string& path)
	{
		const Bytes data = ReadFile(path);
		return loadPack(path, data);
	}

	LoadedPack& Database::loadPack(const std::string& path, ByteSpan fileData)
	{
		if (!target_.known() && packs_.empty()) target_ = DetectTarget(path, fileData).target;
		PackFile pf = ReadPack(fileData);
		auto pack = std::make_unique<LoadedPack>();
		pack->path = path;
		pack->format = pf.format;
		pack->compression = pf.compression;
		pack->align = pf.align;
		for (PackVault& pv : pf.vaults)
		{
			auto v = std::make_unique<Vault>();
			v->name = pv.name;
			v->pack = pack.get();
			v->raw = ParseRawVault(pv.name, std::move(pv.bin), std::move(pv.vlt));
			pack->vaults.push_back(std::move(v));
		}
		// Schema first: a pack may hold the db vault after others.
		std::vector<Vault*> order;
		for (auto& v : pack->vaults) order.push_back(v.get());
		std::stable_partition(order.begin(), order.end(), [](const Vault* v)
		{
			for (const auto& e : v->raw.exports)
				if (e.type == ExportType::DatabaseLoadData) return true;
			return false;
		});
		for (Vault* v : order)
			loadVault(*v);
		linkParents();
		packs_.push_back(std::move(pack));
		return *packs_.back();
	}

	LoadedPack& Database::addPack(const std::string& path, PackFormat format)
	{
		auto pack = std::make_unique<LoadedPack>();
		pack->path = path;
		pack->format = format;
		pack->dirty = true; // its file isn't there yet
		packs_.push_back(std::move(pack));
		return *packs_.back();
	}

	bool Database::removePack(const LoadedPack& pack)
	{
		const auto it = std::find_if(packs_.begin(), packs_.end(), [&](const auto& p)
		{
			return p.get() == &pack;
		});
		if (it == packs_.end()) return false;
		for (const auto& v : pack.vaults)
			if (!v->collections.empty() || !v->classes.empty() || v->isDatabase) return false;
		packs_.erase(it);
		return true;
	}

	std::string Database::unloadProblem(const LoadedPack& pack) const
	{
		if (std::none_of(packs_.begin(), packs_.end(), [&](const auto& p) { return p.get() == &pack; }))
			return "it isn't open in this database";
		for (const auto& v : pack.vaults)
		{
			if (v->isDatabase) return "it holds the database's type table and classes";
			if (!v->classes.empty()) return "its vault " + v->name + " holds classes";
		}
		return {};
	}

	std::string Database::unloadPack(const LoadedPack& pack)
	{
		if (std::string problem = unloadProblem(pack); !problem.empty()) return problem;
		auto gone = [&](const Collection* c)
		{
			return c && c->vault && c->vault->pack == &pack;
		};
		std::vector<const Collection*> removed;
		for (const auto& c : collections_)
			if (gone(c.get())) removed.push_back(c.get());
		for (auto& cls : classes_)
			cls->collections.erase(std::remove_if(cls->collections.begin(), cls->collections.end(), gone), cls->collections.end());
		// The index: a name another pack also defines is that pack's again
		// (the last loaded of them, as findCollection has it).
		std::unordered_set<uint64_t> erased;
		for (const Collection* c : removed)
		{
			const uint64_t id = CollectionId(c->classKey, c->key);
			if (auto it = collectionByKey_.find(id); it != collectionByKey_.end() && it->second == c)
			{
				collectionByKey_.erase(it);
				erased.insert(id);
			}
		}
		for (const auto& c : collections_)
			if (!gone(c.get()) && erased.count(CollectionId(c->classKey, c->key)))
				collectionByKey_[CollectionId(c->classKey, c->key)] = c.get(); // the last one stands
		// Re-link what stays: children that went, and parents that did.
		for (auto& c : collections_)
		{
			if (gone(c.get())) continue;
			c->children.erase(std::remove_if(c->children.begin(), c->children.end(), gone), c->children.end());
		}
		for (auto& c : collections_)
		{
			if (gone(c.get()) || !gone(c->parent)) continue;
			c->parent = findCollection(c->classKey, c->parentKey);
			if (gone(c->parent)) c->parent = nullptr;
			if (c->parent) c->parent->children.push_back(c.get());
		}
		collections_.erase(std::remove_if(collections_.begin(), collections_.end(), [&](const std::unique_ptr<Collection>& c) { return gone(c.get()); }),
			collections_.end());
		packs_.erase(std::find_if(packs_.begin(), packs_.end(), [&](const auto& p) { return p.get() == &pack; }));
		return {};
	}

	void Database::loadVault(Vault& vault)
	{
		const RawVault& raw = vault.raw;
		if (raw.keys64) keys64_ = true;
		for (const auto& d : raw.dependencies)
			names_.add(d.name);
		if (raw.strESize)
		{
			const ByteReader br = raw.binReader();
			for (uint32_t off = raw.strEOffset; off < raw.strEOffset + raw.strESize;)
			{
				const std::string s = br.cstr(off);
				names_.add(s);
				binNames_.add(s);
				off += uint32_t(s.size()) + 1;
			}
		}
		BlockReader blocks(*this, raw, vault);
		for (const auto& e : raw.exports)
			if (e.type == ExportType::DatabaseLoadData)
				loadDatabaseExport(vault, e, blocks);
		if (!hasSchema())
			for (const auto& e : raw.exports)
				if (e.type == ExportType::CollectionLoadData)
					throw FormatError("vault '" + vault.name + "' has collections but no database is loaded; open attributes.bin first");
		// Classes before collections: a vault's collections may use its own classes.
		for (const auto& e : raw.exports)
			if (e.type == ExportType::ClassLoadData)
				loadClassExport(vault, e, blocks);
		for (const auto& e : raw.exports)
		{
			if (e.type == ExportType::CollectionLoadData)
				loadCollectionExport(vault, e, blocks);
			else if (e.type == ExportType::VaultData)
			{
				bool pointers = false;
				for (const auto& [loc, target] : raw.vltFixups) pointers |= loc >= e.offset && loc < uint64_t(e.offset) + e.size;
				if (pointers)
				{
					vault.warnings.push_back("the vault data export " + names_.format(uint32_t(e.id)) + " holds pointers");
					vault.exactlyWritable = false;
				}
				const auto b = raw.vltReader().slice(e.offset, e.size);
				vault.dataExports.push_back(DataExport{ e.id, e.type, Bytes(b.begin(), b.end()) });
			}
			else if (e.type != ExportType::DatabaseLoadData && e.type != ExportType::ClassLoadData)
			{
				vault.warnings.push_back("unknown export type " + names_.format(e.type) + " for " + names_.format(e.id));
				vault.exactlyWritable = false;
			}
		}
		// Export order, as stored.
		size_t ci = 0, coi = 0, di = 0;
		for (const auto& e : raw.exports)
		{
			if (e.type == ExportType::DatabaseLoadData && vault.isDatabase)
				vault.exportOrder.push_back({ ExportRef::Kind::Database, nullptr, nullptr });
			else if (e.type == ExportType::ClassLoadData && ci < vault.classes.size())
				vault.exportOrder.push_back({ ExportRef::Kind::Class, vault.classes[ci++], nullptr });
			else if (e.type == ExportType::CollectionLoadData && coi < vault.collections.size())
				vault.exportOrder.push_back({ ExportRef::Kind::Collection, nullptr, vault.collections[coi++] });
			else if (e.type == ExportType::VaultData && di < vault.dataExports.size())
				vault.exportOrder.push_back({ ExportRef::Kind::Data, nullptr, nullptr, uint32_t(di++) });
		}
		blocks.finish();
	}

	void Database::loadDatabaseExport(Vault& vault, const ExportEntry& e, BlockReader& blocks)
	{
		if (hasSchema())
		{
			vault.warnings.push_back("a second database definition was ignored");
			vault.exactlyWritable = false;
			return;
		}
		vault.isDatabase = true;
		vault.databaseExportId = e.id;
		const RawVault& raw = vault.raw;
		const ByteReader r = raw.vltReader();
		numClassesDeclared_ = r.u32(e.offset);
		// The early Most Wanted demos have a u32 more at +4 (0 in both; the game
		// doesn't read it).
		const uint32_t x = raw.keys64 ? 4 : 0;
		if (raw.keys64) databaseWord4_ = r.u32(e.offset + 4);
		defaultDataSize_ = r.u32(e.offset + 4 + x);
		const uint32_t numTypes = r.u32(e.offset + 8 + x);
		const uint32_t* names = raw.vltPtr(e.offset + 12 + x);
		if (!names)
			throw FormatError("DatabaseLoadData has no type name block");
		vault.typenamesSeq = blocks.vltSeq(e.offset + 12 + x);
		if (e.size < 16 + x + numTypes * 4)
			throw FormatError("DatabaseLoadData is too small for its type table");
		const ByteReader br = raw.binReader();
		types_.reserve(numTypes);
		uint32_t off = *names;
		for (uint32_t i = 0; i < numTypes; ++i)
		{
			TypeInfo t;
			t.name = br.cstr(off);
			off += uint32_t(t.name.size()) + 1;
			t.hash = StringHash32(t.name);
			if (raw.keys64) t.hash64 = StringHash64(t.name);
			t.size = r.u32(e.offset + 16 + x + i * 4);
			t.index = i;
			t.kind = ClassifyType(t.name, t.size);
			names_.add(t.name);
			types_.push_back(std::move(t));
		}
		for (const auto& t : types_)
		{
			typeByHash_[t.hash] = &t;
			if (raw.keys64) typeBy64_[t.hash64] = &t;
		}
		// Types fields use that the table leaves out; the writer writes the table as read.
		for (const UnlistedType& u : kUnlistedTypes)
		{
			TypeInfo t;
			t.name = u.name;
			t.hash = StringHash32(t.name);
			if (typeByHash_.count(t.hash)) continue;
			t.size = u.size;
			t.index = UINT32_MAX;
			names_.add(t.name);
			typeByHash_[t.hash] = &unlistedTypes_.emplace_back(std::move(t));
		}
		applyTypeSchema();
	}

	void Database::loadClassExport(Vault& vault, const ExportEntry& e, BlockReader& blocks)
	{
		const RawVault& raw = vault.raw;
		const ByteReader r = raw.vltReader();
		// { key, reserve, numDefs, defs*, staticSize, static*, layoutSize, u16
		// keyShift, u16 count }; Carbon's demos have { key, reserve, numDefs,
		// defs*, layoutSize, u32 keyShift, u32 count, staticSize, static* }, and
		// Most Wanted's own vaults (no Vers chunk) the same without the static
		// data: its classes have none (no Definition flag for it).
		// The early Most Wanted demos: 0x20 { u64 key; u32 reserve; u32 numDefs;
		// defs*; u32 layoutSize; u32 keyShift; u32 count }, 24-byte definitions.
		const bool mw = raw.mostWantedLayout, noStatic = raw.mostWantedOwn(), k64 = raw.keys64;
		if (e.size < (k64 ? 0x20u : noStatic ? 0x1cu : mw ? 0x24u : 0x20u))
			throw FormatError("ClassLoadData export is too small");
		const uint32_t staticAt = mw ? 32 : 20, x = k64 ? 4 : 0;
		auto cls = std::make_unique<ClassDef>();
		cls->key = k64 ? keyFrom64(r.u64(e.offset)) : r.u32(e.offset);
		cls->collectionReserve = r.u32(e.offset + 4 + x);
		const uint32_t numDefs = r.u32(e.offset + 8 + x);
		cls->staticSize = noStatic ? 0 : r.u32(e.offset + staticAt - 4);
		cls->layoutSize = r.u32(e.offset + (mw ? 16 : 24) + x);
		cls->layoutKeyShift = mw ? uint16_t(r.u32(e.offset + 20 + x)) : r.u16(e.offset + 28);
		cls->layoutCount = mw ? uint16_t(r.u32(e.offset + 24 + x)) : r.u16(e.offset + 30);
		cls->vault = &vault;
		cls->defsSeq = blocks.vltSeq(e.offset + 12 + x);
		if (!numDefs) blocks.vltNull(e.offset + 12 + x);
		if (!noStatic)
		{
			cls->staticSeq = blocks.vltSeq(e.offset + staticAt);
			if (!cls->staticSize) blocks.vltNull(e.offset + staticAt);
		}

		const ByteReader br = raw.binReader();
		if (numDefs)
		{
			const uint32_t* defs = raw.vltPtr(e.offset + 12 + x);
			if (!defs)
				throw FormatError("class " + names_.format(cls->key) + " has definitions but no pointer to them");
			const uint32_t size = k64 ? 24 : 16, at = k64 ? 16 : 8;
			for (uint32_t i = 0; i < numDefs; ++i)
			{
				const uint32_t d = *defs + i * size;
				FieldDef f;
				if (k64)
				{
					f.key = keyFrom64(br.u64(d));
					auto t = typeBy64_.find(br.u64(d + 8));
					f.typeHash = t == typeBy64_.end() ? 0 : t->second->hash;
				}
				else
				{
					f.key = br.u32(d);
					f.typeHash = br.u32(d + 4);
				}
				f.offset = br.u16(d + at);
				f.size = br.u16(d + at + 2);
				f.maxCount = br.u16(d + at + 4);
				f.flags = br.u8(d + at + 6);
				f.alignment = br.u8(d + at + 7);
				f.type = typeByHash(f.typeHash);
				f.fieldEnum = fieldEnumFor(cls->key, f);
				if (!f.type)
				{
					vault.warnings.push_back("field " + names_.format(f.key) + " of " + names_.format(cls->key) + " has unknown type " +
											 (k64 ? Key64Name(br.u64(d + 8)) : names_.format(f.typeHash)));
					if (k64) vault.exactlyWritable = false;
				}
				cls->fields.push_back(f);
			}
		}
		if (cls->staticSize)
		{
			if (const uint32_t* p = raw.vltPtr(e.offset + staticAt))
			{
				cls->staticData = blocks.read(*p, cls->staticSize);
				for (const auto& f : cls->fields)
					if (f.isStatic() && !f.isArray())
						blocks.attach(cls->staticData, *p, f.offset, f.type);
			}
		}
		if (classByKey_.count(cls->key))
		{
			vault.warnings.push_back("class " + names_.format(cls->key) + " is defined twice; the first definition is kept");
			vault.exactlyWritable = false;
			return;
		}
		classByKey_[cls->key] = cls.get();
		vault.classes.push_back(cls.get());
		classes_.push_back(std::move(cls));
	}

	void Database::loadCollectionExport(Vault& vault, const ExportEntry& e, BlockReader& blocks)
	{
		const RawVault& raw = vault.raw;
		const ByteReader r = raw.vltReader();
		const Endian endian = raw.endian;
		// The early Most Wanted demos: a 0x30 header with u64 keys, u64 types and
		// 16-byte entries (u64 key, mData at +8).
		const bool k64 = raw.keys64;
		const uint32_t header = k64 ? 0x30 : 0x20, layoutAt = k64 ? 0x28 : 0x1C, typeSize = k64 ? 8 : 4;
		const uint32_t entrySize = k64 ? 16 : 12, dataAt = k64 ? 8 : 4, x = k64 ? 12 : 0;
		if (e.size < header)
			throw FormatError("CollectionLoadData export is too small");
		auto col = std::make_unique<Collection>();
		col->key = k64 ? keyFrom64(r.u64(e.offset)) : r.u32(e.offset);
		col->classKey = k64 ? keyFrom64(r.u64(e.offset + 8)) : r.u32(e.offset + 4);
		col->parentKey = k64 ? keyFrom64(r.u64(e.offset + 16)) : r.u32(e.offset + 8);
		const uint32_t tableReserve = r.u32(e.offset + 12 + x);
		col->tableKeyShift = r.u32(e.offset + 16 + x);
		const uint32_t numEntries = r.u32(e.offset + 20 + x);
		// u16 types, u16 their padded length; Most Wanted's own: u32 types, unpadded.
		const bool mwOwn = raw.mostWantedOwn();
		const uint32_t numTypes = mwOwn ? r.u32(e.offset + 24 + x) : r.u16(e.offset + 24);
		const uint32_t typesLen = mwOwn ? numTypes : r.u16(e.offset + 26);
		col->vault = &vault;
		col->exportId = e.id;
		col->layoutSeq = blocks.vltSeq(e.offset + layoutAt);
		if (!raw.mostWantedLayout && raw.hasVltPtr(e.offset + layoutAt) && !k64) col->layoutSlotStored = r.u32(e.offset + layoutAt);
		blocks.vltNull(e.offset + layoutAt); // a class without a layout: listed as null in Most Wanted's layout
		col->cls = findClass(col->classKey);
		const std::string where = names_.format(col->classKey) + "/" + names_.format(col->key);
		if (!col->cls)
			throw FormatError("collection " + where + " uses an unknown class");
		if (header + size_t(typesLen) * typeSize + size_t(numEntries) * entrySize > e.size || numTypes > typesLen)
			throw FormatError("collection " + where + " is larger than its export");
		if (tableReserve != numEntries)
		{
			vault.warnings.push_back("collection " + where + " reserves " + std::to_string(tableReserve) + " table slots for " +
									 std::to_string(numEntries) + " entries; it will be written with " + std::to_string(numEntries));
			vault.exactlyWritable = false;
		}

		col->typeHashes.resize(numTypes);
		for (uint32_t i = 0; i < numTypes; ++i)
		{
			if (!k64)
			{
				col->typeHashes[i] = r.u32(e.offset + 0x20 + i * 4);
				continue;
			}
			const uint64_t t = r.u64(e.offset + header + i * 8);
			auto it = typeBy64_.find(t);
			if (it == typeBy64_.end()) throw FormatError("collection " + where + " lists an unknown type " + Key64Name(t));
			col->typeHashes[i] = it->second->hash;
		}

		const ClassDef& cls = *col->cls;
		const ByteReader br = raw.binReader();

		// Layout fields. The stored layout ends at the last field; the class's
		// layout size is that rounded up, and the next block may start inside it.
		if (cls.layoutSize)
		{
			const uint32_t* lp = raw.vltPtr(e.offset + layoutAt);
			if (!lp)
				throw FormatError("collection " + where + " has no layout block");
			// A layout array's capacity is in its inline header (the definition's
			// maxCount can be 0), so read the headers before sizing the block.
			uint32_t used = 0;
			for (const auto& f : cls.fields)
			{
				if (!f.inLayout()) continue;
				const uint16_t capacity = f.isArray() ? br.u16(*lp + f.offset) : 1;
				used = std::max(used, LayoutOffset(f) + LayoutExtent(f, capacity));
			}
			if (used > LayoutBytes(cls))
				throw FormatError("collection " + where + " has layout fields past the class's layout size");
			col->layout = blocks.read(*lp, used);
			for (const auto& f : cls.fields)
			{
				if (!f.inLayout()) continue;
				if (f.isArray())
				{
					const uint16_t capacity = load16(col->layout.bytes.data() + f.offset, endian);
					for (uint32_t k = 0; k < capacity; ++k)
						blocks.attach(col->layout, *lp, f.offset + LayoutArrayHeader(f) + k * f.size, f.type);
				}
				else
				{
					blocks.attach(col->layout, *lp, LayoutOffset(f), f.type);
				}
			}
			col->layout.bytes.resize(LayoutBytes(cls), 0);

			for (const auto& f : cls.fields)
			{
				if (!f.inLayout()) continue;
				Attribute a;
				a.key = f.key;
				a.def = &f;
				a.type = f.type;
				a.inLayout = true;
				a.elemSize = f.size;
				uint32_t first = LayoutOffset(f);
				if (f.isArray())
				{
					const uint8_t* h = col->layout.bytes.data() + f.offset;
					a.isArray = true;
					a.capacity = load16(h, endian);
					a.count = load16(h + 2, endian);
					const uint16_t esz = load16(h + 4, endian);
					a.encodedTypePad = load16(h + 6, endian);
					if (a.count > a.capacity || (f.maxCount && a.capacity > f.maxCount) || (a.capacity && esz != f.size))
						throw FormatError("layout array " + names_.format(f.key) + " of " + where + " has an inconsistent header");
					first = f.offset + LayoutArrayHeader(f);
				}
				else
				{
					a.count = 1;
				}
				const uint32_t bytes = a.count * f.size;
				a.data.bytes.assign(col->layout.bytes.begin() + first, col->layout.bytes.begin() + first + bytes);
				for (const auto& p : col->layout.ptrs)
				{
					if (p.offset >= first && p.offset + 4 <= first + bytes)
					{
						PtrSlot s = p;
						s.offset -= first;
						a.data.ptrs.push_back(std::move(s));
					}
				}
				for (const auto& n : col->layout.nulls)
					if (n.offset >= first && n.offset + 4 <= first + bytes) a.data.nulls.push_back({ n.offset - first, n.seq });
				col->attributes.push_back(std::move(a));
			}
		}

		// Entries.
		const uint32_t entries = e.offset + header + typesLen * typeSize;
		for (uint32_t i = 0; i < numEntries; ++i)
		{
			const uint32_t en = entries + i * entrySize;
			Attribute a;
			a.key = k64 ? keyFrom64(r.u64(en)) : r.u32(en);
			const uint16_t typeIndex = r.u16(en + dataAt + 4);
			a.nodeFlags = r.u8(en + dataAt + 6);
			a.entryFlags = r.u8(en + dataAt + 7);
			if (mwOwn)
			{
				if (a.nodeFlags & NodeFlag::ByValue)
					throw FormatError("entry " + names_.format(a.key) + " of " + where + " has node flag 64, which Most Wanted doesn't use");
				if (a.nodeFlags & NodeFlag::MostWantedByValue) a.nodeFlags = uint8_t((a.nodeFlags & ~NodeFlag::MostWantedByValue) | NodeFlag::ByValue);
			}
			if (typeIndex >= numTypes)
				throw FormatError("entry " + names_.format(a.key) + " of " + where + " has a bad type index");
			a.type = typeByHash(col->typeHashes[typeIndex]);
			a.def = cls.field(a.key);
			if (!a.type)
				throw FormatError("entry " + names_.format(a.key) + " of " + where + " has an unknown type");
			// Kept and written back as read, typed by the collection's type list
			// (World's coop_pursuit_1 event has four fields its class dropped).
			if (!a.def) vault.warnings.push_back(where + " sets " + names_.format(a.key) + ", which its class doesn't define; it is kept as it is");
			const uint32_t valueSize = a.type->size;
			const uint32_t* target = raw.vltPtr(en + dataAt);
			a.dataSeq = blocks.vltSeq(en + dataAt);
			col->loadedEntryKeys.push_back(a.key);

			if (a.nodeFlags & NodeFlag::ByValue)
			{
				// The value is the 4-byte mData itself (a char* for Text).
				a.elemSize = valueSize;
				a.count = 1;
				if (valueSize > 4)
					throw FormatError("entry " + names_.format(a.key) + " of " + where + " is by-value but " +
									  std::to_string(valueSize) + " bytes");
				auto s = r.slice(en + dataAt, valueSize);
				a.data.bytes.assign(s.begin(), s.end());
				if (target)
				{
					a.data.ptrs.push_back(blocks.stringSlot(0, *target, a.dataSeq));
					a.dataSeq = kNewPtr; // the slot carries it
					store32(a.data.bytes.data(), 0, endian);
				}
			}
			else if (a.nodeFlags & NodeFlag::Array)
			{
				if (!target)
					throw FormatError("array " + names_.format(a.key) + " of " + where + " has no data");
				a.isArray = true;
				a.capacity = br.u16(*target);
				a.count = br.u16(*target + 2);
				a.elemSize = br.u16(*target + 4);
				a.encodedTypePad = br.u16(*target + 6);
				if (a.count > a.capacity || (a.elemSize != valueSize && valueSize != 0))
					throw FormatError("array " + names_.format(a.key) + " of " + where + " has an inconsistent header");
				const uint32_t hdr = ArrayHeaderSize(a.encodedTypePad);
				if (a.count != a.capacity)
				{
					// No retail file has spare capacity; earlier builds of the editor left
					// it when an array shrank. The writer zeroes it, so zeros write exactly.
					const ByteSpan bin = br.data();
					const size_t spare = size_t(*target) + hdr + size_t(a.count) * a.elemSize;
					const size_t spareBytes = size_t(a.capacity - a.count) * a.elemSize;
					const bool zeroed = spare <= bin.size() && spareBytes <= bin.size() - spare &&
						std::all_of(bin.begin() + spare, bin.begin() + spare + spareBytes, [](uint8_t b) { return b == 0; });
					if (!zeroed)
					{
						vault.warnings.push_back("array " + names_.format(a.key) + " of " + where + " has data past its count; it is written zeroed");
						vault.exactlyWritable = false;
					}
				}
				a.data = blocks.read(*target + hdr, a.count * a.elemSize);
				for (uint32_t k = 0; k < a.count; ++k)
					blocks.attach(a.data, *target + hdr, k * a.elemSize, a.type);
			}
			else
			{
				if (!target)
					throw FormatError("entry " + names_.format(a.key) + " of " + where + " has no data");
				a.elemSize = valueSize;
				a.count = 1;
				a.data = blocks.read(*target, valueSize);
				blocks.attach(a.data, *target, 0, a.type);
			}
			col->attributes.push_back(std::move(a));
		}

		collectionByKey_[CollectionId(col->classKey, col->key)] = col.get();
		col->cls->collections.push_back(col.get());
		vault.collections.push_back(col.get());
		collections_.push_back(std::move(col));
	}

	void Database::linkParents()
	{
		for (auto& c : collections_)
			c->children.clear();
		for (auto& c : collections_)
		{
			c->parent = c->parentKey ? findCollection(c->classKey, c->parentKey) : nullptr;
			if (c->parent)
				c->parent->children.push_back(c.get());
		}
	}

} // namespace attrib
