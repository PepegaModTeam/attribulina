# AttribSys vault format

What Attribulina reads and writes, verified against the retail Pro Street PC
files in `attrib/databases/pc/prostreet-vanilla/`. The writer rebuilds
`ATTRIBUTES.BIN`, `cars_vault.BIN`, `gameplay.bin` (288 vaults) and
`FE_ATTRIB.BIN` byte for byte from the parsed model (`tests/test_writer.cpp`).
The 360, PS2 and Wii packs in `attrib/databases/360/`, `ps2/` and `wii/`
rebuild byte for byte too.

**V** marks a rule the byte-identical rebuild confirms. **I** marks one the data
doesn't constrain (the rebuild works either way, or nothing exercises it).
Struct layouts come from `nfs-data/ProStreet08Milestone.h`.

**Platforms.** The 360, PS2 and Wii packs follow every rule here. **V**

- **360 and Wii:** a vault is big-endian throughout: chunk headers, offsets,
  fixups and values. The VPAK header and LZHeader blocks stay little-endian, and
  so do the compiled Lua chunks. Their types have the PC's sizes and layouts.
- **PS2:** little-endian like the PC, but its compiler makes a bool 4 bytes.
  `EA::Reflection::Bool` is 4 bytes in its type table, every bool member is too
  (`bool32` in `data/schema/prostreet_ps2.txt`), and `FEPartData` is 68 bytes
  instead of 56 (see Pointer-bearing types).

**Carbon** follows every rule here too, on PC, PS3, Wii, PS2, GameCube and
360: all three packs of each rebuild byte for byte. **V** The PS3's, the Wii's
and the 360's are big-endian with the PC's layouts; the PS2's and the
GameCube's bools are 4 bytes (`carbon_ps2.txt`), the GameCube's values
big-endian. Its types come from
the PS3 build's DWARF types (`nfs-data/carbon/ps3-milestone/types_pahole.txt`).
They explain the 24 string pointers in `attributes.bin` and the 22 in
`gameplay.bin` that nothing did before: `CopCountRecord.CopType` and
`GMapRegionInfo.mName` are `EA::Reflection::Text`, a `char*`. Its handler blobs
can have a short size (see Lua). The 360's files carry the PS2's and the
GameCube's `Vers` value, and their type table is the PC's but for the PS2's
20-byte `SteeringSensitivityParameter`, which no header lays out.
`carbon_ps2.txt` and `carbon_x360.txt` lay it out on all three as the PC's
without its two dead zones (`scripts/carbon_ps2_x360_types.h`, inferred from
the data).

**Carbon's two demos** (a 360 one made after release and a PS2 one) use Most
Wanted's vault layout (below) and older types: Most Wanted's 16-byte
`StringKey`, with `carbon_x360_demo.txt` and `carbon_ps2_demo.txt` taking it
from Most Wanted's header. The 360 demo's
packs rebuild byte for byte, and so do the PS2 demo's `attributes.bin` and
`FE_ATTRIB.bin`. **V** Two of the PS2 demo's 127 gameplay vaults
(`race_bin_challenge`, `race_bin_drift`) keep 254 and 8 strings in `StrE` that
nothing points at; the model doesn't hold them, so those two come out shorter.
The PS2 demo's `gameplay.bin` isn't its `gameplay.lzc` either: the `.bin`
has some gameplay names overwritten with zeros (`*` then zeros then `_520`).

**Most Wanted**: retail PC, Xbox and GameCube, and betas for the 360 and the
PS2. Every pack rebuilds byte for byte. **V** Its vault layout is above. Every
platform's type table holds the same 94 types at the same sizes but for
`EA::Reflection::Bool` (4 bytes on the GameCube and the PS2). The types come
from the GameCube build's DWARF dump (`exes/gamecube/mw-vanilla/NFSMWRELEASE.h`:
no offsets, no namespaces), laid out naturally: `mw_pc.txt` (PC, Xbox, 360)
and `mw_gamecube.txt` (GameCube, PS2), which differ in one bool. Its built-in
vectors and colours are `Attrib::Types::*`, its `StringKey` the 16-byte one.
Its attributes.bin names the platform: the byte order, the bool's width, and
on little-endian 1-byte bools a `timeofdaylighting` class, which the PC's has
and the Xbox's lacks.

**Most Wanted's early demos** (the PS2 OPM and Xbox OXM beta demos) are its
own layout with every key a `StringHash64`, and wider types that hold keys
(below). Both rebuild byte for byte. **V** Their types are retail's but for
ten, which `scripts/mw_demo_types.h` lays out from the data:
`mw_xbox_demo.txt` and `mw_ps2_demo.txt`.

**Undercover's Wii version** is Carbon's engine, and its packs rebuild byte for
byte with Carbon's types and a few of Undercover's own. **V** Its PS2 version
is the same game; no files of it have been seen.

