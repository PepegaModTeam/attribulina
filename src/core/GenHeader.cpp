#include "GenHeader.h"

#include "ClassDesign.h"
#include "ClassList.h"
#include "Hash.h"
#include "Version.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace attrib
{

	namespace
	{
		std::string Hex(uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "0x%08Xu", v);
			return b;
		}

		std::string Offset(uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "0x%X", v);
			return b;
		}

		// `text` with every line that isn't empty moved in by `depth` levels.
		std::string Indented(const std::string& text, int depth)
		{
			const std::string in(4 * depth, ' ');
			std::istringstream lines(text);
			std::ostringstream out;
			for (std::string l; std::getline(lines, l);) out << (l.empty() ? "" : in) << l << "\n";
			return out.str();
		}

		bool IsKeyword(const std::string& s)
		{
			static const std::set<std::string> kw = {
				"alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr",
				"const_cast", "continue", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
				"export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
				"new", "noexcept", "not", "nullptr", "operator", "or", "private", "protected", "public", "register",
				"reinterpret_cast", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
				"switch", "template", "this", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
				"virtual", "void", "volatile", "while", "xor",
				// What the generated class itself declares.
				"ClassKey", "GetClass", "GetBase", "Change", "ChangeWithDefault", "Modify", "GenerateUniqueKey", "GetLayoutSize",
				"Layout", "MutableLayout", "DefaultArea", "ConvertFromInstance", "_LayoutStruct", "Instance" };
			return kw.count(s) != 0;
		}

		// The C++ spelling of a table type, as the game's generated classes spell it.
		std::string CppType(const TypeInfo* t)
		{
			if (!t) return "unsigned int";
			switch (t->kind)
			{
				case TypeKind::Bool: return "bool";
				case TypeKind::Int8: return "signed char";
				case TypeKind::UInt8: return "unsigned char";
				case TypeKind::Int16: return "short";
				case TypeKind::UInt16: return "unsigned short";
				case TypeKind::Int32: return "int";
				case TypeKind::UInt32: return "unsigned int";
				case TypeKind::Int64: return "long long";
				case TypeKind::UInt64: return "unsigned long long";
				case TypeKind::Float: return "float";
				case TypeKind::Double: return "double";
				case TypeKind::Char: return "char";
				case TypeKind::Text: return "const char*";
				case TypeKind::StringKey: return "Attrib::StringKey";
				case TypeKind::RefSpec: return "Attrib::RefSpec";
				case TypeKind::Key: return t->name == "Attrib::Key" ? "unsigned int" : t->name;
				case TypeKind::BinKey: return "unsigned int";
				case TypeKind::Colour: return "unsigned int";
				case TypeKind::FloatColour:
				case TypeKind::Vector4:
				case TypeKind::Quaternion:
					return "UMath::Vector4";
				case TypeKind::Vector2: return "UMath::Vector2";
				case TypeKind::Vector3: return "UMath::Vector3";
				case TypeKind::Matrix: return "UMath::Matrix4";
				case TypeKind::Blob: return "Attrib::Blob";
				default: break;
			}
			if (t->name == "EA::Reflection::Reference") return "const void*";
			return t->name;
		}

		bool NeedsSdkType(const std::string& cpp)
		{
			if (cpp.find(' ') != std::string::npos || cpp.find('*') != std::string::npos) return false;
			static const std::set<std::string> builtin = { "bool", "char", "short", "int", "float", "double" };
			return !builtin.count(cpp);
		}

		// `T const&` reads right for pointer types too (const char* const&).
		std::string ConstRef(const std::string& t)
		{
			return t.back() == '*' ? t + " const&" : "const " + t + "&";
		}
		// A pointer to a const T, the same way (const char* const*).
		std::string ConstPtr(const std::string& t)
		{
			return t.back() == '*' ? t + " const*" : "const " + t + "*";
		}

		struct Field
		{
			const FieldDef* def;
			std::string name, id, type;
		};

		const char* PrimCpp(MemberPrim p)
		{
			switch (p)
			{
				case MemberPrim::Bool: return "bool";
				case MemberPrim::Int8: return "signed char";
				case MemberPrim::UInt8: return "unsigned char";
				case MemberPrim::Int16: return "short";
				case MemberPrim::UInt16: return "unsigned short";
				case MemberPrim::Int32: return "int";
				case MemberPrim::UInt32: return "unsigned int";
				case MemberPrim::Int64: return "long long";
				case MemberPrim::UInt64: return "unsigned long long";
				case MemberPrim::Float: return "float";
				case MemberPrim::Double: return "double";
				case MemberPrim::Char: return "char";
				case MemberPrim::Bool32: return "unsigned int"; // a bool in 4 bytes
				case MemberPrim::CStr: return "const char*";
				default: return "void*";
			}
		}

		// Definitions of the schema structs and enums a set of types needs, those
		// first. Namespaced names (GRace::Type) and the basic Attrib and UMath types
		// are left to the SDK.
		bool IsPrimName(const std::string& n)
		{
			static const std::set<std::string> prim = { "bool", "char", "short", "int", "long", "float", "double", "int8", "uint8", "int16",
														"uint16", "int32", "uint32", "int64", "uint64", "void" };
			return prim.count(n) != 0;
		}

		std::string TypeDefinitions(const std::vector<const TypeInfo*>& types, const TypeSchema* schema, std::vector<std::string>& left)
		{
			std::ostringstream out;
			std::set<std::string> done;
			std::function<void(const StructDef*, const EnumDef*)> emit = [&](const StructDef* st, const EnumDef* en)
			{
				const std::string name = st ? st->name : en->name;
				if (done.count(name)) return;
				done.insert(name);
				if (name.find("::") != std::string::npos || name.find('<') != std::string::npos)
				{
					left.push_back(name);
					return;
				}
				// Each under a guard of its own, so headers that share types go in one file.
				const std::string guard = "ATTRIB_GEN_TYPE_" + CppIdentifier(name);
				if (en)
				{
					const char* base = en->size == 1 ? "unsigned char" : en->size == 2 ? "unsigned short" : en->size == 8 ? "long long" : "int";
					// Scoped: the games' headers have enums that share value names
					// (World's), which can't both be plain enums in one scope.
					out << "#ifndef " << guard << "\n"
						<< "#define " << guard << "\n"
						<< "enum class " << name << " : " << base << "\n"
						<< "{\n";
					std::set<std::string> seen;
					for (const auto& [n, v] : en->values)
						if (seen.insert(n).second) out << "    " << n << " = " << v << ",\n";
					out << "};\n"
						<< "#endif\n"
						<< "\n";
					return;
				}
				// Members' types go first, and what a pointer leads to is declared.
				std::ostringstream forward;
				for (const MemberDef& m : st->members)
				{
					if (m.st) emit(m.st, nullptr);
					else if (m.en) emit(nullptr, m.en);
					else if (m.prim == MemberPrim::Ptr && !m.ptrTarget.empty())
					{
						// What a pointer leads to: defined when the schema has it.
						if (schema)
						{
							if (const StructDef* t = schema->findStruct(m.ptrTarget)) emit(t, nullptr);
							else if (const EnumDef* e = schema->findEnum(m.ptrTarget)) emit(nullptr, e);
						}
						if (!done.count(m.ptrTarget) && m.ptrTarget.find("::") == std::string::npos && !IsPrimName(m.ptrTarget))
							forward << "struct " << m.ptrTarget << ";\n";
					}
				}
				out << forward.str()
					<< "#ifndef " << guard << "\n"
					<< "#define " << guard << "\n"
					<< "struct " << name << " // " << st->size << " bytes\n"
					<< "{\n";
				uint32_t at = 0;
				int pad = 0;
				for (size_t i = 0; i < st->members.size(); ++i)
				{
					const MemberDef& m = st->members[i];
					if (m.offset > at)
					{
						out << "    unsigned char _pad" << pad++ << "[" << m.offset - at << "];\n";
						at = m.offset;
					}
					std::string type = m.prim != MemberPrim::None ? PrimCpp(m.prim) : m.st ? m.st->name : m.en ? m.en->name : m.typeName;
					if (m.prim == MemberPrim::Ptr)
					{
						const std::string target = m.ptrTarget == "uint32" ? "unsigned int" : m.ptrTarget == "int32" ? "int"
							: m.ptrTarget == "uint8" ? "unsigned char" : m.ptrTarget == "int8" ? "signed char" : m.ptrTarget == "uint16" ? "unsigned short"
							: m.ptrTarget == "int16" ? "short" : m.ptrTarget;
						type = target.empty() ? "const void*" : "const " + target + "*";
					}
					out << "    " << type << " " << CppIdentifier(m.name);
					if (m.isBitfield())
					{
						// Bitfields fill from the low bit on the PC and from the high bit on
						// the big-endian consoles; the compiler for each does the same.
						out << " : " << unsigned(m.bitSize);
						// The rest of this unit's bitfields follow.
						const uint32_t unit = m.elemSize ? m.elemSize : 4;
						if (i + 1 >= st->members.size() || st->members[i + 1].offset != m.offset) at = m.offset + unit;
					}
					else
					{
						if (m.count > 1) out << "[" << m.count << "]";
						at = m.offset + m.elemSize * std::max<uint32_t>(m.count, 1);
					}
					out << "; // " << Offset(m.offset) << "\n";
				}
				if (st->size > at) out << "    unsigned char _pad" << pad++ << "[" << st->size - at << "];\n";
				out << "};\n"
					<< "#endif\n"
					<< "\n";
			};
			for (const TypeInfo* t : types)
			{
				if (t->schemaStruct) emit(t->schemaStruct, nullptr);
				else if (t->schemaEnum) emit(nullptr, t->schemaEnum);
			}
			return out.str();
		}
	} // namespace

	std::string CppTypeOf(const TypeInfo* type)
	{
		return CppType(type);
	}

	std::string CppIdentifier(const std::string& name)
	{
		if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) return "Field_" + name.substr(2);
		std::string out;
		for (char c : name) out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
		if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
		if (IsKeyword(out)) out += "_";
		return out;
	}

	std::string GenHeader(const Database& db, const ClassDef& cls, const GenHeaderOptions& o)
	{
		const KeyNames& names = db.names();
		const std::string className = names.format(cls.key);
		const std::string cn = CppIdentifier(className);
		const Target& target = db.target();
		const bool mw = target.game == Game::MostWanted;
		const bool uc = target.game == Game::Undercover || target.game == Game::World;

		std::vector<Field> layout, entries, statics;
		std::set<std::string> ids;
		for (const FieldDef& f : cls.fields)
		{
			Field x{ &f, names.format(f.key), "", CppType(f.type) };
			x.id = CppIdentifier(x.name);
			if (x.id == cn) x.id += "_";
			while (!ids.insert(x.id).second) x.id += "_";
			(f.inLayout() ? layout : f.isStatic() ? statics : entries).push_back(std::move(x));
		}
		std::stable_sort(layout.begin(), layout.end(), [](const Field& a, const Field& b)
		{
			return LayoutOffset(*a.def) < LayoutOffset(*b.def);
		});
		auto lowerLess = [](const Field& a, const Field& b)
		{
			std::string x = a.name, y = b.name;
			for (char& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
			for (char& c : y) c = char(std::tolower(static_cast<unsigned char>(c)));
			return x < y;
		};
		std::vector<Field> byName = layout;
		byName.insert(byName.end(), entries.begin(), entries.end());
		std::stable_sort(byName.begin(), byName.end(), lowerLess);

		// The game's default data area is a buffer of a size compiled into it; a
		// class that needs more gets one of its own.
		uint32_t needs = cls.layoutSize, align = 4;
		for (const FieldDef& f : cls.fields)
		{
			needs = std::max<uint32_t>(needs, f.size);
			align = std::max<uint32_t>(align, 1u << f.alignment);
		}
		const uint32_t area = DefaultDataAreaSize(target);
		const bool ownArea = area && needs > area;
		uint32_t layoutAlign = 1;
		for (const FieldDef& f : cls.fields)
			if (f.inLayout()) layoutAlign = std::max<uint32_t>(layoutAlign, 1u << f.alignment);
		auto defaultOf = [&](const std::string& t)
		{
			return ownArea ? "*reinterpret_cast<" + ConstPtr(t) + ">(DefaultArea())" : "*reinterpret_cast<" + ConstPtr(t) + ">(DefaultDataArea(sizeof(" + t + ")))";
		};

		// Types the SDK must have.
		std::set<std::string> sdkTypes;
		std::vector<const TypeInfo*> schemaTypes;
		for (const FieldDef& f : cls.fields)
		{
			if (f.isStatic()) continue;
			const std::string t = CppType(f.type);
			if (NeedsSdkType(t)) sdkTypes.insert(t);
			if (f.type && (f.type->schemaStruct || f.type->schemaEnum) &&
				std::find(schemaTypes.begin(), schemaTypes.end(), f.type) == schemaTypes.end())
				schemaTypes.push_back(f.type);
		}
		std::vector<std::string> left;
		const std::string defs = o.types ? TypeDefinitions(schemaTypes, db.typeSchema(), left) : std::string();
		if (o.types)
			for (const TypeInfo* t : schemaTypes)
				if (defs.find("struct " + t->name + " ") != std::string::npos || defs.find("enum class " + t->name + " ") != std::string::npos)
					sdkTypes.erase(t->name);

		std::ostringstream h;
		h << "//\n"
		  << "// Attrib::Gen::" << cn << "\n"
		  << "//\n"
		  << "\n"
		  << "//\n"
		  << "// class: " << className << "\n"
		  << "// key: " << Hex(cls.key) << "\n"
		  << "// game: " << target.name() << "\n"
		  << "//\n"
		  << "\n";

		h << "//\n"
		  << "// Requires:\n";
		if (mw)
			h << "//   Attrib::Instance: Instance(const Collection*, unsigned int, UTL::COM::IUnknown*),\n"
			  << "//     Instance(const RefSpec&, unsigned int, UTL::COM::IUnknown*), Instance(const Instance&),\n";
		else
			h << "//   Attrib::Instance: Instance(const Collection*, unsigned int), Instance(const RefSpec&, unsigned int),\n"
			  << "//     Instance(const Instance&),\n";
		h << "//     Change, ChangeWithDefault, operator=, GetConstCollection, IsValid, IsDynamic, Get,\n"
		  << "//     GetAttributePointer, GetLayoutPointer, " << (!layout.empty() && !ownArea ? "SetDefaultLayout, " : "")
		  << "ModifyInternal, GUKeyInternal" << (o.setters ? ", GetOrClone<T>" : "") << "\n"
		  << "//   Attrib::FindCollection, Attrib::FindCollectionWithDefault" << (ownArea ? "" : ", Attrib::DefaultDataArea") << "\n"
		  << "//   Attrib::Attribute (GetLength), Attrib::TAttrib<T> (IsValid, Set), Attrib::Collection, Attrib::RefSpec\n";
		bool layoutArrays = false;
		for (const Field& f : layout) layoutArrays |= f.def->isArray();
		if (layoutArrays) h << "//   Attrib::Private: a layout array's 8-byte Attrib::Array header (GetLength: the u16 mCount at +2)\n";
		for (const std::string& t : left) sdkTypes.insert(t);
		if (!sdkTypes.empty())
		{
			h << "//   types: ";
			for (auto t = sdkTypes.begin(); t != sdkTypes.end(); ++t) h << (t == sdkTypes.begin() ? "" : ", ") << *t;
			h << "\n";
		}
		if (ownArea)
			h << "// It needs " << needs << " bytes of default data, more than the game's " << area << ", so it keeps a zeroed area of its own.\n";
		h << "// Set_ accessors work on a dynamic Instance only (after Modify)\n"
		  << "//\n"
		  << "\n"
		  << "#pragma once\n"
		  << "\n"
		  << "#include <stddef.h> // offsetof\n"
		  << "\n";
		if (!defs.empty())
		{
			h << "// The game's types the fields use, from the type schema.\n"
			  << "#ifndef ATTRIB_GEN_NO_TYPES\n"
			  << defs
			  << "#endif\n"
			  << "\n";
		}

		// What namespace Attrib::Gen holds, written unindented: the class and the
		// checks on its layout.
		std::ostringstream gen;
		gen << (mw ? "struct " : "class ") << cn << " : " << (mw ? "" : "private ") << "Instance\n"
			<< "{\n"
			<< "public:\n"
			<< "    static const unsigned int kClassKey = " << Hex(cls.key) << "; // \"" << className << "\"\n";
		for (const Field& f : byName) gen << "    static const unsigned int kKey_" << f.id << " = " << Hex(f.def->key) << "; // \"" << f.name << "\"\n";
		gen << "\n";

		const bool hasLayout = !layout.empty();
		if (hasLayout)
		{
			gen << "    // The layout every collection holds, " << cls.layoutSize << " bytes (the class's mLayoutSize).\n"
				<< "    struct _LayoutStruct\n"
				<< "    {\n";
			uint32_t at = 0;
			int pad = 0;
			for (const Field& f : layout)
			{
				const uint32_t off = LayoutOffset(*f.def);
				if (off > at) gen << "        unsigned char _pad" << pad++ << "[" << off - at << "];\n";
				if (f.def->isArray())
				{
					const uint32_t first = off + LayoutArrayHeader(*f.def);
					const uint16_t capacity = std::max<uint16_t>(f.def->maxCount, 1);
					gen << "        Attrib::Private _Array_" << f.id << "; // " << Offset(off) << ": the array's header\n";
					if (first > off + 8) gen << "        unsigned char _pad" << pad++ << "[" << first - off - 8 << "];\n";
					gen << "        " << f.type << " " << f.id << "[" << capacity << "]; // " << Offset(first)
						<< (f.def->maxCount ? "" : ": the class gives no capacity; each collection's header says") << "\n";
					at = first + f.def->size * capacity;
				}
				else
				{
					gen << "        " << f.type << " " << f.id << "; // " << Offset(off) << "\n";
					at = off + f.def->size;
				}
			}
			// Room past the last field the class's layout size gives.
			if (cls.layoutSize % layoutAlign == 0 && cls.layoutSize > uint32_t(AlignUp(at, layoutAlign)))
				gen << "        unsigned char _pad" << pad++ << "[" << cls.layoutSize - at << "];\n";
			gen << "    };\n"
				<< "\n";
		}
		if (!statics.empty())
		{
			gen << "    // Static fields keep one value for the class, in a table compiled into the game:\n";
			for (const Field& f : statics) gen << "    //   " << f.type << " " << f.id << " (static data offset " << Offset(f.def->offset) << ")\n";
			gen << "\n";
		}

		// Construction.
		const std::string setDefault = hasLayout && !ownArea ? " SetDefaultLayout(sizeof(_LayoutStruct)); " : " ";
		const char* owner = mw ? ", UTL::COM::IUnknown* owner" : "";
		const char* ownerArg = mw ? ", owner" : "";
		gen << "    static unsigned int ClassKey() { return kClassKey; }\n"
			<< "\n"
			<< "    " << cn << "(unsigned int collectionKey, unsigned int msgPort" << owner << ")\n"
			<< "        : Instance(FindCollection(ClassKey(), collectionKey), msgPort" << ownerArg << ") {" << setDefault << "}\n"
			<< "    " << cn << "(const Collection* collection = 0, unsigned int msgPort = 0" << (mw ? ", UTL::COM::IUnknown* owner = 0" : "") << ")\n"
			<< "        : Instance(collection, msgPort" << ownerArg << ") {" << setDefault << "}\n"
			<< "    " << cn << "(const RefSpec& refspec, unsigned int msgPort" << owner << ")\n"
			<< "        : Instance(refspec, msgPort" << ownerArg << ") {" << setDefault << "}\n"
			<< "    " << cn << "(const Instance& src) : Instance(src) {" << setDefault << "}\n"
			<< "    " << cn << "(const " << cn << "& src) : Instance(src) {" << setDefault << "}\n";
		if (uc && db.typeByHash(StringHash32("Attrib::Gen::ClassRefSpec_" + className)))
			gen << "    // Undercover declares " << cn << "(const ClassRefSpec_" << cn << "&, unsigned int) too; its body isn't known.\n";
		gen << "    ~" << cn << "() {}\n"
			<< "\n"
			<< "    unsigned int GetClass() const { return ClassKey(); }\n"
			<< "    Instance& GetBase() { return *this; }\n"
			<< "    const Instance& GetBase() const { return *this; }\n"
			<< "    void Change(const Collection* c) { Instance::Change(c); }\n"
			<< "    void Change(const RefSpec& refspec) { Instance::Change(refspec); }\n"
			<< "    void Change(unsigned int collectionKey) { Instance::Change(FindCollection(ClassKey(), collectionKey)); }\n"
			<< "    void ChangeWithDefault(const RefSpec& refspec) { Instance::ChangeWithDefault(refspec); }\n"
			<< "    // The collection, else the class's \"default\" one.\n"
			<< "    void ChangeWithDefault(unsigned int collectionKey) { Instance::Change(FindCollectionWithDefault(ClassKey(), collectionKey)); }\n"
			<< "    void Modify(unsigned int dynamicCollectionKey, unsigned int spaceForAdditionalAttributes)\n"
			<< "    {\n"
			<< "        ModifyInternal(ClassKey(), dynamicCollectionKey, spaceForAdditionalAttributes);\n"
			<< "    }\n";
		if (mw)
			gen << "    unsigned int GenerateUniqueKey(const char* name, bool registerName) const { return GUKeyInternal(ClassKey(), name, registerName); }\n";
		else
			gen << "    unsigned int GenerateUniqueKey(const char* name) const { return GUKeyInternal(ClassKey(), name); }\n";
		gen << "    " << cn << "& operator=(const Instance& rhs) { Instance::operator=(rhs); return *this; }\n"
			<< "    " << cn << "& operator=(const " << cn << "& rhs) { Instance::operator=(rhs); return *this; }\n"
			<< "    bool operator==(const Instance& rhs) const { return GetConstCollection() == rhs.GetConstCollection(); }\n"
			<< "    bool operator!=(const Instance& rhs) const { return !(*this == rhs); }\n"
			<< "\n";

		// Fields, by name as the game's classes have them.
		for (const Field& f : byName)
		{
			const FieldDef& d = *f.def;
			const std::string& T = f.type;
			const std::string k = "kKey_" + f.id;
			const bool arr = d.isArray();
			const char* index = arr ? ", unsigned int index" : "";
			std::string note;
			if (d.inLayout()) note = arr ? "layout array" : "layout";
			else note = arr ? "entry array" : "entry";
			if (d.inLayout() && (d.flags & DefFlag::NotSearchable)) note += ", not searchable: the TAttrib form doesn't find it";
			if (arr && !d.inLayout() && d.maxCount && d.maxCount != 0xFFFF) note += ", up to " + std::to_string(d.maxCount);
			gen << "    // " << f.name << ": " << (f.def->type ? f.def->type->name : T) << ", " << note << "\n";
			if (d.inLayout() && !arr)
			{
				gen << "    " << ConstRef(T) << " " << f.id << "() const { return Layout()->" << f.id << "; }\n"
					<< "    bool " << f.id << "(" << T << "& result) const { result = Layout()->" << f.id << "; return true; }\n";
			}
			else if (d.inLayout())
			{
				gen << "    " << ConstRef(T) << " " << f.id << "(unsigned int index) const\n"
					<< "    {\n"
					<< "        if (index < Layout()->_Array_" << f.id << ".GetLength()) return Layout()->" << f.id << "[index];\n"
					<< "        return " << defaultOf(T) << ";\n"
					<< "    }\n"
					<< "    unsigned int Num_" << f.id << "() const { return Layout()->_Array_" << f.id << ".GetLength(); }\n"
					<< "    bool " << f.id << "(" << T << "& result, unsigned int index) const\n"
					<< "    {\n"
					<< "        if (index >= Num_" << f.id << "()) return false;\n"
					<< "        result = Layout()->" << f.id << "[index];\n"
					<< "        return true;\n"
					<< "    }\n";
			}
			else
			{
				const char* idx = arr ? "index" : "0";
				gen << "    " << ConstRef(T) << " " << f.id << "(" << (arr ? "unsigned int index" : "") << ") const\n"
					<< "    {\n"
					<< "        " << ConstPtr(T) << " p = reinterpret_cast<" << ConstPtr(T) << ">(GetAttributePointer(" << k << ", " << idx << "));\n"
					<< "        return p ? *p : " << defaultOf(T) << ";\n"
					<< "    }\n";
				if (arr) gen << "    unsigned int Num_" << f.id << "() const { return Get(" << k << ").GetLength(); }\n";
				gen << "    bool " << f.id << "(" << T << "& result" << index << ") const\n"
					<< "    {\n"
					<< "        " << ConstPtr(T) << " p = reinterpret_cast<" << ConstPtr(T) << ">(GetAttributePointer(" << k << ", " << idx << "));\n"
					<< "        if (!p) return false;\n"
					<< "        result = *p;\n"
					<< "        return true;\n"
					<< "    }\n";
			}
			gen << "    bool " << f.id << "(TAttrib<" << T << ">& result) const\n"
				<< "    {\n"
				<< "        result = TAttrib<" << T << ">(Get(" << k << "));\n"
				<< "        return result.IsValid();\n"
				<< "    }\n";
			if (o.setters)
			{
				gen << "    bool Set_" << f.id << "(" << ConstRef(T) << " input" << index << ")\n"
					<< "    {\n";
				if (d.inLayout())
					gen << "        if (!IsDynamic()" << (arr ? " || index >= Num_" + f.id + "()" : "") << ") return false;\n"
						<< "        MutableLayout()->" << f.id << (arr ? "[index]" : "") << " = input;\n"
						<< "        return true;\n";
				else
					gen << "        TAttrib<" << T << "> a = GetOrClone<" << T << ">(" << k << ");\n"
						<< "        if (!a.IsValid()) return false;\n"
						<< "        return a.Set(" << (arr ? "index" : "0") << ", input);\n";
				gen << "    }\n";
			}
			gen << "\n";
		}

		gen << "private:\n";
		if (hasLayout)
		{
			gen << "    static unsigned int GetLayoutSize() { return sizeof(_LayoutStruct); }\n";
			if (ownArea)
				gen << "    const _LayoutStruct* Layout() const\n"
					<< "    {\n"
					<< "        return reinterpret_cast<const _LayoutStruct*>(IsValid() ? GetLayoutPointer() : DefaultArea());\n"
					<< "    }\n";
			else
				gen << "    const _LayoutStruct* Layout() const { return reinterpret_cast<const _LayoutStruct*>(GetLayoutPointer()); }\n";
			gen << "    _LayoutStruct* MutableLayout() { return reinterpret_cast<_LayoutStruct*>(GetLayoutPointer()); }\n";
		}
		if (ownArea)
			gen << "    static const void* DefaultArea()\n"
				<< "    {\n"
				<< "        alignas(" << std::max<uint32_t>(align, 16) << ") static const unsigned char area[" << needs << "] = {};\n"
				<< "        return area;\n"
				<< "    }\n";
		gen << "};\n"
			<< "\n";

		if (hasLayout)
		{
			const std::string ls = cn + "::_LayoutStruct";
			auto checkOffset = [&](const std::string& member, uint32_t offset, const Field& f)
			{
				gen << "static_assert(offsetof(" << ls << ", " << member << ") == " << Offset(offset) << ", \"" << cn << "::" << f.id << "\");\n";
			};
			gen << "// The offsets the class's definitions give.\n";
			if (cls.layoutSize % layoutAlign == 0)
				gen << "static_assert(sizeof(" << ls << ") == " << cls.layoutSize << ", \"" << cn << ": the layout's size\");\n";
			for (const Field& f : layout)
			{
				const uint32_t off = LayoutOffset(*f.def);
				if (f.def->isArray())
				{
					checkOffset("_Array_" + f.id, off, f);
					checkOffset(f.id, off + LayoutArrayHeader(*f.def), f);
				}
				else
				{
					checkOffset(f.id, off, f);
				}
			}
			gen << "\n";
		}

		h << "namespace Attrib\n"
		  << "{\n"
		  << "    namespace Gen\n"
		  << "    {\n"
		  << "\n"
		  << Indented(gen.str(), 2)
		  << "    } // namespace Gen\n"
		  << "} // namespace Attrib\n";
		return h.str();
	}

} // namespace attrib
