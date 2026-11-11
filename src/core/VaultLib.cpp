#include "VaultLib.h"

#include "ClassDesign.h"
#include "ClassList.h"
#include "Compression.h"
#include "Edit.h"
#include "Game.h"
#include "Hash.h"
#include "Pack.h"
#include "Schema.h"
#include "Values.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string_view>
#include <unordered_map>

namespace attrib::vaultlib
{

	namespace
	{

		std::string Hex64(uint64_t v)
		{
			char b[24];
			std::snprintf(b, sizeof b, "0x%016llX", static_cast<unsigned long long>(v));
			return b;
		}

		void Warn(Vault& v, const std::string& what)
		{
			if (std::find(v.warnings.begin(), v.warnings.end(), what) == v.warnings.end()) v.warnings.push_back(what);
		}

		void Repaired(Vault& v, const std::string& what)
		{
			Warn(v, what);
			v.repaired = true;
			if (v.pack) v.pack->dirty = true; // a pack loaded before this one (the class's)
		}

		// The games' mEncodedTypePad for every array, where VaultLib's differs
		// without changing the header's size (VaultLib writes 16-byte headers for
		// 16-byte elements as the games do).
		size_t RepairPads(Vault& v)
		{
			size_t fixed = 0;
			auto fix = [&](uint16_t pad, const FieldDef& f) -> uint16_t
			{
				const uint16_t want = ArrayPad(f);
				if (pad == want || ArrayHeaderSize(pad) != ArrayHeaderSize(want)) return pad;
				++fixed;
				return want;
			};
			for (Collection* c : v.collections)
				for (Attribute& a : c->attributes)
					if (a.isArray && a.def) a.encodedTypePad = fix(a.encodedTypePad, *a.def);
			// Static arrays are inline in the class's static block, as layout ones are.
			for (ClassDef* cls : v.classes)
				for (const FieldDef& f : cls->fields)
				{
					if (!f.isStatic() || !f.isArray() || f.offset + 8u > cls->staticData.bytes.size()) continue;
					uint8_t* h = cls->staticData.bytes.data() + f.offset;
					store16(h + 6, fix(load16(h + 6, v.raw.endian), f), v.raw.endian);
				}
			return fixed;
		}

		// VaultLib packs every Attrib::Blob with JDLZ (Frameworks.Speed/Attrib/Blob.cs),
		// where the games' files have them as LZCompress packs them (CompressBest):
		// Huffman unless JDLZ is strictly smaller. Unpacked, the Lua is the same,
		// and packed again it is the game's bytes: all 211 of Undercover's PC
		// handlers VaultLib repacked, 253 of Most Wanted's 258 (its other 5 are
		// JDLZ blocks the game's encoder wrote reading past its input). mSize
		// follows the block, as VaultLib's and the editor's own do.
		size_t RepackBlobs(Vault& v)
		{
			std::unordered_map<const DataBlock*, uint32_t> packed;
			size_t n = 0;
			for (Collection* c : v.collections)
				for (Attribute& a : c->attributes)
				{
					if (!a.type || a.type->kind != TypeKind::Blob || a.elemSize < 8) continue;
					for (uint32_t i = 0; i < a.count; ++i)
					{
						PtrSlot* p = a.data.ptrAt(i * a.elemSize + 4);
						if (!p || !p->data) continue;
						auto done = packed.find(p->data.get());
						if (done == packed.end())
						{
							Bytes& b = p->data->bytes;
							const auto h = ParseLzHeader(b);
							if (!h || h->kind != LzKind::Jdlz) continue;
							Bytes again;
							try
							{
								again = CompressBest(Decompress(b));
							}
							catch (const std::exception&)
							{
								continue;
							}
							if (again == b) continue;
							b = std::move(again);
							done = packed.emplace(p->data.get(), uint32_t(b.size())).first;
							++n;
						}
						store32(a.elem(i), done->second, v.raw.endian);
					}
				}
			return n;
		}

		// Undercover's values VaultLib loses for good, which no repair brings back:
		// a warning for a vault that has them.
		void WarnUndercoverLosses(Vault& v)
		{
			bool vinyls = false, aliases = false;
			for (const Collection* c : v.collections)
				for (const Attribute& a : c->attributes)
				{
					const StructDef* st = a.type ? a.type->schemaStruct : nullptr;
					if (!st || !a.count) continue;
					vinyls |= st->name == "VinylsAttrib::VinylLayer";
					aliases |= st->name == "FEAutosculptAliasing::Alias";
				}
			// VinylLayer.cs aligns to 8 after mirrored, where the game's transform
			// starts at 6, and VinylColor.cs pads each 3-byte colour to 4.
			if (vinyls)
				Warn(v, "Its vinyl layers (VinylsAttrib::VinylLayer) lost four bytes each: VaultLib writes 0 over transform.translationX, "
					"colors[1].swatch, colors[2].saturation and colors[3].brightness. The values aren't in the file.");
			// Alias.cs points the sliders somewhere and never writes them.
			if (aliases)
				Warn(v, "Its autosculpt aliases (FEAutosculptAliasing::Alias) lost their sliders: VaultLib keeps each alias's slider count "
					"and pointer but doesn't write the sliders, so they read whatever bytes are there. They aren't in the file.");
		}