**Current-gen Undercover** (PC, 360) is Pro Street's layout with `StrN` before
`DepN`, a few new pieces of data and its own types (below). All four of the
PC's packs and of the 360's rebuild byte for byte. **V** So do those of
another PC build (`pc/undercover-vlted`: its own `Vers`, 256 gameplay vaults
to 219), whose files carry no tool's mark. An NFS-VltEd 4.6 re-save of that
build read before (each `StrE` starting with the string `NFS-VltEd 4.6.0.0 by
nfsu360`, which nothing points at) loaded with every pointer explained, and
saving wrote it in the retail layout. A 360 alpha (build 161) follows the
same rules, and its five files rebuild byte for byte too. **V** No PS3 files have been seen. Nitro
hasn't been checked against this.

**World** (the final PC build) is current-gen Undercover's layout with `EndC`
chunks, and its event vaults are files of their own with a signature (below).
All 71 packs of `pc/world-final` rebuild byte for byte: `attributes.bin`,
`commerce.bin`, `FE_ATTRIB.bin` and the 68 event vaults in `gc.vaults`. **V**
Its 2009 and 2010 betas follow the same rules (the 2009 one has no `EndC`), and
all of their packs rebuild byte for byte too: 338 and 346, event vaults
included. **V** So do the 30 folders of `pc/world-various`, named by build
number from `_AT1` (the 2009 beta) and `34` (the 2010 beta) to `1613-1614` (the
final build's `Vers`): some 9800 packs, 3843 of them distinct. **V** One schema
lays out every build. Between the 2010 beta and the final build come 13 more
`Vers` values, one per schema change, each with its own classes.

**Files VaultLib saved** (the community Attribulator and OGVI) follow none of
the games' header rules: every vault gets World's final `Vers`, StrN before
DepN and EndC chunks, and its bin's strings open with "Generated by VaultBoy by
heyitsleo". Class layout sizes, array pads, index tables, World's `gpcore`
slot and the packing of every blob (JDLZ) come out wrong too. Pro Street
doesn't work with it at all: its camera lists and tiers come out wrong, and
the file can't give them back. `src/core/VaultLib.h` lists each difference,
read from VaultLib's source, and what the reader does about it.

## Hashes

- Every key is `Attrib::StringHash32`: lookup2 with initial value `0xABCDEF00`.
  That covers class, collection and field keys, type names, dependency names
  and Lua handler names.
- An empty `StringKey` string is stored with hash 0, not `StringHash32("")`. **V**
- **Collection export id**: `StringHash32("<class>/<collection>")`. **V** This
  holds for 6207/6211 named collections in `ATTRIBUTES.BIN`. The four misses
  are name collisions in `attrib_keys.txt`, not exceptions.
- **Other export ids**: a class export's id is its class key. The database
  export's id is `StringHash32("Attrib::Database")`.

### 64-bit keys (early Most Wanted demos)

The PS2 OPM and Xbox OXM beta demos of Most Wanted key everything with
`Attrib::StringHash64`: Bob Jenkins' lookup8 with level `0xABCDEF0011223344`,
and 0 for an empty string (`StringHash64` in `src/core/Hash.h`; the Xbox demo's
`0x41E60`, the PS2 demo's `0x1514C8`, Pro Street 360 milestone `0x823681B8`,
Undercover debug `0x825D0F08`, MW PC `0x4522F0`). That covers class,
collection and field keys, type references (`StringHash64` of the type's name),
export ids and export types, and dependency ids. **V** in every vault of both
demos. A collection's export id is `StringHash64("<class>/<collection>")` (all
4210 whose names are known), a class's its key, the database's
`StringHash64("Attrib::Database")`. Some values stay 32-bit: `StringKey` is the
16-byte one with both hashes, `gameplay.message_id` is a `StringHash32` of the
message's name, `AICollisionReactionRecord.Goal` a `StringHash32`. Marks below:
**V** holds in every vault of both demos, **C** read from the Xbox demo's code
(`exes/xbox/mw-early-oxm-demobeta/default.xbe`).

They are otherwise Most Wanted's own layout (no `Vers`, the rules under
"Most Wanted's layout"), with these differences (`RawVault::keys64`):

