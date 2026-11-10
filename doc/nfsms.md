# nfsms

nfsms (NFS ModScript) is the script format of NFS-VltEd 4.6 by nfsu360. The community Attribulator reads it too and adds a few commands. Attribulina runs these scripts for compatibility on every platform of the games it supports. NFS-VltEd itself edits only the PC's files, and its words for the parts of values are byte offsets in the PC's layout. Attribulina's tables also give the member of its own schema each word sets, and it finds that member by name, so a word sets the same member on a PS2 or GameCube, whose 4-byte bools move the members after them (see Properties and values). Only Nitro is refused: NFS-VltEd never read it. Attribulina's own format is the mod script (`doc/scripts.md`), which says more and checks more; nfsms gets its own window so the two don't mix.

What follows comes from the NFS-VltEd 4.6 decompilation (`NFS-VltEd/`: the command classes, the ModScript installer form and the type classes) and from Attribulator's source (github.com/ArchieGoldmill/Attribulator, `Attribulator.Plugins.ModScript` and `ImportModScriptWindow`). Three real mods were tried: a Pro Street AI and difficulty overhaul of about 60 scripts and 15,000 value lines (`tests/test_nfsms.cpp` runs it when the `nfsms test` folder is there), and the two under NFS-VltEd compatible below. Every command, the way each type's values are read and the checks NFS-VltEd's installer makes were also compared with the decompilation; what is still done differently is listed at the end of that section. The code is `src/core/Nfsms.h`.

## Lines

One command per line: the command, then its parameters separated by spaces. A parameter in double quotes may hold spaces; there are no escapes. `#` starts a comment (outside quotes). `ui_text` takes the rest of its line up to a `#` as its text. Names may be given as hashes, `0x1234ABCD`.

Lines are read the way NFS-VltEd reads them, which real mods rely on:

