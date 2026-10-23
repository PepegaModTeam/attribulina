# nfsms word tables

`gen_words.py` writes `data/nfsms/<game>_pc.txt`: NFS-VltEd 4.6's words for the parts of a game's values (`update_field tires x GRIP_SCALE Front 1.1`), the byte each one sets in the PC files, and the member of our schema at that byte. `PropertyNames` in `src/core/Nfsms.cpp` reads them, and `doc/nfsms.md` says how scripts use them. Each table's header explains its format.

```bash
python3 scripts/nfsms/gen_words.py              # Most Wanted, Carbon, Pro Street, Undercover, World
python3 scripts/nfsms/gen_words.py --game PS    # one of them (MW, C, PS, UC, W)
python3 scripts/nfsms/arrays.py                 # the evidence for the compiled game lists
```

Python 3, standard library only, about 10 seconds. It writes no bytecode, so nothing appears beside the scripts. It reads:

- `NFS-VltEd/`, the decompilation (read only);
- `data/schema/`, for the paths and for the other platforms' layouts;
- the type tables of `attrib/databases/`: the PC files of every game NFS-VltEd knows (World's final build and both betas included) for the game lists, the consoles' for the platform checks;
- for Undercover, `nfs-data/SpeedDebugOpt-enUS.h` and, for types it lacks, the Pro Street, Carbon and Most Wanted headers, through `scripts/gen_schema.py`'s parsers. `--schema-out DIR` writes the layout it makes; it is only used in memory otherwise.

## The scripts

- `registry.py` reads the registry, `A/_0007_0014.cs`: one C# class per AttribSys type and list of games. `ARRAYS` says which games each compiled list holds and why. For a game, the class in place is the last registration that holds it; one written out for the game is taken over a compiled list that only may hold it.
- `arrays.py` prints the evidence for `ARRAYS` and checks it; `gen_words.py` warns when the two disagree.
- `decode.py` decodes a class: its properties in declaration order, matched by type to the reads of its `Read(BinaryReader)` method, which give each one its offset.
- `schema.py` reads our schema files, finds the members at a word's bytes, and resolves a path in another schema.
- `typetable.py` reads a pack's type table (names and sizes), either byte order.
- `common.py` holds the paths and the decompilation's file names.

## Decisions

- **Compiled game lists.** A list of three or more games is a static array whose bytes the decompilation lacks. Roslyn names such fields by the SHA-1 of their bytes and emits them sorted; the obfuscator renamed them in that order. So `A/_000E_000A.cs` declares them in SHA-1 order, across all sizes. Twelve lists are known otherwise: nine by their types, three because other code uses the same bytes (the message box's button lists, the 6-entry ALL). Their hashes ascend in field order, and the other lists can only be those whose hash sorts where their field is. That fixes all but two: `_0009/3` (EngineLFOParams: PS,W,WB or PS,UC,W) and `_001A/3` (stShiftPair: C,UC,W, MW,C,WB or MW,UC,W). A list is taken to name only games that have one of its types, which every known list does; it decides `_0009/4` and halves `_0009/3`.
- **Confidence.** "read": registered for the game or for all of them, or in a list other code shows. "inferred": every list the evidence leaves holds the game. "uncertain": some do. A type whose list can't hold the game isn't in its table.
- **Paths.** A word's path is the member it covers, or the deepest member holding all the ones it covers. Padding members (`__align0`) don't count. When the word starts inside that member, `+N` follows. No path when the covered members share nothing but the value, or the type has no struct in our schema. Every path is checked to lead back to the word's offset.
- **Words named after another member.** The setters are proxy delegates, so the property a read lands in comes from declaration order alone. When that puts a word on the member another word is named after, the table says so (Carbon's and Undercover's `GMapCurve`, Undercover's `Sound::PursuitMusicTuning`).
- **World.** NFS-VltEd's W is the final build (`pc/world-final`); its WB, the betas, gets no table until their schemas exist. Our World schema has no header behind it, so a path names a member of `world_pc.txt`, and the members of World's own types (`scripts/world/world_types.h`) take NFS-VltEd's names where it has words for them.
- **Undercover.** NFS-VltEd's Undercover classes read the PC game, the current-generation one. Our schemas only lay out the Wii's and the PS2's (Carbon's engine), so the paths come from Undercover's PC types laid out from its 360 debug header at the PC's alignment, checked against the PC type table: every struct a word uses matches its size.
- **Other platforms.** Each table is checked against the other schemas of its game: Pro Street's PS2; Most Wanted's GameCube (and PS2 beta); Carbon's PS2 (and GameCube), 360 and PS2 demo; Undercover's Wii and PS2. The 360, Wii, PS3 and Xbox schemas that are the PC's need no check. A type whose path doesn't resolve there, whose struct is missing, whose files lack it, or that has another size gets a `# not on` or `# on` comment, and the run prints the counts.
