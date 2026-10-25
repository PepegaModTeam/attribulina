#include "ClassList.h"

#include "Hash.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace attrib
{

	namespace sc = script;

	namespace
	{
		bool IsHex(std::string_view s)
		{
			if (s.size() < 3 || s.size() > 10 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
			for (size_t i = 2; i < s.size(); ++i)
				if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
			return true;
		}
	} // namespace

	uint32_t ClassStatementKey(const sc::Statement& s)
	{
		const std::string& t = s.word;
		if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return StringHash32(t.substr(1, t.size() - 2));
		if (IsHex(t)) return uint32_t(std::strtoul(t.c_str() + 2, nullptr, 16));
		return StringHash32(t);
	}

	std::shared_ptr<ClassList> ClassList::Parse(std::string_view text, const std::string& source)
	{
		auto list = std::make_shared<ClassList>();
		list->source = source;
		list->script = sc::Parse(text, source);
		const std::vector<sc::Statement>& st = list->script.statements;
		for (size_t i = 0; i < st.size(); ++i)
		{
			const sc::Statement& s = st[i];
			if (s.kind == sc::Statement::Kind::Meta && s.word == "game") list->target = ParseTarget(s.text);
			else if (s.kind == sc::Statement::Kind::Meta && s.word == "title") list->title = s.text;
			else if (s.kind == sc::Statement::Kind::Meta && s.word == "version")
			{
				list->keys64 = s.text == "64-bit keys";
				if (!list->keys64) list->vers = std::strtoull(s.text.c_str(), nullptr, 0);
			}
			else if (s.kind == sc::Statement::Kind::Class)
			{
				const uint32_t key = ClassStatementKey(s);
				if (list->index_.emplace(key, i).second) list->keys_.push_back(key);
			}
		}
		return list;
	}

	std::shared_ptr<ClassList> ClassList::Load(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) throw std::runtime_error("can't read " + path);
		std::ostringstream ss;
		ss << in.rdbuf();
		std::string source = path;
		const size_t slash = source.find_last_of("/\\");
		if (slash != std::string::npos) source = source.substr(slash + 1);
		return Parse(ss.str(), source);
	}

	const sc::Statement* ClassList::find(uint32_t classKey) const
	{
		auto it = index_.find(classKey);
		return it == index_.end() ? nullptr : &script.statements[it->second];
	}

	bool IsGameClass(const Database& db, uint32_t classKey)
	{
		const Game game = db.target().game;
		for (const auto& l : db.classLists())
		{
			if (game != Game::Unknown && (!l->target || l->target->game != game)) continue;
			if (l->find(classKey)) return true;
		}
		return false;
	}

	namespace
	{
		const Vault* DatabaseVault(const Database& db)
		{
			for (const auto& pk : db.packs())
				for (const auto& v : pk->vaults)
					if (v->isDatabase) return v.get();
			return nullptr;
		}
	} // namespace

	const ClassList* RetailClassesFor(const Database& db)
	{
		const Vault* dv = DatabaseVault(db);
		const uint64_t vers = dv && dv->raw.hasVers ? dv->raw.version : 0;
		const ClassList* best = nullptr;
		bool bestVers = false;
		size_t bestShared = 0;
		for (const auto& l : db.classLists())
		{
			if (!l->target || !(*l->target == db.target())) continue;
			const bool sameVers = l->vers == vers && l->keys64 == db.keys64();
			size_t shared = 0;
			for (const auto& c : db.classes()) shared += l->find(c->key) != nullptr;
			// The files' Vers value, then the most in common; of two alike, the
			// one with fewer the database lacks.
			const bool better = !best || (sameVers != bestVers ? sameVers
				: shared != bestShared ? shared > bestShared : l->keys().size() < best->keys().size());
			if (better)
			{
				best = l.get();
				bestVers = sameVers;
				bestShared = shared;
			}
		}
		return best;
	}

	std::vector<const sc::Statement*> MissingRetailClasses(const Database& db)
	{
		std::vector<const sc::Statement*> out;
		if (const ClassList* l = RetailClassesFor(db))
			for (uint32_t k : l->keys())
				if (!db.findClass(k)) out.push_back(l->find(k));
		return out;
	}

	sc::ApplyReport RestoreRetailClass(Database& db, uint32_t classKey)
	{
		sc::ApplyReport rep;
		const ClassList* l = RetailClassesFor(db);
		const sc::Statement* s = l ? l->find(classKey) : nullptr;
		if (!s)
		{
			rep.messages.push_back(sc::Message(sc::Message::Level::Error, {},
											  l ? db.names().format(classKey) + " isn't one of " + l->title + "'s classes"
												: "no retail class list for " + db.target().name()));
			return rep;
		}
		sc::Script one;
		one.source = l->source;
		one.statements.push_back(*s);
		sc::ApplySettings st;
		st.fragment = true;
		return sc::Apply(db, one, st);
	}

	std::string WriteClassList(const Database& db, const std::string& title)
	{
		sc::Script s = sc::NewScript(db);
		s.setMeta("title", title);
		if (const Vault* dv = DatabaseVault(db); dv && dv->raw.hasVers)
		{
			char vers[24];
			std::snprintf(vers, sizeof(vers), "0x%016llX", (unsigned long long)dv->raw.version);
			s.setMeta("version", vers);
		}
		else if (db.keys64())
		{
			s.setMeta("version", "64-bit keys");
		}
		for (const auto& c : db.classes()) s.statements.push_back(sc::ClassStatement(db, *c, true));
		return sc::Print(s);
	}

} // namespace attrib