| Where | Demos |
| --- | --- |
| `DepN` | `u32 count; u32 0; u64 ids[]; u32 nameOffsets[]; names` **V, C** |
| `ExpN` | `u32 count; u32 0`, then 32-byte entries `{u64 id; u64 type; u32 nameOffset (0); u32 size; u32 offset; u32 0}` **V** |
| `PtrN` | 16-byte entries `{u32 fixupOffset; u16 type; u16 index; u64 value}` (the value fits 32 bits), a 16-byte terminator, and a group with no entries has no selection entry either: 146 of the 162 gameplay vaults have no bin pointers and start with `{0, 2, 0, 0}` **V** |
| `DatabaseLoadData` | 0x14: a u32 at +4 (0 in both; the loader at `0x47620` doesn't read it), then `mDefaultDataSize`, `mNumTypes`, `mTypenames`, the sizes at +20 **V, C** |
| `ClassLoadData` | 0x20: `{u64 mClass; u32 mCollectionReserve; u32 mNumDefinitions; Definition*; u32 mLayoutSize; u32 mLayoutKeyShift; u32 mLayoutCount}` **V** |
| `Definition` | 0x18: `{u64 mKey; u64 mType; u16 mOffset; u16 mSize; u16 mMaxCount; u8 mFlags; u8 mAlignment}`, sorted by the u64 key **V** |
| `CollectionLoadData` | 0x30: `{u64 mKey; u64 mClass; u64 mParent; u32 mTableReserve; u32 mTableKeyShift; u32 mNumEntries; u32 mNumTypes; void* mLayout; u32 0}`, then `u64 types[mNumTypes]` unpadded **V, C** |
| `AttribEntry` | 0x10: `{u64 mKey; u32 mData; u16 mType; u8 mNodeFlags; u8 mEntryFlags}` **V, C** |
| Packs | aligned to 16, the Xbox demo's too **V** |

- **Values.** `Attrib::RefSpec` is 24 bytes `{u64 class; u64 collection;
  Collection* (null, listed); u32 0}`, `GCollectionKey` a u64. Eight more types
  grow because they hold those (`AICollisionReactionRecord`,
  `EffectLinkageRecord`, `JunkmanMod`, `TrafficPatternRecord`,
  `TireEffectRecord`, `CollisionStream`, `EmitterDataRecord`), or have fewer
  members than retail's (`LeaderSupport` lacks `PriorityChance` and
  `PriorityTime`, `TrafficPatternRecord` `Percent`). `scripts/mw_demo_types.h`
  has their layouts: the keys at those offsets resolve to class, collection and
  field keys, the RefSpecs' pointers are the ones listed as null, and the
  numbers after them read sensibly. **V** (`EmitterDataRecord`: no field uses
  it, inferred).
- **Orders.** As in retail: `PtrN`, exports and entries keep their stored
  order. Here the `StrE` order (first use) and the type lists (no extras) are
  derivable too. Key shifts are 0 everywhere; a collection whose entries change
  is written with 0, as the tool did: the game puts a key at
  `low32(rotl64(key, shift)) % size` and grows a table whose probes pass 16
  (`Collection::AddAttribute`, `0x45680`). **C** `mCollectionReserve` is
  `max(1, floor(1.4 × collections))` for 53 of 54 classes (`ecar`: 117 for 83),
  so it is kept as read.
- **Lua.** 130 `bytecode` blobs (126 HUFF, 3 JDLZ, one empty), Lua 5.0 chunks
  as retail's, 16-aligned and before their `Blob`. Each defines one function
  named as the handler collection, as retail Most Wanted's. **V**
- **PS2 and Xbox.** Both little-endian, with the same 89 types; the PS2's bools
  are 4 bytes (and its last layout bools sit as retail's GameCube's do). Their
  `GAMEPLAY.BIN` is the same file. **V**

