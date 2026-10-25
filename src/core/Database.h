#pragma once

// The editable model of an AttribSys database: types, classes and their field
// definitions, and collections (the data rows) with their attribute values.
// doc/vault-format.md describes the file format this is read from and written to.
//
// Load data layouts (Pro Street milestone PDB, verified against PC retail):
//   DatabaseLoadData (0x10) { u32 mNumClasses; u32 mDefaultDataSize; u32 mNumTypes;
//                             char* mTypenames } followed by u32 typeSizes[mNumTypes]
//   ClassLoadData (0x20)    { u32 mClass; u32 mCollectionReserve; u32 mNumDefinitions;
//                             Definition* mDefinitions; u32 mStaticSize; void* mStaticData;
//                             u32 mLayoutSize; u16 mLayoutKeyShift; u16 mLayoutCount }
//   Definition (0x10)       { u32 mKey; u32 mType (StringHash32 of the type name);
//                             u16 mOffset; u16 mSize; u16 mMaxCount; u8 mFlags; u8 mAlignment (log2) }
//   CollectionLoadData (0x20) { u32 mKey; u32 mClass; u32 mParent; u32 mTableReserve;
//                             u32 mTableKeyShift; u32 mNumEntries; u16 mNumTypes; u16 mTypesLen;
//                             void* mLayout } followed by u32 typeHashes[mTypesLen]
//                             (mNumTypes used, zero padded) and AttribEntry[mNumEntries]
//   AttribEntry (0xC)       { u32 mKey; u32 mData; u16 mType (index into typeHashes);
//                             u8 mNodeFlags; u8 mEntryFlags }
//   Attrib::Array (8)       { u16 mAlloc; u16 mCount; u16 mSize (element size); u16 mEncodedTypePad }
//                             followed by the elements (at +16 when mEncodedTypePad has 0x8000)
//
// The early Most Wanted demos (RawVault::keys64) widen every key to a
// StringHash64: DatabaseLoadData (0x14) has a u32 0 at +4 (types at +20);
// ClassLoadData (0x20) { u64 mClass; u32 mCollectionReserve; u32 mNumDefinitions;
// Definition*; u32 mLayoutSize; u32 mLayoutKeyShift; u32 mLayoutCount };
// Definition (0x18) { u64 mKey; u64 mType; u16 ...; u8; u8 }; CollectionLoadData
// (0x30) { u64 mKey; u64 mClass; u64 mParent; u32 mTableReserve; u32
// mTableKeyShift; u32 mNumEntries; u32 mNumTypes; void* mLayout; u32 0 } then
// u64 types and AttribEntry (0x10) { u64 mKey; u32 mData; u16; u8; u8 }. The
// model keeps 32-bit keys and maps them (Database::keyFrom64, keyTo64).
//
// Several orders in the files come from Attribulator's internal containers and
// can't be derived from the data (PtrN order, StrE order, export order, entry
// order, some type lists, mTableKeyShift). The model keeps them as "hints" so
// an unmodified database is written back byte for byte.

#include "Hash.h"
#include "KeyNames.h"
#include "Pack.h"
#include "RawVault.h"
#include "Schema.h"
#include "TypeLayouts.h"

#include <deque>
#include <map>
#include <memory>
#include <unordered_map>

namespace attrib
{

	// How the editor treats a type. Kinds the schema doesn't know stay Unknown and
	// are edited as bytes.
	enum class TypeKind : uint8_t
	{
		Unknown,
		Bool, Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Float, Double, Char,
		Text,        // char* into the vault's string table
		StringKey,   // { u32 StringHash32(text) (0 for ""); char* text }, or MW's 16 bytes (Values.h StringKeyAt)
		RefSpec,     // { u32 classKey; u32 collectionKey; Collection* (runtime, 0 on disk) }
		Key,         // u32 StringHash32 of a name
		BinKey,      // u32 bStringHash of a name
		Colour,      // 4 bytes A, R, G, B
		FloatColour, // 4 floats
		Vector2, Vector3, Vector4, Quaternion, Matrix,
		Blob,        // { u32 size; void* data }
		Curve,       // see Curve.h
		Struct,      // a game struct the type schema describes
		Enum,        // a game enum the type schema describes
		// { u32 collectionKey; Collection* (runtime, 0 on disk) }: a reference to a
		// collection of the class its type names, Attrib::Gen::ClassRefSpec_<class>
		// (Undercover on; Values.h ClassRefSpecClass).
		ClassRefSpec,
	};