- The command needs no space after it: `ui_text#` is a blank line of text, and `script"Main\AI Curves.nfsms"` includes a file.
- A line that doesn't start with a known command is a comment. Mods leave notes (`Balance!`) and stray characters (`-`) between commands, so a misspelled command is skipped without a word too. The editor shows such lines in the comment colour.
- Paths are written with `\`.
- A file saved with a byte order mark reads as one without it, as in NFS-VltEd.

```
version 4.6
game PS
update_field pvehicle 911gt3 ENGINE[0] Torque 300
add_node pvehicle 911gt3 911gt3_race
```

Scripts run line by line, as NFS-VltEd runs them: a line that fails (an unknown command, a node that isn't there, a value that doesn't fit) is reported at its line, and the next line runs. `attribcli --strict` stops at the first one instead.

## Commands

| Command | Here |
| --- | --- |
| `version 4.6` | 4.6 and older (`4.5`, `4.6.0.0`) run without a word, as in NFS-VltEd. A newer or unreadable version gives a warning; NFS-VltEd refuses the script (below). |
| `game PS C MW ...` | The games the script is for: MW, C (Carbon), PS (Pro Street), UC, W (World), WB. Another game gives a warning. NFS-VltEd takes the codes only as written here and refuses a script that doesn't list the database's game. |
| `update_field class node field[i] [word...] value` | Sets a value. `field[^]` is the last element. The words lead into the value (see below). `update_collection` is the same command; NFS-VltEd writes lists with it. A field the node only inherits gets its own value here; NFS-VltEd fails the line, so scripts for it `add_field` first. NFS-VltEd takes one word at most (update_collection: a list's word, the index and the element's word), sets element 0 of an array given no index, and ignores an index on a field that isn't an array. |
| `add_node class [parent] node` | A new collection, in the parent's vault (or the first vault that has the class). Its layout fields start at zero, not as copies of the parent's. NFS-VltEd copies them from the class's `default` collection; NFS-VltEd compatible runs do too. |
| `copy_node class source [parent] node` | A copy of `source` with its values, in its vault. Without a parent the copy is a root collection. |
| `add_overwrite`, `copy_overwrite` | The same, deleting a collection of that name first (Attribulator). |
| `delete_node class node` | Deletes it and everything under it. |
| `delete_nodes class` | Deletes every collection of the class. |
| `rename_node class node newname` | Renames it. A new name can't hold white space. A hash (`0x168b2ddf`) is a key, as NFS-VltEd reads it: the collection gets that key, with the name the dictionary has for it. |
| `move_node class node [parent]` | Puts it under `parent`, or at the root. NFS-VltEd always takes the parent, so it can't move a node to the root. |
| `change_vault class node vault` | Saves it in another vault. For a gameplay node a vault that doesn't exist is made in the same pack, and a gameplay vault left empty is removed (as Attribulator does). NFS-VltEd moves gameplay nodes only, and refuses the names db, frontend, cars and commerce (in any case) and names with white space. |
| `add_field class node field [count]` | Gives the node its own value, zeroed (not a copy of the inherited one). An array gets `count` zero elements. Nothing happens when it has the field already (NFS-VltEd fails the line). NFS-VltEd compatible runs do as NFS-VltEd: see below. |
| `delete_field class node field` | Removes its own value so it inherits again. Layout fields can't be removed. On a node without its own value it does nothing; NFS-VltEd fails the line. |
| `resize_field class node field size` | An array's length. |
| `resize_collection class node field[i] property... size` | The length of an array inside a value (a pointer array member). |
| `append_array class node field [value]` | Adds an element at the end (Attribulator). NFS-VltEd doesn't know Attribulator's commands: the line is a comment there. |
| `copy_fields class from to base\|optional\|overwrite` | As NFS-VltEd: `base` copies the layout fields; of the source's own other values (never inherited ones), `overwrite` replaces those the target has and `optional` adds those it lacks, with their lengths. Without one of the three the line fails. The target's layout `CollectionName` keeps its own name. |
| `convert_field` | Not run, with a warning; NFS-VltEd fails the line ("The command is not supported"). |
| `script file` | Runs another script, relative to this one's folder. Scripts are written on Windows, where file names match in any case, so a name that isn't there as written finds the file whose name differs only in case (Reformed's `script"VaultScripts\Gameplay\rfr_Challenge.nfsms"` for `rfr_challenge.nfsms`); the same goes for `file_copy`, `folder_create` and `ui_image`. |
| `file_copy from to`, `folder_create folder` | Installer steps: copy a file from the script's folder into the game's folder, or make a folder there. The game's folder is the one above `GLOBAL`. Both only write inside it, and only when the whole script runs for real (not on the trial copy, not for a single line). |
| `ui_text`, `ui_image`, `ui_control`, `ui_option` | The setup (below). |

Rename, add and copy also set the `CollectionName` field to the new name when the class has one, as Attribulator does.

NFS-VltEd opens World's `GLOBAL\attributes.bin`, `commerce.bin` and `fe_attrib.bin`, and not the event vaults in `gc.vaults`. Attribulina loads those too. No collection of any World build is in two vaults, so a line finds the same collection either way. A line naming an event's collection (`gameplay race_bin_01/1_2_1_sprint`) edits its event vault, which NFS-VltEd can't, and the saved file has no `Sign` (`doc/vault-format.md`).

## Properties and values

After the field come the words that lead to the part being set. NFS-VltEd has a class for each game type, and that class names the parts of the value. Those names are what scripts use:

```
update_field vehicle 911gt3 TORQUE MinY 90                          # a Curve's yMin
update_collection vehicle 911gt3 TORQUE YValues 6 Value 0.98        # point 6 of its Y
update_field presetride player_d_day Parts[8] KitNumber 3            # mKitNumber
update_field presetride player_d_day Parts[8] PartClassKey car_part  # the class of the reference mPart
update_field part_physics tires_lvl_4_kit03 UpgradeDescriptions[0] 4 1.5   # mBlendingPower
```

- `data/nfsms/<game>_pc.txt` lists those names for each game's types: `mw_pc.txt`, `carbon_pc.txt`, `prostreet_pc.txt`, `undercover_pc.txt`, `world_pc.txt` (World's final build, NFS-VltEd's W) and `world_beta_pc.txt` (its 2009 and 2010 betas, NFS-VltEd's WB, chosen by the files' `Vers`). `scripts/nfsms/gen_words.py` writes them from the decompilation: `A/_0007_0014.cs` registers a class per type and list of games, and each class reads its properties in order, which gives each name its byte in the PC's files. Some game lists are compiled arrays the decompilation lacks; `scripts/nfsms/README.md` explains how they were worked out, and each type in a table says how sure its game assignment is.
- Each name also has the path of the member of Attribulina's schema at that byte (`KitNumber 20 mKitNumber`, `PartClassKey 4 mPart.mClassKey`). Attribulina follows the path by member names, so a name sets the same member on every platform: on the PS2 and the GameCube a bool is 4 bytes, so `FEPartData`'s `LogoTextureId` is at byte 36 there instead of 24. A name without a path (a type with no struct in the schema, or a name over several members) goes by its byte. That lets a name lead into a reference (`PartClassKey`) and an author's number stand for a member (`4`).
- Most Wanted's string keys are 16 bytes, and NFS-VltEd's Most Wanted class for a `MODEL` field has a third word, `Collision64`, for the 64-bit hash. Like `Collision`, it fails its line: Attribulina computes both hashes from the text, which `Visual` sets.
- World has no header, so its table's paths name the members of `world_pc.txt`, many of which come from NFS-VltEd's own World classes (`scripts/world/world_types.h`). Where NFS-VltEd numbers a type's parts (`IntegratedKitWheelOffset`'s 1, 2 and 3), the word is the number and the member is named after what it holds, or `Value<n>` when nothing says. NFS-VltEd names `Commerce::HatBonus`'s parts alphabetically; the game reads the part count at byte 12, where NFS-VltEd's `TopSpeed` lands, and the table says so. Word 1 of `IntegratedKitWheelOffset` is a `StringKey`'s hash, which Attribulina computes from the text (word 2), so it fails its line.
- Most Wanted's early demos (64-bit keys) take the same words: a reference's `SurfaceCollectionKey` sets its 64-bit key by the name given. Three name members their types don't have yet: `LeaderSupport`'s `PriorityChance` and `PriorityTime`, and `TrafficPatternRecord`'s `6` (`Percent`).
- NFS-VltEd's Undercover classes are for its PC version, a later game than the Wii's and the PS2's, whose types are Carbon's. Most names still find their member there. Twelve names of four types (`CollisionReactionRecord`, `EffectLinkageRecord`, `GMapRegionInfo`, `TireEffectRecord`) don't: their lines fail and name the member the Wii's type lacks, and the Wii's own member names work instead. The table marks those, and the types the Wii's files don't have, with `# not on wii` comments. On the PC and the 360 every name finds its member but a few that NFS-VltEd itself reads in its own way: `Sound::JittererParams` as eight 16-bit numbers where the game has four floats (its names 2, 4, 6 and 8 would set half of one, and fail), and `VinylsAttrib::VinylLayer` as seven 32-bit numbers each over several members (names 3 and 4 work, and a change recorded to one member uses the member's own name). A class reference (`Attrib::Gen::ClassRefSpec_<class>`) is `Class`, the collection's name, as NFS-VltEd reads it; its `Collection` is the pointer the game sets, which can't be set.
- A list (a Curve's `XValues`, `YValues` and `Y2Values`, or a list of references such as `CCarSlotEntry`'s `Parts`) takes the index and then the element's word (`Value`, or `Group` and `Name` for a reference), the way NFS-VltEd's `update_collection` writes it.
- A type NFS-VltEd sees as one value (a float, an enum, a GCollectionKey, a StringKey) ignores the word after the field, as NFS-VltEd does, so the word may be left out.
- A type NFS-VltEd has no class for has one word, `Data`: the value's bytes in hex, as many as it holds (`Data "00 00 80 3F"`). They are the bytes as that platform's file holds them. On a big-endian platform (360, PS3, Wii, GameCube) a `Data` line gets a warning, because a script written for the PC gives them little-endian.
- References also take `Class` and `Collection` (`ClassKey`, `CollectionKey`). A reference given no word gets its collection set and keeps its class, as in Attribulator. NFS-VltEd fails such a line: its class for a reference has the two words and no default.
- Our schema's member names work too, without case and without their `m` (`mKitNumber` or `KitNumber`), and so do Attribulator's forms: `field[^]` for the last element, `X`, `Y`, `Z`, `W` (or `R`, `G`, `B`, `A`) for vectors and colours, and `M[row,column]` or `Data[row,column]` for a matrix, counted from 1. NFS-VltEd compatible runs take NFS-VltEd's own words alone, as written.
- A word the type doesn't have fails its line and lists the words it does have.