**The model's keys.** Attribulina keeps 32-bit keys and maps them
(`Database::keyFrom64`, `keyTo64`): a 64-bit key whose name the dictionary has
becomes that name's `StringHash32`, as in every other game; one with no known
name (42 collections and 11 fields, all in `db`) gets the name `0x` and its 16
hex digits, which then shows, can be typed, and maps back to the key. Values
holding keys go through the same map (`LoadKey`, `StoreKey` in `Values.h`;
`RefSpecAt` gives a reference's layout). New names are written as their
`StringHash64`.

### Most Wanted's layout

Most Wanted's vaults and two Carbon demos (the PS2 one and a late 360 one,
which start with `Vers` like Carbon's) differ in four places
(`RawVault::mostWantedLayout`). **V** on the 360 demo's three packs, the PS2
demo's, and every pack of Most Wanted's five folders (below).

- **`ExpN` entries are 20 bytes**: `{u32 id; u32 type; u32 nameOffset; u32
  size; u32 offset}` (Most Wanted's `ExportEntry`). `nameOffset` is 0 in
  every file. The reader tells the two apart by whether every entry lands in
  `DatN`.
- **`ClassLoadData` is 0x24 bytes**: `{u32 mClass; u32 mCollectionReserve; u32
  mNumDefinitions; Definition* mDefinitions; u32 mLayoutSize; u32
  mLayoutKeyShift; u32 mLayoutCount; u32 mStaticSize; void* mStaticData}`:
  Most Wanted's 0x1C with the static data added at the end. No class in these
  files has static data.
- **Null pointers are listed in `PtrN`** as `{loc, 1, 0, 0}`, in the same
  groups and among the type 3 entries. That covers every pointer of every
  value (RefSpec's `Collection*` at +8 included, which is most of them), a
  collection's `mLayout` when its class has no layout, and a class's
  `mStaticData` when it has none. The writer finds them by type
  (`PointerLayout::unfollowed` holds the pointers it never follows) and
  orders them by where they stood (`DataBlock::nulls`); new ones go after
  by location, like new pointers.
- **The vlt's pointer slots hold `0xEFFECADD`** (fixed up or listed as null),
  except a by-value entry's string, which holds 0. The bin's hold 0.

Most Wanted's own vaults (`RawVault::mostWantedOwn()`) have no `Vers` chunk:
they start with `DepN`, which also gives the byte order. They differ from the
demos' in a few more places. **V** on retail PC, Xbox (`gameplay.lzc`) and
GameCube, and the 360's and the PS2's betas: every pack rebuilds byte for byte.

- **`ClassLoadData` is 0x1C bytes**: `{mClass; mCollectionReserve;
  mNumDefinitions; Definition* mDefinitions; mLayoutSize; mLayoutKeyShift;
  mLayoutCount}` (`NFSMWRELEASE.h`). No static data: its `Definition` flags are
  `kArray` 1, `kInLayout` 2, `kIsBound` 4 and `kIsNotSearchable` 8, with no
  static bit.
- **`CollectionLoadData`'s type count is a u32**, and the type list after it
  isn't padded to an even count (later games: u16 count, u16 padded length).
- **A by-value entry's node flag is 32** (later games: 64, with 32 for static).
  The reader turns it into 64 and the writer back.
- **Compiled Lua starts on 16 bytes**, and a `Blob`'s data comes before the
  `Blob` itself: in all 258 of a retail gameplay pack's handlers (236 in the
  PS2 beta). The writer places what a block points at before the block in
  these vaults; later games put the `Blob` first, its data after.
- **The PS2 beta's packs align to 16**, not 0x40: the string block, each bin
  and vlt, and the end (`PackFile::align`).
- **The GameCube's and the PS2's 4-byte layout bools** that a definition puts
  off a 4-byte boundary (the last fields of four classes, e.g. a class 30 bytes
  long with a bool at 26) are at the next boundary in the data, where they
  always read 0 or 1; the packed offset reads parts of their neighbours. The
  layout block runs past `mLayoutSize` to hold them (`LayoutOffset`,
  `LayoutBytes`). The game's compiled layouts presumably read them there.

Everything else (chunks, alignment, `Definition`, the bin) follows the rules here.

### Current-gen Undercover

What differs from Pro Street's rules, checked on all 224 vaults of the 360's
files, the PC's 222 and the other PC build's 259 by a model that rebuilds
them byte for byte. The types come from `nfs-data/SpeedDebugOpt-enUS.h`, the
360 debug build's header (its `Attrib::SchemaVersion()` returns the retail
360 `Vers`), laid out naturally; all 112 of its structs match that build's PDB
offsets. The PC's and the 360's types have the same sizes, no bitfields and a
12-byte `Vector3`, so one schema (`undercover_pc.txt`) covers both.

- **Chunk order** `Vers StrN DepN DatN ExpN PtrN`. Both chunks are 16-aligned
  and `DatN` starts at 0x50 either way. The writer keeps the order it read
  (`RawVault::strNFirst`), and a new vault takes it from the vault it is made
  like. **V**
- **`Vers`** `0x8364A394EF316ABD` on the 360's `attributes.bin`, `cars_vault`,
  `FE_ATTRIB` and 14 gameplay vaults, `0xCE50492389B94950` on its other 207,
  and on all of the PC's (little-endian); `0x1445379ED96333E1` on all of the
  other PC build's; `0x482E894A9E6DE7F2` on all of the 360 alpha's. **V**
- **Definition flag 0x20** (`kHasHandler` in the debug header): the type has a
  constructor or a destructor (`RefSpec`, the `ClassRefSpec_*` types, `Curve`
  and what holds them). Every entry of such a field has node flag 8
  (`kHasHandler`), and only those: 5023 of 55088 on the 360. A new entry gets
  it (`OverrideAttribute`). **V**
- **`mMaxCount` 0** means no limit: 507 entry arrays of the `collisionsfx_*`
  classes hold elements with it. **V**
- **Static index tables.** `aud_moment`, `speech`, `traffic_engine` and
  `traffic_horn` have one static field, `IndexTable_CollectionName`, of type
  `Attrib::Query::Static_Inorder_N_to_1<Attrib::Query::Typespace<Attrib::Key,Attrib::Key,EA::Reflection::UInt32> >`,
  which no type table lists (`kUnlistedTypes` in `TypeLayouts.h`). 16 bytes
  (the PDB): `{u32 mCount; u32* mKeys; ISet* mIndices; u32* mVals}`, `mCount`
  keys in ascending order, `mCount` ISets `{u32 index; u32 count}` into
  `mVals`. The three blocks follow the static block, 4-aligned, in pointer
  order. **V** In `aud_moment`, `traffic_engine` and `traffic_horn` a key is a
  parent's key (0 for the roots) and its values its children's; in `speech` a
  key is a collection's and its one value its parent's, for 227 of 228
  (`default` lists `0x4F8B31A9`, which no collection has; the other root lists
  0), on the PC and the 360 alike. **V** The order of a key's values is a hint:
  a parent's children follow no order found (not the export order, the keys,
  the names, their hashes or the reverse of any), and the PC's and the 360's
  files share it. World's (`aud_moment`, `traffic_engine`, `traffic_horn`)
  list every parent's children in the order of their names, compared as bytes
  (`strcmp`), in every build. **V** The editor keeps them in step when a
  collection of the class is created, deleted, renamed or moved
  (`UpdateIndexTables` in `Edit.h`): what still holds stays as stored, the
  rest is added or dropped, a new child last, or by its name in World. Whether
  the game minds a stale one is unknown.
