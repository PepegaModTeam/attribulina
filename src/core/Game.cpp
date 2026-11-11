#include "Game.h"

#include "Folder.h"
#include "Hash.h"
#include "Pack.h"
#include "RawVault.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unordered_set>

namespace attrib
{

	namespace fs = std::filesystem;

	namespace
	{

		using P = Platform;

		// Most Wanted (its own vaults and its early demos'), Pro Street, Carbon,
		// Undercover (PC, 360, and its Wii and PS2 version) and World (its final PC
		// build, its 2009 and 2010 betas and the builds between) are edited for real
		// so far.
		// To add a game or platform: its type schema (scripts/gen_schema.py,
		// data/schema/<game>_<platform>.txt with the game and platforms in its
		// header), the hand pointer layouts its types need (TypeLayouts.cpp),
		// round-trip tests for its packs, its NFS-VltEd words for nfsms
		// (scripts/nfsms/gen_words.py writes data/nfsms/<game>_pc.txt; nfsms runs on
		// each supported platform, and tests/test_nfsms.cpp checks every word leads
		// to the same member on each), its Vers values below, then list the platform
		// in `supported`. The Game and Platform prompt lists every game already.
		// Pro Street's PS3 files, Carbon's Xbox files, and Undercover's PS2 and PS3
		// files haven't been seen. Carbon's retail 360 files carry its PS2's and
		// GameCube's Vers value and the PC's layouts; a demo made after release has
		// older ones (carbon_x360_demo.txt). Undercover's 360 alpha (build 161) has
		// a schema of its own (undercover_x360_alpha.txt). Undercover's PS3 may share
		// the 360's Vers value (it would be detected as the 360), so it stays
		// unsupported until its files show what tells them apart.
		const std::vector<GameInfo> kGames = {
			// The 360's and the PS2's files here are betas.
			{Game::MostWanted, "mw", "Most Wanted", {P::PC, P::PS2, P::Xbox, P::Xbox360, P::GameCube}, {P::PC, P::PS2, P::Xbox, P::Xbox360, P::GameCube}},
			{Game::Carbon, "carbon", "Carbon", {P::PC, P::PS2, P::PS3, P::Xbox360, P::GameCube, P::Wii},
			 {P::PC, P::PS2, P::PS3, P::Xbox, P::Xbox360, P::GameCube, P::Wii}},
			{Game::ProStreet, "prostreet", "Pro Street", {P::PC, P::PS2, P::PS3, P::Xbox360, P::Wii}, {P::PC, P::PS2, P::PS3, P::Xbox360, P::Wii}},
			// The Wii's and the PS2's Undercover is one game on Carbon's engine,
			// unlike the others (Pro Street's layout with StrN first, its own types).
			{Game::Undercover, "undercover", "Undercover", {P::PC, P::PS2, P::Xbox360, P::Wii}, {P::PC, P::PS2, P::PS3, P::Xbox360, P::Wii}},
			// One schema for the final build, the betas and the builds between
			// (IsWorldBetaVersion).
			{Game::World, "world", "World", {P::PC}, {P::PC}},
			// Nitro (2009) runs on Carbon's engine too, with many types of its own;
			// its Wii version has AttribSys files.
			{Game::Nitro, "nitro", "Nitro", {}, {P::Wii}},
		};

		const std::vector<PlatformInfo> kPlatforms = {
			{P::PC, "pc", "PC", Endian::Little},
			{P::PS2, "ps2", "PS2", Endian::Little},
			{P::PS3, "ps3", "PS3", Endian::Big},
			{P::Xbox, "xbox", "Xbox", Endian::Little},
			{P::Xbox360, "x360", "Xbox 360", Endian::Big},
			{P::GameCube, "gamecube", "GameCube", Endian::Big},
			{P::Wii, "wii", "Wii", Endian::Big},
		};

		struct KnownVersion
		{
			uint64_t value;
			Endian endian;
			Game game;
			Platform platform;
			// For files that share a value and byte order: a class attributes.bin
			// has (or, with `without`, lacks) on this platform,
			uint32_t classKey = 0;
			bool without = false;
			// or the width of EA::Reflection::Bool in its type table.
			uint32_t boolSize = 0;
		};

