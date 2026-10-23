#include "Values.h"

#include "Hash.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace attrib
{

	std::string TypeLabel(const TypeInfo* type)
	{
		if (!type) return "?";
		std::string n = type->name;
		for (const char* prefix : { "EA::Reflection::", "Attrib::Types::", "Attrib::" })
			if (n.rfind(prefix, 0) == 0)
			{
				n = n.substr(std::strlen(prefix)); break;
			}
		if (type->kind == TypeKind::Unknown)
			n += " (" + std::to_string(type->size) + " B)";
		return n;
	}

	uint32_t LoadKey(const Database& db, const uint8_t* p, uint32_t size, Endian e)
	{
		return size == 8 ? db.keyFrom64(load64(p, e)) : load32(p, e);
	}

	bool StoreKey(const Database& db, uint8_t* p, uint32_t size, uint32_t key, Endian e)
	{
		if (size != 8)
		{
			store32(p, key, e);
			return true;
		}
		const std::optional<uint64_t> k = db.findKey64(key);
		if (!k) return false;
		store64(p, *k, e);
		return true;
	}

	bool ShownInHex(const std::string& typeName)
	{
		return typeName == "PathEventEnum";
	}

	bool IsSignedKind(TypeKind k)
	{
		return k == TypeKind::Int8 || k == TypeKind::Int16 || k == TypeKind::Int32 || k == TypeKind::Int64 || k == TypeKind::Enum;
	}

	// ------------------------------------------------------------ value types ---

	namespace
	{

		// Structs with their own editor, and the size that editor expects.
		struct WellKnown
		{
			TypeKind kind;
			uint32_t size;
		};
		const std::unordered_map<std::string, WellKnown>& WellKnownStructs()
		{
			static const std::unordered_map<std::string, WellKnown> m = {
				{"Attrib::RefSpec", {TypeKind::RefSpec, 12}},  {"Attrib::StringKey", {TypeKind::StringKey, 8}},
				{"Curve", {TypeKind::Curve, 44}},              {"Attrib::Blob", {TypeKind::Blob, 8}},
				{"UMath::Vector2", {TypeKind::Vector2, 8}},    {"UMath::Vector3", {TypeKind::Vector3, 12}},
				{"UMath::Vector4", {TypeKind::Vector4, 16}},   {"bVector2", {TypeKind::Vector2, 8}},
				{"bVector3", {TypeKind::Vector3, 12}},         {"bVector4", {TypeKind::Vector4, 16}},
				{"UMath::Matrix4", {TypeKind::Matrix, 64}},    {"bMatrix4", {TypeKind::Matrix, 64}},
				{"UMath::Quaternion", {TypeKind::Quaternion, 16}},
				{"UCrc32", {TypeKind::Key, 4}},                {"GCollectionKey", {TypeKind::Key, 4}},
				{"Attrib::Key", {TypeKind::Key, 4}},           {"type_bStringHash", {TypeKind::BinKey, 4}},
			};
			return m;
		}

		void ApplyStruct(ValueType& vt, const StructDef* st)
		{
			if (!st) return;
			vt.st = st;
			vt.kind = TypeKind::Struct;
			if (vt.hint.empty()) vt.hint = st->hint;
			auto it = WellKnownStructs().find(st->name);
			// Most Wanted's StringKey is 16 bytes (StringKeyAt), the early Most
			// Wanted demos' RefSpec 24 (RefSpecAt) and their keys 8.
			if (it != WellKnownStructs().end() && (it->second.size == st->size || (it->second.kind == TypeKind::StringKey && st->size == 16) ||
												   (it->second.kind == TypeKind::RefSpec && st->size == 24) ||
												   (it->second.kind == TypeKind::Curve && st->size == 48)))
			{
				vt.kind = it->second.kind;
			}
			else if (st->size == 4 && (st->hint == "key" || st->hint == "binkey"))
			{
				vt.kind = st->hint == "key" ? TypeKind::Key : TypeKind::BinKey;
			}
			else if (st->size == 8 && st->hint == "key")
			{
				vt.kind = TypeKind::Key;
			}
		}

		TypeKind PrimKind(MemberPrim p)
		{
			switch (p)
			{
				case MemberPrim::Bool:
				case MemberPrim::Bool32: return TypeKind::Bool;
				case MemberPrim::Int8: return TypeKind::Int8;
				case MemberPrim::UInt8: return TypeKind::UInt8;
				case MemberPrim::Int16: return TypeKind::Int16;
				case MemberPrim::UInt16: return TypeKind::UInt16;
				case MemberPrim::Int32: return TypeKind::Int32;
				case MemberPrim::UInt32: return TypeKind::UInt32;
				case MemberPrim::Int64: return TypeKind::Int64;
				case MemberPrim::UInt64: return TypeKind::UInt64;
				case MemberPrim::Float: return TypeKind::Float;
				case MemberPrim::Double: return TypeKind::Double;
				case MemberPrim::Char: return TypeKind::Char;
				case MemberPrim::CStr: return TypeKind::Text;
				default: return TypeKind::Unknown;
			}
		}


	} // namespace

	ValueType TypeOfAttribute(const Attribute& a, const std::string& fieldName)
	{
		ValueType vt;
		vt.name = fieldName;
		vt.size = a.elemSize;
		if (!a.type) return vt;
		vt.kind = a.type->kind;
		vt.size = a.type->size;
		vt.typeName = TypeLabel(a.type);
		if (a.type->schemaEnum) vt.en = a.type->schemaEnum;
		if (a.type->kind == TypeKind::Unknown && a.type->size == 4 && ShownInHex(a.type->name)) vt.hint = "hex";
		if (a.type->kind == TypeKind::Struct) ApplyStruct(vt, a.type->schemaStruct);
		else if (a.type->schemaStruct) vt.st = a.type->schemaStruct;
		return vt;
	}

	ValueType TypeOfMember(const MemberDef& m)
	{
		ValueType vt;
		vt.member = &m;
		vt.name = m.name;
		vt.hint = m.hint;
		vt.size = m.elemSize;
		vt.typeName = m.typeName;
		if (m.isBitfield()) vt.typeName += " :" + std::to_string(m.bitSize);
		if (m.prim != MemberPrim::None)
		{
			vt.kind = PrimKind(m.prim);
		}
		else if (m.en)
		{
			vt.kind = TypeKind::Enum;
			vt.en = m.en;
		}
		else
		{
			ApplyStruct(vt, m.st);
		}
		const bool u32 = vt.size == 4 && (vt.kind == TypeKind::UInt32 || vt.kind == TypeKind::Int32) && !m.isBitfield();
		if (u32 && m.hint == "key") vt.kind = TypeKind::Key;
		// An early Most Wanted demo's StringHash64 key.
		if (vt.size == 8 && (vt.kind == TypeKind::UInt64 || vt.kind == TypeKind::Int64) && m.hint == "key") vt.kind = TypeKind::Key;
		if (u32 && m.hint == "binkey") vt.kind = TypeKind::BinKey;
		return vt;
	}

	ValueType TypeOfNamed(const Database& db, const std::string& typeName)
	{
		ValueType vt;
		vt.typeName = typeName;
		for (MemberPrim p : {MemberPrim::Bool, MemberPrim::Int8, MemberPrim::UInt8, MemberPrim::Int16, MemberPrim::UInt16,
			 MemberPrim::Int32, MemberPrim::UInt32, MemberPrim::Int64, MemberPrim::UInt64, MemberPrim::Float,
			 MemberPrim::Double, MemberPrim::Char, MemberPrim::Bool32})
			if (typeName == PrimName(p))
			{
				vt.kind = PrimKind(p);
				vt.size = PrimSize(p);
				return vt;
			}
		// Names used by the hand-written pointer table.
		if (typeName == "f32")
		{
			vt.kind = TypeKind::Float;
			vt.size = 4;
			return vt;
		}
		if (typeName == "u8")
		{
			vt.kind = TypeKind::UInt8;
			vt.size = 1;
			return vt;
		}
		if (const TypeSchema* s = db.typeSchema())
		{
			if (const StructDef* st = s->findStruct(typeName))
			{
				vt.size = st->size;
				ApplyStruct(vt, st);
				return vt;
			}
			if (const EnumDef* en = s->findEnum(typeName))
			{
				vt.kind = TypeKind::Enum;
				vt.en = en;
				vt.size = en->size;
				return vt;
			}
		}
		auto it = WellKnownStructs().find(typeName);
		if (it != WellKnownStructs().end())
		{
			vt.kind = it->second.kind;
			vt.size = it->second.size;
		}
		return vt;
	}

	bool ExpandsIntoMembers(const ValueType& vt)
	{
		return vt.kind == TypeKind::Struct && vt.st;
	}

	uint32_t ClassRefSpecClass(const std::string& typeName)
	{
		const size_t at = typeName.find("ClassRefSpec_");
		return at == std::string::npos ? 0 : StringHash32(typeName.substr(at + 13));
	}

	// ----------------------------------------------------------------- helpers ---

	std::string HexBytes(const uint8_t* p, size_t n)
	{
		std::string s;
		s.reserve(n * 3);
		char b[4];
		for (size_t i = 0; i < n; ++i)
		{
			std::snprintf(b, sizeof(b), i ? " %02x" : "%02x", p[i]);
			s += b;
		}
		return s;
	}

	bool ParseHexBytes(const std::string& s, std::vector<uint8_t>& out)
	{
		out.clear();
		int nibble = -1;
		for (char c : s)
		{
			int v;
			if (c >= '0' && c <= '9') v = c - '0';
			else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
			else if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r')
			{
				if (nibble >= 0)
				{
					out.push_back(uint8_t(nibble)); nibble = -1;
				}
				continue;
			}
			else return false;
			if (nibble < 0) nibble = v;
			else
			{
				out.push_back(uint8_t(nibble * 16 + v)); nibble = -1;
			}
		}
		if (nibble >= 0) out.push_back(uint8_t(nibble));
		return true;
	}

	// "name" -> its hash, "0x1234ABCD" -> that value.
	bool ParseKey(const std::string& text, uint32_t& out, bool binHash)
	{
		// Up to 8 digits: a longer 0x... is a name (an early Most Wanted demo's
		// name for a 64-bit key, Key64Name).
		if (text.size() > 2 && text.size() <= 10 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		{
			char* end = nullptr;
			const unsigned long v = std::strtoul(text.c_str() + 2, &end, 16);
			if (end && *end == 0)
			{
				out = uint32_t(v); return true;
			}
		}
		out = text.empty() ? 0 : (binHash ? BinHash(text) : StringHash32(text));
		return true;
	}

	PtrSlot& EnsureStringSlot(DataBlock& b, uint32_t off)
	{
		if (PtrSlot* s = b.ptrAt(off))
		{
			s->isString = true;
			s->data.reset();
			return *s;
		}
		PtrSlot s;
		s.offset = off;
		s.isString = true;
		b.ptrs.push_back(std::move(s));
		return b.ptrs.back();
	}

	void DropSlot(DataBlock& b, uint32_t off)
	{
		auto& v = b.ptrs;
		v.erase(std::remove_if(v.begin(), v.end(), [&](const PtrSlot& s)
		{
			return s.offset == off;
		}), v.end());
	}

	// --------------------------------------------------------------- integers ---

	// The schema gives bit offsets in PC order, from the low bit. Big-endian
	// compilers (360, PS3, Wii) fill a bitfield's word from the high bit instead.
	namespace
	{
		uint32_t BitShift(const MemberDef& m, uint32_t size, Endian e)
		{
			if (e == Endian::Little) return m.bitOffset;
			const int s = int(size * 8) - int(m.bitOffset) - int(m.bitSize);
			return s > 0 ? uint32_t(s) : 0;
		}
	} // namespace

	int64_t ReadInteger(const DataBlock& b, uint32_t off, const ValueType& vt, Endian e)
	{
		const uint8_t* p = b.bytes.data() + off;
		uint64_t u = 0;
		switch (vt.size)
		{
			case 1: u = p[0]; break;
			case 2: u = load16(p, e); break;
			case 4: u = load32(p, e); break;
			case 8: u = load64(p, e); break;
			default: return 0;
		}
		if (vt.member && vt.member->isBitfield())
		{
			const uint64_t mask = vt.member->bitSize >= 64 ? ~0ull : ((1ull << vt.member->bitSize) - 1);
			return int64_t((u >> BitShift(*vt.member, vt.size, e)) & mask);
		}
		if (IsSignedKind(vt.kind))
		{
			switch (vt.size)
			{
				case 1: return int8_t(u);
				case 2: return int16_t(u);
				case 4: return int32_t(u);
				default: return int64_t(u);
			}
		}
		return int64_t(u);
	}

	void WriteInteger(DataBlock& b, uint32_t off, const ValueType& vt, int64_t v, Endian e)
	{
		uint8_t* p = b.bytes.data() + off;
		uint64_t u = uint64_t(v);
		if (vt.member && vt.member->isBitfield())
		{
			uint64_t cur = 0;
			switch (vt.size)
			{
				case 1: cur = p[0]; break;
				case 2: cur = load16(p, e); break;
				case 4: cur = load32(p, e); break;
				case 8: cur = load64(p, e); break;
			}
			const uint32_t shift = BitShift(*vt.member, vt.size, e);
			const uint64_t mask = (vt.member->bitSize >= 64 ? ~0ull : ((1ull << vt.member->bitSize) - 1)) << shift;
			u = (cur & ~mask) | ((u << shift) & mask);
		}
		switch (vt.size)
		{
			case 1: p[0] = uint8_t(u); break;
			case 2: store16(p, uint16_t(u), e); break;
			case 4: store32(p, uint32_t(u), e); break;
			case 8: store64(p, u, e); break;
		}
	}

} // namespace attrib