Values are read by the field's type: numbers (`0x1F` too), `true`/`false` (or 1/0) for bools, enum names or numbers, text and string keys as they are, key names hashed. A name in a number field is its StringHash32, as in Attribulator. An empty `""` makes a reference null.

References are checked once the whole script has run, because mods set a reference's collection a line before its class and point at collections they make further down. One that still leads nowhere then is set anyway, as NFS-VltEd sets it without a word, and reported as a warning at the line that set it: `no collection vehicle/0x33341dbf_ai for the reference vehicle[4]`. That one is a slip in the mod tried here, which makes `elise_0x33341dbf_ai` and points at `0x33341dbf_ai`.

## The setup

A script can ask the user before it runs. Running it whole opens the setup prompt:

```
ui_text Faster cars for the drag events.
ui_image preview.png
ui_control checkbox nitrous true "Bigger nitrous"
ui_control radiobutton tier street "Street tires"
ui_control radiobutton tier race "Race tires"

ui_option nitrous
update_field pvehicle 911gt3 NOS_CAPACITY 2
ui_option nitrous

ui_option tier race
update_field ...
ui_option tier race
```

- `ui_text` lines are shown as text, and `ui_text#` as a blank line. `ui_image` pictures go in a row above them (files next to the script).
- `ui_control checkbox <name> <true|false> "label"` is a box with its starting state.
- `ui_control radiobutton <group> <name> "label"` is a choice in a group. No choice is made until the user picks one, as in NFS-VltEd.
- The prompt keeps the script's order, with one exception it copies from NFS-VltEd: the radio buttons of a group sit together where the first one is written, so a `ui_text` between two of them shows after the group.
- A `ui_option <box>` or `ui_option <group> <name>` line starts a block that runs only when that box is ticked or that choice made. The next `ui_option` line ends it, so mods repeat the opening line to close a block. A `ui_option` without a name is an error and ends nothing (NFS-VltEd can't read it either). Blocks don't nest.
- NFS-VltEd's installer reads the whole script before it shows the setup, and runs none of it when a `ui_option` names no control written above it, a block is closed by a `ui_option` naming another control, two controls share a name (a radio button's is its group and name together), or a `ui_control` doesn't read. Here a block ends at the next `ui_option` whatever it names, and the others are errors at their lines; NFS-VltEd compatible runs refuse such a script as NFS-VltEd does.