		constexpr uint32_t kPipEventTrigger = 0x4D25C5ACu; // StringHash32("pip_event_trigger")
		// StringHash32("input"): a class every World build's attributes.bin has and
		// no other game's. OGVI (after the community Attribulator) saved a Carbon
		// attributes.bin with the final build's Vers (crashdumps/crash5); without
		// the class, World's values don't make a file World's.
		constexpr uint32_t kWorldInput = 0x9C74903Eu;
		constexpr uint64_t kWorldBeta2009 = 0xD5424397B2A8D273ull, kWorldBeta2010 = 0x2EDE8B2CB6712231ull;

		// Vers values, read from the databases in attrib/databases. A Vers value is
		// Attrib::SchemaVersion(), a constant the AttribSys code generator wrote into
		// each build, stamped on every vault as it was exported (one gameplay.bin can
		// hold two). The game never checks it: it only sends it to Attribulator's live
		// link, as "AttribSys.<platform>\<16 hex digits>". It follows the schema's
		// fields, not a platform's layouts: Pro Street's PS2 files (4-byte bools) and
		// Wii files (the PC's layouts) share one, and Carbon's PS2, GameCube and 360
		// files another. Byte order tells most pairs apart (Pro Street's PC and 360
		// files, Carbon's PS2 and GameCube files). Carbon's Wii and PS3 files share a
		// value and byte order (only the PS3's attributes.bin has the
		// picture-in-picture classes), and so do its GameCube and 360 files (the
		// GameCube's bools are 4 bytes). A platform with no files there (Pro Street
		// and Undercover on PS3, Carbon's Xbox files) may share a value too, and
		// would be detected as the platform listed here.
		constexpr KnownVersion kKnownVersions[] = {
			{0xD0A78AB8BE7F5412ull, Endian::Little, Game::ProStreet, P::PC},
			{0xD0A78AB8BE7F5412ull, Endian::Big, Game::ProStreet, P::Xbox360},
			{0x6CDFE96B890BB714ull, Endian::Little, Game::ProStreet, P::PS2},
			{0x6CDFE96B890BB714ull, Endian::Big, Game::ProStreet, P::Wii},
			{0x2217DA39ED2E3778ull, Endian::Little, Game::Carbon, P::PC},
			{0x2217DA39ED2E3778ull, Endian::Big, Game::Carbon, P::Wii, kPipEventTrigger, true},
			{0x2217DA39ED2E3778ull, Endian::Big, Game::Carbon, P::PS3, kPipEventTrigger},
			{0x672CE560AA76F3B3ull, Endian::Little, Game::Carbon, P::PS2},
			{0x672CE560AA76F3B3ull, Endian::Big, Game::Carbon, P::GameCube, 0, false, 4},
			{0x672CE560AA76F3B3ull, Endian::Big, Game::Carbon, P::Xbox360, 0, false, 1},
			{0x9D112D3EEB4C5396ull, Endian::Little, Game::Carbon, P::PS2},     // the PS2 demo: attributes.bin,
			{0xF649D472EE93BDB5ull, Endian::Little, Game::Carbon, P::PS2},     // FE_ATTRIB.bin
			{0xDD773A275FF41CFFull, Endian::Little, Game::Carbon, P::PS2},     // and gameplay.bin
			{0x53378FC2333FFCAFull, Endian::Big, Game::Carbon, P::Xbox360},    // a 360 demo made after release,
			{0xD6202625D020CCD1ull, Endian::Big, Game::Carbon, P::Xbox360},    // its gameplay.bin
			// The PC's retail files share the 360's value (every vault, gameplay's
			// too). RetailVersion takes the first entry for a platform.
			{0x8364A394EF316ABDull, Endian::Little, Game::Undercover, P::PC},
			// Another PC build's (pc/undercover-vlted: 256 gameplay vaults to
			// retail's 219, unmarked, byte for byte as the writer writes them), and
			// NFS-VltEd's re-save of it, which keeps Vers as read.
			{0x1445379ED96333E1ull, Endian::Little, Game::Undercover, P::PC},
			{0x8364A394EF316ABDull, Endian::Big, Game::Undercover, P::Xbox360},
			{0xCE50492389B94950ull, Endian::Big, Game::Undercover, P::Xbox360}, // its gameplay.bin
			{0x482E894A9E6DE7F2ull, Endian::Big, Game::Undercover, P::Xbox360}, // an alpha, build 161
			{0x0E657D8D9C499917ull, Endian::Big, Game::Undercover, P::Wii},     // Carbon's engine
			// Not seen: the PS2's is the Wii's game, the only little-endian one, and
			// the PS2's and the Wii's (or GameCube's) files share a value in Pro
			// Street and Carbon.
			{0x0E657D8D9C499917ull, Endian::Little, Game::Undercover, P::PS2},
			{kWorldBeta2009, Endian::Little, Game::World, P::PC, kWorldInput},
			{kWorldBeta2010, Endian::Little, Game::World, P::PC, kWorldInput},
			// World's builds between the 2010 beta and the final one (pc/world-various,
			// by the folders' build numbers): one value from each schema change.
			{0xC39FD381B76D8AE2ull, Endian::Little, Game::World, P::PC, kWorldInput}, // 62 to 77
			{0xD043549EBA6A5F5Cull, Endian::Little, Game::World, P::PC, kWorldInput}, // 190
			{0x1E4F597223305838ull, Endian::Little, Game::World, P::PC, kWorldInput}, // 270
			{0xF01ACDFC2058F05Full, Endian::Little, Game::World, P::PC, kWorldInput}, // 363, 396
			{0x66C6A5C81816AE48ull, Endian::Little, Game::World, P::PC, kWorldInput}, // 541, 593
			{0xAD783903AB6CA791ull, Endian::Little, Game::World, P::PC, kWorldInput}, // 541's commerce.bin
			{0xBFD963F5E690A25Cull, Endian::Little, Game::World, P::PC, kWorldInput}, // 704
			{0xDFBA5AE4F7318068ull, Endian::Little, Game::World, P::PC, kWorldInput}, // 749
			{0x9C960D4D12274E05ull, Endian::Little, Game::World, P::PC, kWorldInput}, // 808, 846
			{0xD3526CE0D2FA2312ull, Endian::Little, Game::World, P::PC, kWorldInput}, // 922
			{0xD64B2787AD688155ull, Endian::Little, Game::World, P::PC, kWorldInput}, // 1102, 1169
			{0x53BB61D0407FEDCAull, Endian::Little, Game::World, P::PC, kWorldInput}, // 1251
			{0xD6337C64395D729Dull, Endian::Little, Game::World, P::PC, kWorldInput}, // 1283, 1334
			{0xA8A63737B1C13FAAull, Endian::Little, Game::World, P::PC, kWorldInput}, // 1398 to 1412
			{0x6838F8C1836A18DEull, Endian::Little, Game::World, P::PC, kWorldInput}, // the final build, 1548 on
			{0x5E3BDBFFAC32D7DBull, Endian::Big, Game::Nitro, P::Wii},
		};

