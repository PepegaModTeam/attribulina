# Mod scripts

A mod script (`.amod`) is a text file that lists changes to an AttribSys database: values to set, collections to make, move, rename or delete. The editor makes one from the changes you made, or from collections, classes and fields you pick. The editor or `attribcli` applies it to another copy of the game's files. The code is in `src/core/Script.h` (parser in `ScriptParse.cpp`, applying in `ScriptApply.cpp`, making scripts in `ScriptGen.cpp`).

```
attribulina 1
game prostreet_pc
title "Louder street horn"
author "someone"

option horn integer "Horn type" default 3 min 0 max 7

edit pvehicle/street {
    HornType = $horn was 5
}
collection pvehicle/street_tuned under pvehicle/street in vault db {
    aivehicle = aivehicle/street
}
```

## Why it is a list of changes and not a program

Every statement names the thing it changes and what it becomes. That choice buys several things a scripting language wouldn't give:

- Applying a script twice gives the same database as applying it once. The exceptions are `+=` and its siblings, which add to whatever is there.
- The editor can write a script from the difference between two databases, and the test suite checks that applying that script to the first one leaves no difference to the second.
- A reader sees what a mod changes without running it, and two mods can be compared line by line.
- The whole script is checked before anything changes: the editor applies it to a copy of the database first.
- A later in-game patcher can apply the same statements while a vault loads (see the last section). Arbitrary code couldn't be applied there.

Configurators still get what they need: options the user sets before applying, used as values and in `if` blocks.

## Files and names

Scripts are UTF-8 text. A statement ends at the end of the line or at `;`, and `#` starts a comment. The first statement is `attribulina 1`, the format version. A script without it still applies, with a warning.

- A collection is `class/collection`: `pvehicle/street`. Gameplay collections are named by paths and keep their slashes: `gameplay/prostreet_career/bins/race_bin_an_2_1/gr_1`.
- A name with spaces or other odd characters goes in quotes: `frontend/"$1 CAYMAN"`.
- An unnamed key is its hash, `0x1234ABCD`. A name that only looks like a hash is quoted: `"0x1234ABCD"`.
- Words are made of anything except spaces and `" # { } [ ] ( ) , ; = / $`. So `911gt3`, `race_bin-1` and `1.5e-3` are single words, and arithmetic needs spaces: `$power * 2`, not `$power*2`.
- Names a script gives to new collections and keys go into the editor's dictionary and into `user_keys.txt`. Packs only store hashes, so this is how the names show up the next time.

## Statements

| Statement | What it does |
| --- | --- |
| `attribulina 1` | The format version. |
| `game prostreet_pc` | The game and platform the script was made for (see below), or `prostreet_all` for every platform. Another game or platform gives a warning; the values still apply, typed by the fields they go into. |
| `title`, `author`, `version`, `description`, `url` | Text the editor shows before applying. |
| `option ...` | A setting asked for when the script is applied (see Options). |
| `edit C { ... }` | Changes values of `C`, which must exist. `*` and `?` in the collection name edit every match: `edit pvehicle/*_track { ... }`. |
| `create C [from S] [under P] [in vault V] [{ ... }]` | Makes `C`, which must not exist. `from` copies the own values of `S`. The parent is `P`, else `S`'s parent, else none; `under root` says none. The vault defaults to the parent's, else the copied one's. |
| `collection C [under P] [in vault V] [exact] [{ ... }]` | Makes `C` if it's missing, or else moves it under `P` and into `V` when it isn't there already. Then the block sets its values. With `exact`, own values the block doesn't set are reverted, so `C` ends up holding exactly what the script lists. |
| `delete C [with children]` | Deletes `C`. A collection with others under it needs `with children`. Deleting one that is gone already is a note, not an error. |
| `rename C to NAME` | Renames `C`. The collections under it follow. |
| `move C under P` | Puts `C` under another parent (`under root` for none), which changes what it inherits. |
| `move C to vault V` | Saves `C` in another vault. |
| `create vault V in FILE` | A new, empty vault in an open pack such as `gameplay.lzc`. |
| `delete vault V` | Removes an empty vault. |
| `class C [in vault V] { fields }` | Makes class `C` if it's missing, or else lays it out again to these fields, its collections' values following (see Classes). A class of the game stays as it is: the statement applies only when it describes it as it is. |
| `delete class C` | Deletes class `C` and every collection of it. Deleting one that is gone already is a note, not an error. |
| `if ... { } else { }` | Runs one of the blocks, by option values. Works at the top and inside blocks. |
| `echo value ...` | Prints its values, joined by spaces, to the Log (attribcli: standard output). `$option` prints the option's value. |
| `echo warning value ...` | The same as a warning. |
| `echo error value ...` | Stops the script with that text, so a script can refuse a setup it doesn't fit: `if $tier == "pro" { echo error "pro needs the 1.1 patch" }`. |
| `include "parts/kits.amod"` | Applies another script at this point. A relative path starts from the folder of the script with the line (see Including scripts). |

