#include "ClassDesign.h"

#include "ClassList.h"
#include "Edit.h"
#include "Hash.h"
#include "Values.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace attrib
{

	const char* FieldStorageName(FieldStorage s)
	{
		switch (s)
		{
			case FieldStorage::Layout: return "layout";
			case FieldStorage::Static: return "static";
			default: return "entry";
		}
	}

	const FieldDesign* ClassDesign::field(uint32_t fieldKey) const
	{
		for (const FieldDesign& f : fields)
			if (f.key == fieldKey) return &f;
		return nullptr;
	}

	namespace
	{
		uint32_t Align(uint32_t v, uint32_t a)
		{
			return uint32_t(AlignUp(v, a ? a : 1));
		}

		uint8_t Log2(uint32_t a)
		{
			uint8_t n = 0;
			while (n < 7 && (2u << n) <= a) ++n;
			return n;
		}

		FieldStorage StorageOf(const FieldDef& f)
		{
			return f.inLayout() ? FieldStorage::Layout : f.isStatic() ? FieldStorage::Static : FieldStorage::Entry;
		}

		bool IsHandlerTypeName(const std::string& n)
		{
			// Structs the game gives a handler that hold none of the kinds below
			// directly (their pointers lead to RefSpecs): Undercover's CCarSlotEntry,
			// World's PowerUps records.
			static const char* const kListed[] = { "CCarSlotEntry", "PowerUps::EmitterEffectAndTarget", "PowerUps::GameplayEffectAndTarget" };
			for (const char* l : kListed)
				if (n == l) return true;
			return n == "Attrib::RefSpec" || n == "Curve" || n.rfind("Attrib::Gen::ClassRefSpec_", 0) == 0;
		}

		bool StructHasHandler(const StructDef* st, int depth)
		{
			if (!st || depth > 8) return false;
			for (const MemberDef& m : st->members)
				if (m.prim == MemberPrim::None && m.st && (IsHandlerTypeName(m.st->name) || StructHasHandler(m.st, depth + 1))) return true;
			return false;
		}

		// Undercover (PC, 360, PS3) and World mark the fields of types with a
		// constructor or destructor (DefFlag::HasHandler); the games before don't.
		bool UsesHandlerFlag(const Database& db)
		{
			const Target& t = db.target();
			if (t.game == Game::World ||
				(t.game == Game::Undercover && (t.platform == Platform::PC || t.platform == Platform::Xbox360 || t.platform == Platform::PS3)))
				return true;
			for (const auto& c : db.classes())
				for (const FieldDef& f : c->fields)
					if (f.flags & DefFlag::HasHandler) return true;
			return false;
		}

		// Laying out goes by the types alone, so a retail class restored into a
		// database whose other classes were deleted comes back as it was.
		bool HandlerOf(bool usesHandler, const TypeInfo& type)
		{
			if (!usesHandler) return false;
			switch (type.kind)
			{
				case TypeKind::RefSpec:
				case TypeKind::ClassRefSpec:
				case TypeKind::Curve:
					return true;
				default:
					return IsHandlerTypeName(type.name) || StructHasHandler(type.schemaStruct, 0);
			}
		}

		bool SameDef(const FieldDef& a, const FieldDef& b)
		{
			return a.key == b.key && a.typeHash == b.typeHash && a.offset == b.offset && a.size == b.size && a.maxCount == b.maxCount &&
				a.flags == b.flags && a.alignment == b.alignment;
		}

		bool SameLayout(const ClassDef& a, const ClassDef& b)
		{
			if (a.fields.size() != b.fields.size() || a.layoutSize != b.layoutSize || a.layoutCount != b.layoutCount ||
				a.layoutKeyShift != b.layoutKeyShift || a.staticSize != b.staticSize || a.collectionReserve != b.collectionReserve)
				return false;
			for (size_t i = 0; i < a.fields.size(); ++i)
				if (!SameDef(a.fields[i], b.fields[i])) return false;
			return true;
		}

		// The problems of the fields alone (LayOutClass refuses them).
		std::vector<std::string> FieldProblems(const Database& db, const ClassDesign& d)
		{
			std::vector<std::string> out;
			std::set<uint32_t> seen;
			const KeyNames& names = db.names();
			uint32_t layoutEnd = 0, staticEnd = 0;
			for (const FieldDesign& f : d.fields)
			{
				const std::string label = !f.name.empty() ? f.name : f.key ? names.format(f.key) : std::string("a field");
				if (!f.key && f.name.empty())
				{
					out.push_back("a field has no name");
					continue;
				}
				if (!f.name.empty() && f.key != StringHash32(f.name)) out.push_back(label + ": its key isn't its name's hash");
				if (db.keys64() && f.name.empty() && !names.find(f.key) && !db.findKey64(f.key))
					out.push_back(label + ": a database with 64-bit keys needs every field's name");
				if (!seen.insert(f.key).second) out.push_back("two fields named " + label);
				const TypeInfo* t = db.typeByHash(f.typeHash);
				if (!t && !f.size)
				{
					out.push_back(label + ": the type table has no type " + names.format(f.typeHash));
					continue;
				}
				const uint32_t size = f.size ? *f.size : t->size;
				if (!size) out.push_back(label + ": its type has no size");
				if (f.storage == FieldStorage::Static && f.array) out.push_back(label + ": a static field can't be an array");
				if (f.storage == FieldStorage::Layout)
				{
					const uint32_t align = 1u << (f.alignment ? *f.alignment : t ? NaturalAlignment(*t) : 2);
					const uint32_t at = f.offset ? *f.offset : Align(layoutEnd, align);
					const uint32_t header = f.array ? (align > 8 ? 16u : 8u) : 0;
					layoutEnd = std::max(layoutEnd, at + header + size * (f.array ? f.count : 1u));
				}
				else if (f.storage == FieldStorage::Static)
				{
					const uint32_t align = 1u << (f.alignment ? *f.alignment : t ? NaturalAlignment(*t) : 2);
					staticEnd = std::max(staticEnd, (f.offset ? *f.offset : Align(staticEnd, align)) + size);
				}
			}
			// Definitions keep offsets in 16 bits.
			if (layoutEnd > 0xFFFF) out.push_back("the layout takes " + std::to_string(layoutEnd) + " bytes, more than the 65535 a class can have");
			if (staticEnd > 0xFFFF) out.push_back("the static values take " + std::to_string(staticEnd) + " bytes, more than the 65535 a class can have");
			return out;
		}

		uint64_t FileKey(const Database& db, uint32_t key, const std::string& name)
		{
			if (!db.keys64()) return key;
			if (const std::optional<uint64_t> k = db.findKey64(key)) return *k;
			return StringHash64(name.empty() ? db.names().format(key) : name);
		}

		Vault* DatabaseVault(Database& db)
		{
			for (const auto& pk : db.packs())
				for (const auto& v : pk->vaults)
					if (v->isDatabase) return v.get();
			return nullptr;
		}

		void EraseCollectionExport(Vault& vault, const Collection& col)
		{
			auto& order = vault.exportOrder;
			order.erase(std::remove_if(order.begin(), order.end(), [&](const ExportRef& r)
			{
				return r.collection == &col;
			}), order.end());
			auto& v = vault.collections;
			v.erase(std::remove(v.begin(), v.end(), &col), v.end());
		}

		// An attribute holding no value yet for `f` (layout arrays get their slots).
		Attribute EmptyAttribute(const FieldDef& f, bool inLayout)
		{
			Attribute a;
			a.key = f.key;
			a.def = &f;
			a.type = f.type;
			a.inLayout = inLayout;
			a.isArray = f.isArray();
			a.elemSize = f.size;
			a.count = a.isArray ? 0 : 1;
			a.capacity = a.isArray && inLayout ? f.maxCount : 0;
			a.encodedTypePad = a.isArray ? ArrayPad(f) : 0;
			a.data.bytes.assign(size_t(a.count) * a.elemSize, 0);
			return a;
		}

		// Keeps the first `n` elements of an attribute's value.
		void Truncate(Attribute& a, uint32_t n)
		{
			if (a.count <= n) return;
			const uint32_t end = n * a.elemSize;
			a.data.bytes.resize(end);
			auto& p = a.data.ptrs;
			p.erase(std::remove_if(p.begin(), p.end(), [&](const PtrSlot& s)
			{
				return s.offset >= end;
			}), p.end());
			auto& nl = a.data.nulls;
			nl.erase(std::remove_if(nl.begin(), nl.end(), [&](const DataBlock::NullPtr& s)
			{
				return s.offset >= end;
			}), nl.end());
			a.count = n;
		}

		// `src`'s value as the value of field `f`, kept in the layout or as an entry.
		Attribute Reshape(const Attribute& src, const FieldDef& f, bool inLayout)
		{
			Attribute a = src;
			DetachCopy(a);
			a.def = &f;
			a.type = f.type;
			a.inLayout = inLayout;
			a.isArray = f.isArray();
			a.elemSize = f.size;
			a.entryFlags = 0;
			if (inLayout)
			{
				a.nodeFlags = 0;
				if (a.isArray)
				{
					a.capacity = f.maxCount;
					Truncate(a, a.capacity);
					a.encodedTypePad = ArrayPad(f);
				}
			}
			else
			{
				if (a.isArray)
				{
					a.capacity = uint16_t(a.count);
					a.encodedTypePad = ArrayPad(f);
					a.nodeFlags = NodeFlag::Array;
				}
				else
				{
					a.nodeFlags = a.elemSize <= 4 ? NodeFlag::ByValue : 0;
				}
				if (f.flags & DefFlag::HasHandler) a.nodeFlags |= NodeFlag::HasHandler;
			}
			return a;
		}
	} // namespace

	uint32_t CollectionReserveFor(size_t collections)
	{
		return std::max<uint32_t>(1, uint32_t(collections * 7 / 5));
	}

	uint8_t NaturalAlignment(const TypeInfo& type)
	{
		if (type.schemaStruct) return Log2(type.schemaStruct->align);
		auto bySize = [&](uint32_t most)
		{
			uint32_t a = 1;
			while (a < most && type.size % (a * 2) == 0) a *= 2;
			return Log2(a);
		};
		switch (type.kind)
		{
			case TypeKind::Vector4:
			case TypeKind::Quaternion:
			case TypeKind::Matrix:
			case TypeKind::FloatColour:
				return 4;
			case TypeKind::Int64:
			case TypeKind::UInt64:
			case TypeKind::Double:
				return 3;
			case TypeKind::Colour: // four bytes
				return 0;
			default:
				// Floats, vectors, keys and the like: on 4 at most.
				return bySize(4);
		}
	}

	bool TypeHasHandler(const Database& db, const TypeInfo& type)
	{
		return HandlerOf(UsesHandlerFlag(db), type);
	}

	const TypeInfo* FindTypeByName(const Database& db, std::string_view name)
	{
		if (const TypeInfo* t = db.typeByHash(StringHash32(name))) return t;
		auto lower = [](std::string_view s)
		{
			std::string o(s);
			for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
			return o;
		};
		const std::string want = lower(name);
		const TypeInfo* found = nullptr;
		for (const TypeInfo& t : db.types())
		{
			std::string label = t.name;
			for (const char* prefix : { "EA::Reflection::", "Attrib::Types::", "Attrib::" })
				if (label.rfind(prefix, 0) == 0)
				{
					label = label.substr(std::char_traits<char>::length(prefix));
					break;
				}
			if (lower(label) != want) continue;
			if (found) return nullptr; // two types go by it
			found = &t;
		}
		return found;
	}

	std::vector<std::string> DesignProblems(const Database& db, const ClassDesign& d, const ClassDef* editing)
	{
		std::vector<std::string> out;
		const std::string label = !d.name.empty() ? d.name : db.names().format(d.key);
		if (d.name.empty() && !d.key) out.push_back("the class has no name");
		if (!d.name.empty() && d.key != StringHash32(d.name)) out.push_back("the class's key isn't its name's hash");
		if (const ClassDef* c = db.findClass(d.key); c && c != editing) out.push_back("there is a class named " + label + " already");
		if (db.keys64() && d.name.empty() && !db.names().find(d.key)) out.push_back("a database with 64-bit keys needs the class's name");
		if (!IsGameClass(db, d.key))
			for (const FieldDesign& f : d.fields)
				if (f.storage == FieldStorage::Static)
				{
					out.push_back((f.name.empty() ? db.names().format(f.key) : f.name) +
								  ": only the game's own classes have static fields (the game keeps their values in a table compiled into it)");
					break;
				}
		if (d.fields.size() > 0xFFFF) out.push_back("a class has at most 65535 fields");
		for (std::string& p : FieldProblems(db, d)) out.push_back(std::move(p));
		return out;
	}

	namespace
	{
		void LayOutWith(const Database& db, bool usesHandler, const ClassDesign& d, ClassDef& out);
	}

	uint32_t DefaultDataAreaSize(const Target& t)
	{
		switch (t.game)
		{
			case Game::MostWanted:
			case Game::Carbon:
				return 2048;
			case Game::ProStreet: return 1368;
			case Game::Undercover: return t.platform == Platform::Wii || t.platform == Platform::PS2 ? 2048 : 2960;
			case Game::World: return 2752;
			default: return 0;
		}
	}

	std::vector<std::string> DesignWarnings(const Database& db, const ClassDesign& d)
	{
		std::vector<std::string> out;
		if (!DesignProblems(db, d, db.findClass(d.key)).empty()) return out;
		ClassDef laid;
		LayOutClass(db, d, laid);
		const uint32_t area = DefaultDataAreaSize(db.target());
		uint32_t largest = laid.layoutSize;
		for (const FieldDef& f : laid.fields) largest = std::max<uint32_t>(largest, f.size);
		if (area && largest > area)
			out.push_back("it needs " + std::to_string(largest) + " bytes of default data, more than the " + std::to_string(area) +
						  " the game has: an Instance with no collection, or a field no collection sets, reads past them (give your code a default area of its own)");
		return out;
	}

	void LayOutClass(const Database& db, const ClassDesign& d, ClassDef& out)
	{
		LayOutWith(db, UsesHandlerFlag(db), d, out);
	}

	namespace
	{
	void LayOutWith(const Database& db, bool usesHandler, const ClassDesign& d, ClassDef& out)
	{
		const std::vector<std::string> problems = FieldProblems(db, d);
		if (!problems.empty()) throw std::runtime_error(problems.front());
		out.key = d.key ? d.key : StringHash32(d.name);
		out.fields.clear();
		uint32_t layoutEnd = 0, layoutAlign = 1, staticEnd = 0, staticAlign = 1;
		uint16_t searchable = 0;
		std::vector<std::pair<uint64_t, FieldDef>> defs;
		for (const FieldDesign& fd : d.fields)
		{
			const TypeInfo* t = db.typeByHash(fd.typeHash);
			FieldDef f;
			f.key = fd.key;
			f.typeHash = fd.typeHash;
			f.type = t;
			f.size = fd.size ? *fd.size : uint16_t(t->size);
			f.alignment = fd.alignment ? *fd.alignment : t ? NaturalAlignment(*t) : 2;
			uint8_t flags = DefFlag::IsBound;
			if (fd.array) flags |= DefFlag::Array;
			if (fd.storage == FieldStorage::Layout) flags |= DefFlag::InLayout;
			if (fd.storage == FieldStorage::Static) flags |= DefFlag::IsStatic;
			if (!fd.searchable) flags |= DefFlag::NotSearchable;
			if (t && HandlerOf(usesHandler, *t)) flags |= DefFlag::HasHandler;
			f.flags = fd.flags ? *fd.flags : flags;
			f.maxCount = fd.array ? fd.count : fd.maxCount ? *fd.maxCount : uint16_t(1);
			const uint32_t al = 1u << f.alignment;
			switch (fd.storage)
			{
				case FieldStorage::Layout:
					f.offset = fd.offset ? *fd.offset : uint16_t(Align(layoutEnd, al));
					layoutEnd = std::max(layoutEnd, LayoutOffset(f) + LayoutExtent(f, f.maxCount));
					layoutAlign = std::max(layoutAlign, al);
					if (!(f.flags & DefFlag::NotSearchable)) ++searchable;
					break;
				case FieldStorage::Static:
					f.offset = fd.offset ? *fd.offset : uint16_t(Align(staticEnd, al));
					staticEnd = std::max<uint32_t>(staticEnd, f.offset + f.size);
					staticAlign = std::max(staticAlign, al);
					break;
				case FieldStorage::Entry:
					f.offset = fd.offset ? *fd.offset : uint16_t(0);
					break;
			}
			defs.emplace_back(FileKey(db, f.key, fd.name), f);
		}
		// In the order a vault stores them (by key), so the class reads the same
		// after it has been saved and loaded again.
		std::stable_sort(defs.begin(), defs.end(), [](const auto& a, const auto& b)
		{
			return a.first < b.first;
		});
		for (auto& [k, f] : defs) out.fields.push_back(f);
		out.layoutSize = d.layoutSize ? *d.layoutSize : layoutEnd ? Align(layoutEnd, layoutAlign) : 0;
		out.layoutCount = d.layoutCount ? *d.layoutCount : searchable;
		out.layoutKeyShift = d.layoutKeyShift ? *d.layoutKeyShift : uint16_t(0);
		out.staticSize = d.staticSize ? *d.staticSize : staticEnd ? Align(staticEnd, staticAlign) : 0;
		out.collectionReserve = d.collectionReserve ? *d.collectionReserve : CollectionReserveFor(out.collections.size());
	}
	} // namespace

	ClassDesign DesignOf(const Database& db, const ClassDef& cls)
	{
		ClassDesign d;
		d.key = cls.key;
		if (const std::string* n = db.names().find(cls.key)) d.name = *n;
		std::vector<const FieldDef*> layout, statics, entries;
		for (const FieldDef& f : cls.fields)
			(f.inLayout() ? layout : f.isStatic() ? statics : entries).push_back(&f);
		auto byOffset = [](const FieldDef* a, const FieldDef* b)
		{
			return a->offset < b->offset;
		};
		std::stable_sort(layout.begin(), layout.end(), byOffset);
		std::stable_sort(statics.begin(), statics.end(), byOffset);
		std::vector<const FieldDef*> order = layout;
		order.insert(order.end(), statics.begin(), statics.end());
		order.insert(order.end(), entries.begin(), entries.end());
		for (const FieldDef* f : order)
		{
			FieldDesign fd;
			fd.key = f->key;
			if (const std::string* n = db.names().find(f->key)) fd.name = *n;
			fd.typeHash = f->typeHash;
			fd.storage = StorageOf(*f);
			fd.array = f->isArray();
			fd.count = fd.array ? f->maxCount : 0;
			fd.searchable = !(f->flags & DefFlag::NotSearchable);
			if (!f->type || f->type->size != f->size) fd.size = f->size;
			d.fields.push_back(std::move(fd));
		}
		// What the rules give, then the stored values that differ, field by field
		// in the order laid out: an explicit alignment or offset moves the
		// fields after it.
		const bool usesHandler = UsesHandlerFlag(db);
		ClassDef plain;
		plain.collections = cls.collections;
		LayOutWith(db, usesHandler, d, plain);
		for (size_t i = 0; i < d.fields.size(); ++i)
		{
			FieldDesign& fd = d.fields[i];
			const FieldDef& stored = *order[i];
			bool changed = false;
			if (plain.field(fd.key)->alignment != stored.alignment)
			{
				fd.alignment = stored.alignment;
				LayOutWith(db, usesHandler, d, plain);
				changed = true;
			}
			const FieldDef& laid = *plain.field(fd.key);
			if (laid.offset != stored.offset) fd.offset = stored.offset, changed = true;
			if (laid.flags != stored.flags) fd.flags = stored.flags, changed = true;
			if (!fd.array && laid.maxCount != stored.maxCount) fd.maxCount = stored.maxCount, changed = true;
			if (changed) LayOutWith(db, usesHandler, d, plain);
		}
		if (plain.layoutSize != cls.layoutSize) d.layoutSize = cls.layoutSize;
		if (plain.layoutCount != cls.layoutCount) d.layoutCount = cls.layoutCount;
		if (plain.layoutKeyShift != cls.layoutKeyShift) d.layoutKeyShift = cls.layoutKeyShift;
		if (plain.staticSize != cls.staticSize) d.staticSize = cls.staticSize;
		// A collection table's size, which nothing reproduces: kept as stored.
		d.collectionReserve = cls.collectionReserve;
		return d;
	}

	// ------------------------------------------------------------------ editor ---

	void ClassEditor::touch(Vault* v)
	{
		if (!v || !v->pack) return;
		v->pack->dirty = true;
		touched_.insert(v->pack);
	}

	void ClassEditor::growDatabaseValues(const ClassDef& cls)
	{
		Vault* dbVault = DatabaseVault(db_);
		// The default data area stands in for a value no collection has: the
		// layout of a missing collection, an entry nothing sets.
		uint32_t need = cls.layoutSize;
		for (const FieldDef& f : cls.fields) need = std::max<uint32_t>(need, f.size);
		if (need > db_.defaultDataSize_)
		{
			db_.defaultDataSize_ = need;
			touch(dbVault);
		}
		const uint32_t classes = uint32_t((db_.classes_.size() + 2) * 3 / 2);
		if (classes > db_.numClassesDeclared_)
		{
			db_.numClassesDeclared_ = classes;
			touch(dbVault);
		}
	}

	ClassDef& ClassEditor::create(Vault& vault, const ClassDesign& design)
	{
		const std::vector<std::string> problems = DesignProblems(db_, design, nullptr);
		if (!problems.empty()) throw std::runtime_error(problems.front());
		if (!vault.isDatabase)
			throw std::runtime_error("a class goes in the database's vault (" + (DatabaseVault(db_) ? DatabaseVault(db_)->name : std::string("db")) +
									 " in attributes.bin): the game loads it first, and skips a collection whose class isn't loaded yet");
		auto cls = std::make_unique<ClassDef>();
		cls->vault = &vault;
		LayOutClass(db_, design, *cls);
		cls->staticData.bytes.assign(cls->staticSize, 0);
		if (!design.name.empty()) db_.names_.add(design.name);
		for (const FieldDesign& f : design.fields)
			if (!f.name.empty()) db_.names_.add(f.name);
		ClassDef* raw = cls.get();
		db_.classByKey_[raw->key] = raw;
		vault.classes.push_back(raw);
		// At the end of the vault's exports, before World's VaultData.
		auto& order = vault.exportOrder;
		auto pos = order.end();
		while (pos != order.begin() && (pos - 1)->kind == ExportRef::Kind::Data) --pos;
		order.insert(pos, ExportRef{ ExportRef::Kind::Class, raw, nullptr });
		db_.classes_.push_back(std::move(cls));
		growDatabaseValues(*raw);
		touch(&vault);
		return *raw;
	}

	bool ClassEditor::redesign(ClassDef& cls, const ClassDesign& design)
	{
		if (IsGameClass(db_, cls.key))
			throw std::runtime_error(db_.names().format(cls.key) + " is a class of the game: its compiled code reads it as it is, so it can't be changed");
		if (design.key != cls.key)
			throw std::runtime_error("a new design keeps the class's name; rename the class first");
		const std::vector<std::string> problems = DesignProblems(db_, design, &cls);
		if (!problems.empty()) throw std::runtime_error(problems.front());
		ClassDef next;
		next.collections = cls.collections;
		LayOutClass(db_, design, next);
		if (SameLayout(next, cls)) return false;
		for (const FieldDesign& f : design.fields)
			if (!f.name.empty()) db_.names_.add(f.name);

		const std::vector<FieldDef> before = cls.fields;
		auto oldDef = [&](uint32_t key) -> const FieldDef*
		{
			for (const FieldDef& f : before)
				if (f.key == key) return &f;
			return nullptr;
		};
		auto sameKind = [](const FieldDef& a, const FieldDef& b)
		{
			return a.typeHash == b.typeHash && a.size == b.size && a.isArray() == b.isArray();
		};

		// Each collection's values under the new definitions, taken before any
		// changes: an entry moving into the layout takes what the collection
		// inherits too. Field defs point into `next` until it is moved in.
		struct Staged
		{
			std::vector<Attribute> layout, entries;
		};
		std::unordered_map<Collection*, Staged> staged;
		for (Collection* c : cls.collections)
		{
			Staged& s = staged[c];
			for (const FieldDef& f : next.fields)
			{
				if (!f.inLayout()) continue;
				const FieldDef* o = oldDef(f.key);
				const Attribute* src = nullptr;
				if (o && sameKind(*o, f)) src = o->inLayout() ? c->find(f.key) : o->isStatic() ? nullptr : c->resolve(f.key);
				s.layout.push_back(src ? Reshape(*src, f, true) : EmptyAttribute(f, true));
			}
			for (const Attribute& a : c->attributes)
			{
				const FieldDef* f = next.field(a.key);
				const FieldDef* o = oldDef(a.key);
				if (!f || !o || f->inLayout() || f->isStatic() || !sameKind(*o, *f)) continue;
				// An entry that stays one keeps its value as it is (its pointer order too).
				if (a.inLayout) s.entries.push_back(Reshape(a, *f, false));
				else
				{
					Attribute kept = a;
					kept.def = f;
					kept.type = f->type;
					kept.nodeFlags = uint8_t((kept.nodeFlags & ~NodeFlag::HasHandler) | ((f->flags & DefFlag::HasHandler) ? NodeFlag::HasHandler : 0));
					s.entries.push_back(std::move(kept));
				}
			}
		}
		// Static values that stay static.
		DataBlock statics;
		statics.bytes.assign(next.staticSize, 0);
		for (const FieldDef& f : next.fields)
		{
			const FieldDef* o = oldDef(f.key);
			if (!f.isStatic() || !o || !o->isStatic() || !sameKind(*o, f)) continue;
			if (uint32_t(o->offset) + o->size > cls.staticData.bytes.size() || uint32_t(f.offset) + f.size > statics.bytes.size()) continue;
			std::copy_n(cls.staticData.bytes.begin() + o->offset, f.size, statics.bytes.begin() + f.offset);
			for (const PtrSlot& p : cls.staticData.ptrs)
				if (p.offset >= o->offset && p.offset < uint32_t(o->offset) + o->size)
				{
					PtrSlot q = p;
					q.offset = p.offset - o->offset + f.offset;
					q.seq = kNewPtr;
					statics.ptrs.push_back(std::move(q));
				}
		}
		DetachCopy(statics);
		std::sort(statics.ptrs.begin(), statics.ptrs.end(), [](const PtrSlot& a, const PtrSlot& b)
		{
			return a.offset < b.offset;
		});

		// Move the new definitions in, then point the staged values at them.
		cls.fields = std::move(next.fields);
		cls.layoutSize = next.layoutSize;
		cls.layoutCount = next.layoutCount;
		cls.layoutKeyShift = next.layoutKeyShift;
		cls.staticSize = next.staticSize;
		cls.collectionReserve = next.collectionReserve;
		cls.staticData = std::move(statics);
		cls.staticSeq = kNewPtr;
		cls.defsSeq = kNewPtr;
		auto repoint = [&](Attribute& a)
		{
			a.def = cls.field(a.key);
			a.type = a.def->type;
		};
		for (Collection* c : cls.collections)
		{
			Staged& s = staged[c];
			c->attributes.clear();
			for (Attribute& a : s.layout)
			{
				repoint(a);
				c->attributes.push_back(std::move(a));
			}
			for (Attribute& a : s.entries)
			{
				repoint(a);
				c->attributes.push_back(std::move(a));
			}
			c->layout = DataBlock{};
			c->layout.bytes.assign(LayoutBytes(cls), 0);
			c->layoutSeq = kNewPtr;
			c->typeHashes.clear();
			touch(c->vault);
		}
		UpdateIndexTables(cls);
		growDatabaseValues(cls);
		touch(cls.vault);
		return true;
	}

	void ClassEditor::rename(ClassDef& cls, const std::string& newName)
	{
		if (newName.empty()) throw std::runtime_error("a class needs a name");
		if (IsGameClass(db_, cls.key))
			throw std::runtime_error(db_.names().format(cls.key) + " is a class of the game: the game's code finds it by its name, so it can't be renamed");
		const uint32_t newKey = StringHash32(newName);
		if (newKey == cls.key) return;
		if (db_.findClass(newKey)) throw std::runtime_error("there is a class named " + newName + " already");
		db_.names_.add(newName);
		db_.classByKey_.erase(cls.key);
		db_.classByKey_[newKey] = &cls;
		for (Collection* c : cls.collections)
		{
			db_.collectionByKey_.erase((uint64_t(c->classKey) << 32) | c->key);
			c->classKey = newKey;
			db_.collectionByKey_[(uint64_t(newKey) << 32) | c->key] = c;
			const std::string name = db_.names().format(c->key);
			c->exportId = c->vault ? CollectionExportId(*c->vault, db_.names(), newKey, name) : CollectionExportId(db_.names(), newKey, name);
			touch(c->vault);
		}
		cls.key = newKey;
		touch(cls.vault);
	}

	void ClassEditor::remove(ClassDef& cls, const std::function<void(Collection&)>& beforeRemove)
	{
		const std::vector<Collection*> cols = cls.collections;
		if (beforeRemove)
			for (Collection* c : cols) beforeRemove(*c);
		const std::set<const Collection*> gone(cols.begin(), cols.end());
		for (Collection* c : cols)
		{
			if (c->vault)
			{
				EraseCollectionExport(*c->vault, *c);
				touch(c->vault);
			}
			auto it = db_.collectionByKey_.find((uint64_t(c->classKey) << 32) | c->key);
			if (it != db_.collectionByKey_.end() && it->second == c) db_.collectionByKey_.erase(it);
		}
		auto& all = db_.collections_;
		all.erase(std::remove_if(all.begin(), all.end(), [&](const std::unique_ptr<Collection>& p)
		{
			return gone.count(p.get()) != 0;
		}), all.end());
		if (Vault* v = cls.vault)
		{
			v->classes.erase(std::remove(v->classes.begin(), v->classes.end(), &cls), v->classes.end());
			v->exportOrder.erase(std::remove_if(v->exportOrder.begin(), v->exportOrder.end(), [&](const ExportRef& r)
			{
				return r.kind == ExportRef::Kind::Class && r.cls == &cls;
			}), v->exportOrder.end());
			touch(v);
		}
		auto k = db_.classByKey_.find(cls.key);
		if (k != db_.classByKey_.end() && k->second == &cls) db_.classByKey_.erase(k);
		auto& classes = db_.classes_;
		classes.erase(std::remove_if(classes.begin(), classes.end(), [&](const std::unique_ptr<ClassDef>& p)
		{
			return p.get() == &cls;
		}), classes.end());
	}

} // namespace attrib