		std::string Lower(std::string_view s)
		{
			std::string o(s);
			for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
			return o;
		}

		std::string Hex64(uint64_t v)
		{
			char b[24];
			std::snprintf(b, sizeof(b), "0x%016llX", static_cast<unsigned long long>(v));
			return b;
		}

		// What the first vault of a pack shows.
		struct Clues
		{
			bool read = false;              // it is a pack
			std::optional<Endian> endian;
			std::optional<uint64_t> version; // the Vers value; none in Most Wanted's vaults
			bool strNFirst = false;         // StrN before DepN (Undercover, World)
			bool endC = false;              // an EndC chunk (World)
			bool typeTable = false;         // holds the DatabaseLoadData (attributes.bin)
			uint32_t boolSize = 0;          // EA::Reflection::Bool in it
			bool proStreetTypes = false;    // FEPartData, which Carbon's table (and the Wii's Undercover) lacks
			bool lastGenUndercoverTypes = false; // eNISAudio_en_index: the Wii's and the PS2's Undercover
			// Attrib::Gen::ClassRefSpec_<class>: current-gen Undercover's and World's,
			// which have Pro Street's types too.
			bool classRefSpecTypes = false;
			std::unordered_set<uint32_t> classes; // the classes in it
			bool keys64 = false;            // no Vers chunk, and StringHash64 keys (early Most Wanted demos)
			bool mostWantedLayout = false;  // Most Wanted's exports (RawVault::mostWantedLayout)
		};

