# Classes

An AttribSys class is a list of field definitions; its collections hold the
values. This page covers making, changing, deleting and restoring classes in
Attribulina, the `Attrib::Gen` headers it writes for them, and what the
games' code does with a class, which decides the rules. The code is in
`src/core/ClassDesign.h` (designs, layout, `ClassEditor`), `src/core/ClassList.h`
(the game's classes), `src/core/GenHeader.h` (headers) and `src/app/AppClass.cpp`
(the Class Designer). `doc/vault-format.md` has the `ClassLoadData` and
`Definition` layouts.

## Fields

Each field has a name, a type from the database's type table, and a place:

| Kept in | What it is |
| --- | --- |
| layout | Every collection holds its own value at a fixed offset in its layout block. The game's compiled `_LayoutStruct` reads it there; it is never inherited. |
| entry | Optional: a collection sets it or inherits it from its parent, and the game finds it by key. |
| static | One value for the whole class. Only the game's own classes have them (below). |

An array in the layout has a fixed number of slots: an 8-byte `Attrib::Array`
header (16 for 16-byte-aligned elements) and that many elements. An entry array
grows as needed, up to its definition's count (0 or 65535: no limit).

A layout field is *searchable* unless it has the NotSearchable flag: the
class's layout table lists it, so `Instance::Get`, `TAttrib` and the Lua
bindings find it by key. A field that isn't is read through `_LayoutStruct`
only.

## How a design is laid out

`LayOutClass` follows the rules every retail class follows (checked on all
2561 classes of the 27 supported folders, 49078 fields; `tests/test_class.cpp`):

- Layout fields in the design's order, each at its type's natural alignment;
  an array's header goes on the element's alignment. Static fields the same way
  in the static block. Entries have offset 0.
- Alignment comes from the type alone (`NaturalAlignment`): the schema's for a
  struct, 16 for Vector4, Quaternion, Matrix and FloatColour, 8 for 64-bit
  numbers, 1 for Colour, else the size's, up to 4. Every retail definition has
  it.
- Flags: bound (4) on every field, as in every retail one; array, in layout,
  static and not searchable as designed; HasHandler (32) in Undercover (PC,
  360, PS3) and World for RefSpec, ClassRefSpec, Curve, the structs that hold
  them, and three structs listed by name, again as every retail definition has
  it.
- `mSize` is the type's size, `mMaxCount` 1 for a non-array, the slots or the
  limit for an array.
- `mLayoutSize` is the end of the last layout field rounded up to the largest
  layout alignment; `mLayoutCount` the searchable layout fields; the key shift
  0; `mStaticSize` the static block's end rounded the same way.
- Definitions are written sorted by key (the game binary-searches them).
- `mCollectionReserve` starts at max(1, 1.4 × collections).

What the rules don't give is kept as a stored value in the design: 28 offsets
(Most Wanted's packed GameCube and PS2 bools, layout arrays whose capacity only
their collections give), World's layout counts, key shifts and static sizes,
the PS2's and GameCube's unrounded layout sizes, and every class's collection
reserve. So a retail class laid out again from its design is the class as
stored, and a class the user makes is laid out the way the tool that made the
game's classes would have.

## Making and changing a class

The Class Designer (View > Class Designer, Edit > New Class..., or a class's
menu) edits a design: fields with where each is kept, name, type, array
count and searchable, and shows the offsets and sizes the rules give, the
problems that stop it and warnings. Create Class makes it; Apply Changes lays
a class of the user's out again.

- A new class goes in the database's vault (`db` in attributes.bin), where
  every retail class is: the game loads it before anything else, and skips a
  collection whose class isn't loaded yet.
- It can't have static fields. The game copies static values into a table
  compiled into it (`ClassStaticDesc`), and a class it doesn't know there gets
  none: Undercover writes through a null pointer while loading such a class.
- Its types are the type table's. A type the table doesn't list is 0 bytes to
  the game, and handlers (constructors, destructors) are compiled in, so a new
  type couldn't have one anyway.
- A change carries the collections' values over: a field that keeps its key
  and type keeps each collection's value wherever it is kept now (an entry
  moved into the layout takes the value the collection had or inherited, a
  layout field moved out becomes the collection's own entry), a layout array
  keeps the elements that fit, and values of fields that went or changed type
  are dropped.
- Renaming a class changes its key and its collections' export ids.
  References to its collections from other classes keep the old class key.
- The database's `mNumClasses` and `mDefaultDataSize` grow to cover new
  classes, as retail files have them, though the game reads neither as a limit.
- The designer warns when a layout or a field type is larger than the game's
  default data area (below).

Scripts make and change classes with a `class` statement (`doc/scripts.md`),
and a diff or the change recorder writes one for every class made or
changed, so a mod with a class of its own is one `.amod` file.

## The game's classes

`data/classes/<game>_<platform>[_<build>].txt` lists the classes each
supported game folder defines, as `class` statements with their stored values
and static values (`WriteClassList`; `ATTRIB_WRITE_CLASS_LISTS=1 attribtests
classes:` writes them again from `attrib/databases`). The editor and attribcli
load them into every database. Carbon's PS3 late beta has the same classes
as its retail files and shares their list.

- **A class of the game can't be redefined or renamed**, only deleted and
  restored. Its compiled code reads its layout as it is and finds it by name.
  A class counts as the game's when any list of the database's game has it,
  on any platform, so a platform with no files here (Pro Street on PS3) is
  covered. The designer shows it read only, and Copy as New Class makes a
  design of the user's from it (without static fields).
- **Restoring** (Edit > Restore Game Class, the Classes tree's menu) applies
  its statement from the list that fits the database: the one made from files
  with the same `Vers` value (the `version` line; `64-bit keys` for Most
  Wanted's early demos), else the one with the most classes in common. The
  class comes back as the retail files define it, without its collections.
  Every retail class of every list restores exactly this way.
- **Deleting one** is allowed, with a warning: `FindCollection` returns null
  for its collections, Gen getters return zeros, `Modify` and
  `GenerateUniqueKey` on it crash, and game code that expects its collections
  may crash too.

## Attrib::Gen headers

A class's menu, Attrib::Gen Header..., the designer's header tab and
`attribcli header <database> <class>` write C++ for reading its collections
the way the game's own generated classes do, so code that has the game's
`Attrib::Instance` (an SDK made from the debug headers) uses a class the editor
made with no other glue:

- `class X : private Attrib::Instance` (Most Wanted: `struct X : Instance`)
  with `kClassKey` and a `kKey_<field>` per field (`StringHash32` of the names).
- `_LayoutStruct` with every layout field at its offset, a layout array as
  `Attrib::Private _Array_X` followed by its slots, padding where the offsets
  need it, and `static_assert`s on the size and every offset.
- The constructors, `Change`, `ChangeWithDefault`, `Modify`,
  `GenerateUniqueKey`, `operator=` and `operator==` the game's classes have,
  then per field, in name order as the game has them: `const T& X()` (with an
  index for arrays), `bool X(T&)`, `bool X(TAttrib<T>&)`, `Num_X()` for
  arrays, and `Set_X`, which works on a dynamic Instance only (after
  `Modify`), as in the game.
- Types as the game's code spells them (`UMath::Vector4`, `const char*`, an
  unsigned int for a Colour). The game's structs and enums the fields use can
  come from the type schema (the header's types option; each under its own
  guard), or from the SDK.
- A class that needs more default data than the game's buffer gets a zeroed
  area of its own.
- Static fields get a comment instead of accessors.

`scripts/check_gen_headers.sh <zig>` compiles the headers of every class of
the PC games, and one made in the tests, against `scripts/gen_headers/sdk_stub.h`
as 32-bit Windows code, so the `static_assert`s check every layout with the
game's sizes and alignments.

## What the games do with a class

Read in Undercover's 360 debug build (PDB names), Pro Street's 360 milestone,
Most Wanted's GameCube ELF and Carbon's PS3 ELF, with the PC executables
scanned for the same code. Addresses are Undercover 360's unless another
build is named.

**The generated classes.** `Attrib::Gen::X` has no members of its own (16
bytes, Most Wanted 20: its `Instance` starts with an `IUnknown* mOwner`).

- Constructors call the matching `Instance` constructor (from
  `FindCollection(ClassKey(), key)` for a key; `0x82606EC0`, Pro Street PC
  `0x402380`), then `SetDefaultLayout(sizeof(_LayoutStruct))` (`0x8252C918`):
  `if (!mLayoutPtr) mLayoutPtr = DefaultDataArea(size)`. A class with no layout
  fields skips it. Most Wanted's take a third argument, the owner.
- `Change(key)` is `Instance::Change(FindCollection(ClassKey(), key))`;
  `ChangeWithDefault(key)` falls back to the collection named `default`;
  `Modify(key, n)` is `ModifyInternal(ClassKey(), key, n)`;
  `GenerateUniqueKey(name)` is `GUKeyInternal(ClassKey(), name)` (Most
  Wanted's takes a `bool registerName`); `operator==` compares the collections.
- A layout getter reads `_LayoutStruct` through `GetLayoutPointer()`. An entry
  getter calls `GetAttributePointer(key, index)` (`0x825D8E00`), and when that
  is null reads `DefaultDataArea(sizeof(T))`. `Num_X()` is
  `Get(key).GetLength()` for an entry, the header's `mCount` for a layout
  array. `Set_X` on an entry is `GetOrClone<T>(key)` then `TAttrib::Set`; on a
  layout field it needs `IsDynamic()`.
- `Collection::GetData` (`0x825D8690`) looks a key up in the collection, then
  its parents, then, only for a collection with a layout, in the class's layout
  table, owned by the collection itself, which is why layout fields never
  inherit.

**`mNumClasses`** is the starting size of the database's class table
(`DatabasePrivate` constructor `0x825E0CE8`, Pro Street 360 `0x82365F08`). The
table grows by a quarter when it fills or a probe passes 16 slots. Nothing
asserts; a wrong value costs speed and memory only.

**`mDefaultDataSize`** is unused at runtime. `DefaultDataArea(bytes)`
(`0x82A0F668`) ignores its argument and returns one zeroed buffer of a size
compiled into each game: 2960 bytes in Undercover (360 PDB; the PC's by where
the next variable starts), 1368 in Pro Street, 2048 in Most Wanted and Carbon
(their ELF symbols), at most 2752 in World. Nothing checks a size against it:
an Instance with no collection, or a field no collection sets, reads past it
when the class needs more. Retail files hold the largest layout or type size.

**`mCollectionReserve`** is the starting size of the class's collection table
(`0x825E37D4`); it grows the same way. Speed and memory only.

**`mLayoutCount` and `mLayoutKeyShift`** size the class's layout table, an
`Attrib::HashMap` like a collection's entry table (`rotl(key, shift) % size`,
linear probing; ClassPrivate constructor `0x825E3750`). Every layout field
without NotSearchable is added to it. The table grows when full, so a wrong
count or shift only costs probe time or memory. New classes get the
searchable count and shift 0, as retail up to Undercover has; World's tool
sized its tables larger.

**Flags.** IsBound (4) is read nowhere (Undercover 360 and Pro Street 360
scanned whole; Most Wanted's `Definition::GetFlag` calls test 1, 2 and 8 only).
NotSearchable (8) only keeps a field out of the layout table. Static (16)
puts the value in the class's static data. HasHandler (32) adds node flag 8.

**The type table.** The database builds a `TypeDesc` per listed type at load,
keyed by `StringHash32` of its name, with its size and its handler from a
table compiled into the game (`TypeDesc::Lookup` `0x82A11048`: 33 types in
Undercover, RefSpec, six ClassRefSpec types, Curve and structs holding them).
An unlisted type is index 0, size 0: `Set_` through `TAttrib` copies nothing
and no handler runs.

**Load order.** A vault runs its exports' `Initialize` in export order. A class
export ignores which vault it is in and does nothing if the class is loaded
already (the first definition wins); a collection whose class isn't loaded is
skipped and never retried (`0x825DC840`). A class keeps a pointer to its
definitions in the vault's memory, so the vault must stay loaded while any
collection uses it. Retail keeps every class in `db`, loaded first and never
unloaded, and so does the editor.

**Static data** goes into the class's compiled `gStatics` through
`ClassStaticDesc::GetStatic(classKey)` (`0x82A0F6A8`): ten classes in
Undercover (ecar, traffic_horn, style_points, adaptivedifficulty,
traffic_engine, speech, aud_moment, sunandsky, visuallookeffect,
zone_visual_style), one in Pro Street (sunandsky). Any other class's static
pointer stays null, and loading a static array or a static field with a
handler writes through it.

**A class that is gone.** `FindCollection` returns null without asserting, Gen
constructors build an Instance with no collection, layout getters read the
default area, entry getters too, `Num_` returns 0. `Modify` and
`GenerateUniqueKey` dereference the missing class (`0x825E1724`,
`0x825D8DD0`) and crash, and every collection of the class, in any vault, is
skipped.