	struct TypeInfo
	{
		std::string name;
		uint32_t hash = 0;   // StringHash32(name): what the model refers to it by
		uint64_t hash64 = 0; // StringHash64(name), in a keys64 database
		uint32_t size = 0;
		uint32_t index = 0;
		TypeKind kind = TypeKind::Unknown;
		// From the type schema, when it has this type at this size.
		const StructDef* schemaStruct = nullptr;
		const EnumDef* schemaEnum = nullptr;
		// Pointer layout derived from the schema, for types the hand-written
		// table (TypeLayouts.cpp) doesn't list.
		std::shared_ptr<const PointerLayout> derivedPointers;

		// Where this type's pointers are: the hand table first, then the schema.
		const PointerLayout* pointers() const;
	};

	namespace DefFlag
	{
		// HasHandler (Undercover on): the type has a constructor or destructor; its
		// entries carry NodeFlag::HasHandler.
		constexpr uint8_t Array = 1, InLayout = 2, IsBound = 4, NotSearchable = 8, IsStatic = 16, HasHandler = 32;
	}
	namespace NodeFlag
	{
		constexpr uint8_t Release = 1, Array = 2, Inherited = 4, HasHandler = 8, LaidOut = 16, Static = 32,
			ByValue = 64, InUse = 128;
		// Most Wanted's own vaults mark a by-value entry with 32 (they have no static
		// flag); the reader turns it into ByValue and the writer back.
		constexpr uint8_t MostWantedByValue = 32;
	}

	// Original position of a pointer in the vault's PtrN chunk; -1 for pointers
	// the editor created. The writer emits known pointers in this order.
	using PtrSeq = int32_t;
	constexpr PtrSeq kNewPtr = -1;

	struct FieldDef
	{
		uint32_t key = 0;
		uint32_t typeHash = 0;
		const TypeInfo* type = nullptr;
		uint16_t offset = 0;
		uint16_t size = 0;
		uint16_t maxCount = 0;
		uint8_t flags = 0;
		uint8_t alignment = 0; // log2

		bool isArray() const
		{
			return flags & DefFlag::Array;
		}
		bool inLayout() const
		{
			return flags & DefFlag::InLayout;
		}
		bool isStatic() const
		{
			return flags & DefFlag::IsStatic;
		}
	};

	// Bytes that may contain pointers. Each pointer slot holds either a string
	// (Text, StringKey, ...) or a nested block (Blob data, Curve arrays, ...).
	// Nested blocks can be shared between slots (FEPartData does this).
	struct DataBlock;
	struct PtrSlot
	{
		uint32_t offset = 0; // of the pointer inside the owning bytes
		bool isString = false;
		std::string text;
		std::shared_ptr<DataBlock> data;
		PtrSeq seq = kNewPtr;
	};
	struct DataBlock
	{
		Bytes bytes; // pointer slots are zero
		std::vector<PtrSlot> ptrs;
		uint32_t align = 4; // for nested blocks; top-level blocks get theirs from their definition
		// Most Wanted's layout lists null pointers too: where each one stood in
		// PtrN. The writer finds null pointers by type; these only order them.
		struct NullPtr
		{
			uint32_t offset;
			PtrSeq seq;
		};
		std::vector<NullPtr> nulls;

		PtrSlot* ptrAt(uint32_t offset);
		const PtrSlot* ptrAt(uint32_t offset) const;
	};

	struct Vault;
	struct ClassDef;
	struct Collection;
	struct LoadedPack;
	struct ClassList; // ClassList.h

	struct Attribute
	{
		uint32_t key = 0;
		const FieldDef* def = nullptr;
		const TypeInfo* type = nullptr;
		bool inLayout = false;
		bool isArray = false;
		uint16_t capacity = 0;       // Array::mAlloc
		uint16_t encodedTypePad = 0; // Array::mEncodedTypePad (0x8000: 16-byte elements)
		uint8_t nodeFlags = 0;       // AttribEntry flags (entries only)
		uint8_t entryFlags = 0;
		uint32_t count = 1;
		uint32_t elemSize = 0;
		DataBlock data;              // count * elemSize bytes
		PtrSeq dataSeq = kNewPtr;    // the entry's mData pointer (entries only)