		Clues ReadClues(ByteSpan data)
		{
			Clues c;
			PackFile pf = ReadPack(data);
			c.read = true;
			if (pf.vaults.empty()) return c;
			const Bytes& vlt = pf.vaults.front().vlt;
			if (vlt.size() < 8) return c;
			constexpr uint32_t kVers = FourCC("Vers"), kDepN = FourCC("DepN"), kStrN = FourCC("StrN"), kEndC = FourCC("EndC");
			for (Endian e : {Endian::Little, Endian::Big})
			{
				const uint32_t first = load32(vlt.data(), e);
				if (first == kVers || first == kDepN)
				{
					c.endian = e;
					if (first == kVers && vlt.size() >= 16) c.version = load64(vlt.data() + 8, e);
					break;
				}
			}
			if (!c.endian) return c;
			bool seenDep = false;
			for (size_t off = 0; off + 8 <= vlt.size();)
			{
				const uint32_t id = load32(vlt.data() + off, *c.endian), size = load32(vlt.data() + off + 4, *c.endian);
				if (size < 8 || off + size > vlt.size()) break;
				if (id == kDepN) seenDep = true;
				if (id == kStrN && !seenDep) c.strNFirst = true;
				if (id == kEndC) c.endC = true;
				off += size;
			}
			if (!c.version)
			{
				// An early Most Wanted demo's DepN: { u32 count; u32 pad; u64 ids[] },
				// the ids StringHash64 of "<vault>.vlt" and "<vault>.bin".
				const std::string& name = pf.vaults.front().name;
				if (vlt.size() >= 32 && load32(vlt.data() + 12, *c.endian) == 0)
				{
					const uint64_t id = load64(vlt.data() + 16, *c.endian);
					c.keys64 = id == StringHash64(name + ".vlt") || id == StringHash64(name + ".bin");
				}
			}
			// The type table (attributes.bin's db vault): the bool width and a few names.
			for (size_t vi = 0; vi < pf.vaults.size(); ++vi)
			{
				PackVault& pv = pf.vaults[vi];
				if (vi != 0 && pv.name != "db") continue;
				RawVault raw;
				try
				{
					raw = ParseRawVault(pv.name, std::move(pv.bin), std::move(pv.vlt));
				}
				catch (const std::exception&)
				{
					continue;
				}
				c.mostWantedLayout |= raw.mostWantedLayout;
				for (const ExportEntry& e : raw.exports)
				{
					if (e.type != ExportType::DatabaseLoadData) continue;
					const ByteReader r = raw.vltReader(), br = raw.binReader();
					// The early Most Wanted demos have a u32 more at +4.
					const uint32_t x4 = raw.keys64 ? 4 : 0;
					const uint32_t* names = raw.vltPtr(e.offset + 12 + x4);
					if (!names || e.size < 16 + x4) continue;
					const uint32_t n = r.u32(e.offset + 8 + x4);
					if (e.size < 16 + x4 + n * 4) continue;
					c.typeTable = true;
					for (const ExportEntry& x : raw.exports)
						if (x.type == ExportType::ClassLoadData && !raw.keys64) c.classes.insert(uint32_t(x.id)); // a class export's id is its key
					uint32_t off = *names;
					for (uint32_t i = 0; i < n && off < raw.bin.size(); ++i)
					{
						const std::string name = br.cstr(off);
						off += uint32_t(name.size()) + 1;
						if (name == "EA::Reflection::Bool") c.boolSize = r.u32(e.offset + 16 + x4 + i * 4);
						if (name == "FEPartData") c.proStreetTypes = true;
						if (name == "eNISAudio_en_index") c.lastGenUndercoverTypes = true;
						if (name.rfind("Attrib::Gen::ClassRefSpec_", 0) == 0) c.classRefSpecTypes = true;
					}
					return c;
				}
			}
			return c;
		}

