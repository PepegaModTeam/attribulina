#include "Edit.h"

#include "Folder.h"
#include "Gameplay.h"
#include "Hash.h"
#include "Values.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace attrib
{

	namespace
	{
		// Deep-copies nested blocks (keeping sharing within the copy) and clears the
		// writer's ordering hints: a copy is a new set of pointers.
		void Detach(DataBlock& b, std::unordered_map<const DataBlock*, std::shared_ptr<DataBlock>>& seen)
		{
			b.nulls.clear();
			for (auto& p : b.ptrs)
			{
				p.seq = kNewPtr;
				if (!p.data) continue;
				auto it = seen.find(p.data.get());
				if (it != seen.end())
				{
					p.data = it->second;
					continue;
				}
				auto copy = std::make_shared<DataBlock>(*p.data);
				seen[p.data.get()] = copy;
				p.data = copy;
				Detach(*copy, seen);
			}
		}
	} // namespace

	void DetachCopy(Attribute& a)
	{
		std::unordered_map<const DataBlock*, std::shared_ptr<DataBlock>> seen;
		Detach(a.data, seen);
		a.dataSeq = kNewPtr;
	}

	void DetachCopy(DataBlock& b)
	{
		std::unordered_map<const DataBlock*, std::shared_ptr<DataBlock>> seen;
		Detach(b, seen);
	}

	void InsertElement(Attribute& a, uint32_t at)
	{
		at = std::min(at, a.count);
		const uint32_t pos = at * a.elemSize;
		a.data.bytes.insert(a.data.bytes.begin() + pos, a.elemSize, 0);
		for (auto& p : a.data.ptrs)
			if (p.offset >= pos) p.offset += a.elemSize;
		for (auto& n : a.data.nulls)
			if (n.offset >= pos) n.offset += a.elemSize;
		++a.count;
		// An entry array's mAlloc is its count, as in every file read (a layout
		// array's is the space its layout has).
		if (a.isArray && !a.inLayout) a.capacity = uint16_t(a.count);
	}

	void RemoveElement(Attribute& a, uint32_t at)
	{
		if (at >= a.count) return;
		const uint32_t begin = at * a.elemSize, end = begin + a.elemSize;
		a.data.bytes.erase(a.data.bytes.begin() + begin, a.data.bytes.begin() + end);
		auto& ptrs = a.data.ptrs;
		ptrs.erase(std::remove_if(ptrs.begin(), ptrs.end(), [&](const PtrSlot& p)
		{
			return p.offset >= begin && p.offset < end;
		}),
				   ptrs.end());
		for (auto& p : ptrs)
			if (p.offset >= end) p.offset -= a.elemSize;
		auto& nulls = a.data.nulls;
		nulls.erase(std::remove_if(nulls.begin(), nulls.end(), [&](const DataBlock::NullPtr& n)
		{
			return n.offset >= begin && n.offset < end;
		}),
					nulls.end());
		for (auto& n : nulls)
			if (n.offset >= end) n.offset -= a.elemSize;
		--a.count;
		// Spare capacity would be written as zeroed elements past the count.
		if (a.isArray && !a.inLayout) a.capacity = uint16_t(a.count);
	}

	ElementData CopyElement(const DataBlock& b, uint32_t at, uint32_t elemSize)
	{
		ElementData e;
		const uint32_t begin = at * elemSize, end = begin + elemSize;
		if (end > b.bytes.size()) return e;
		e.bytes.assign(b.bytes.begin() + begin, b.bytes.begin() + end);
		for (const PtrSlot& p : b.ptrs)
			if (p.offset >= begin && p.offset < end)
			{
				PtrSlot q = p;
				q.offset -= begin;
				e.ptrs.push_back(std::move(q));
			}
		return e;
	}

	void PutElement(DataBlock& b, uint32_t at, uint32_t elemSize, const ElementData& e)
	{
		const uint32_t begin = at * elemSize, end = begin + elemSize;
		if (end > b.bytes.size() || e.bytes.size() != elemSize) return;
		std::copy(e.bytes.begin(), e.bytes.end(), b.bytes.begin() + begin);
		b.ptrs.erase(std::remove_if(b.ptrs.begin(), b.ptrs.end(), [&](const PtrSlot& p)
		{
			return p.offset >= begin && p.offset < end;
		}),
					 b.ptrs.end());
		for (PtrSlot p : e.ptrs)
		{
			p.offset += begin;
			b.ptrs.push_back(std::move(p));
		}
		std::sort(b.ptrs.begin(), b.ptrs.end(), [](const PtrSlot& x, const PtrSlot& y)
		{
			return x.offset < y.offset;
		});
	}

	uint32_t MaxElements(const Attribute& a, bool pastLimit)
	{
		if (!a.isArray) return 1;
		if (a.inLayout) return a.capacity;
		const uint32_t m = ElementLimit(a);
		return m && !pastLimit ? m : 0xFFFF;
	}

	uint32_t ElementLimit(const Attribute& a)
	{
		return a.isArray && !a.inLayout && a.def ? a.def->maxCount : 0;
	}

	std::string ElementLimitWarning(const Attribute& a, uint32_t count, const std::string& field)
	{
		const uint32_t limit = ElementLimit(a);
		if (!limit || count <= limit) return {};
		return field + " has " + std::to_string(count) + " elements, more than the " + std::to_string(limit) +
			" its definition allows; game code made for " + std::to_string(limit) + " may ignore or mishandle the rest";
	}

	Attribute& OverrideAttribute(Database& /*db*/, Collection& col, const FieldDef& field)
	{
		if (Attribute* own = col.find(field.key))
			return *own;
		Attribute a;
		const Attribute* inherited = col.parent ? col.parent->resolve(field.key) : nullptr;
		if (inherited)
		{
			a = *inherited;
			DetachCopy(a);
			if (a.inLayout)
			{
				// Layout fields are never inherited, so this can't happen; be safe.
				a.inLayout = false;
			}
			if (a.isArray) a.nodeFlags = NodeFlag::Array;
			else if (a.elemSize <= 4) a.nodeFlags = NodeFlag::ByValue;
			else a.nodeFlags = 0;
		}
		else
		{
			a.key = field.key;
			a.def = &field;
			a.type = field.type;
			a.isArray = field.isArray();
			a.elemSize = field.type ? field.type->size : field.size;
			a.count = a.isArray ? 0 : 1;
			a.capacity = 0;
			a.encodedTypePad = a.isArray ? ArrayPad(field) : 0;
			a.data.bytes.assign(size_t(a.count) * a.elemSize, 0);
			if (a.isArray)
				a.nodeFlags = NodeFlag::Array;
			else if (a.elemSize <= 4)
				a.nodeFlags = NodeFlag::ByValue;
		}
		if (field.flags & DefFlag::HasHandler) a.nodeFlags |= NodeFlag::HasHandler; // as every retail entry of such a field
		a.inLayout = false;
		a.entryFlags = 0;
		col.attributes.push_back(std::move(a));
		return col.attributes.back();
	}

	bool SetCollectionNameField(Database& db, Collection& col, const std::string* onlyIf)
	{
		const FieldDef* f = col.cls ? col.cls->field(StringHash32("CollectionName")) : nullptr;
		if (!f || f->isArray() || !f->type || f->type->kind != TypeKind::Text) return false;
		const std::string* name = db.names().find(col.key);
		if (!name) return false;
		const Attribute* cur = col.resolve(f->key);
		const PtrSlot* s = cur ? cur->data.ptrAt(0) : nullptr;
		const std::string text = s && s->isString ? s->text : std::string();
		if (text == *name || (onlyIf && text != *onlyIf)) return false;
		Attribute& a = OverrideAttribute(db, col, *f);
		if (a.data.bytes.size() < a.elemSize) a.data.bytes.resize(a.elemSize, 0);
		EnsureStringSlot(a.data, 0).text = *name;
		return true;
	}

	bool RevertAttribute(Collection& col, uint32_t fieldKey)
	{
		auto& v = col.attributes;
		auto it = std::find_if(v.begin(), v.end(), [&](const Attribute& a)
		{
			return a.key == fieldKey && !a.inLayout;
		});
		if (it == v.end()) return false;
		v.erase(it);
		return true;
	}

	Vault& CreateVault(LoadedPack& pack, const std::string& name, const Vault& like)
	{
		if (name.empty())
			throw std::runtime_error("a vault needs a name");
		if (FindVault(pack, name))
			throw std::runtime_error("the pack already has a vault named " + name);
		if (pack.format == PackFormat::VaultFile && !pack.vaults.empty())
			throw std::runtime_error(pack.path + " is one of World's event vault files, which hold one vault each");
		auto v = std::make_unique<Vault>();
		v->name = name;
		v->pack = &pack;
		v->raw.name = name;
		v->raw.endian = like.raw.endian;
		v->raw.hasVers = like.raw.hasVers;
		v->raw.version = like.raw.version;
		v->raw.mostWantedLayout = like.raw.mostWantedLayout;
		v->raw.keys64 = like.raw.keys64;
		v->raw.strNFirst = like.raw.strNFirst;
		v->raw.endC = like.raw.endC;
		for (const char* ext : { ".vlt", ".bin" })
			v->raw.dependencies.push_back(Dependency{ like.raw.keys64 ? StringHash64(name + ext) : StringHash32(name + ext), name + ext });
		// Packs keep their vaults sorted by name (the writer sorts them anyway).
		auto pos = std::find_if(pack.vaults.begin(), pack.vaults.end(), [&](const auto& o)
		{
			return o->name > name;
		});
		return **pack.vaults.insert(pos, std::move(v));
	}

	bool RemoveVault(LoadedPack& pack, Vault& vault)
	{
		if (!vault.collections.empty() || !vault.classes.empty() || vault.isDatabase) return false;
		auto& vs = pack.vaults;
		const auto it = std::find_if(vs.begin(), vs.end(), [&](const auto& v)
		{
			return v.get() == &vault;
		});
		if (it == vs.end()) return false;
		vs.erase(it);
		return true;
	}

	Vault* FindVault(LoadedPack& pack, const std::string& name)
	{
		for (const auto& v : pack.vaults)
			if (v->name == name) return v.get();
		return nullptr;
	}

	Vault* FindVault(const Database& db, const std::string& name)
	{
		for (const auto& pk : db.packs())
			if (Vault* v = FindVault(*pk, name)) return v;
		return nullptr;
	}

	namespace
	{
		bool SameNoCase(const std::string& a, const std::string& b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
			{
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
		}

		constexpr size_t kVaultFileNameSize = 0x2C; // the event vault header's name field, with its terminator
	} // namespace

	std::string VaultNameProblem(const Database& db, const LoadedPack& pack, const std::string& name, const Vault* self)
	{
		if (name.empty()) return "A vault needs a name.";
		if (name.front() == ' ' || name.back() == ' ') return "A vault name can't start or end with a space.";
		for (const char ch : name)
		{
			const auto u = static_cast<unsigned char>(ch);
			if (u < 0x20 || u >= 0x7F || std::strchr("/\\:*?\"<>|", ch))
				return std::string("A vault name can't hold ") + (u < 0x20 || u >= 0x7F ? "that character" : std::string("'") + ch + "'") +
					": it names the vault's files.";
		}
		if (pack.format == PackFormat::VaultFile && name.size() >= kVaultFileNameSize)
			return "An event vault's name is at most " + std::to_string(kVaultFileNameSize - 1) + " characters.";
		for (const auto& pk : db.packs())
			for (const auto& v : pk->vaults)
				if (v.get() != self && SameNoCase(v->name, name))
					return "The database has a vault named " + v->name + " already.";
		return {};
	}

	std::vector<Collection*> CollectionsNamingVault(const Database& db, const std::string& vaultName)
	{
		std::vector<Collection*> out;
		const ClassDef* cls = db.findClass(StringHash32("gameplay"));
		if (!cls) return out;
		const uint32_t field = StringHash32(gameplay::kGameplayVault);
		for (Collection* c : cls->collections)
		{
			const Attribute* a = c->find(field);
			const PtrSlot* s = a ? a->data.ptrAt(0) : nullptr;
			if (s && s->isString && s->text == vaultName) out.push_back(c);
		}
		return out;
	}

	std::vector<Collection*> RenameVault(Database& db, Vault& vault, const std::string& name)
	{
		if (!vault.pack) throw std::runtime_error("vault " + vault.name + " isn't in a pack");
		if (vault.isDatabase) throw std::runtime_error("the database's vault " + vault.name + " keeps its name");
		if (vault.pack->format == PackFormat::VaultFile)
			throw std::runtime_error("the event vault " + vault.name + " is named after its file. Make a new event vault file and move the collections to it");
		if (name == vault.name) return {};
		if (const std::string p = VaultNameProblem(db, *vault.pack, name, &vault); !p.empty()) throw std::runtime_error(p);
		const std::string old = vault.name;
		const std::vector<Collection*> naming = CollectionsNamingVault(db, old);
		vault.name = vault.raw.name = name;
		for (Dependency& d : vault.raw.dependencies)
			for (const char* ext : { ".vlt", ".bin" })
				if (d.name == old + ext)
				{
					d.name = name + ext;
					d.id = vault.raw.keys64 ? StringHash64(d.name) : StringHash32(d.name);
					db.names().add(d.name);
				}
		std::stable_sort(vault.pack->vaults.begin(), vault.pack->vaults.end(), [](const auto& a, const auto& b)
		{
			return a->name < b->name;
		});
		for (Collection* c : naming)
			EnsureStringSlot(c->find(StringHash32(gameplay::kGameplayVault))->data, 0).text = name;
		return naming;
	}

	std::vector<Collection*> VaultContents(const Vault& vault)
	{
		std::vector<Collection*> out;
		std::unordered_set<const Collection*> seen;
		std::function<void(Collection*)> add = [&](Collection* c)
		{
			if (!seen.insert(c).second) return;
			for (Collection* ch : c->children) add(ch);
			out.push_back(c);
		};
		for (Collection* c : vault.collections) add(c);
		return out;
	}

	LoadedPack& CreateEventVaultFile(Database& db, const std::string& path, const std::string& name, const Vault& like)
	{
		for (const auto& pk : db.packs())
			if (SameNoCase(pk->path, path)) throw std::runtime_error(path + " is open already");
		LoadedPack probe;
		probe.format = PackFormat::VaultFile;
		if (const std::string p = VaultNameProblem(db, probe, name); !p.empty()) throw std::runtime_error(p);
		LoadedPack& pack = db.addPack(path, PackFormat::VaultFile);
		Vault& v = CreateVault(pack, name, like);
		// As every event vault has it: StringHash32("VaultData"), 4 bytes.
		DataExport slot{ StringHash32("VaultData"), ExportType::VaultData, Bytes(4, 0) };
		for (const DataExport& d : like.dataExports)
			if (d.type == ExportType::VaultData)
			{
				slot.id = d.id;
				slot.type = d.type;
			}
		v.dataExports.push_back(std::move(slot));
		ExportRef ref{ ExportRef::Kind::Data, nullptr, nullptr };
		ref.data = 0;
		v.exportOrder.push_back(ref);
		return pack;
	}

	std::string NewEventVaultPath(const Database& db, const std::string& name)
	{
		const LoadedPack* at = nullptr;
		for (const auto& pk : db.packs())
		{
			if (pk->format == PackFormat::VaultFile) return EventVaultPath(pk->path, name + ".bin");
			for (const auto& v : pk->vaults)
				if (v->isDatabase && !at) at = pk.get();
		}
		if (!at && !db.packs().empty()) at = db.packs().front().get();
		return at ? EventVaultPath(at->path, name + ".bin") : std::string();
	}

	const Vault* EventVaultModel(const Database& db)
	{
		const Vault* model = nullptr;
		for (const auto& pk : db.packs())
			for (const auto& v : pk->vaults)
			{
				if (pk->format == PackFormat::VaultFile) return v.get();
				if (v->isDatabase || !model) model = v.get();
			}
		return model;
	}

	uint32_t CollectionExportId(const KeyNames& names, uint32_t classKey, const std::string& name)
	{
		return StringHash32(names.format(classKey) + "/" + name);
	}

	uint64_t CollectionExportId(const Vault& vault, const KeyNames& names, uint32_t classKey, const std::string& name)
	{
		return vault.raw.keys64 ? StringHash64(names.format(classKey) + "/" + name) : CollectionExportId(names, classKey, name);
	}

	namespace
	{
		// Exports are grouped by class: a collection goes after its class's last
		// collection in the vault (its parent, if there, is earlier), else at the end,
		// before World's VaultData, which ends its vaults.
		void InsertExport(Vault& vault, Collection& col)
		{
			auto& order = vault.exportOrder;
			auto pos = order.end();
			while (pos != order.begin() && (pos - 1)->kind == ExportRef::Kind::Data) --pos;
			for (auto it = order.begin(); it != order.end(); ++it)
				if ((it->kind == ExportRef::Kind::Collection && it->collection->classKey == col.classKey) ||
					(it->kind == ExportRef::Kind::Class && it->cls == col.cls))
					pos = it + 1;
			order.insert(pos, ExportRef{ ExportRef::Kind::Collection, nullptr, &col });
		}

		void EraseExport(Vault& vault, const Collection& col)
		{
			auto& order = vault.exportOrder;
			order.erase(std::remove_if(order.begin(), order.end(), [&](const ExportRef& r)
			{
				return r.collection == &col;
			}), order.end());
			auto& v = vault.collections;
			v.erase(std::remove(v.begin(), v.end(), &col), v.end());
		}
	} // namespace

	void KeepParentsFirst(Vault& vault)
	{
		std::vector<ExportRef> out;
		out.reserve(vault.exportOrder.size());
		std::unordered_set<const Collection*> placed;
		// Collections whose parent comes later, by parent, in the order met.
		std::vector<std::pair<const Collection*, std::vector<ExportRef>>> waiting;
		auto waitingFor = [&](const Collection* p) -> std::vector<ExportRef>*
		{
			for (auto& w : waiting)
				if (w.first == p) return &w.second;
			return nullptr;
		};
		std::function<void(const ExportRef&)> place = [&](const ExportRef& r)
		{
			out.push_back(r);
			if (r.kind != ExportRef::Kind::Collection) return;
			placed.insert(r.collection);
			if (std::vector<ExportRef>* kids = waitingFor(r.collection))
			{
				const std::vector<ExportRef> now = std::move(*kids);
				kids->clear();
				for (const ExportRef& k : now) place(k);
			}
		};
		for (const ExportRef& r : vault.exportOrder)
		{
			const Collection* p = r.kind == ExportRef::Kind::Collection ? r.collection->parent : nullptr;
			if (p && p->vault == &vault && !placed.count(p))
			{
				if (std::vector<ExportRef>* w = waitingFor(p)) w->push_back(r);
				else waiting.emplace_back(p, std::vector<ExportRef>{r});
			}
			else
			{
				place(r);
			}
		}
		// Parents that aren't exported here after all: keep their children.
		for (auto& w : waiting)
			for (const ExportRef& k : w.second) out.push_back(k);
		vault.exportOrder = std::move(out);
	}

	namespace
	{
		constexpr uint32_t kSpeechDefaultValue = 0x4F8B31A9u, kDefaultKey = 0xEEC2271Au; // StringHash32("default")

		struct IndexTable
		{
			std::vector<std::pair<uint32_t, std::vector<uint32_t>>> entries; // keys ascending
		};

		IndexTable ReadIndexTable(const DataBlock& b, uint32_t off, Endian e)
		{
			IndexTable t;
			const uint32_t count = load32(b.bytes.data() + off, e);
			const PtrSlot* keys = b.ptrAt(off + 4), * sets = b.ptrAt(off + 8), * vals = b.ptrAt(off + 12);
			if (!count || !keys || !keys->data || !sets || !sets->data) return t;
			for (uint32_t i = 0; i < count && (i + 1) * 4 <= keys->data->bytes.size() && (i + 1) * 8 <= sets->data->bytes.size(); ++i)
			{
				std::vector<uint32_t> v;
				const uint32_t at = load32(sets->data->bytes.data() + i * 8, e), n = load32(sets->data->bytes.data() + i * 8 + 4, e);
				for (uint32_t j = 0; j < n && vals && vals->data && (at + j + 1) * 4 <= vals->data->bytes.size(); ++j)
					v.push_back(load32(vals->data->bytes.data() + (at + j) * 4, e));
				t.entries.emplace_back(load32(keys->data->bytes.data() + i * 4, e), std::move(v));
			}
			return t;
		}

		void WriteIndexTable(DataBlock& b, uint32_t off, const IndexTable& t, Endian e)
		{
			Bytes keys, sets, vals;
			for (const auto& [k, v] : t.entries)
			{
				keys.resize(keys.size() + 4);
				store32(keys.data() + keys.size() - 4, k, e);
				sets.resize(sets.size() + 8);
				store32(sets.data() + sets.size() - 8, uint32_t(vals.size() / 4), e);
				store32(sets.data() + sets.size() - 4, uint32_t(v.size()), e);
				for (uint32_t x : v)
				{
					vals.resize(vals.size() + 4);
					store32(vals.data() + vals.size() - 4, x, e);
				}
			}
			store32(b.bytes.data() + off, uint32_t(t.entries.size()), e);
			auto put = [&](uint32_t at, Bytes bytes)
			{
				PtrSlot* s = b.ptrAt(off + at);
				if (bytes.empty())
				{
					if (s) b.ptrs.erase(b.ptrs.begin() + (s - b.ptrs.data()));
					return;
				}
				if (!s)
				{
					b.ptrs.push_back(PtrSlot{});
					s = &b.ptrs.back();
					s->offset = off + at;
				}
				auto block = std::make_shared<DataBlock>();
				block->bytes = std::move(bytes);
				block->align = 4;
				s->isString = false;
				s->data = std::move(block);
			};
			put(4, std::move(keys));
			put(8, std::move(sets));
			put(12, std::move(vals));
		}
	} // namespace

	const KeyNames* IndexTableNames(const Database& db)
	{
		return db.target().game == Game::World ? &db.names() : nullptr;
	}

	bool RebuildIndexTables(ClassDef& cls, const KeyNames* byName)
	{
		bool any = false;
		const Endian e = cls.vault ? cls.vault->raw.endian : Endian::Little;
		std::unordered_map<uint32_t, const Collection*> byKey;
		for (const Collection* c : cls.collections) byKey[c->key] = c;
		for (const FieldDef& f : cls.fields)
		{
			if (!f.isStatic() || f.isArray() || !f.type || f.type->name != kIndexTableType) continue;
			if (uint64_t(f.offset) + 16 > cls.staticData.bytes.size()) continue;
			// It holds when its entries agree with the collections' parents one of
			// the two ways UpdateIndexTables reads it, all but one (speech's root
			// maps to a key no collection has). VaultLib's values are what follows
			// the ISets, mostly zeros.
			size_t values = 0, asChildren = 0, asParent = 0;
			for (const auto& [k, v] : ReadIndexTable(cls.staticData, f.offset, e).entries)
			{
				values += v.size();
				for (uint32_t x : v)
					if (auto it = byKey.find(x); it != byKey.end() && it->second->parentKey == k) ++asChildren;
				if (auto it = byKey.find(k); it != byKey.end() && v.size() == 1 && v[0] == it->second->parentKey) ++asParent;
			}
			if (std::max(asChildren, asParent) + 1 >= values) continue;
			WriteIndexTable(cls.staticData, f.offset, IndexTable{}, e);
			any = true;
		}
		if (any) UpdateIndexTables(cls, byName);
		return any;
	}

	bool UpdateIndexTables(ClassDef& cls, const KeyNames* byName)
	{
		bool changed = false;
		const Endian e = cls.vault ? cls.vault->raw.endian : Endian::Little;
		for (const FieldDef& f : cls.fields)
		{
			if (!f.isStatic() || f.isArray() || !f.type || f.type->name != kIndexTableType) continue;
			if (uint64_t(f.offset) + 16 > cls.staticData.bytes.size()) continue;
			const IndexTable stored = ReadIndexTable(cls.staticData, f.offset, e);
			std::unordered_map<uint32_t, const Collection*> byKey;
			for (const Collection* c : cls.collections) byKey[c->key] = c;
			// Which way it maps, by the entries that hold either way.
			size_t asChildren = 0, asParent = 0;
			for (const auto& [k, v] : stored.entries)
			{
				for (uint32_t x : v)
					if (auto it = byKey.find(x); it != byKey.end() && it->second->parentKey == k) ++asChildren;
				if (auto it = byKey.find(k); it != byKey.end() && v.size() == 1 && v[0] == it->second->parentKey) ++asParent;
			}
			// An empty table (a class restored, or with no collections yet) goes by
			// the class: speech maps each collection to its parent, the others
			// each parent to its children.
			if (!asChildren && !asParent && !stored.entries.empty()) continue; // neither: leave it
			const bool children = stored.entries.empty() ? cls.key != StringHash32("speech") : asChildren >= asParent;
			std::map<uint32_t, std::vector<uint32_t>> want;
			for (const Collection* c : cls.collections)
			{
				if (children) want[c->parentKey].push_back(c->key);
				else want[c->key] = { c->parentKey };
			}
			std::map<uint32_t, const std::vector<uint32_t>*> had;
			for (const auto& [k, v] : stored.entries) had[k] = &v;
			// World's files list a parent's children in the order of their names.
			auto ordered = [&](std::vector<uint32_t> v)
			{
				if (!children || !byName) return v;
				for (uint32_t x : v)
					if (!byName->find(x)) return v;
				std::stable_sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return *byName->find(a) < *byName->find(b); });
				return v;
			};
			IndexTable t;
			for (auto& [k, v] : want)
			{
				auto h = had.find(k);
				if (h == had.end())
				{
					// Undercover's speech default maps to a key no collection has, in
					// the PC's and the 360's files alike (its other root to 0).
					if (!children && k == kDefaultKey && v.size() == 1 && !v[0]) v[0] = kSpeechDefaultValue;
					t.entries.emplace_back(k, ordered(v));
					continue;
				}
				std::vector<uint32_t> keep;
				if (children)
				{
					// The stored order of those still there, then the new ones.
					for (uint32_t x : *h->second)
						if (std::find(v.begin(), v.end(), x) != v.end()) keep.push_back(x);
					for (uint32_t x : v)
						if (std::find(keep.begin(), keep.end(), x) == keep.end()) keep.push_back(x);
					keep = ordered(std::move(keep));
				}
				else
				{
					// A root keeps what it stored (speech's default lists a key no collection has).
					keep = v;
					if (const Collection* c = byKey[k]; c && !c->parentKey && h->second->size() == 1 && !byKey.count((*h->second)[0])) keep = *h->second;
				}
				t.entries.emplace_back(k, std::move(keep));
			}
			if (t.entries == stored.entries) continue;
			WriteIndexTable(cls.staticData, f.offset, t, e);
			changed = true;
		}
		return changed;
	}

	Collection& CollectionEditor::create(Vault& vault, ClassDef& cls, const std::string& name, Collection* parent)
	{
		if (name.empty())
			throw std::runtime_error("a collection needs a name");
		return create(vault, cls, StringHash32(name), &name, parent);
	}

	Collection& CollectionEditor::create(Vault& vault, ClassDef& cls, uint32_t key, Collection* parent)
	{
		return create(vault, cls, key, nullptr, parent);
	}

	Collection& CollectionEditor::create(Vault& vault, ClassDef& cls, uint32_t key, const std::string* name, Collection* parent)
	{
		if (db_.findCollection(cls.key, key))
			throw std::runtime_error("collection " + db_.names().format(key) + " already exists in " + db_.names().format(cls.key));
		auto col = std::make_unique<Collection>();
		col->key = key;
		col->classKey = cls.key;
		col->cls = &cls;
		col->vault = &vault;
		col->parent = parent;
		col->parentKey = parent ? parent->key : 0;
		col->tableKeyShift = 0;
		if (name)
		{
			col->exportId = CollectionExportId(vault, db_.names(), cls.key, *name);
			db_.names().add(*name);
		}
		if (parent)
		{
			col->layout = parent->layout;
			std::unordered_map<const DataBlock*, std::shared_ptr<DataBlock>> seen;
			Detach(col->layout, seen);
			for (const Attribute& a : parent->attributes)
				if (a.inLayout)
				{
					col->attributes.push_back(a);
					DetachCopy(col->attributes.back());
				}
		}
		else
		{
			col->layout.bytes.assign(LayoutBytes(cls), 0);
			for (const auto& f : cls.fields)
			{
				if (!f.inLayout()) continue;
				Attribute a;
				a.key = f.key;
				a.def = &f;
				a.type = f.type;
				a.inLayout = true;
				a.elemSize = f.size;
				a.isArray = f.isArray();
				a.count = a.isArray ? 0 : 1;
				a.capacity = a.isArray ? f.maxCount : 0;
				a.encodedTypePad = a.isArray ? ArrayPad(f) : 0;
				a.data.bytes.assign(size_t(a.count) * a.elemSize, 0);
				col->attributes.push_back(std::move(a));
			}
		}
		Collection* raw = col.get();
		db_.collectionByKey_[(uint64_t(cls.key) << 32) | key] = raw;
		cls.collections.push_back(raw);
		vault.collections.push_back(raw);
		InsertExport(vault, *raw);
		if (parent) parent->children.push_back(raw);
		db_.collections_.push_back(std::move(col));
		UpdateIndexTables(cls, IndexTableNames(db_));
		return *raw;
	}

	bool CollectionEditor::remove(Collection& col)
	{
		if (!col.children.empty())
			return false;
		auto erase = [&](std::vector<Collection*>& v)
		{
			v.erase(std::remove(v.begin(), v.end(), &col), v.end());
		};
		if (col.parent) erase(col.parent->children);
		if (col.cls) erase(col.cls->collections);
		if (col.vault) EraseExport(*col.vault, col);
		const uint64_t id = (uint64_t(col.classKey) << 32) | col.key;
		auto it = db_.collectionByKey_.find(id);
		if (it != db_.collectionByKey_.end() && it->second == &col)
			db_.collectionByKey_.erase(it);
		ClassDef* cls = col.cls;
		auto& all = db_.collections_;
		all.erase(std::remove_if(all.begin(), all.end(), [&](const std::unique_ptr<Collection>& p)
		{
			return p.get() == &col;
		}),
				  all.end());
		if (cls) UpdateIndexTables(*cls, IndexTableNames(db_));
		return true;
	}

	void CollectionEditor::rename(Collection& col, const std::string& newName)
	{
		if (newName.empty())
			throw std::runtime_error("a collection needs a name");
		const uint32_t newKey = StringHash32(newName);
		if (newKey == col.key) return;
		if (db_.findCollection(col.classKey, newKey))
			throw std::runtime_error("collection " + db_.names().format(newKey) + " already exists in " + db_.names().format(col.classKey));
		db_.collectionByKey_.erase((uint64_t(col.classKey) << 32) | col.key);
		col.key = newKey;
		col.exportId = col.vault ? CollectionExportId(*col.vault, db_.names(), col.classKey, newName)
			: CollectionExportId(db_.names(), col.classKey, newName);
		db_.names().add(newName);
		db_.collectionByKey_[(uint64_t(col.classKey) << 32) | newKey] = &col;
		for (Collection* child : col.children)
			child->parentKey = newKey;
		if (col.cls) UpdateIndexTables(*col.cls, IndexTableNames(db_));
	}

	void CollectionEditor::rename(Collection& col, uint32_t newKey)
	{
		if (const std::string* n = db_.names().find(newKey))
		{
			rename(col, *n);
			return;
		}
		if (newKey == col.key) return;
		if (db_.findCollection(col.classKey, newKey))
			throw std::runtime_error("collection " + db_.names().format(newKey) + " already exists in " + db_.names().format(col.classKey));
		db_.collectionByKey_.erase((uint64_t(col.classKey) << 32) | col.key);
		col.key = newKey;
		const std::string shown = db_.names().format(newKey);
		col.exportId = col.vault ? CollectionExportId(*col.vault, db_.names(), col.classKey, shown)
			: CollectionExportId(db_.names(), col.classKey, shown);
		db_.collectionByKey_[(uint64_t(col.classKey) << 32) | newKey] = &col;
		for (Collection* child : col.children)
			child->parentKey = newKey;
		if (col.cls) UpdateIndexTables(*col.cls, IndexTableNames(db_));
	}

	bool CollectionEditor::move(Collection& col, Collection* newParent)
	{
		// A root whose parent isn't loaded still names it; moving to the root clears that.
		if (newParent == col.parent && (newParent || !col.parentKey)) return true;
		if (newParent && newParent->cls != col.cls) return false;
		for (const Collection* p = newParent; p; p = p->parent)
			if (p == &col) return false;
		if (col.parent)
		{
			auto& v = col.parent->children;
			v.erase(std::remove(v.begin(), v.end(), &col), v.end());
		}
		col.parent = newParent;
		col.parentKey = newParent ? newParent->key : 0;
		if (newParent) newParent->children.push_back(&col);
		if (col.vault) KeepParentsFirst(*col.vault);
		if (col.cls) UpdateIndexTables(*col.cls, IndexTableNames(db_));
		return true;
	}

	void CollectionEditor::moveToVault(Collection& col, Vault& vault)
	{
		if (col.vault == &vault) return;
		if (col.vault && col.vault->raw.endian != vault.raw.endian)
			throw std::runtime_error("vault " + vault.name + " has the other byte order than " + col.vault->name);
		if (col.vault) EraseExport(*col.vault, col);
		col.vault = &vault;
		vault.collections.push_back(&col);
		InsertExport(vault, col);
		KeepParentsFirst(vault);
	}

} // namespace attrib