## The nfsms window

View > nfsms Script, or File > Run nfsms Script... to open one and run it.

- **Run Line** runs the line with the caret, or the selected lines. Setup blocks above it still decide whether it runs, using the choices made last (the defaults until the setup was shown). A line skipped that way says so.
- **Run All** runs the whole script, after the setup prompt when the script has `ui_` lines.
- Either way it runs line by line on the open database. A line that fails is marked (hover its number for why) and listed above the script, and the lines after it still run, as in NFS-VltEd. The status line says how many failed.
- **Strict** (off by default) changes that: warnings count as errors, and the script is tried on a copy of the database first and applied only when no line fails, so a failing line leaves the database as it was.
- **NFS-VltEd compatible** (off by default) reads values the way NFS-VltEd does, for scripts that rely on its quirks, and writes the nfsms lines the editor makes in its form (below).
- **Included scripts.** A message from a line of an included script names that script and line. Clicking it opens the script in a tab of the window at that line, and the script with the first error opens by itself after a run. Each tab marks its own failing lines, and the `script` line that included them is marked too. **Open Included** opens the script on the caret's `script` line. A run reads included scripts from their tabs, saved or not, so you can fix a line there and run again; Save and Save As act on the tab in front, Run Line runs lines of it, and Run All always runs the first tab's script.
- **To Mod Script** writes the script as a mod script into the Script panel (below).
- **Record changes** writes what you change in the editor into the window as nfsms commands, the way NFS-VltEd records into its script window, in NFS-VltEd's words (`MinY`, `KitNumber`, a curve point as `update_collection ... YValues 2 Value`), so NFS-VltEd can run the result. A value you change several times in a row (a slider being dragged) stays one line. A reference becomes two lines (`Class` and `Collection`), a whole array `resize_field` and one line per element. A new collection is `add_node` followed by `add_field` for each value outside its layout: NFS-VltEd's `add_node` gives a node only its layout fields, and its `update_field` fails on a field the node doesn't have. An element added at the end of an array (a mod script's `append`, a car added to `car_list`) is Attribulator's `append_array` and the new element's parts as `update_field ... Cars[^] ...`, so it adds to whatever the array holds where the script runs; `if missing` can't be said and comes out as a comment. nfsms can't take an element out or put one in the middle, so a `remove` or `insert` writes the whole array as it is now, and `resize ... by` the length it has now. nfsms has no conditions on the database: its `ui_option` blocks test the setup's choices only, so a mod script's `if exists ...` has no nfsms form. What nfsms can't say (Lua handlers, raw bytes, a curve with more points) comes out as a `#` comment. The Script panel has the same switch for mod scripts.
- **F12** (or Go to Definition in the right-click menu) shows what the word under the caret leads to. On a node it opens that collection; on the field, the words after it or the value, it also opens the rows down to the part the line sets and highlights it, read the way a run reads it (`Parts[7] KitNumber` is `Parts[7].mKitNumber`; on the field alone, `Parts[7]`). On the command or the class it shows what the line changes. `add_node` and `copy_node` show the new node once it is there, the parent or source before. On a `script` line it opens the included script in a tab, and on a `ui_option` line it goes to the `ui_control` line it tests.
- **Follow line** does the same for the line's own collection and value each time the caret moves to another line.
- A field's right-click menu in the collection panel has **Add to nfsms Script** and **Copy as nfsms**: `update_field` lines that set the field to its value there, in NFS-VltEd's words (on an element's row, that element only).
- **Add to Script (nfsms)**, in the menu of a collection, a class or a gameplay path, has the items of Add to Script (`doc/scripts.md`) and writes the same thing as nfsms lines at the end of the window's script. A collection comes out as `add_node` with its parent (and `change_vault` when its vault isn't the parent's), then its own values, each field the layout doesn't hold given to the node with `add_field` first (an array with its length). Run where the collection is missing, that makes it as it is here. Where it exists, `add_node` fails and the values still go in; NFS-VltEd fails the `add_field` lines there too, since it refuses to add a field a node has, where Attribulina leaves it be. Chosen Fields with Edits only writes `update_field` lines alone, for collections that exist. Exact is off there: nfsms has no command for reverting the values a database has besides the listed ones. Class Definition is greyed out, as nfsms has no class commands. A gameplay folder's vaults come out as comments, since `change_vault` makes a missing one, and Lua handlers as comments too.

## NFS-VltEd compatible

The nfsms window's **NFS-VltEd compatible** switch (also Edit > NFS-VltEd Compatible nfsms, `NfsmsVltEd` in the ini; `attribcli apply --vlted` and `convert --vlted`) is for scripts written against NFS-VltEd 4.6's own reading of the values, which differs from the game's in places. Off (the default), words lead to the game's members, as Attribulina's schema has them. On, a run does what NFS-VltEd does:

- **Its widths.** Each word is NFS-VltEd's type at its byte (the tables' `:type`, `scripts/nfsms/gen_words.py`). Where that is wider than our member there, the number is spread over the members by byte, into nested structs and inline arrays too: part_fedata's `4` is an int32 over `tier`, `drift`, `drag` and `grip`, so `16777474` (0x01000102) is tier 2, drift on, drag off, grip on, and Undercover's vinyl layers take seven u32s over `partNameHash`, `transform` and `colors[4]`. A word as wide as our member but a number where we have a float (or the other way round) sets the bytes: PresetRidePaint's `6 0x3f800000` makes `mSaturation` 1. The bytes are the PC's; on the PS2's and GameCube's layouts (4-byte bools) that is a warning. Pro Street has eight more such words, `CPartsPackageEntry`'s `KitNumber` among them.
- **Arrays hold their capacity.** A value written past an array's count, but within the slots NFS-VltEd holds (a layout array's whole space, an entry array's capacity), waits: the game doesn't read it until a `resize_field` later in the run takes the count over it. A layout array's `resize_field` moves the count within its slots and keeps their values, so shrinking and growing again brings them back. An entry array is cut or grows with zeros.
- **Value commands need an own field.** `update_field`, `update_collection`, `resize_field` and `resize_collection` fail on a field the node only inherits, as in NFS-VltEd: `add_field` it first.
- **add_field and add_node start from `default`.** `add_field` copies the class's `default` collection's own value; an array gets `count` slots, the first from `default`'s, and `default`'s count. `add_node` takes its layout fields from `default`. `add_field` on a field the node has fails.
- **Curves**, fields or members of a struct (`AI::GlueCurve`'s `Easy_` and `Hard_` words). A point set changes that list alone and leaves Y2 as it is (NFS-VltEd never works Y2 out). `resize_collection` resizes a list, but the file keeps each list's point count, so the game reads that many values from where each list starts in the lists NFS-VltEd writes one after another: with X and Y one shorter, X's last point is Y's first and Y's last is Y2's first. A note says so.
- **MODEL's words.** `Collision` sets the hash alone (a name hashed, `0x...` as the number) and `Visual` the text alone, keeping the hash. Most Wanted's MODEL is 16 bytes, and its `Collision64` sets the 64-bit hash alone (StringHash64 of a name).
- **A reference's pointer word**, the u32 after its two keys that the game fills in, can be set: `Value` of a reference in a list (`CCarSlotEntry`'s `Parts`), and the words over it in a struct (PresetRidePaint's `4`, PhysicsTuningDescription's `3`).
- **Values as NFS-VltEd reads them.** The text is read by NFS-VltEd's type for the word (or for a one-value type, its one property), the way .NET reads it under en-US, and then set in our member as those bytes say. A whole number is decimal digits with a sign, and it must fit NFS-VltEd's type: no hex (`0x10`), no fraction (`1.0`), no name. A float is digits with commas grouping thousands and an exponent, or `Infinity`, `-Infinity` or `NaN`, never hex or `inf`. A bool is true or false in any case, not 1 or 0. An enum takes its number, not its name. The words NFS-VltEd reads in hex take `0x` hex or a decimal number, and Undercover's `CarPaintSwatch` `RGB` takes hex digits with or without `0x`. Keys and bin keys keep their names: a bin key's decimal number is the key itself, and `""` is 0xFFFFFFFF. So the sign is NFS-VltEd's: Shifted's `partData[0] 6 -1` makes FEPartData's 6, an int32 over our bin key `BrandHALId`, 0xFFFFFFFF.
- **Words as NFS-VltEd writes them.** Only its own words, in the case it has them: `TORQUE MinY`, not `miny`, `yMin` (our member) or `Y[2]` (Attribulator's form), and no `[^]`. A type it has no class for has `Data` alone. A word after the field of a one-value type is ignored, numbers too. update_field takes one word at most. update_collection takes a list's word, the index and the element's word (`Value`; `Group`, `Name`, `Value`), and without the list's word the element's class must have a single list. An array given no index is its element 0, and an index on a field that isn't an array is ignored.
- **Commands as NFS-VltEd has them.** append_array, add_overwrite and copy_overwrite do nothing (NFS-VltEd reads them as comments). move_node needs the parent, change_vault moves gameplay nodes only and refuses db, frontend, cars and commerce and names with white space, delete_field fails on a node without its own value, convert_field fails, and a reference given no word fails. A new node's `CollectionName` is set only when it is a layout field, to the name NFS-VltEd shows for the key: the dictionary's, else lower-case hex (`0xabcdef12`). `Data` takes bytes split by spaces, each one or two hex digits.
- **Scripts it refuses.** A whole run reads the script and the scripts it includes first, and runs none of it when NFS-VltEd's installer would refuse it: a version newer than 4.6 or one that doesn't read, a `game` code other than MW, C, PS, UC, W and WB as written or a list without the database's game, and the setup's mistakes (see The setup). The first one is reported at its line. Run Line checks only the version and game lines it runs.

Some things are still done differently. NFS-VltEd cuts a line at its first `#`, even inside quotes, where Attribulina keeps it in the value. NFS-VltEd's script window splits lines at a lone carriage return too. When a layout array grows, NFS-VltEd shows the bytes the file holds past its count, where Attribulina gives zeros. An `add_field` count below `default`'s gives NFS-VltEd an array whose header and elements disagree, where Attribulina cuts the array to the count. NFS-VltEd's script window runs every line, ignoring the setup and `script` lines, and records an installer's commands in its own script when recording is on. NFS-VltEd's code that reads a script file and its includes isn't in the decompilation, so how it treats `ui_` lines in an included script, and blocks across files, is a guess (read as one script, in order).

Values written past an array's count are kept for the rest of the run, not across runs (NFS-VltEd keeps them in its memory and in the file it saves). `tests/test_nfsms.cpp` runs two mods made for NFS-VltEd this way with no failed line, when they are present: the Shifted mod for Pro Street (`nfsms/Shifted`; read the other way, 511 of its lines fail) and NFS Undercover Reformed's vault installer with its default choices (`nfsms/Part 2 (Vault)`; about 1140 fail the other way, and 15796 did before curves in structs were read). With other choices a few of its lines fail in either reading because the mod's own scripts name what isn't there (a node a "re-enable" option expects the "disable" one to have made, a value the node only inherits); NFS-VltEd fails those lines too.

With the switch on, what the editor writes as nfsms (Record changes, Add to Script (nfsms), the field menu) is what NFS-VltEd reads: no `[^]` (NFS-VltEd can't read it) and no Attribulator commands (`append_array` becomes `resize_field` and the new elements by index); a word wider than our member is written whole, as the value has it now, and so is the word over a member NFS-VltEd has no word for (a vinyl layer's `transform`, a padding byte); a reference's pointer word isn't written; an array given to a node with `add_field` gets its length with `resize_field` after it; a curve is written whole, Y2 included; a word NFS-VltEd reads as a number is written as the number the value's bytes make (an enum member's number, not its name); and a value holding `#` or `"`, which NFS-VltEd can't read, comes out as a comment. Either way, the recorder gives a value that a collection only inherited `add_field` first.