Inside an `edit`, `create` or `collection` block:

| Statement | What it does |
| --- | --- |
| `FIELD = value` | Sets the field. A value the collection inherits becomes its own. |
| `FIELD[2].member[0] = value` | Sets part of a value: array elements, struct members, and members of what a pointer leads to (`partData[0].autosculptCamera1[1].x`). |
| `FIELD += value` | Also `-=`, `*=` and `/=`, on numbers. Results for integer fields are rounded. |
| `FIELD = value was old` | Warns when the field wasn't `old` before. The editor writes `was` when it makes a script from your changes, so a script applied to a different version of the game says where it doesn't fit. |
| `revert FIELD` | Removes the collection's own value, so it inherits again. |
| `resize FIELD 4` | Sets an array's length. New elements are zero. An array can go past the count its field's definition allows (GEAR_RATIO's 9), with a warning; an array in the layout can't go past the space the layout has for it. |
| `insert FIELD[0] = value` | Inserts an element (zero without `= value`). |
| `remove FIELD[3]` | Removes an element. |
| `echo value ...` | As at the top, and a word that names one of the collection's fields prints its value: `echo "horn was" HornType`. |

## Classes

A `class` statement lists its fields, one per line: where the value is kept, the field's name, and its type, a type of the game's type table by its full name or by the short name the editor shows when only one type has it (`Float`, `RefSpec`, `EA::Reflection::Float`, `Attrib::Types::Vector4`).

```
class mod_racer {
    layout SPEED Float
    layout TARGET RefSpec
    layout WEIGHTS Float[4]        # four slots in every collection's layout
    layout DEBUG_ONLY Bool unsearchable
    entry COUNT Int32
    entry NAMES StringKey[]        # as many as a collection sets
    entry STAGES Float[8]          # at most 8
}
collection mod_racer/base under root {
    SPEED = 1.5
    NAMES = ["a", "b"]
}
```

- `layout` fields are in every collection, at a fixed place in its layout block, in the order listed. `entry` fields are optional: a collection sets them or inherits them. `static` fields hold one value for the class, `static NAME TYPE = value`; only the game's own classes have them.
- `[n]` after the type makes an array: a layout array's slots, an entry array's most elements (`[]`: no limit).
- `unsearchable` keeps a layout field out of the class's layout table, so the game finds it through its compiled layout only, not by key.
- The class goes in the database's vault (`db` in attributes.bin), where the game loads classes first. `in vault` names it; another vault is refused.
- For a class that exists, the statement is its new design: fields keep their values in every collection when they keep their name and type, wherever they are kept now, and the others' values go. A class of the game (one the game's own files define, `doc/classes.md`) can't be changed: a statement that describes it as it is applies, any other stops the script.
- Offsets, sizes and counts follow from the fields, by the rules the game's classes follow (`doc/classes.md`). A statement written from a game class keeps the few stored values those rules don't give, as words after the field or the class: `at 22`, `align 2`, `flags 0x0E`, `max 1`, `size 12`, `reserve 1273`, `layoutsize 96`, `layoutcount 61`, `keyshift 1`, `staticsize 1036`, and a last line `bytes "..."` holding the static block when bytes of it belong to no field (World's `gameplay`).

The editor writes a `class` statement for a class from its menu (Add to Script > Class Definition), and a diff or the change recorder writes one for every class made or changed.

## Values

Values are typed when they are applied, by the field they go into. `a/b` is a reference for a reference field and a division for a number.

| Kind | Written as |
| --- | --- |
| bool | `true`, `false` |
| integers | `12`, `-3`, `0x1F`. The field's size and sign are checked: `300` doesn't fit a `uint8`. |
| float, double | `0.5`, `1e-3`, `inf`. The editor writes the fewest digits that read back as the same value, so a script never changes a float by rounding. |
| enum | Its name, `ENGINE_REAR`, or a number. |
| key, bin key | A name is hashed (StringHash32, or bStringHash for `type_bStringHash`): `street`, `"$1 CAYMAN"`. `0x...` and `0` are values. A bare number like `7` is the name "7". |
| string key | `"text"`: the hash is the text's, and `""` is 0. `("text", 0x1234ABCD)` keeps a stored hash that isn't the text's (retail strings mangled to `?` have them). Most Wanted's 16-byte string key (Carbon's 360 files) also stores a 64-bit hash: `("text", 0x1234ABCD, 0x0123456789ABCDEF)`. `null` for none. |
| text | `"text"` or `null`. |
| reference | `pvehicle/street`, or `null`. |
| class reference | Undercover's references to one class (`Attrib::Gen::ClassRefSpec_<class>`): the collection's name, `wall_reverb_00`, or `0` for none. |
| vectors, colours | `(x, y, z, w)`. A colour's four bytes are given in memory order, `(a, r, g, b)`. |
| matrix | Four rows: `((1, 0, 0, 0), (0, 1, 0, 0), ...)`. |
| curve | `{ xMin = 0, xMax = 100, yMin = 0, yMax = 1, X = [0, 0.5, 1], Y = [0, 0.8, 1] }`, with X and Y normalised to 0..1 the way they are stored. Y2 is recomputed unless the curve gives `Y2 = [...]`. Undercover's curves have a `graphScale` too. |
| blob | `lua """source"""`: a handler's Lua, compiled with Lua 5.0 and packed the way the Sequence Builder does it. `bytes "0a 0b ..."` for other blobs, `null` for none. |
| struct | `{ member = value, ... }`. Members it doesn't list keep their values. |
| arrays | `[a, b, c]` sets the length and the elements. A struct's fixed-size array needs exactly its length. |
| anything | `bytes "..."`: the value's exact bytes. |

Strings take `\"`, `\\`, `\n`, `\t` and `\xHH`. `"""..."""` is a raw string that may span lines. A line of only spaces before the closing quotes isn't part of it, so the quotes can be indented.

Numbers can be worked out with `+ - * /` and parentheses: `MAX_RPM = $base * 1.1 + 200`.

## Options

Options are the base for configurators: settings the user picks before a script applies.

```
option power number "Torque scale" default 1.1 min 1 max 2 help "1 is stock"
option kits bool "Body kits" default true
option tier choice "Tier" choices ["street", "race", "pro"] default "race"
option frames integer "Frames" default 5
option tag text "Name suffix" default "_race"
```

The types are `bool`, `number`, `integer`, `choice` and `text`. In values, `$power` is the option's value. `if` tests them: `if $kits and not ($tier == "street") { ... } else if $power { ... } else { ... }`. The editor shows one control per option before it applies a script. `attribcli` takes `--set power=1.2`.

That is all options do for now. Groups, pages, pictures, presets and options that depend on each other are left for when nfsms has been looked at; they will be new words after `option`, so existing scripts keep working.

## Including scripts

`include` applies another script where the line is. A big mod can be split into files this way, and a configurator can pick parts with `if`:

```
attribulina 1
option kits bool "Body kits" default true
include "parts/engines.amod"
if $kits { include "parts/kits.amod" }
```

- A relative path starts from the folder of the script that has the `include`. So a script in `parts/` includes its neighbours by name, and a mod's folder can be moved as a whole. Full paths work too.
- Write `/` between folders, which Windows takes as well. `\` works too, doubled inside quotes: `"parts\\kits.amod"`. A path made of plain words needs no quotes: `include parts/kits.amod`.
- An included script is a whole script, with its own `attribulina 1` and `game` lines and includes of its own. Its `title`, `author` and other text lines are ignored; the editor shows the including script's.
- Options are shared. The included scripts' options are asked for together with the including script's, and every script can use any of them as `$id`. Declaring an id twice is an error, except when the same file is included twice: it's read once.
- Every file is read before anything changes, even one under an `if` that won't run, so a missing file stops the script before its first statement.
- A mistake in an included script stops at the `include` line and names the file and line it's in. The editor's Log reads `mod.amod:4: in parts/kits.amod:12: no class pvehicl`, and the Script panel marks line 4. `attribcli` prints `parts/kits.amod:12: error: no class pvehicl`. Warnings name them the same way.
- A script can't include itself, directly or through other scripts.
- `include` goes at the top of a script or inside an `if` there, not inside an `edit` block.
- Saving or adding to a script keeps its `include` lines as they are; the included scripts aren't copied in.

In the editor, the Script panel's script has a folder once it has been saved, so an unsaved one can include full paths only. The console line includes from the panel script's folder. The Apply prompt lists the scripts a script includes. `attribcli` takes relative paths in `-e` statements and in a script on standard input from the current folder, and `attribcli check` lists what a script includes.

## Applying

Statements apply in order, and an included script's where its `include` is. The first error stops the script with its file, line and column: `mod.amod:12:5: pvehicle has no field MAX_RPMM`.

- The editor applies a script to a copy of the database first, and only applies it for real when the copy took it without an error. A script that stops halfway changes nothing. Making the copy takes 0.4 seconds for the four Pro Street packs in a Release build. `attribcli` saves nothing when a script stops.
- Warnings don't stop a script: a `was` that doesn't match, a reference that the script points at a collection that doesn't exist (checked at the end, so the script can make that collection further down), a script for another game or platform, an array made longer than its definition allows. `--strict` in `attribcli` and the checkbox in the editor make them stop it instead.
- A value that a statement leaves as it was keeps its exact bytes and pointer order. An unchanged pack still saves byte for byte; the tests write every collection of the four Pro Street packs as a script and apply it back without a single change.
- `ATTRIB_SCRIPT_TRACE=1` logs every value a script changes, to stderr.

## Making scripts

In the editor, the Script panel (View > Script) holds one script, with line numbers and colours. You can type in it, open and save `.amod` files, and Check it (a trial on a copy, with the options at their defaults) or Apply it. Errors and warnings mark their lines; hover the line number to read one, or click it in the list to go there. The console line under the script runs one change at once, `edit pvehicle/street { HornType = 3 }`, with Up and Down for earlier ones. These add to the script, merging values into an `edit` of the same collection when it has one:

- A collection's menu, Add to Script: This Collection, or With Everything Under It. That gives `collection` statements with the parent, vault and every own value.
- A class's menu, Add to Script: Every Collection, or Chosen Fields... (only those fields; or `exact`, or `edit` blocks for collections that must exist already).
- A field's menu in the collection panel: Add to Script, for that one value.
- File > Export Changes as Script...: everything that changed since the files were saved, or since the originals (the `.bak` files the first save keeps), with `was` for the old values.

File > Apply Script... applies a file without opening it in the panel.

A diff between two databases compares only packs that both have open. It gives, in this order:

1. new vaults, then classes made or changed,
2. renames: a deleted and a new collection with the same values, vault and parent,
3. new collections, as `collection` statements with all their values, layout fields included, parents first,
4. moves to another parent or vault,
5. changed values, as the smallest part that changed (`ENGINE.TORQUE[2] = ...`) with `was`. A changed array length sets the whole array.
6. deletions, children first, then deleted classes (their collections go with them), then removed vaults.

## Games and platforms

Every database is one game on one platform, and the `game` line names it as `<game>_<platform>`. Scripts the editor and `attribcli` make get the open database's.

| Game | Id | Platforms | Edited on |
| --- | --- | --- | --- |
| Most Wanted | `mw` | `pc`, `ps2`, `xbox`, `x360`, `gamecube` | all (`x360` and `ps2` from betas; the early `ps2` and `xbox` demos too) |
| Carbon | `carbon` | `pc`, `ps2`, `ps3`, `xbox`, `x360`, `gamecube`, `wii` | `pc`, `ps2`, `ps3`, `x360` (from a late demo), `gamecube`, `wii` |
| Pro Street | `prostreet` | `pc`, `ps2`, `ps3`, `x360`, `wii` | all |
| Undercover | `undercover` | `pc`, `ps2`, `ps3`, `x360`, `wii` | `pc`, `x360`, `ps2`, `wii` (the last two on Carbon's engine) |
| World | `world` | `pc` | none yet |
| Nitro | `nitro` | `wii` | none yet |

So `game prostreet_pc`, `game prostreet_x360`, `game prostreet_ps2` and `game prostreet_wii`. Each platform has its own id even where the types are the PC's (Pro Street's 360 and Wii files, Carbon's PS3 and Wii files). Attribulina edits Most Wanted, Pro Street, Carbon and Undercover so far (`undercover_pc`, `undercover_x360`, `undercover_wii`, `undercover_ps2`; not its PS3 files, which haven't been seen). The other games and platforms are listed so files and scripts can say what they are. The table is `src/core/Game.cpp`.

Most Wanted's early demos key everything with a 64-bit hash, and so do the references and `GCollectionKey`s in their values. A script sets them by name as anywhere else (`simsurface/organic`); a name the dictionary doesn't know is learned and written as its 64-bit hash, and a bare `0x1234ABCD` can't stand for one, so such a line fails. A key with no known name is written in the demos' scripts as `"0x"` and 16 hex digits, quoted, which reads back as the same key.

A mod made for every platform says `game prostreet_all` (any game's id with `_all`). It applies to all of that game's databases without a warning. That works because values are typed by the field they go into when the script applies: `true` fills a PS2 bool's four bytes, and a big-endian file gets its numbers the right way round. The exception is `bytes "..."`, which is copied as written, so a script for every platform warns when it sets one wider than a byte. The editor and `attribcli` still write the open database's id into the scripts they make; change the line by hand to share one.

The files say which they are: every vault starts with a `Vers` value that differs between games and between versions of their data, and byte order tells apart most platforms that share one (Pro Street's PC and 360 files, its PS2 and Wii files, Carbon's PS2 and GameCube files). Undercover's PS2 files haven't been seen; the editor takes the Wii's value in little-endian files to be theirs. Carbon's Wii and PS3 files share a value and byte order, and `attributes.bin`'s classes tell them apart (only the PS3's has the picture-in-picture ones). The editor reads all this when it opens a database and asks only when the value isn't one it knows, or the files can't settle it (a lone pack without `attributes.bin`). Most Wanted's vaults have none, so its files always ask. File > Detect Game and Platform turns detection off, and then the editor asks each time, with what the files say chosen. `attribcli --target prostreet_ps2` names it for files that don't say, or reads them as another.

## attribcli

`attribcli` is a console program next to `Attribulina.exe` (premake builds it on Windows and Linux). It reads `data/` beside itself, like the editor, and a database is a game folder (with `attributes.bin`) or one pack file in it. `--target <game>_<platform>` reads it as that game and platform (see above).

```
attribcli apply GLOBAL mod.amod --set power=1.2       # changes GLOBAL, keeping <file>.bak originals
attribcli apply GLOBAL mod.amod --out build/GLOBAL    # writes the changed packs there instead
attribcli apply GLOBAL -e "edit pvehicle/street { HornType = 3 }"
attribcli apply GLOBAL - < mod.amod                   # a script on standard input
attribcli apply GLOBAL a.amod b.amod --dry-run        # applies in memory, saves nothing
attribcli check mod.amod [GLOBAL]                     # reads it, lists its options; with a database, tries it
attribcli diff vanilla/GLOBAL modded/GLOBAL -o mine.amod
attribcli export GLOBAL pvehicle/911gt3 --children -o 911.amod
attribcli export GLOBAL car --field isBoosterPack --edit
attribcli get GLOBAL pvehicle/street [HornType ...]   # values as script text; inherited ones are marked
attribcli list GLOBAL [pvehicle]                      # classes, or one class's collections as a tree
attribcli header GLOBAL mod_racer -o mod_racer.h      # the class's Attrib::Gen header (doc/classes.md)
attribcli info GLOBAL
```

The exit code is 0 when it worked, 1 when a script or a file failed, and 2 for a mistake in the command line. Saving follows the editor's rules: a `.lzc` is written together with its `.bin` twin, and the first original of each file is kept as `.bak`.

## No shell

`attribcli` runs `.amod` files, so there is no separate interactive shell. For one change at a time, the console line in the Script panel applies a statement to the open database as soon as you press Enter.

## nfsms

Scripts from NFS-VltEd and Attribulator run in their own window and from `attribcli`; `doc/nfsms.md` describes that support and how each command maps onto this editor. To Mod Script in that window, or `attribcli convert`, turns one into a mod script, with its setup as options.

## In-game patches later

The longer-term plan is to patch the database in the running game, through hooked code, instead of rewriting files. Each statement here comes down to a class, a collection, and bytes at a field (or a new collection with its values), which is what a patcher needs as vaults load. Values would have to be resolved to bytes ahead of time for the game's platform; a script that only uses `edit` and `collection` is the easiest case. New vaults, deletes and moves change what the game's loader expects and would need more care.
