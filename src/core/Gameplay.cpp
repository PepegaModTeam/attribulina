#include "Gameplay.h"

#include "Compression.h"
#include "Edit.h"
#include "Hash.h"
#include "Values.h"
#include "lua/Lua.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace attrib
{

	using namespace gameplay;

	namespace
	{

		std::string Lower(std::string s)
		{
			for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		std::string LastSegment(const std::string& path)
		{
			const size_t at = path.rfind('/');
			return at == std::string::npos ? path : path.substr(at + 1);
		}

		Endian EndianOf(const Collection& c)
		{
			return c.vault ? c.vault->raw.endian : Endian::Little;
		}

		bool IsKeyType(const Attribute& a)
		{
			return a.type && a.type->name == "GCollectionKey" && (a.elemSize == 4 || a.elemSize == 8);
		}

		// A GCollectionKey element: a u32, or an early Most Wanted demo's u64
		// StringHash64 (Values.h LoadKey), as the model's key.
		uint32_t KeyAt(const Database& db, const Attribute& a, uint32_t i, Endian e)
		{
			return LoadKey(db, a.elem(i), a.elemSize, e);
		}
		void SetKeyAt(const Database& db, Attribute& a, uint32_t i, uint32_t key, Endian e)
		{
			if (!StoreKey(db, a.elem(i), a.elemSize, key, e)) throw std::runtime_error("no 64-bit key for " + db.names().format(key));
		}
		bool IsTextType(const Attribute& a)
		{
			return a.type && a.type->kind == TypeKind::Text;
		}

		std::string TextAt(const Attribute& a, uint32_t i)
		{
			const PtrSlot* p = a.data.ptrAt(i * a.elemSize);
			return p ? p->text : std::string();
		}

		void SetText(Attribute& a, uint32_t i, const std::string& text)
		{
			const uint32_t off = i * a.elemSize;
			if (PtrSlot* p = a.data.ptrAt(off))
			{
				p->isString = true;
				p->data.reset();
				p->text = text;
				return;
			}
			PtrSlot s;
			s.offset = off;
			s.isString = true;
			s.text = text;
			a.data.ptrs.push_back(std::move(s));
		}

		// The collection's own value for a field, created (copying any inherited
		// value) when it has none.
		Attribute& Own(Database& db, Collection& c, const char* field)
		{
			const FieldDef* f = c.cls ? c.cls->field(StringHash32(field)) : nullptr;
			if (!f) throw std::runtime_error(std::string("the gameplay class has no field ") + field);
			return OverrideAttribute(db, c, *f);
		}

		void AppendKey(Database& db, Collection& owner, uint32_t key)
		{
			Attribute& ch = Own(db, owner, kChildren);
			if (ch.count >= MaxElements(ch))
				throw std::runtime_error("the Children list of " + db.names().format(owner.key) + " is full");
			InsertElement(ch, ch.count);
			SetKeyAt(db, ch, ch.count - 1, key, EndianOf(owner));
		}

		void RemoveKey(const Database& db, Collection& owner, uint32_t key)
		{
			Attribute* ch = owner.find(StringHash32(kChildren));
			if (!ch) return;
			for (uint32_t i = ch->count; i-- > 0;)
				if (KeyAt(db, *ch, i, EndianOf(owner)) == key) RemoveElement(*ch, i);
		}

		Vault& EnsureVault(LoadedPack& pack, const std::string& name, const Vault& like)
		{
			if (Vault* v = FindVault(pack, name)) return *v;
			return CreateVault(pack, name, like);
		}

	} // namespace

	// ------------------------------------------------------------------ model ---

	GameplayModel::GameplayModel(const Database& db) : db_(db)
	{
		root_ = std::make_unique<Node>();
		cls_ = db.findClass(StringHash32("gameplay"));
		if (!cls_) return;
		const KeyNames& names = db.names();

		auto ensure = [&](const std::string& path) -> Node*
		{
			auto it = byPath_.find(path);
			if (it != byPath_.end()) return it->second;
			Node* parent = root_.get();
			size_t start = 0;
			while (true)
			{
				const size_t slash = path.find('/', start);
				const std::string prefix = path.substr(0, slash);
				auto f = byPath_.find(prefix);
				Node* n;
				if (f != byPath_.end())
				{
					n = f->second;
				}
				else
				{
					nodes_.push_back(std::make_unique<Node>());
					n = nodes_.back().get();
					n->path = prefix;
					n->segment = prefix.substr(start);
					n->parent = parent;
					parent->children.push_back(n);
					byPath_[prefix] = n;
				}
				parent = n;
				if (slash == std::string::npos) return n;
				start = slash + 1;
			}
		};
		for (Collection* c : cls_->collections)
		{
			Node* n = ensure(names.format(c->key));
			n->collection = c;
			byCollection_[c] = n;
		}
		std::function<size_t(Node*)> finish = [&](Node* n) -> size_t
		{
			std::sort(n->children.begin(), n->children.end(),
					  [](const Node* a, const Node* b)
			{
				return Lower(a->segment) < Lower(b->segment);
			});
			size_t count = n->collection ? 1 : 0;
			for (Node* ch : n->children) count += finish(ch);
			n->elementCount = count;
			return count;
		};
		finish(root_.get());

		// Owners (Children lists), templates, state graphs.
		const uint32_t kChildrenKey = StringHash32(kChildren), kTemplateKey = StringHash32(kTemplate);
		const uint32_t kTemplateRef = StringHash32("templateref"), kStateRef = StringHash32("stateref");
		for (Collection* c : cls_->collections)
		{
			const Endian e = EndianOf(*c);
			if (const Attribute* ch = c->find(kChildrenKey); ch && IsKeyType(*ch))
				for (uint32_t i = 0; i < ch->count; ++i)
					if (Collection* kid = db.findCollection(cls_->key, KeyAt(db, *ch, i, e)))
						owners_.try_emplace(kid, c);
			const Attribute* t = c->find(kTemplateKey);
			const bool flagged = t && t->count && t->elemSize && t->elem(0)[0] != 0;
			const std::string n = names.format(c->key);
			if (flagged || (!c->children.empty() && n.find('/') == std::string::npos)) templates_.push_back(c);
			if (const Attribute* tr = c->find(kTemplateRef); tr && IsKeyType(*tr) && tr->count)
				if (Collection* act = db.findCollection(cls_->key, KeyAt(db, *tr, 0, e)))
				{
					states_[act].push_back(c);
					stateTemplates_[c] = act;
				}
			if (const Attribute* sr = c->find(kStateRef); sr && IsKeyType(*sr) && sr->count)
				if (Collection* st = db.findCollection(cls_->key, KeyAt(db, *sr, 0, e))) handlers_[st].push_back(c);
		}
		auto byName = [&](Collection* a, Collection* b)
		{
			return Lower(names.format(a->key)) < Lower(names.format(b->key));
		};
		std::sort(templates_.begin(), templates_.end(), byName);
		templateSet_.insert(templates_.begin(), templates_.end());
		for (auto& [k, v] : states_) std::sort(v.begin(), v.end(), byName);
		for (auto& [k, v] : handlers_) std::sort(v.begin(), v.end(), byName);

		// Reference fields, from what their values point at.
		struct Stats
		{
			bool text = false;
			size_t values = 0, hits = 0;
			std::map<uint32_t, size_t> classes;
			std::map<Collection*, size_t> templates;
		};
		std::map<uint32_t, Stats> stats;
		const uint32_t skipName = StringHash32(kName), skipCollectionName = StringHash32(kCollectionName);
		for (Collection* c : cls_->collections)
		{
			const Endian e = EndianOf(*c);
			for (const Attribute& a : c->attributes)
			{
				if (IsKeyType(a))
				{
					Stats& s = stats[a.key];
					for (uint32_t i = 0; i < a.count; ++i)
					{
						const uint32_t k = KeyAt(db, a, i, e);
						if (!k) continue;
						++s.values;
						Collection* target = db.findCollection(cls_->key, k);
						if (!target)
							for (const auto& other : db.classes())
								if ((target = db.findCollection(other->key, k))) break;
						if (!target) continue;
						++s.hits;
						++s.classes[target->classKey];
						if (target->parent) ++s.templates[target->parent];
					}
				}
				else if (IsTextType(a) && a.key != skipName && a.key != skipCollectionName)
				{
					Stats& s = stats[a.key];
					s.text = true;
					for (uint32_t i = 0; i < a.count; ++i)
					{
						const std::string t = TextAt(a, i);
						if (t.empty()) continue;
						++s.values;
						auto it = byPath_.find(t);
						if (it == byPath_.end() || !it->second->collection) continue;
						++s.hits;
						++s.classes[cls_->key];
						if (it->second->collection->parent) ++s.templates[it->second->collection->parent];
					}
				}
			}
		}
		for (const auto& f : cls_->fields)
		{
			const bool keyType = f.type && f.type->name == "GCollectionKey";
			auto it = stats.find(f.key);
			if (!keyType && (it == stats.end() || !it->second.text)) continue;
			RefField r;
			r.targetClass = cls_->key;
			if (it != stats.end())
			{
				const Stats& s = it->second;
				// A Text field counts when (nearly) all its values are paths.
				if (s.text && (s.hits == 0 || s.hits * 10 < s.values * 9)) continue;
				r.isText = s.text;
				size_t best = 0;
				for (const auto& [k, n] : s.classes)
					if (n > best)
					{
						best = n; r.targetClass = k;
					}
				std::vector<std::pair<Collection*, size_t>> ts(s.templates.begin(), s.templates.end());
				std::sort(ts.begin(), ts.end(), [](auto& a, auto& b)
				{
					return a.second > b.second;
				});
				for (const auto& [t, n] : ts) r.templates.push_back(t);
			}
			else if (!keyType)
			{
				continue;
			}
			refFields_[f.key] = std::move(r);
		}
	}

	const GameplayModel::Node* GameplayModel::node(const std::string& path) const
	{
		auto it = byPath_.find(path);
		return it == byPath_.end() ? nullptr : it->second;
	}

	const GameplayModel::Node* GameplayModel::nodeOf(const Collection* c) const
	{
		auto it = byCollection_.find(c);
		return it == byCollection_.end() ? nullptr : it->second;
	}

	bool GameplayModel::isTemplate(const Collection* c) const
	{
		return templateSet_.count(c) != 0;
	}

	Collection* GameplayModel::owner(const Collection* c) const
	{
		if (auto it = owners_.find(c); it != owners_.end()) return it->second;
		const Node* n = nodeOf(c);
		for (const Node* p = n ? n->parent : nullptr; p; p = p->parent)
			if (p->collection) return p->collection;
		return nullptr;
	}

	std::vector<Collection*> GameplayModel::listedChildren(const Collection* c) const
	{
		std::vector<Collection*> out;
		if (!cls_ || !c) return out;
		const Attribute* ch = c->find(StringHash32(kChildren));
		if (!ch || !IsKeyType(*ch)) return out;
		for (uint32_t i = 0; i < ch->count; ++i)
			if (Collection* kid = db_.findCollection(cls_->key, KeyAt(db_, *ch, i, EndianOf(*c)))) out.push_back(kid);
		return out;
	}

	std::string GameplayModel::childrenVaultName(const Collection* c) const
	{
		const Attribute* a = c ? c->resolve(StringHash32(kGameplayVault)) : nullptr;
		return a && IsTextType(*a) && a->count ? TextAt(*a, 0) : std::string();
	}

	const GameplayModel::RefField* GameplayModel::refField(uint32_t fieldKey) const
	{
		auto it = refFields_.find(fieldKey);
		return it == refFields_.end() ? nullptr : &it->second;
	}

	Collection* GameplayModel::resolveKey(uint32_t key, const RefField* field) const
	{
		if (!key) return nullptr;
		if (field && field->targetClass)
			if (Collection* c = db_.findCollection(field->targetClass, key)) return c;
		if (cls_)
			if (Collection* c = db_.findCollection(cls_->key, key)) return c;
		for (const auto& other : db_.classes())
			if (Collection* c = db_.findCollection(other->key, key)) return c;
		return nullptr;
	}

	bool GameplayModel::derivesFrom(const Collection* c, const Collection* templ) const
	{
		for (const Collection* p = c ? c->parent : nullptr; p; p = p->parent)
			if (p == templ) return true;
		return false;
	}

	std::vector<Collection*> GameplayModel::candidates(const RefField& field, bool all) const
	{
		std::vector<Collection*> out;
		const ClassDef* target = db_.findClass(field.targetClass ? field.targetClass : (cls_ ? cls_->key : 0));
		if (!target) return out;
		const bool filter = !all && target == cls_ && !field.templates.empty();
		for (Collection* c : target->collections)
		{
			if (filter)
			{
				bool ok = false;
				for (const Collection* t : field.templates) ok |= derivesFrom(c, t);
				if (!ok) continue;
			}
			out.push_back(c);
		}
		const KeyNames& names = db_.names();
		std::sort(out.begin(), out.end(), [&](Collection* a, Collection* b)
		{
			return Lower(names.format(a->key)) < Lower(names.format(b->key));
		});
		return out;
	}

	std::vector<Collection*> GameplayModel::statesOf(const Collection* activityTemplate) const
	{
		auto it = states_.find(activityTemplate);
		return it == states_.end() ? std::vector<Collection*>{} : it->second;
	}

	std::vector<Collection*> GameplayModel::allStatesOf(const Collection* activityTemplate) const
	{
		std::vector<Collection*> out;
		std::unordered_set<const Collection*> seen;
		for (const Collection* t = activityTemplate; t && seen.insert(t).second; t = t->parent)
			if (auto it = states_.find(t); it != states_.end()) out.insert(out.end(), it->second.begin(), it->second.end());
		return out;
	}

	Collection* GameplayModel::stateTemplate(const Collection* state) const
	{
		auto it = stateTemplates_.find(state);
		return it == stateTemplates_.end() ? nullptr : it->second;
	}

	std::string GameplayModel::stateName(const Collection* state) const
	{
		if (const Attribute* a = state->find(StringHash32(kName)); a && IsTextType(*a) && a->count)
			if (std::string n = TextAt(*a, 0); !n.empty()) return n;
		const std::string n = db_.names().format(state->key);
		if (const Collection* t = stateTemplate(state))
		{
			const std::string prefix = "stategraph_" + db_.names().format(t->key) + "_";
			if (n.size() > prefix.size() && n.rfind(prefix, 0) == 0) return n.substr(prefix.size());
		}
		return n;
	}

	std::vector<Collection*> GameplayModel::handlersOf(const Collection* state) const
	{
		auto it = handlers_.find(state);
		return it == handlers_.end() ? std::vector<Collection*>{} : it->second;
	}

	// ------------------------------------------------------------------ edits ---

	std::vector<Collection*> GameplaySubtree(const GameplayModel& m, Collection& c)
	{
		std::vector<Collection*> out;
		std::unordered_set<const Collection*> seen;
		std::function<void(Collection*)> walk = [&](Collection* x)
		{
			if (!seen.insert(x).second) return;
			out.push_back(x);
			for (Collection* kid : m.listedChildren(x)) walk(kid);
		};
		walk(&c);
		return out;
	}

	std::vector<Collection*> GameplayFolderContents(const GameplayModel& m, const GameplayModel::Node& folder)
	{
		std::vector<Collection*> out;
		std::unordered_set<const Collection*> seen;
		std::function<void(Collection*)> listed = [&](Collection* x)
		{
			if (!seen.insert(x).second) return;
			out.push_back(x);
			for (Collection* kid : m.listedChildren(x)) listed(kid);
		};
		std::function<void(const GameplayModel::Node&)> walk = [&](const GameplayModel::Node& n)
		{
			if (n.collection) listed(n.collection);
			for (const GameplayModel::Node* k : n.children) walk(*k);
		};
		walk(folder);
		return out;
	}

	std::string StorageVaultName(const GameplayModel& m, const Collection* owner)
	{
		return owner ? m.childrenVaultName(owner) : std::string();
	}

	Collection& CreateGameplayElement(Database& db, const GameplayModel& m, const NewGameplayElement& e)
	{
		ClassDef* cls = m.cls();
		if (!cls) throw std::runtime_error("no gameplay class is loaded");
		if (e.path.empty()) throw std::runtime_error("the element needs a name");
		if (!e.templ) throw std::runtime_error("choose a template");
		if (db.findCollection(cls->key, StringHash32(e.path))) throw std::runtime_error(e.path + " already exists");

		// Stored where the owner keeps its children.
		const Collection* anchor = e.owner ? e.owner : e.templ;
		Vault* vault = anchor->vault;
		if (const std::string vname = StorageVaultName(m, e.owner); !vname.empty() && anchor->vault && anchor->vault->pack)
			vault = &EnsureVault(*anchor->vault->pack, vname, *anchor->vault);
		if (!vault) throw std::runtime_error("no vault to store the element in");

		Collection& c = CollectionEditor(db).create(*vault, *cls, e.path, e.templ);
		db.binNames().add(e.path);
		for (const char* f : { kName, kCollectionName }) SetText(Own(db, c, f), 0, e.path);
		if (cls->field(StringHash32(kTemplate)))
		{
			Attribute& t = Own(db, c, kTemplate);
			std::fill(t.data.bytes.begin(), t.data.bytes.end(), uint8_t(0));
		}
		if (!e.childrenVault.empty()) SetText(Own(db, c, kGameplayVault), 0, e.childrenVault);
		if (e.owner) AppendKey(db, *e.owner, c.key);
		return c;
	}

	lua::Version LuaVersionOf(const Database& db)
	{
		const Target t = db.target();
		const bool current = t.game == Game::World || (t.game == Game::Undercover && t.platform != Platform::Wii && t.platform != Platform::PS2);
		return current ? lua::Version::Lua51 : lua::Version::Lua50;
	}

	lua::Version VersionOfChunk(const Database& db, ByteSpan chunk)
	{
		return lua::DetectVersion(chunk).value_or(LuaVersionOf(db));
	}

	std::string HandlerFunction(const Database& db, std::string_view handlerName)
	{
		return lua::HandlerFunctionName(handlerName, db.target().game == Game::MostWanted);
	}

	bool RenameHandlerFunction(Database& db, Collection& handler, const std::string& oldFunction, std::string* error)
	{
		const std::string newFunction = HandlerFunction(db, db.names().format(handler.key));
		try
		{
			const Bytes chunk = HandlerChunk(handler);
			if (chunk.empty() || oldFunction == newFunction) return true;
			std::string src = lua::Decompile(chunk).source;
			// Whole identifiers only.
			size_t renamed = 0;
			for (size_t at = src.find(oldFunction); at != std::string::npos; at = src.find(oldFunction, at + newFunction.size()))
			{
				const bool before = at > 0 && (std::isalnum(static_cast<unsigned char>(src[at - 1])) || src[at - 1] == '_');
				const size_t end = at + oldFunction.size();
				const bool after = end < src.size() && (std::isalnum(static_cast<unsigned char>(src[end])) || src[end] == '_');
				if (before || after)
				{
					at += 1;
					continue;
				}
				src.replace(at, oldFunction.size(), newFunction);
				++renamed;
			}
			if (!renamed)
			{
				if (error) *error = "its Lua doesn't define " + oldFunction;
				return false;
			}
			const lua::CompileResult r = lua::Compile(src, VersionOfChunk(db, chunk));
			if (!r.ok())
			{
				if (error) *error = r.error;
				return false;
			}
			SetHandlerChunk(db, handler, r.chunk);
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	Collection& DuplicateGameplayElement(Database& db, const GameplayModel& m, Collection& src, const std::string& newPath,
										 bool withChildren, std::vector<std::string>* warnings)
	{
		ClassDef* cls = m.cls();
		if (!cls) throw std::runtime_error("no gameplay class is loaded");
		const KeyNames& names = db.names();
		const std::string oldRoot = names.format(src.key);
		if (newPath.empty() || newPath == oldRoot) throw std::runtime_error("choose a new name");
		const std::string oldLeaf = LastSegment(oldRoot), newLeaf = LastSegment(newPath);
		LoadedPack* pack = src.vault ? src.vault->pack : nullptr;
		if (!pack) throw std::runtime_error("the element isn't in a loaded pack");

		const std::vector<Collection*> subtree = withChildren ? GameplaySubtree(m, src) : std::vector<Collection*>{ &src };
		const std::unordered_set<const Collection*> inTree(subtree.begin(), subtree.end());

		// New names, checked before anything changes.
		std::unordered_map<const Collection*, std::string> newName;
		std::unordered_map<uint32_t, uint32_t> keyMap;
		std::unordered_map<std::string, std::string> pathMap;
		std::unordered_set<uint32_t> taken;
		for (Collection* o : subtree)
		{
			const std::string op = names.format(o->key);
			std::string np;
			if (o == &src) np = newPath;
			else if (op.rfind(oldRoot + "/", 0) == 0) np = newPath + op.substr(oldRoot.size());
			else np = newPath + "/" + LastSegment(op);
			const uint32_t k = StringHash32(np);
			if (db.findCollection(cls->key, k) || !taken.insert(k).second) throw std::runtime_error(np + " already exists");
			newName[o] = np;
			keyMap[o->key] = k;
			pathMap[op] = np;
		}
		// Vaults the subtree's elements keep their children in get new names.
		std::unordered_map<std::string, std::string> vaultMap;
		std::unordered_set<std::string> usedVaults;
		for (const auto& v : pack->vaults) usedVaults.insert(v->name);
		for (Collection* o : subtree)
		{
			const Attribute* gv = o->find(StringHash32(kGameplayVault));
			if (!gv || !IsTextType(*gv) || !gv->count) continue;
			const std::string old = TextAt(*gv, 0);
			if (old.empty() || vaultMap.count(old)) continue;
			std::string base = old;
			if (const size_t at = old.find(oldLeaf); at != std::string::npos && !oldLeaf.empty()) base.replace(at, oldLeaf.size(), newLeaf);
			else base = old + "_" + newLeaf;
			std::string name = base;
			for (int n = 2; usedVaults.count(name) || (name == old); ++n) name = base + "_" + std::to_string(n);
			usedVaults.insert(name);
			vaultMap[old] = name;
		}

		std::unordered_map<const Collection*, Collection*> copies;
		for (Collection* o : subtree)
		{
			Vault* vault = o->vault;
			if (o != &src)
			{
				// Stored in its (copied) owner's new children vault.
				const Collection* ow = m.owner(o);
				const std::string ov = ow && inTree.count(ow) ? m.childrenVaultName(ow) : std::string();
				if (auto it = vaultMap.find(ov); it != vaultMap.end()) vault = &EnsureVault(*pack, it->second, *o->vault);
			}
			Collection& n = CollectionEditor(db).create(*vault, *cls, newName[o], o->parent);
			db.binNames().add(newName[o]);
			n.layout = o->layout;
			for (auto& p : n.layout.ptrs) p.seq = kNewPtr;
			n.attributes = o->attributes;
			for (Attribute& a : n.attributes) DetachCopy(a);
			copies[o] = &n;
		}

		// Re-point names, vaults and references inside the copy.
		const uint32_t kGv = StringHash32(kGameplayVault), kCh = StringHash32(kChildren);
		for (auto& [o, n] : copies)
		{
			const Endian e = EndianOf(*n);
			for (Attribute& a : n->attributes)
			{
				if (IsKeyType(a))
				{
					for (uint32_t i = 0; i < a.count; ++i)
					{
						auto it = keyMap.find(KeyAt(db, a, i, e));
						if (it != keyMap.end()) SetKeyAt(db, a, i, it->second, e);
					}
				}
				else if (IsTextType(a))
				{
					for (uint32_t i = 0; i < a.count; ++i)
					{
						const std::string t = TextAt(a, i);
						if (a.key == kGv)
						{
							if (auto it = vaultMap.find(t); it != vaultMap.end()) SetText(a, i, it->second);
						}
						else if (auto it = pathMap.find(t); it != pathMap.end())
						{
							SetText(a, i, it->second);
						}
					}
				}
			}
			if (!withChildren)
			{
				// Only the element: it starts without children of its own.
				RevertAttribute(*n, kCh);
				if (Attribute* gv = n->find(kGv); gv && IsTextType(*gv) && gv->count)
				{
					const std::string t = TextAt(*gv, 0);
					if (auto it = vaultMap.find(t); it != vaultMap.end()) SetText(*gv, 0, it->second);
				}
			}
		}
		// Handlers answer under a function named for them: give the copies theirs.
		const uint32_t kBytecode = StringHash32("bytecode");
		for (auto& [o, n] : copies)
		{
			if (!n->find(kBytecode)) continue;
			std::string error;
			if (!RenameHandlerFunction(db, *n, HandlerFunction(db, names.format(o->key)), &error) && warnings)
				warnings->push_back(newName[o] + ": could not recompile its Lua under the new name (" + error + ")");
		}
		Collection& root = *copies[&src];
		if (Collection* ow = m.owner(&src)) AppendKey(db, *ow, root.key);
		return root;
	}

	std::vector<Collection*> DeleteGameplayElement(Database& db, const GameplayModel& m, Collection& c)
	{
		std::vector<Collection*> subtree = GameplaySubtree(m, c);
		const std::unordered_set<const Collection*> inTree(subtree.begin(), subtree.end());
		for (Collection* x : subtree)
			for (Collection* kid : x->children)
				if (!inTree.count(kid))
					throw std::runtime_error(db.names().format(x->key) + " is the template of " + db.names().format(kid->key) +
											 ", which isn't being deleted");
		std::vector<Vault*> vaults;
		for (Collection* x : subtree)
			if (const std::string v = m.childrenVaultName(x); !v.empty() && x->vault && x->vault->pack)
				if (Vault* found = FindVault(*x->vault->pack, v)) vaults.push_back(found);
		if (Collection* ow = m.owner(&c); ow && !inTree.count(ow)) RemoveKey(db, *ow, c.key);
		// Templates inside the subtree go after the collections that use them.
		std::vector<Collection*> left = subtree;
		CollectionEditor ed(db);
		while (!left.empty())
		{
			const size_t before = left.size();
			for (size_t i = left.size(); i-- > 0;)
				if (left[i]->children.empty())
				{
					ed.remove(*left[i]);
					left.erase(left.begin() + ptrdiff_t(i));
				}
			if (left.size() == before) throw std::runtime_error("could not delete every collection in the subtree");
		}
		for (Vault* v : vaults)
			if (v->pack) RemoveVault(*v->pack, *v);
		return subtree;
	}

	// --------------------------------------------------- messages and handlers ---

	std::vector<MessageDef> LoadMessages(const std::string& path)
	{
		const Bytes data = ReadFile(path);
		std::vector<MessageDef> out;
		// "carbon_pc.txt": Carbon's.
		const size_t slash = path.find_last_of("/\\");
		const std::string file = path.substr(slash == std::string::npos ? 0 : slash + 1);
		const Game game = GameById(file.substr(0, file.find('_')));
		size_t start = 0;
		for (size_t i = 0; i <= data.size(); ++i)
		{
			if (i < data.size() && data[i] != '\n') continue;
			std::string line(reinterpret_cast<const char*>(data.data()) + start, i - start);
			start = i + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty() || line[0] == '#') continue;
			MessageDef m;
			size_t pos = 0;
			while (pos < line.size())
			{
				const size_t sp = line.find(' ', pos);
				const std::string tok = line.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
				pos = sp == std::string::npos ? line.size() : sp + 1;
				if (tok.empty()) continue;
				if (m.name.empty())
				{
					m.name = tok;
				}
				else
				{
					const size_t colon = tok.find(':');
					m.fields.emplace_back(tok.substr(0, colon), colon == std::string::npos ? std::string() : tok.substr(colon + 1));
				}
			}
			m.game = game;
			if (!m.name.empty()) out.push_back(std::move(m));
		}
		return out;
	}

	std::vector<const MessageDef*> MessagesFor(const std::vector<MessageDef>& all, Game game)
	{
		std::map<std::string, const MessageDef*> best;
		for (const MessageDef& m : all)
		{
			const MessageDef*& b = best[m.name];
			if (!b || (b->game != game && (m.game == game || m.fields.size() > b->fields.size()))) b = &m;
		}
		std::vector<const MessageDef*> out;
		for (const auto& [name, m] : best) out.push_back(m);
		return out;
	}

	void SetGameplayKey(Database& db, Collection& c, const char* field, uint32_t key)
	{
		Attribute& a = Own(db, c, field);
		if (a.count == 0) InsertElement(a, 0);
		SetKeyAt(db, a, 0, key, EndianOf(c));
	}

	void SetGameplayText(Database& db, Collection& c, const char* field, const std::string& text)
	{
		Attribute& a = Own(db, c, field);
		if (a.count == 0) InsertElement(a, 0);
		SetText(a, 0, text);
	}

	Bytes HandlerChunk(const Collection& handler)
	{
		const Attribute* a = handler.resolve(StringHash32("bytecode"));
		const PtrSlot* p = a && a->count ? a->data.ptrAt(4) : nullptr;
		if (!p || !p->data) return {};
		const Bytes& b = p->data->bytes;
		return ParseLzHeader(b) ? Decompress(b) : b;
	}

	void SetHandlerChunk(Database& db, Collection& handler, ByteSpan chunk)
	{
		Attribute& a = Own(db, handler, "bytecode");
		if (a.count == 0) InsertElement(a, 0);
		const Bytes packed = CompressBest(chunk);
		PtrSlot* p = a.data.ptrAt(4);
		if (!p)
		{
			PtrSlot s;
			s.offset = 4;
			a.data.ptrs.push_back(std::move(s));
			p = &a.data.ptrs.back();
		}
		auto block = std::make_shared<DataBlock>();
		// A new one as the vault's others: Most Wanted puts compiled Lua on 16 bytes.
		block->align = p->data ? p->data->align : handler.vault && handler.vault->raw.mostWantedOwn() ? 16 : 1;
		block->bytes = packed;
		p->isString = false;
		p->data = std::move(block);
		store32(a.elem(0), uint32_t(packed.size()), EndianOf(handler));
	}

	void SetHandlerTransitions(Database& db, Collection& handler, const std::vector<uint32_t>& states)
	{
		const uint32_t key = StringHash32("transitionlist");
		if (states.empty())
		{
			RevertAttribute(handler, key);
			return;
		}
		Attribute& a = Own(db, handler, "transitionlist");
		while (a.count) RemoveElement(a, a.count - 1);
		for (uint32_t s : states)
		{
			if (a.count >= MaxElements(a)) break;
			InsertElement(a, a.count);
			SetKeyAt(db, a, a.count - 1, s, EndianOf(handler));
		}
	}

	std::vector<std::string> ChangeStateTargets(std::string_view src)
	{
		std::vector<std::string> out;
		const std::string_view fn = "ChangeState";
		for (size_t at = src.find(fn); at != std::string_view::npos; at = src.find(fn, at + fn.size()))
		{
			// Skip names that only end in ChangeState (MyChangeState).
			if (at > 0 && (std::isalnum(static_cast<unsigned char>(src[at - 1])) || src[at - 1] == '_')) continue;
			size_t p = at + fn.size();
			while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
			if (p >= src.size() || src[p] != '(') continue;
			++p;
			while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
			if (p >= src.size() || (src[p] != '"' && src[p] != '\'')) continue;
			const char q = src[p++];
			const size_t end = src.find(q, p);
			if (end == std::string_view::npos) break;
			std::string name(src.substr(p, end - p));
			if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
		}
		return out;
	}

} // namespace attrib