## As a mod script

To Mod Script (in the window), or `attribcli convert GLOBAL mod.nfsms -o mod.amod`, writes an nfsms script as a mod script (`doc/scripts.md`):

- The setup's `ui_text` lines become the script's `description`, a checkbox a `bool` option, and a radio group a `choice` option asked with the text above it, whose choices are its buttons' texts and `(none)`, the default, since NFS-VltEd starts with nothing chosen.
- A `ui_option` block becomes an `if` on its option: `if $dif == "Hard" { ... }`.
- Included scripts are written in place, after a `# from` comment.
- Values come out as Attribulina's paths (`TORQUE.yMin`, `Parts[8].mKitNumber`), typed by their fields, and commands as `create`, `create ... from`, `delete ... with children`, `rename`, `move` and `move ... to vault`. `append_array` is an `append` (format 2), so the element goes at the end of the array on any database. Consecutive values of one collection share an `edit` block.
- `add_node` with a parent is a `create` and then a `move` under the parent. A `create ... under` would start the node from the parent's layout values, and nfsms starts it from zeros (NFS-VltEd compatible: from the class's `default`).
- A name written as its hash comes out as the name when the database's name lists know it, since both are the same key: a field (`0x767fffc1 = false` is `isBoosterPack = false`), a reference's collection (`"part_fedata"/"t3_low_forced_induction_package_2"`), a key, `rename_node`'s new name, and a bin key by the bin key names (`HAL_ID 0xdac3d6ae` is `HAL_ID = "CAR_PART_STOCK"`). A hash no list knows stays as written. MODEL's Visual and Collision set to the same text come out as the text alone.
- To read values by their fields' types and to find what earlier lines made, it runs the script on a copy of the database, with every setup block taken. A line that fails there comes out as a comment saying why.
- `file_copy`, `folder_create` and `ui_image` have no mod script form and come out as comments.

Applied with the same choices, the mod script makes the same database as the nfsms script: `tests/test_nfsms.cpp` checks that for a script with every kind of command and for the real mod.

## Command line

`attribcli apply GLOBAL mod.nfsms` runs it like a mod script (`--vlted`: NFS-VltEd compatible). A setup takes its choices from `--set`: `--set nitrous=true` for a box, `--set tier=race` for a group. Without them boxes keep their starting state and no group has a choice. `file_copy` writes into the game's folder unless `--dry-run` or `--out` is given. Lines that fail are listed and the rest is saved; with `--strict` warnings count as failures, the first failing line stops it and nothing is saved.