		// The game's classes as its files define them: VaultLib writes a class's
		// collection reserve (its collections in the vault saved), layout size (a
		// running sum), layout count (every layout field) and key shift (0) by
		// rules of its own (ModernBase/Exports/ClassLoad.cs), and an unrounded
		// static size. A class whose fields are as Attribulina's list of the
		// game's classes has them (data/classes) takes the list's values; one a
		// mod changed keeps its own.
		void RepairClasses(Vault& v, const Database& db)
		{
			if (v.classes.empty()) return;
			const ClassList* list = RetailClassesFor(db);
			if (!list) return;
			size_t fixed = 0;
			for (ClassDef* cls : v.classes)
			{
				const script::Statement* st = list->find(cls->key);
				if (!st) continue;
				ClassDef laid;
				try
				{
					LayOutClass(db, script::DesignFromStatement(db, *st), laid);
				}
				catch (const std::exception&)
				{
					continue;
				}
				if (laid.fields.size() != cls->fields.size()) continue;
				bool same = true;
				for (size_t i = 0; i < laid.fields.size() && same; ++i)
				{
					const FieldDef &a = laid.fields[i], &b = cls->fields[i];
					same = a.key == b.key && a.typeHash == b.typeHash && a.offset == b.offset && a.size == b.size && a.maxCount == b.maxCount &&
						a.flags == b.flags && a.alignment == b.alignment;
				}
				if (!same) continue;
				if (cls->collectionReserve == laid.collectionReserve && cls->layoutSize == laid.layoutSize && cls->layoutCount == laid.layoutCount &&
					cls->layoutKeyShift == laid.layoutKeyShift && cls->staticSize == laid.staticSize)
					continue;
				cls->collectionReserve = laid.collectionReserve;
				cls->layoutSize = std::max(cls->layoutSize, laid.layoutSize);
				cls->layoutCount = laid.layoutCount;
				cls->layoutKeyShift = laid.layoutKeyShift;
				if (laid.staticSize > cls->staticSize)
				{
					// What VaultLib left out of the static block: World's gameplay
					// class has 1036 bytes and no static fields, which VaultLib's size
					// (its static fields' sum) makes 0. The list's bytes line has it.
					const size_t had = cls->staticData.bytes.size();
					cls->staticSize = laid.staticSize;
					if (had < laid.staticSize) cls->staticData.bytes.resize(laid.staticSize, 0);
					for (const script::Statement& f : st->body)
					{
						std::vector<uint8_t> bytes;
						if (f.kind != script::Statement::Kind::ClassField || f.word != "bytes" || !f.value ||
							f.value->kind != script::Value::Kind::Bytes || !ParseHexBytes(f.value->text, bytes))
							continue;
						for (size_t k = had; k < bytes.size() && k < cls->staticData.bytes.size(); ++k) cls->staticData.bytes[k] = bytes[k];
					}
				}
				++fixed;
			}
			if (fixed)
				Repaired(v, std::to_string(fixed) + (fixed == 1 ? " class has" : " classes have") + " VaultLib's collection reserve, layout "
					"count or size. They take the values of " + list->title + ", whose classes they are.");
		}

		// Undercover's and World's index tables (Edit.h): VaultLib writes their keys
		// and ISets but no values block, leaving mVals at whatever follows
		// (Support.Undercover/VLT/Attrib/Query/Static_Inorder_N_to_1.cs), and
		// speech's parent to children where the game maps each collection to its
		// parent. Written again from the class's collections when the class's
		// vault loads, then brought in line as each later vault brings more.
		void RepairIndexTables(Vault& v, const Database& db)
		{
			std::vector<std::string> rebuilt;
			for (const auto& up : db.classes())
			{
				ClassDef& cls = *up;
				if (!cls.vault || !cls.vault->vaultLib) continue;
				if (cls.vault == &v)
				{
					if (RebuildIndexTables(cls, IndexTableNames(db))) rebuilt.push_back(db.names().format(cls.key));
				}
				else
				{
					UpdateIndexTables(cls, IndexTableNames(db));
				}
			}
			if (rebuilt.empty()) return;
			std::string names;
			for (size_t i = 0; i < rebuilt.size(); ++i) names += (i ? i + 1 == rebuilt.size() ? " and " : ", " : "") + rebuilt[i];
			Repaired(v, "The index tables of " + names + " lack their values, which VaultLib leaves out. They are written again from "
				"the classes' collections.");
		}

	} // namespace

	bool Marked(const RawVault& r)
	{
		static constexpr std::string_view kMark = "Generated by VaultBoy";
		return r.strESize >= kMark.size() && uint64_t(r.strEOffset) + kMark.size() <= r.bin.size() &&
			std::string_view(reinterpret_cast<const char*>(r.bin.data() + r.strEOffset), kMark.size()) == kMark;
	}