		uint8_t* elem(uint32_t i)
		{
			return data.bytes.data() + size_t(i) * elemSize;
		}
		const uint8_t* elem(uint32_t i) const
		{
			return data.bytes.data() + size_t(i) * elemSize;
		}
	};

	struct ClassDef
	{
		uint32_t key = 0;
		uint32_t collectionReserve = 0;
		std::vector<FieldDef> fields; // file order (sorted by key in retail)
		uint32_t staticSize = 0;
		DataBlock staticData;
		uint32_t layoutSize = 0;
		uint16_t layoutKeyShift = 0;
		uint16_t layoutCount = 0;
		Vault* vault = nullptr;
		std::vector<Collection*> collections; // every loaded collection of this class

		// Writer hints.
		PtrSeq defsSeq = kNewPtr;
		PtrSeq staticSeq = kNewPtr;

		const FieldDef* field(uint32_t fieldKey) const;
	};

	struct Collection
	{
		uint32_t key = 0;
		uint32_t classKey = 0;
		uint32_t parentKey = 0;
		ClassDef* cls = nullptr;
		Collection* parent = nullptr;
		std::vector<Collection*> children;
		uint32_t tableKeyShift = 0;
		DataBlock layout;                   // raw layout block; layout fields are copied out into attributes
		std::vector<Attribute> attributes;  // layout fields first (class order), then entries (file order)
		Vault* vault = nullptr;

		// Writer hints.
		uint64_t exportId = 0;                  // StringHash32("class/collection"), or StringHash64 in keys64 vaults
		std::vector<uint32_t> typeHashes;       // stored type list (some carry unused extra types)
		std::vector<uint32_t> loadedEntryKeys;  // entry keys as loaded: the key shift is kept while unchanged
		PtrSeq layoutSeq = kNewPtr;
		// What the layout pointer's slot holds before its fixup overwrites it:
		// 0 everywhere but one collection of World's gpcore.bin.
		uint32_t layoutSlotStored = 0;

		Attribute* find(uint32_t fieldKey);
		const Attribute* find(uint32_t fieldKey) const;
		// Looks the field up here and then up the parent chain (layout fields are
		// never inherited). `owner` receives the collection that holds it.
		const Attribute* resolve(uint32_t fieldKey, const Collection** owner = nullptr) const;
	};

	// An export kept as it was read: World's VaultData (ExportType::VaultData).
	struct DataExport
	{
		uint64_t id = 0;
		uint32_t type = 0;
		Bytes bytes;
	};

	// One export of a vault, in DatN order.
	struct ExportRef
	{
		enum class Kind : uint8_t
		{
			Database, Class, Collection, Data
		} kind;
		ClassDef* cls = nullptr;
		Collection* collection = nullptr;
		uint32_t data = 0; // Kind::Data: its index in Vault::dataExports
	};

	struct Vault
	{
		std::string name;
		LoadedPack* pack = nullptr;
		RawVault raw;            // the vault as loaded (header chunks are reused when writing)
		bool isDatabase = false; // holds the DatabaseLoadData (the "db" vault)
		std::vector<ClassDef*> classes;
		std::vector<Collection*> collections; // export order
		std::vector<DataExport> dataExports;
		std::vector<std::string> warnings;
		// False when something was read that the writer can't reproduce.
		bool exactlyWritable = true;

		// Writer hints.
		std::vector<ExportRef> exportOrder;
		std::vector<std::string> stringOrder; // StrE strings as loaded
		uint64_t databaseExportId = 0;
		PtrSeq typenamesSeq = kNewPtr;
	};

	namespace detail
	{
		class BlockReader;
	}

	struct LoadedPack
	{
		std::string path;
		PackFormat format = PackFormat::Vpak; // a VPAK, or one of World's event vaults (Pack.h)
		std::optional<LzKind> compression;
		uint32_t align = 0x40; // of its blocks (PackFile::align)
		std::vector<std::unique_ptr<Vault>> vaults; // pack order
		bool dirty = false;
	};

