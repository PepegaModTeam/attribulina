#pragma once

// Model mutations used by the editor. They keep the in-memory model
// consistent (class/vault/parent links, pointer slots); saving is the writer's job.

#include "Database.h"

#include <string>

namespace attrib
{

	// Gives a copied attribute its own nested blocks and clears its writer
	// hints, so it is saved as new data rather than as the original's pointers.
	void DetachCopy(Attribute& a);
	// The same for a block (a copied layout).
	void DetachCopy(DataBlock& b);

	// Inserts a zeroed element at `at` (0..count), shifting pointer slots.
	// Both keep an entry array's capacity (Array::mAlloc) at its count, as every
	// file read has it, so an array grown and shrunk back writes as it was read.
	void InsertElement(Attribute& a, uint32_t at);
	// Removes element `at`, dropping its pointer slots and shifting the rest.
	void RemoveElement(Attribute& a, uint32_t at);
	// One element of an array kept aside (the inspector keeps what shrinking an
	// array removed, to bring back when it grows again): its bytes and the
	// pointers in it, their offsets from the element's start.
	struct ElementData
	{
		Bytes bytes;
		std::vector<PtrSlot> ptrs;
	};
	ElementData CopyElement(const DataBlock& b, uint32_t at, uint32_t elemSize);
	// Puts `e` in element `at` of `b`, which must hold it, replacing its pointers.
	void PutElement(DataBlock& b, uint32_t at, uint32_t elemSize, const ElementData& e);
	// Largest element count the attribute may hold. A layout array is bounded by
	// its inline capacity (the game's compiled layout fixes the space), an entry
	// array by its definition's count (mMaxCount, 0 = 65535). With `pastLimit`
	// an entry array may hold 65535 (Array::mCount is 16 bits): the game reads an
	// array by its own count, so scripts may go past the definition, with a
	// warning, and the editor may when its option is on.
	uint32_t MaxElements(const Attribute& a, bool pastLimit = false);
	// The count an entry array's definition allows (mMaxCount), 0 if it gives
	// none or the array is in the layout.
	uint32_t ElementLimit(const Attribute& a);
	// "GEAR_RATIO has 12 elements, more than the 9 its definition allows", or
	// empty when `count` is within the limit.
	std::string ElementLimitWarning(const Attribute& a, uint32_t count, const std::string& field);

	// Gives `col` its own value for `field`: a copy of the inherited value if
	// there is one, otherwise zeros. Returns the existing attribute if already set.
	Attribute& OverrideAttribute(Database& db, Collection& col, const FieldDef& field);
	// Removes a collection's own (non-layout) value so it inherits again.
	bool RevertAttribute(Collection& col, uint32_t fieldKey);

	// Sets the CollectionName text field (gameplay has one, as do some other
	// classes) to the collection's name, as an own value when it was inherited:
	// copies take their new name there, as nfsms and Attribulator do. With
	// `onlyIf`, only when it held that text (a rename follows the old name).
	// False when the class has no such field, the name isn't known, or nothing
	// changed.
	bool SetCollectionNameField(Database& db, Collection& col, const std::string* onlyIf = nullptr);

	// A collection's export id: StringHash32("<class name>/<collection name>").
	uint32_t CollectionExportId(const KeyNames& names, uint32_t classKey, const std::string& name);
	// As `vault` stores it: StringHash64 of the same text in the early Most Wanted demos.
	uint64_t CollectionExportId(const Vault& vault, const KeyNames& names, uint32_t classKey, const std::string& name);

	// A new, empty vault in `pack`, with the header fields (version, endianness,
	// World's EndC chunks) of `like` and the usual dependencies "<name>.vlt" and
	// "<name>.bin". Throws if the pack already has a vault of that name, or is
	// one of World's event vault files (they hold one vault each).
	Vault& CreateVault(LoadedPack& pack, const std::string& name, const Vault& like);
	// Removes an empty vault (no collections, no classes, not the database).
	bool RemoveVault(LoadedPack& pack, Vault& vault);
	Vault* FindVault(LoadedPack& pack, const std::string& name);
	// The vault of that name in any pack of the database, or null.
	Vault* FindVault(const Database& db, const std::string& name);

	// Why `name` can't name a new vault in `pack`, or be `self`'s new name: it
	// is empty, starts or ends with a space, holds a character a file name can't
	// (its dependencies are "<name>.vlt" and "<name>.bin", and World names an
	// event vault's file by it), is too long for an event vault file's header,
	// or a vault of the database has it already, in any case (the game finds
	// vaults by name, and Windows file names ignore case). Empty when it can.
	std::string VaultNameProblem(const Database& db, const LoadedPack& pack, const std::string& name, const Vault* self = nullptr);