- **Alignments** are Pro Street's; nested float, RefSpec, `FEPartDetail`,
  `Slider` and `FEQuickUpgradeEntry` arrays and the index tables' blocks are
  4-aligned. **V**

NFS-VltEd's PC re-save also differed where NFS-VltEd writes its own way, none of which
the editor keeps: the signature string, two empty strings in `db`'s `StrE`
that nothing points at, bins padded to 8, 16 bytes past each JDLZ handler
block (it writes `headerSize + CompressedSize`, and JDLZ's size counts its
header already), the pack's vaults packed end to end after a first bin at
0x40, and 394 blocks of `db` placed off the retail rule. **V**

The **360 alpha** (build 161, `360/undercover-alpha161`) follows the rules above
on all 272 of its vaults. **V** Its type table has 474 types to retail's 489:
seven at other sizes, 22 of retail's missing, and seven of its own (four
`ClassRefSpec_*` types and three enums). The seven come from its files, read
against retail's members (`scripts/undercover_alpha_types.h`, laid out in
`undercover_x360_alpha.txt`): `CollisionReactionRecord` and `PhysicsUpgrade`
are Pro Street's 24 and 20 bytes; `HighwayPatternCarData`, `PresetRidePaint`
(and `PresetRideRandomPaint`), `Sound::SirenTuning` and
`RenderReflect::cSamplerStateAttribDefinition` are retail's without their last
members (the sampler without the three PS3 members whose enums the table
lacks; its `mName` explains the 36 string pointers of `db` that retail's
64-byte layout left uncovered). **I** for the member names, which no header of
the build gives. Its `gameplay.lzc` is RFPK, an EA RefPack stream in an
LZHeader (`LzKind::Rfpk`): `CompressedSize` counts the stream only, the stream
starts with `10 FB` and the uncompressed size in three big-endian bytes, and
it holds `gameplay.bin` whole. The game's encoder (`REFPACK_Compress`, from the
360 debug build) writes the file again byte for byte. **V** Of the two builds
whose `LZDecompress` was read, Undercover's 360 debug build has an RFPK case
and Pro Street's milestone none, and no other file seen uses it. Its 207
handlers are Lua 5.1.

### World

What differs from current-gen Undercover's rules, checked on every vault of
the final build. No header of any World build is known. Its types come from
Carbon's headers (World began as Carbon), then World's own in `scripts/world/`,
then Undercover's debug header for what Carbon lacks (`world_pc.txt`,
`scripts/gen_schema.py --game world`). World's own are the 2010 beta's
`GameCore` types and enums, which its EASharp bindings expose to C#
(`exes/pc/world-2010-beta/gameplay.dll`, offsets from the native setters its
`nfsw.exe` registers; `scripts/world/easharp.py`), and a few that NFS-VltEd's
World classes, the data and the code describe (`scripts/world/world_types.h`).

- **`EndC`.** The vlt ends with an `EndC` chunk (8 zero bytes), and the bin has
  another right after `StrE`, before the data (`RawVault::endC`). The 2009 beta
  has neither. **V**