		// Folder names, deepest first: "databases/ps2/prostreet-vanilla",
		// "Need for Speed ProStreet/GLOBAL".
		std::vector<std::string> Components(const std::string& path)
		{
			std::vector<std::string> out;
			fs::path p(std::u8string(path.begin(), path.end()));
			for (const auto& part : p)
			{
				const std::u8string u = part.u8string();
				out.push_back(Lower(std::string(u.begin(), u.end())));
			}
			std::reverse(out.begin(), out.end());
			return out;
		}

		std::vector<std::string> Words(const std::string& component)
		{
			std::vector<std::string> w;
			std::string cur;
			for (char ch : component)
			{
				if (std::isalnum(static_cast<unsigned char>(ch))) cur += ch;
				else if (!cur.empty()) w.push_back(std::move(cur)), cur.clear();
			}
			if (!cur.empty()) w.push_back(cur);
			return w;
		}

		Platform PlatformInPath(const std::string& path, std::string* where)
		{
			for (const std::string& comp : Components(path))
			{
				const std::vector<std::string> w = Words(comp);
				for (size_t i = 0; i < w.size(); ++i)
				{
					const std::string& x = w[i];
					Platform p = P::Unknown;
					if (x == "pc") p = P::PC;
					else if (x == "ps2") p = P::PS2;
					else if (x == "ps3") p = P::PS3;
					else if (x == "x360" || x == "xbox360" || x == "360" || x == "xenon") p = P::Xbox360;
					else if (x == "xbox") p = i + 1 < w.size() && w[i + 1] == "360" ? P::Xbox360 : P::Xbox;
					else if (x == "gamecube" || x == "gc" || x == "ngc") p = P::GameCube;
					else if (x == "wii") p = P::Wii;
					if (p != P::Unknown)
					{
						if (where) *where = comp;
						return p;
					}
				}
			}
			return P::Unknown;
		}

		Game GameInPath(const std::string& path, std::string* where)
		{
			for (const std::string& comp : Components(path))
			{
				std::string joined;
				for (const std::string& w : Words(comp)) joined += w;
				const std::vector<std::string> w = Words(comp);
				auto has = [&](const char* word)
				{
					return std::find(w.begin(), w.end(), word) != w.end();
				};
				Game g = Game::Unknown;
				if (joined.find("mostwanted") != std::string::npos || has("mw")) g = Game::MostWanted;
				else if (joined.find("carbon") != std::string::npos) g = Game::Carbon;
				else if (joined.find("prostreet") != std::string::npos) g = Game::ProStreet;
				else if (joined.find("undercover") != std::string::npos || has("uc")) g = Game::Undercover;
				else if (has("world") || joined.find("nfsworld") != std::string::npos) g = Game::World;
				if (g != Game::Unknown)
				{
					if (where) *where = comp;
					return g;
				}
			}
			return Game::Unknown;
		}

		// The platform a guess settles on: the folder's name when it fits the game
		// and the byte order, else the likeliest that does. `choices` is how many fit.
		Platform GuessPlatform(Game g, const Clues& c, Platform hinted, size_t* choices)
		{
			std::vector<Platform> fits;
			for (const PlatformInfo& p : kPlatforms)
			{
				if (g != Game::Unknown && !ReleasedOn(g, p.platform)) continue;
				// Undercover's Carbon-engine version came out on these two.
				if (g == Game::Undercover && c.lastGenUndercoverTypes && p.platform != P::PS2 && p.platform != P::Wii) continue;
				// Its current-gen version (StrN first) didn't; the other puts DepN first.
				if (g == Game::Undercover && c.strNFirst && (p.platform == P::PS2 || p.platform == P::Wii)) continue;
				if (c.endian && p.endian != *c.endian) continue;
				// The PS2's and the GameCube's compilers make a bool 4 bytes.
				if (c.boolSize && (c.boolSize == 4) != (p.platform == P::PS2 || p.platform == P::GameCube)) continue;
				fits.push_back(p.platform);
			}
			if (fits.empty() && g != Game::Unknown) return GuessPlatform(Game::Unknown, c, hinted, choices);
			*choices = fits.size();
			if (std::find(fits.begin(), fits.end(), hinted) != fits.end()) return hinted;
			for (Platform p : {P::PC, P::Xbox360, P::PS2, P::GameCube, P::Xbox, P::PS3, P::Wii})
				if (std::find(fits.begin(), fits.end(), p) != fits.end()) return p;
			return hinted != P::Unknown ? hinted : P::PC;
		}