	void RepairHeader(Vault& v, const Target& t)
	{
		RawVault& r = v.raw;
		const bool worldHeader = t.known() && t.game != Game::World && r.hasVers && r.version == kVaultLibVers;
		v.vaultLib = Marked(r) || worldHeader;
		if (!worldHeader) return;
		const std::string game = InfoOf(t.game).name;
		const std::string was = "Its header is World's, as VaultLib writes it for every game (Attribulator and OGVI save with it): Vers " +
			Hex64(r.version) + (r.strNFirst != StrNFirstIn(t) ? ", StrN before DepN" : "") + (r.endC ? ", EndC chunks" : "") + ".";
		if (t.game == Game::MostWanted)
		{
			// Most Wanted's own vaults have no Vers chunk; without it the reader
			// takes their layout (RawVault::mostWantedOwn).
			if (!r.mostWantedLayout) return;
			r.hasVers = false;
			r.version = 0;
			r.strNFirst = r.endC = false;
			Repaired(v, was + " Most Wanted's vaults have none of it: it is read as Most Wanted's, and saving writes Most Wanted's header.");
			return;
		}
		const std::optional<uint64_t> vers = RetailVersion(t, r.endian);
		if (!vers) return;
		r.version = *vers;
		r.strNFirst = StrNFirstIn(t);
		r.endC = EndCIn(t);
		Repaired(v, was + " It is read as " + game + "'s, and saving writes " + game + "'s header.");
	}

	void RepairLayoutSize(Vault& vault, ClassDef& cls, const std::string& className, const std::string& where, uint32_t fieldsEnd)
	{
		// The end of the fields, as the stored layout always ends: retail Carbon's
		// visuallook is 1112, not rounded to its matrices' 16 bytes.
		const std::string what = className + "'s layout size is " + std::to_string(cls.layoutSize) + " bytes, but the fields of " + where +
			" take " + std::to_string(fieldsEnd) + ", so it is read and saved as " + std::to_string(fieldsEnd) + "." +
			(vault.vaultLib ? " VaultLib adds the size up from the fields by a rule of its own: 8 bytes for every array's header (16 "
							  "for 16-byte elements) and its definition's count for its length (0 in some classes)."
							: " Another tool may have written this file.");
		Repaired(vault, what);
		if (cls.vault && cls.vault != &vault) Repaired(*cls.vault, what);
		cls.layoutSize = fieldsEnd;
	}

	bool MarkedPack(const std::string& path)
	{
		try
		{
			for (PackVault& pv : ReadPack(ReadFile(path)).vaults)
				if (Marked(ParseRawVault(pv.name, std::move(pv.bin), std::move(pv.vlt)))) return true;
		}
		catch (const std::exception&)
		{
		}
		return false;
	}

	void MoveToEventVaultFile(LoadedPack& pack, const std::string& path)
	{
		const std::string from = pack.path;
		pack.path = path;
		pack.format = PackFormat::VaultFile;
		pack.dirty = true;
		for (const auto& v : pack.vaults)
		{
			if (std::none_of(v->dataExports.begin(), v->dataExports.end(), [](const DataExport& d) { return d.type == ExportType::VaultData; }))
			{
				// As every event vault has it: StringHash32("VaultData"), the
				// vault's GameCore::VaultSlot, last of its exports.
				DataExport slot{ StringHash32("VaultData"), ExportType::VaultData, Bytes(4, 0) };
				if (v->name == "gpcore") store32(slot.bytes.data(), 0xFFFFFFFFu, v->raw.endian);
				v->dataExports.push_back(std::move(slot));
				ExportRef ref{ ExportRef::Kind::Data, nullptr, nullptr };
				ref.data = uint32_t(v->dataExports.size() - 1);
				v->exportOrder.push_back(ref);
			}
			Repaired(*v, "OGVI saved this event vault as a pack in " + from + ", where the game doesn't look for one, and left its "
				"VaultData out. Saving writes it to " + path + " as the game's event vault file and removes the other.");
		}
	}

	void RepairValues(Vault& v, const Database& db)
	{
		const Target& t = db.target();
		RepairClasses(v, db);
		// One text for every vault, so the Log's lines group.
		if (RepairPads(v))
			Repaired(v, "Its arrays have VaultLib's mEncodedTypePad, 1, 2 or 8 by their elements' alignment. They take the games' 0, "
				"which the game sets as it loads anyway.");
		if (t.game == Game::World && v.name == "gpcore")
			for (DataExport& d : v.dataExports)
				if (d.type == uint32_t(ExportType::VaultData) && d.bytes.size() == 4 && load32(d.bytes.data(), v.raw.endian) == 0)
				{
					store32(d.bytes.data(), 0xFFFFFFFFu, v.raw.endian);
					Repaired(v, "Its VaultData slot is 0, as Attribulator writes it for every event vault; gpcore's is -1 in the game's files, "
						"and it takes that again.");
				}
		RepairIndexTables(v, db);
		if (RepackBlobs(v))
			Repaired(v, "Its compiled Lua is packed with JDLZ, as VaultLib packs every blob. It is packed again as the game's LZCompress "
				"packs it, Huffman unless JDLZ is smaller, which gives back the game's own bytes. The Lua is the same.");
		if (t.game == Game::Undercover) WarnUndercoverLosses(v);
	}

} // namespace attrib::vaultlib