	class Database
	{
	public:
		Database();
		~Database();
		Database(const Database&) = delete;
		Database& operator=(const Database&) = delete;

		// Loads a pack. The pack that holds the DatabaseLoadData (attributes.bin)
		// must be loaded before any pack that only holds collections.
		LoadedPack& loadPack(const std::string& path);
		LoadedPack& loadPack(const std::string& path, ByteSpan fileData);

		bool hasSchema() const
		{
			return !types_.empty();
		}

		// The game and platform (Game.h). Set it before loading: the type schema
		// is picked by it. A database loaded without one detects it from its
		// first pack (DetectTarget), certain or not.
		const Target& target() const
		{
			return target_;
		}
		void setTarget(const Target& t)
		{
			target_ = t;
		}

		// Type schemas to choose from. Applied when the database is loaded (add
		// them before loading attributes.bin): one for the target's game that
		// lays out its platform, else the game's that fits the type table best
		// (names and sizes). Another game's schema never applies; without a
		// target, the best fit of all does.
		void addTypeSchema(std::shared_ptr<const TypeSchema> schema);
		const TypeSchema* typeSchema() const
		{
			return typeSchema_.get();
		}
		std::shared_ptr<const TypeSchema> sharedTypeSchema() const
		{
			return typeSchema_;
		}
		const std::vector<TypeInfo>& types() const
		{
			return types_;
		}

		// The game's own classes, as its retail files define them (data/classes,
		// ClassList.h): which classes are the game's (they can be deleted and
		// restored but not redefined) and what restoring one gives.
		void addClassList(std::shared_ptr<const ClassList> list)
		{
			classLists_.push_back(std::move(list));
		}
		const std::vector<std::shared_ptr<const ClassList>>& classLists() const
		{
			return classLists_;
		}
		const TypeInfo* typeByHash(uint32_t hash) const;
		// DatabaseLoadData's mNumClasses, about 1.5 x (classes + 2) in retail
		// files. ClassEditor raises it as classes are added.
		uint32_t numClassesDeclared() const
		{
			return numClassesDeclared_;
		}
		// DatabaseLoadData's mDefaultDataSize: the largest class layout in retail
		// files (World's: its largest type). ClassEditor raises it as classes are
		// added.
		uint32_t defaultDataSize() const
		{
			return defaultDataSize_;
		}
		// The u32 at DatabaseLoadData + 4 in a keys64 database (0 in the demos).
		uint32_t databaseWord4() const
		{
			return databaseWord4_;
		}

		const std::vector<std::unique_ptr<ClassDef>>& classes() const
		{
			return classes_;
		}
		ClassDef* findClass(uint32_t key) const;
		// Last loaded wins when two vaults define the same collection.
		Collection* findCollection(uint32_t classKey, uint32_t key) const;

		const std::vector<std::unique_ptr<LoadedPack>>& packs() const
		{
			return packs_;
		}

		// StringHash32 names (keys, classes, fields, collections).
		KeyNames& names()
		{
			return names_;
		}
		const KeyNames& names() const
		{
			return names_;
		}
		// bStringHash names (type_bStringHash values). Vault strings go into both.
		KeyNames& binNames()
		{
			return binNames_;
		}
		const KeyNames& binNames() const
		{
			return binNames_;
		}

		size_t collectionCount() const
		{
			return collections_.size();
		}

		// The early Most Wanted demos key everything with StringHash64
		// (RawVault::keys64). The model keeps 32-bit keys: StringHash32 of the
		// name whose StringHash64 the key is, or, for a key no name is known for,
		// StringHash32 of the key written as "0x" and 16 hex digits, which then
		// is its name, so it shows and can be typed like any other.
		bool keys64() const
		{
			return keys64_;
		}
		// Records the pair (and a "0x..." name) the first time; const so values
		// can be read through it.
		uint32_t keyFrom64(uint64_t key) const;
		// The StringHash64 a model key stands for: the one it was read from,
		// else its name's (a "0x" + 16 hex digits name is the key itself). 0 for
		// 0. Throws FormatError when the key has no name; findKey64 says nothing.
		uint64_t keyTo64(uint32_t key) const;
		std::optional<uint64_t> findKey64(uint32_t key) const;
		// StringHash64("<class>/<name>"), a keys64 vault's collection export id.
		uint64_t exportId64(uint32_t classKey, const std::string& name) const;