		Detection FromClues(const std::string& path, const Clues& c)
		{
			Detection d;
			d.endian = c.endian;
			d.boolSize = c.boolSize;
			std::string platformWhere, gameWhere;
			const Platform hintedPlatform = PlatformInPath(path, &platformWhere);
			const Game hintedGame = GameInPath(path, &gameWhere);
			// A known Vers value whose files lack the class it needs (another
			// tool's save): its game, for the reason.
			Game versWithoutClass = Game::Unknown;
			if (c.version)
			{
				std::vector<Target> hits;
				bool byClass = false, byBools = false;
				for (const KnownVersion& k : kKnownVersions)
				{
					if (k.value != *c.version || k.endian != *c.endian) continue;
					// A class or the bool width that tells platforms apart, when
					// attributes.bin was read.
					if (k.classKey && c.typeTable)
					{
						if ((c.classes.count(k.classKey) != 0) == k.without)
						{
							versWithoutClass = k.game;
							continue;
						}
						byClass = true;
					}
					if (k.boolSize && c.boolSize)
					{
						if (k.boolSize != c.boolSize) continue;
						byBools = true;
					}
					hits.push_back({ k.game, k.platform });
				}
				std::string vers = "Vers " + Hex64(*c.version) + (*c.endian == Endian::Big ? ", big-endian" : ", little-endian");
				if (hits.size() == 1)
				{
					d.target = hits.front();
					d.certain = true;
					if (byClass) vers += std::string(", its classes are ") + InfoOf(d.target.platform).name + "'s";
					if (byBools) vers += ", " + std::to_string(c.boolSize) + "-byte bools, as on the " + InfoOf(d.target.platform).name;
					d.reason = vers;
					return d;
				}
				if (!hits.empty())
				{
					// Platforms that share it: the folder's name picks, else the first.
					d.target = hits.front();
					std::string which;
					for (const Target& t : hits)
					{
						which += (which.empty() ? "" : " and ") + std::string(InfoOf(t.platform).name);
						if (t.platform == hintedPlatform) d.target = t;
					}
					d.reason = vers + ": " + InfoOf(d.target.game).name + "'s on " + which;
					if (d.target.platform == hintedPlatform) d.reason += "; the folder is named " + platformWhere;
					else d.platformWeak = true;
					return d;
				}
			}

			// Most Wanted's own vaults (no Vers, 32-bit keys) and its attributes.bin:
			// the byte order and the bool's width name the platform, and of the two
			// little-endian ones with 1-byte bools only the PC's has the
			// timeofdaylighting class (the Xbox's, the GameCube's and the PS2's lack
			// it; the 360's has it, and the 360 is big-endian).
			// The early demos (64-bit keys) are the PS2's (4-byte bools) and the Xbox's.
			if (c.endian && !c.version && c.typeTable && c.boolSize)
			{
				static const uint32_t kTimeOfDayLighting = StringHash32("timeofdaylighting");
				const bool big = *c.endian == Endian::Big, wide = c.boolSize == 4;
				const Platform p = wide ? (big ? P::GameCube : P::PS2)
					: big ? P::Xbox360
					: !c.keys64 && c.classes.count(kTimeOfDayLighting) ? P::PC
					: P::Xbox;
				d.target = { Game::MostWanted, p };
				d.certain = true;
				d.reason = std::string("no Vers chunk, as in Most Wanted's vaults; ") +
					(c.keys64 ? "64-bit keys, as in its early demos; " : "") + (big ? "big-endian; " : "little-endian; ") +
					(wide ? "4-byte bools" : "1-byte bools");
				if (!wide && !big && !c.keys64)
					d.reason += c.classes.count(kTimeOfDayLighting) ? "; a timeofdaylighting class, as the PC's has" : "; no timeofdaylighting class, as on the Xbox";
				return d;
			}

			std::vector<std::string> why;
			Game g = Game::Unknown;
			if (!c.read)
			{
				why.push_back("the files can't be read");
			}
			else if (!c.endian)
			{
				why.push_back("the first vault doesn't start with a Vers or DepN chunk");
			}
			else
			{
				if (versWithoutClass != Game::Unknown)
					why.push_back("Vers " + Hex64(*c.version) + " is " + InfoOf(versWithoutClass).name + "'s, but attributes.bin lacks a class every " +
						InfoOf(versWithoutClass).name + " build has, so another tool may have written it");
				else
					why.push_back(c.version ? "Vers " + Hex64(*c.version) + " isn't one Attribulina knows" : "the vaults have no Vers chunk");
				if (c.keys64) why.push_back("their keys are 64-bit, as in the early Most Wanted demos");
				why.push_back(*c.endian == Endian::Big ? "big-endian" : "little-endian");
				if (!c.version)
				{
					g = Game::MostWanted; // its vaults start with DepN
				}
				// World's chunk order and EndC say nothing once World's Vers came
				// without World's classes: a tool that writes every game as World
				// wrote both.
				else if (c.strNFirst && versWithoutClass != Game::World)
				{
					g = c.endC || hintedGame == Game::World ? Game::World : Game::Undercover;
					why.push_back(std::string("its chunks are ordered as ") + (g == Game::World ? "World's" : "Undercover's"));
				}
				else if (versWithoutClass == Game::World && c.mostWantedLayout)
				{
					// VaultLib's World header over Most Wanted's own exports.
					g = Game::MostWanted;
					why.push_back("its exports are laid out as Most Wanted's");
				}
				else if (c.typeTable)
				{
					// Class references came with current-gen Undercover; World's files
					// take the chunk order's branch above.
					g = c.classRefSpecTypes || c.lastGenUndercoverTypes ? Game::Undercover : c.proStreetTypes ? Game::ProStreet : Game::Carbon;
					why.push_back(std::string("its types are ") + (g == Game::ProStreet ? "Pro Street's"
																   : c.classRefSpecTypes ? "current-gen Undercover's (class references)"
																   : g == Game::Undercover ? "Undercover's (the Wii's and the PS2's)"
																   : "Carbon's"));
				}
				if (c.boolSize) why.push_back(std::to_string(c.boolSize) + "-byte bools");
			}
			if (g == Game::Unknown && hintedGame != Game::Unknown)
			{
				g = hintedGame;
				why.push_back("the folder is named " + gameWhere);
			}
			if (g == Game::Unknown) g = Game::ProStreet;
			size_t choices = 0;
			const Platform p = GuessPlatform(g, c, hintedPlatform, &choices);
			if (p == hintedPlatform && hintedPlatform != P::Unknown) why.push_back("the folder is named " + platformWhere);
			else d.platformWeak = choices > 1;
			d.target = { g, p };
			for (size_t i = 0; i < why.size(); ++i) d.reason += (i ? "; " : "") + why[i];
			return d;
		}

	} // namespace