- **Event vaults** (`GLOBAL\gc.vaults\<name>.bin`, which the game loads from
  `GLOBAL\gc.vaults`): one vault per file, with a header in place of VPAK's,
  little-endian: `char name[0x2C]` (the file's name, zero-padded), then
  `u32 binOffset` (0x40), `binSize`, `vltOffset`, `vltSize`, `fileSize`. The
  bin, the vlt and the file's end are on 0x40 boundaries, zero-padded. All 751
  files of the three builds fit it. **V** `gpcore.bin` holds the gameplay
  core; the others hold one event each.
- **`Sign`.** An event vault's vlt has a `Sign` chunk before `EndC`: 8 bytes,
  then 128 per dependency (two: the `.vlt` and the `.bin`). The game decrypts
  each to a 16-byte hash and counts the bytes that match a hash of the vlt up to
  `Sign`, and of each dependency's data (`nfsw.exe` 0x6CA140, 0x6C7490). A vault
  without `Sign` counts as matching in full, as the VPAKs' vaults do. Where the
  game acts on the count wasn't found. The writer keeps the chunk
  (`RawVault::sign`) while the bin and the vlt before it are as read, and
  leaves it out of an edited vault, which it no longer signs. The community
  Attribulator writes every event vault without one. **V** (rebuild) **I**
  (what the game does with a vault whose signature doesn't match)
- **`VaultData`.** Every event vault's last export is `VaultData`, of type
  `VaultDataType` (both names are strings in `nfsw.exe`): 4 bytes, 0 in 745
  files and 0xFFFFFFFF in the 6 copies of `gpcore.bin`. It reads as the vault's
  slot, `GameCore::VaultSlot` (the 2010 beta's bindings: -1 `kVaultSlot_Core`,
  0 `kVaultSlot_Event`), and the community Attribulator calls it
  `VaultSlotExport`. The model keeps it as read (`Vault::dataExports`), and a
  new collection goes before it. Its export id is `StringHash32("VaultData")`
  in every file. **V**
- **A new event vault** (`CreateEventVaultFile`, Edit.h) is a file of its own
  in `gc.vaults` holding one vault with the header fields of the database's
  others, the `DepN` of any new vault, no `Sign`, and `VaultData` 0. The
  folder's name sits beside `InitGameplayStage1` in `nfsw.exe`'s boot stages,
  so the game reads the folder as it starts, and `gpcore.bin` names no event
  vault. **I** (that the game takes a file it hasn't seen; not tried in game)
- **`mLayoutCount`** isn't Undercover's rule in 49 of the final build's 113
  classes: `caraudio` lays out 32 fields and stores 61, `emitterdata` lays out
  44 and stores 9, and 33 classes have a key shift. They size the class's layout
  table, which grows as needed, so World's tool sized its tables larger. The
  writer writes them as stored, and a class laid out again (`doc/classes.md`)
  keeps them. **V**
- **A stale layout slot.** One collection of `gpcore.bin` has `0x00120000` in
  its layout pointer's slot, which the fixup overwrites. The writer keeps it
  (`Collection::layoutSlotStored`). **V**
- **Fields a class lacks.** The `coop_pursuit_1` event sets four fields
  (`CourseId`, `EventID`, `MaxTier`, `MinTier`) that World's `gameplay` class
  no longer has. The reader keeps them, typed by the collection's type list,
  and writes them back as read; scripts leave them out with a comment. **V**
- **Static index tables** are in World's type table, 16 bytes, and follow the
  collections of `aud_moment`, `traffic_engine` and `traffic_horn` as in
  Undercover. **V**

### PtrN

The chunk is a list of `{u32 fixupOffset; u16 type; u16 index; u32 value}`
entries, in this order:

```
{0, 2, 1, 0}                       fixups located in the bin follow
{loc, 3, 1, target} ...
{0, 2, 0, 0}                       fixups located in the vlt follow
{loc, 3, 1, target} ...
{0, 0, 0, 0}                       terminator, then zero padding to 16
```

- Type 2 selects the block that the following fixup offsets are in.
- Type 3 stores `blocks[index] + value` at that location. The index is always 1,
  so every pointer targets the bin. Pointer fields hold 0 in the file.
- Type 1 (Most Wanted's layout only, above) lists a pointer that stays null.
- The order within each group comes from the authoring tool and isn't
  derivable. It is kept as a hint (see "Ordering hints").

### bin

1. **`StrE` chunk.** Deduplicated NUL-terminated strings from +8, zero-padded
   to 16. The chunk size covers the whole chunk. Every string is referenced,
   including one empty string. The order is a hint.
2. **Data area.** Blocks are emitted per export, in export order. Each export
   is walked breadth-first: first its top-level blocks, then the targets of
   pointers inside placed blocks (each block's pointers by ascending offset).
   - A block is placed once, even if several pointers target it.
   - Pointers into `StrE` aren't followed.
   - Padding is zero, and nothing follows the last block. **V**

Top-level blocks:

| Export | Blocks |
| --- | --- |
| Database | typenames (each type name NUL-terminated, in type order) |
| Class | definitions (sorted by key), then static data |
| Collection | layout, then each out-of-line entry value, in entry order |

Block alignment:

| Block | Size | Alignment |
| --- | --- | --- |
| typenames | sum of (len + 1) | 1 (I: it always lands 16-aligned) |
| definitions | 16 × count | 8 **V** |
| static data | `mStaticSize` | 1 << max static-field alignment (I: only `sunandsky` has one) |
| layout | end of the last field, *not* `mLayoutSize` | 1 << max layout-field alignment **V** |
| entry value | type size | 1 << field alignment **V** |
| entry array | header + alloc × elemSize | max(2, 1 << field alignment) **V** |
| nested `float[]` (Curve, VariableArray) | length × 4 | 4 **V** |
| nested `RefSpec[]`, `MiniRefSpec[]`, `FEPartDetail` | count × size | 4 (I: 8 breaks the rebuild, 1 doesn't) |
| `Attrib::Blob` data | size | 1 (I: 1, 2 and 4 all reproduce) |

The layout block can end before `mLayoutSize`, so the next block may start
inside the class's nominal layout size.

## Load data

```
DatabaseLoadData (0x10) + u32 typeSize[mNumTypes]
  u32 mNumClasses (214); u32 mDefaultDataSize (0x558); u32 mNumTypes; char* mTypenames
ClassLoadData (0x20)
  u32 mClass; u32 mCollectionReserve; u32 mNumDefinitions; Definition* mDefinitions;
  u32 mStaticSize; void* mStaticData; u32 mLayoutSize; u16 mLayoutKeyShift; u16 mLayoutCount
Definition (0x10)
  u32 mKey; u32 mType (StringHash32 of the type name, not an index); u16 mOffset; u16 mSize;
  u16 mMaxCount; u8 mFlags; u8 mAlignment (log2)
  flags: 1 array, 2 in layout, 4 bound, 8 not searchable, 16 static, 32 has handler (Undercover on)
CollectionLoadData (0x20) + u32 typeHashes[mTypesLen] + AttribEntry[mNumEntries]
  u32 mKey; u32 mClass; u32 mParent; u32 mTableReserve; u32 mTableKeyShift; u32 mNumEntries;
  u16 mNumTypes; u16 mTypesLen (mNumTypes rounded up to even, zero padded); void* mLayout
AttribEntry (0xC)
  u32 mKey; u32 mData; u16 mType (index into typeHashes); u8 mNodeFlags; u8 mEntryFlags (0)
```

Derived fields (`doc/classes.md` has the rules a new class is laid out by and
what the games' code does with each):
- Definitions are sorted by key (the game binary-searches them). **V**
- `mLayoutCount` and `mLayoutKeyShift` size the class's layout table (the
  searchable layout fields, found by key): the count of layout fields without
  the not-searchable flag and 0, up to Undercover; World's are larger. The
  table grows as needed. **V**
- `mTableReserve` equals the entry count. **V**
- `mLayoutSize = align(end of the last layout field, 1 << max alignment)`, left
  unrounded in some PS2 and GameCube classes. **V**
- `mCollectionReserve` is about 1.4 × the class's collections across the
  game's packs, the starting size of its collection table, which grows. **V**
  (the code) **I** (the factor); it is preserved.
- `mNumClasses` is the starting size of the database's class table, about
  1.5 × (classes + 2); it grows too. **V** `mDefaultDataSize` is the largest
  layout or type size, and the game doesn't use it: its default data area is a
  buffer of a size compiled into it. **V** Both are preserved, and grow when a
  class is added.
- Every field has the bound flag, which no code reads. Alignment is the type's
  natural one, and HasHandler goes with the types the game gives a handler
  (`NaturalAlignment`, `TypeHasHandler` in `ClassDesign.h`). **V** on every
  retail definition.

## Values

**Entries.** Node flags are 0x00 (pointer to the value), 0x02 (pointer to an
`Attrib::Array`) or 0x40 (the value is the 4-byte `mData` itself). 0x40 is
set exactly when the entry isn't an array and the type is at most 4 bytes.
An inline `Text` is a `char*`, so it carries a vlt-located fixup. **V**

**Arrays.** The header is `{u16 mAlloc; u16 mCount; u16 mSize; u16 mEncodedTypePad}`.
Elements start at +8, or at +16 when `mEncodedTypePad & 0x8000`, which the
`Vector4` arrays use. In retail `mAlloc == mCount` (every entry array of
every folder in `attrib/databases`, the re-saves included), and resizing an
entry array keeps it so (`InsertElement`, `RemoveElement`). NFS-VltEd keeps
`mAlloc` when an array it grew shrinks back, so its files can hold spare slots
no retail file has. They do the game no harm and nothing needs them; the
re-saves in `attrib/databases` have none. Earlier builds of the editor left
`mAlloc` above the count the same way. The writer writes the spare slots
zeroed, so a zeroed tail reads as exactly writable, and only one holding data
is a warning. An array the editor resizes gets `mAlloc` back at its count. No entry array's
count goes past its definition's `mMaxCount` (Pro Street PC and PS2, Carbon
PC, the 360 demo, Undercover Wii). The game reads an entry array by its own
header, so the editor lets scripts, and the inspector with Edit > Allow Arrays
Past Their Limit, write more and leaves the definition as it is; what the game
code does with the extra elements is untested. **I**

**Layout fields.** These have the in-layout flag. They live in the collection's
own layout block at their definition offset and are never inherited. An
array field (flags `0x07`) holds an inline `Attrib::Array` header at its offset,
followed by `mAlloc` element slots:
- The capacity comes from that header's `mAlloc`. The definition's `mMaxCount`
  can be 0, as in `collisionsfx_core`.
- The header is 16 bytes for 16-byte-aligned element types.

**Inheritance.** A non-layout field that a collection doesn't set is
looked up through `mParent`, within the same class.

### Pointer-bearing types

These are offsets inside one element (`src/core/TypeLayouts.cpp`). Every bin
fixup in the Pro Street PC packs (`ATTRIBUTES.BIN`, `cars_vault.BIN`,
`gameplay.bin`, `FE_ATTRIB.BIN`) is one of these. The type schema
(`data/schema/prostreet_pc.txt`) derives the same layouts from the headers;
`tests/test_schema.cpp` checks they agree. Each entry applies only to a type of
the PC's size; another size takes its layout from the schema.

| Type | Pointers |
| --- | --- |
| `EA::Reflection::Text` | `char*` @0 |
| `Attrib::StringKey` | `{u32 hash; char*}`, string @4. Most Wanted's (Carbon's demos) is 16 bytes, `{u64 StringHash64; u32 StringHash32; char*}`, string @12; the editor sets both hashes when the text changes (`StringKeyAt` in `Values.h`). |
| `ControllerDataRecord` | `char*` @4 |
| `CCarKitSlotEntry`, `TargetTimeOverrides` | `char*` @12 |
| `SlotStats`, `FEPartCamera` (`FE_ATTRIB.BIN`) | `char*` @0 |
| `CCarSlotEntry` | `RefSpec[u32 @4]` @0, `char*` @8 |
| `Curve` | `float[u32 @20]` @16, `float[u32 @28]` @24, `float[u32 @36]` @32 |
| `VariableArray` | `float[u32 @4]` @0 |
| `DynamicSizeArray` | `RefSpec[u32 @4]` @0 |
| `FEPartData` | `MiniRefSpec[u8 @28/29/30]` @32/36/40, `FEPartDetail` @48, `char*` @52 |
| `Attrib::Blob` | `{u32 size; u8* data}`, data @4 |
| Undercover's index table (16 bytes, no type table lists it) | `u32[u32 @0]` @4, `ISet[u32 @0]` @8 (8 bytes each), `u32[sum of the ISets' counts]` @12 |

`FEPartData`'s three `MiniRefSpec` pointers may target one shared block, so
the model keeps nested blocks shared.

On PS2 its four bools (`drift`, `drag`, `grip`, `speed`) take 4 bytes each, which
moves everything after them by 12: the counts are at 40/41/42, the arrays at
44/48/52, `FEPartDetail` at 60 and the `char*` at 64, in 68 bytes. **V** Read
with the PC's offsets, the arrays look like strings and three pointers per
element are left over.

### Other value types

| Type | Notes |
| --- | --- |
| `Attrib::RefSpec` | `{u32 classKey; u32 collectionKey; Collection* (0 on disk)}`. A class with collection key 0 means "none of this class" (e.g. `EffectLinkageRecord.mEffect` with no effect). |
| `Attrib::Types::Colour` | 4 bytes A, R, G, B. **I**: inferred from `frontend/LevelColours`, which reads as a green-to-lime gradient in that order. |
| `Attrib::Gen::ClassRefSpec_<class>` | Undercover's 8-byte reference to a collection of one class: `{u32 collectionKey; Collection* (0 on disk, not in PtrN)}`. The class is the type's: `StringHash32` of `<class>`, the template argument in the PDB. 175 such types; five are used. |
| `Curve` | Undercover's (and its Wii version's) is 48 bytes: a float `graphScale` at 16, then the arrays at 20, 28 and 36 and `allocatedMemory` at 44 (`CurveAt` in `Curve.h`). X and Y are normalized to 0..1 against `xMin..xMax` / `yMin..yMax`. Y2 holds the precomputed natural-spline second derivatives, which the game never recomputes. After editing X or Y, regenerate Y2 from the normalized arrays. 3621 of 3622 retail curves match their Y2; `0xE3C144F9/close` is stale. See `tools/attrib/curve.py`. |
| `StringKey` | Retail song names had non-ASCII letters replaced with `?` (`D?N?`), but their hashes come from the original text. Keep a loaded hash unless the user edits the string. |

## Lua

- **Storage.** Compiled gameplay Lua is in class `gameplay`, field `bytecode`
  (`Attrib::Blob`). The blob data is a whole LZHeader block (HUFF or JDLZ), and
  the blob size is its compressed length.
- **Carbon's short sizes.** In retail Carbon (every platform), 244 of 312 blobs
  store a size short of their block (`0x34` for a `0x9E`-byte HUFF block); its
  demos and Undercover's Wii files don't. The game decompresses by the
  LZHeader, so the reader takes a blob to the end of the block it starts with
  when that is further (`NestedField::toLzEnd`), and the size stays as stored
  until the blob is changed. **V**
- **JDLZ's last byte.** The JDLZ encoder reads past its input. The editor
  passes it zeros, so a few retail blocks (2 in Carbon, 4 in Undercover's Wii
  files) differ in their last byte from what it writes. Those export to scripts
  as `bytes`; an unchanged blob keeps its stored bytes.
- **Keys.** The collection key is `StringHash32(<handler path>)`. The Lua
  function name is `h_` plus that hash in hex.
- **Count.** There are 251 blobs. An empty one has size 0 and no pointer
  (`gpcore/messagehandler`).
- **Versions.** MW, Carbon and Pro Street use Lua 5.0 bytecode; Undercover and
  World use 5.1. All are little-endian with a 4-byte float `lua_Number`.
- `doc/lua.md` covers the chunks, the decompiler and the compiler;
  `doc/gameplay.md` covers the handlers, states and the rest of the gameplay class.

## Ordering hints

These orders come from the authoring tool's containers and can't be derived
from the data. The model keeps them so unmodified files rebuild exactly:
- **`PtrN` order:** the original position of every pointer
  (`PtrSlot::seq`, `Attribute::dataSeq`, ...). New pointers follow within their
  group, sorted by location.
- **`StrE` order:** strings as loaded. New strings are appended in walk order.
- **Export order:** collections are grouped by class, and a parent precedes its
  children within a vault. In `db`, each class export is followed by its
  collections. A new collection goes after the last one of its class in the vault.
- **Entry order:** kept, and new entries are appended. It isn't hash-slot order.
- **Collection type list:** the derived rule is layout field types by
  offset, then entry types in entry order, each distinct. 139 collections carry
  extra unused types, so the stored list is kept and missing types are appended.
- **`mTableKeyShift`:** kept while the entry keys are as loaded. Otherwise the
  writer picks the shift with the shortest worst probe in the game's table:
  slot `rotl(key, shift & 31) % mTableReserve`, linear probing. The game
  rebuilds a larger table when a probe exceeds 16, so any shift loads.