	private:
		friend class CollectionEditor;
		friend class ClassEditor;

		void loadVault(Vault& vault);
		void loadDatabaseExport(Vault& vault, const ExportEntry& e, detail::BlockReader& blocks);
		void loadClassExport(Vault& vault, const ExportEntry& e, detail::BlockReader& blocks);
		void loadCollectionExport(Vault& vault, const ExportEntry& e, detail::BlockReader& blocks);
		void linkParents();
		void applyTypeSchema();

		Target target_;
		std::vector<std::shared_ptr<const TypeSchema>> schemaCandidates_;
		std::vector<std::shared_ptr<const ClassList>> classLists_;
		std::shared_ptr<const TypeSchema> typeSchema_;
		std::vector<TypeInfo> types_;
		std::deque<TypeInfo> unlistedTypes_; // kUnlistedTypes this database's fields may use
		std::unordered_map<uint32_t, const TypeInfo*> typeByHash_;
		uint32_t numClassesDeclared_ = 0;
		uint32_t defaultDataSize_ = 0;
		uint32_t databaseWord4_ = 0;

		std::vector<std::unique_ptr<ClassDef>> classes_;
		std::unordered_map<uint32_t, ClassDef*> classByKey_;
		std::vector<std::unique_ptr<Collection>> collections_;
		std::unordered_map<uint64_t, Collection*> collectionByKey_;
		std::vector<std::unique_ptr<LoadedPack>> packs_;
		mutable KeyNames names_{ &StringHash32 }; // keyFrom64 names the keys it meets
		KeyNames binNames_{ &BinHash };
		bool keys64_ = false;
		mutable std::unordered_map<uint64_t, uint32_t> from64_;
		mutable std::unordered_map<uint32_t, uint64_t> to64_;
		std::unordered_map<uint64_t, const TypeInfo*> typeBy64_;
	};

	// "0x" and 16 hex digits: the name a keys64 database gives a key it has no
	// name for (Database::keyFrom64), and back.
	std::string Key64Name(uint64_t key);
	std::optional<uint64_t> ParseKey64Name(std::string_view name);

	// What a pack file holds, read without loading it into a database.
	struct PackSummary
	{
		std::vector<std::string> vaults;
		bool hasDatabase = false; // a vault holds the DatabaseLoadData (attributes.bin)
	};
	PackSummary DescribePack(ByteSpan fileData);

	TypeKind ClassifyType(const std::string& name, uint32_t size);
	const char* TypeKindName(TypeKind kind);
	// Bytes before the first element of an array stored inside a layout block
	// (an Attrib::Array header, padded to 16 for 16-byte-aligned element types).
	uint32_t LayoutArrayHeader(const FieldDef& f);
	// Bytes a layout field occupies: its size, or header + capacity elements
	// (capacity is the inline header's mAlloc).
	uint32_t LayoutExtent(const FieldDef& f, uint16_t capacity);
	// Where a layout field's data is: its definition's offset, except a 4-byte
	// bool the definition puts off a 4-byte boundary. Most Wanted's GameCube and
	// PS2 definitions keep packed offsets for their last bools (a class's 26, 30
	// bytes long), while the data, as the game's compiled layout reads it, has
	// them at the next boundary: always 0 or 1 there.
	uint32_t LayoutOffset(const FieldDef& f);
	// The bytes a class's layout block holds: its layout size, or more when such
	// a bool ends past it.
	uint32_t LayoutBytes(const ClassDef& cls);
	// A new array's Attrib::Array::mEncodedTypeAndPad: 0x8000 when its elements
	// are 16-aligned (they start 16 bytes in), else 0, as in every retail array.
	// The low bits are a type index the game fills in as it loads (0 on disk).
	uint16_t ArrayPad(const FieldDef& f);
	// Header size of an out-of-line Attrib::Array.
	inline uint32_t ArrayHeaderSize(uint16_t encodedTypePad)
	{
		return (encodedTypePad & 0x8000) ? 16u : 8u;
	}

} // namespace attrib