	// The gameplay collections whose own gameplayvault names `vaultName`: the
	// elements whose children the game loads from that vault.
	std::vector<Collection*> CollectionsNamingVault(const Database& db, const std::string& vaultName);

	// Renames `vault`. Its dependencies follow, and so does every gameplayvault
	// value that named it (CollectionsNamingVault), which it returns. Throws for
	// a name VaultNameProblem refuses, for the database's vault, and for one of
	// World's event vaults, which its file is named after (make another event
	// vault file and move the collections into it).
	std::vector<Collection*> RenameVault(Database& db, Vault& vault, const std::string& name);

	// What deleting `vault` with what it holds takes: its collections and every
	// collection under them (in any vault), each child before its parent.
	std::vector<Collection*> VaultContents(const Vault& vault);

	// World's event vaults are files of their own (gc.vaults\<name>.bin, Pack.h),
	// loaded from that folder when the game starts. A new one at `path`, holding
	// vault `name` with the header fields of `like` (another of the database's
	// vaults) and the VaultData export every event vault ends with, set to slot
	// 0 (kVaultSlot_Event). Throws for a name VaultNameProblem refuses or a
	// path that is open already. Database::removePack drops one emptied again.
	LoadedPack& CreateEventVaultFile(Database& db, const std::string& path, const std::string& name, const Vault& like);
	// Where a new event vault `name` of the database goes (EventVaultPath,
	// Folder.h): beside its open event vaults, else in the gc.vaults folder
	// beside its attributes.bin. Empty when no pack is open.
	std::string NewEventVaultPath(const Database& db, const std::string& name);
	// The vault a new event vault takes its header fields from: an open event
	// vault, else the database's vault. Null when no vault is open.
	const Vault* EventVaultModel(const Database& db);

	// Reorders a vault's exports so every collection comes after its parent when
	// the parent is in the same vault (the game looks parents up as it loads).
	// Everything else keeps its order.
	void KeepParentsFirst(Vault& vault);

	// Current-gen Undercover's static index tables (a static field of the type
	// kIndexTableType, TypeLayouts.h) list the collections of their class: in
	// aud_moment, traffic_engine and traffic_horn each parent's key (0 for the
	// roots) with its children's keys, in speech each collection's key with its
	// parent's. Which of the two a table is comes from its stored entries (an
	// empty one's from the class: speech's the second way). After
	// a collection of the class is created, deleted, renamed or moved this brings
	// the table in line: entries that still hold stay as stored (their order, a
	// root's unexplained value in speech), the rest are added or dropped.
	// CollectionEditor calls it; true when a table changed.
	bool UpdateIndexTables(ClassDef& cls);
	// The same, from nothing, for a table that names keys none of the class's
	// collections has (VaultLib leaves the values block out, and mVals points at
	// what follows: VaultLib.h). A table that holds keeps its order. True when
	// one was written again.
	bool RebuildIndexTables(ClassDef& cls);

	class CollectionEditor
	{
	public:
		explicit CollectionEditor(Database& db) : db_(db)
		{
		}

		// New collection `name` of `cls` in `vault`, child of `parent` (may be
		// null). The layout is copied from the parent, or zeroed. Throws if the
		// name is taken in that class.
		Collection& create(Vault& vault, ClassDef& cls, const std::string& name, Collection* parent);
		// The same for a collection known only by its key (its export id stays 0
		// until the caller sets it).
		Collection& create(Vault& vault, ClassDef& cls, uint32_t key, Collection* parent);
		// Fails (returns false) if the collection has children.
		bool remove(Collection& col);
		// Throws if the name is taken in the same class.
		void rename(Collection& col, const std::string& newName);
		// To a key whose name may not be known (an nfsms rename_node to 0x...):
		// the export id is the key's name's, or its hex's.
		void rename(Collection& col, uint32_t newKey);
		// Puts `col` under `newParent` (null: a root collection). Its own values
		// stay; what it inherits changes. False when newParent is of another
		// class, is col, or is under col.
		bool move(Collection& col, Collection* newParent);
		// Moves `col` (not its children) to `vault`, so it is saved in that
		// vault's file. Throws if the vault has the other byte order.
		void moveToVault(Collection& col, Vault& vault);

	private:
		Collection& create(Vault& vault, ClassDef& cls, uint32_t key, const std::string* name, Collection* parent);

		Database& db_;
	};

} // namespace attrib