	const std::vector<GameInfo>& Games()
	{
		return kGames;
	}
	const std::vector<PlatformInfo>& Platforms()
	{
		return kPlatforms;
	}

	const GameInfo& InfoOf(Game g)
	{
		static const GameInfo unknown{ Game::Unknown, "unknown", "an unknown game", {}, {} };
		for (const GameInfo& i : kGames)
			if (i.game == g) return i;
		return unknown;
	}

	const PlatformInfo& InfoOf(Platform p)
	{
		static const PlatformInfo unknown{ P::Unknown, "unknown", "an unknown platform", Endian::Little };
		for (const PlatformInfo& i : kPlatforms)
			if (i.platform == p) return i;
		return unknown;
	}

	Game GameById(std::string_view id)
	{
		const std::string l = Lower(id);
		for (const GameInfo& g : kGames)
			if (l == g.id) return g.game;
		return Game::Unknown;
	}

	Platform PlatformById(std::string_view id)
	{
		const std::string l = Lower(id);
		for (const PlatformInfo& p : kPlatforms)
			if (l == p.id) return p.platform;
		return P::Unknown;
	}

	bool ReleasedOn(Game g, Platform p)
	{
		const std::vector<Platform>& ps = InfoOf(g).platforms;
		return std::find(ps.begin(), ps.end(), p) != ps.end();
	}

	bool SupportedOn(Game g, Platform p)
	{
		const std::vector<Platform>& ps = InfoOf(g).supported;
		return std::find(ps.begin(), ps.end(), p) != ps.end();
	}

	bool SupportedAnywhere(Game g)
	{
		return !InfoOf(g).supported.empty();
	}

	std::string Target::id() const
	{
		if (!known()) return {};
		return std::string(InfoOf(game).id) + "_" + InfoOf(platform).id;
	}

	std::string Target::name() const
	{
		return std::string(InfoOf(game).name) + " on " + InfoOf(platform).name;
	}

	std::optional<Target> ParseTarget(std::string_view id)
	{
		const size_t us = id.find('_');
		if (us == std::string_view::npos) return std::nullopt;
		const Target t{ GameById(id.substr(0, us)), PlatformById(id.substr(us + 1)) };
		if (!t.known()) return std::nullopt;
		return t;
	}

	std::string ScriptTarget::name() const
	{
		if (allPlatforms()) return std::string(InfoOf(game).name) + " on every platform";
		return Target{ game, platform }.name();
	}

	std::optional<ScriptTarget> ParseScriptTarget(std::string_view id)
	{
		const size_t us = id.find('_');
		if (us == std::string_view::npos) return std::nullopt;
		const Game g = GameById(id.substr(0, us));
		if (g != Game::Unknown && Lower(id.substr(us + 1)) == "all") return ScriptTarget{ g, P::Unknown };
		const std::optional<Target> t = ParseTarget(id);
		if (!t) return std::nullopt;
		return ScriptTarget{ t->game, t->platform };
	}

	Detection DetectTarget(const std::string& path, ByteSpan fileData)
	{
		Clues c;
		try
		{
			c = ReadClues(fileData);
		}
		catch (const std::exception&)
		{
			c = Clues{};
		}
		return FromClues(path, c);
	}

	Detection DetectTarget(const std::string& path)
	{
		std::string file = path;
		std::error_code ec;
		const fs::path p(std::u8string(path.begin(), path.end()));
		if (fs::is_directory(p, ec))
		{
			if (auto attrs = FindFileNoCase(path, "attributes.bin")) file = *attrs;
		}
		else if (auto attrs = DatabaseFileFor(path))
		{
			// Its type table tells more than another pack (bools, Pro Street's types).
			file = *attrs;
		}
		try
		{
			return DetectTarget(path, ReadFile(file));
		}
		catch (const std::exception&)
		{
			return FromClues(path, Clues{});
		}
	}

	bool IsWorldBetaVersion(uint64_t vers)
	{
		return vers == kWorldBeta2009 || vers == kWorldBeta2010;
	}

	std::optional<uint64_t> RetailVersion(const Target& t, Endian e)
	{
		for (const KnownVersion& k : kKnownVersions)
			if (k.game == t.game && k.platform == t.platform && k.endian == e) return k.value;
		return std::nullopt;
	}

	bool StrNFirstIn(const Target& t)
	{
		// Undercover's Wii and PS2 version is Carbon's engine, with Carbon's order.
		return t.game == Game::World || (t.game == Game::Undercover && t.platform != P::Wii && t.platform != P::PS2);
	}

	bool EndCIn(const Target& t)
	{
		return t.game == Game::World;
	}

	std::string Contradiction(const Target& t, const Detection& d)
	{
		if (t.platform == P::Unknown) return {};
		const PlatformInfo& p = InfoOf(t.platform);
		if (d.endian && p.endian != *d.endian)
			return std::string("these files are ") + (*d.endian == Endian::Big ? "big-endian" : "little-endian") + ", and " + p.name +
			"'s are " + (p.endian == Endian::Big ? "big-endian" : "little-endian");
		// The PS2's and the GameCube's compilers make a bool 4 bytes.
		const bool wide = t.platform == P::PS2 || t.platform == P::GameCube;
		if (d.boolSize && (d.boolSize == 4) != wide)
			return "these files' bools are " + std::to_string(d.boolSize) + " bytes, and " + p.name + "'s are " + (wide ? "4" : "1");
		return {};
	}

} // namespace attrib
