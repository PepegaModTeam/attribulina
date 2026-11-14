# Memory files

The games keep copies of some files in memory files: `Global\GlobalMemoryFile.bin`,
`InGameMemoryFile.bin`, `PermanentMemoryFile.bin`, `CarHeadersMemoryFile.bin`,
`FrontEndMemoryFile.bin`, and in World's final build `InfoMemoryFile.bin`. The
game reads one whole into memory, and from then on every file it opens is looked
up in the loaded memory files first. A file found there is read from the copy
and the loose file is never opened. So an attribute pack saved while a memory
file still holds the old copy changes nothing in the game.

Every PC game's `GlobalMemoryFile.bin` holds `GLOBAL\ATTRIBUTES.BIN`, the whole
file. Undercover's also holds `GLOBAL\CARS_VAULT.BIN` and `GLOBAL\GAMEPLAY.LZC`.
`attrib/memoryfiles/pc/<game>/` has the retail files of Most Wanted, Carbon,
Pro Street and Undercover, and each copy of an attribute pack in them is
byte for byte the retail pack.

The code is `src/core/MemoryFile.h`. Saving in the editor and in attribcli
updates the copies (below).

## Format

All values are little-endian u32s, on the 360 too: `AddMemoryFile` reads them
with `lwbrx`. The Pro Street 360 milestone's PDB names the structs.

```
MemoryFile (header, 16 bytes)
  +0   bTNode links (next, prev)   zero in the files; AddMemoryFile links the loaded image into MemoryFileList
  +8   Magic                       0x53219999 (MEMORY_FILE_MAGIC)
  +12  NumFileEntries              the class has room for 8192
MemoryFileEntry (20 bytes each, from +16)
  +0   Hash                        bFileGetFilenameHash of the game path
  +4   Offset                      of the copy, from the start of the file
  +8   FileSize                    the size bFile gives the file
  +12  MemorySize                  how many of its bytes the copy holds
  +16  Data                        a pointer; AddMemoryFile sets it to image + Offset
```

The copies follow the table in table order. The first one starts at the end
of the table rounded up to 16, and each next one at the end of the one before
it rounded up to 16. The bytes between copies are zero, and the file ends where
the last copy ends. All 849 entries of the 18 PC files sit exactly where that
rule puts them, and `WriteMemoryFile` lays them out by it, so reading and
writing a retail file gives back the same bytes (`tests/test_memoryfile.cpp`).

The `Data` values in the files look like the heap addresses the copies had
when the file was made (`0x00611000`, `0x00711000`). The game overwrites them
on load and never reads them, and the editor keeps them as stored.

### The hash

`bFileGetFilenameHash` (Undercover 360 debug `0x82A4DC08`) upper-cases a to z,
turns `/` into `\` and takes `bStringHash` of the result. The paths are relative
to the game's folder: `GLOBAL\ATTRIBUTES.BIN` is `0x9775379A`. The memory files
themselves load from `Global\`, so for a pack beside a memory file the editor
takes `GLOBAL\` plus the pack's path from that folder (`GamePath`). That holds
whatever the folder is called here: `attrib/databases/pc/prostreet-vanilla` works
like the game's `GLOBAL`. A file in a folder under it is `GLOBAL\GC.VAULTS\X.BIN`,
and one in a folder beside it `FRONTEND\X.BIN`.

### Partial copies

Some entries hold only the start of a file, the part the game read when the
memory file was made. `GLOBAL\DYNTEX.BIN` and the `CARS\...\TEXTURES.BIN` files
are like that, and a few entries have both sizes 0. `FileSize` is still the
whole file's size, and opening such a file gives that size without touching the
disk. `bFile::ReadAsync` (`0x82A71FB0`) copies a read that starts below
`MemorySize` out of the copy. Bytes of it past `MemorySize` come back filled
with `0x21`, and only a read starting at or past `MemorySize` goes to the loose
file. No attribute pack has a partial copy in the PC files. If one did, saving
would make it whole, because a pack whose header grew would otherwise be read
partly as `0x21`.

## What the code does with them

All from the Undercover 360 debug build (`nfs-data/SpeedDebugOpt-enUS.exe`,
`tools/xex/ucdis.py`):

- `bLoadMemoryFile(name, pool)` returns at once when `DisableMemoryFiles` is set.
  Otherwise it allocates the file's size with `bMalloc` and queues the read.
  `LoadMemoryFileCallback` then calls `AddMemoryFile`.
- `AddMemoryFile` asserts the magic ("memory_file->Magic == MEMORY_FILE_MAGIC",
  `bFile.cpp` line 595), links the image into `MemoryFileList`, byte-swaps the
  four sizes of each entry and sets `Data`.
- `FindMemoryFileEntry(name)` hashes the name and walks every loaded memory file's
  entries for it. The `bFile` constructor (`0x82A752B8`) takes `FileSize` from
  the entry it finds and skips `OpenLowLevel`, and `bFile::ReadAsync` `memcpy`s
  from `Data`. Nothing else in the build touches `MemoryFileList` or an entry, so
  a copy's place in the file doesn't matter beyond the 16-byte boundary the
  builder kept.
- The PC builds do the same. `AddMemoryFile` is at `0x65AA00` in Most Wanted's
  `speed.exe`, `0x6A9E30` in Carbon's `NFSC.exe`, `0x6D3480` in Pro Street's
  `nfs.exe` and `0x6A59B0` in Undercover's `nfs_steam.exe` (World's final
  `nfsw.exe` has the same loop at `0x616467`), and each sets every entry's
  `Data` to the image plus `Offset`. The release builds drop the magic assert.
  Undercover's `FindMemoryFileEntry` (`0x6A5A00`) has two callers: the `bFile`
  constructor takes `FileSize` (`0x6AB94E`), and `ReadAsync` (`0x6AA437`) fills
  the bytes past `MemorySize` with `0x21` and `memcpy`s the rest from `Data`.
- Undercover's PC map names `FindMemoryFileEntry` and `UnloadGlobalMemFilesStage`,
  and Most Wanted's `LoadMemoryFile` and `LoadMemoryFileCallback`. Each PC
  executable names the files it loads: Most Wanted `Global\GlobalMemoryFile.bin`
  and `InGameMemoryFile.bin`; Carbon and Pro Street those and
  `CarHeadersMemoryFile.bin`; Undercover those and `PermanentMemoryFile.bin`;
  World's 2010 beta the same four; World's final build `InGameMemoryFile.bin`
  and `InfoMemoryFile.bin`.

The 16-byte boundary is the builder's only alignment. 15 of the 31 copies in
Carbon's `GlobalMemoryFile.bin` start on a boundary of 16 and not 32, and none
of the 849 entries has padding beyond what reaching the next 16 needs. The game
copies data out with `memcpy` anyway, so a copy that moves to another multiple
of 16 reads the same.

## Saving

File > Update Memory Files (on by default, `UpdateMemoryFiles=` in the ini) and
attribcli's `apply` and `repair` (unless `--no-memory-files`) do this after
the packs are written:

1. Find the memory files beside each saved file: files named `*MemoryFile.bin`
   in any case, in its folder and the folder above (the Global folder of a file in
   `gc.vaults`), that open with the magic. Only the table is read at first.
2. For each memory file whose table has the hash of a saved file's game path,
   read it, give that entry the saved bytes whole (`FileSize` and `MemorySize`
   the new size) and drop the entries of files the save removed (World event
   vault files whose vault was deleted). Every other entry keeps its bytes,
   sizes, `Data` and place in the table. The copies after a changed one move to
   their new 16-byte boundaries.
3. Write the memory file through a `.tmp`, keeping the first original as
   `<file>.bak` (not with attribcli's `--no-backup`). A memory file whose copy
   is already the saved bytes is left alone, so saving again writes nothing.

Nothing else in a memory file changes. Every other copy keeps its bytes, its
`FileSize` and `MemorySize` (a partial copy such as `DYNTEX.BIN` stays partial),
its `Data` and its place in the table. The copies before the changed one stay
where they were, byte for byte. The ones after it keep their bytes and move
together by the change in the copy's size rounded to 16, which changes only
their `Offset` fields. The game finds a copy by its `Offset` alone, so it reads
the same bytes for every other file. Saving the original pack again gives back
the original memory file byte for byte. `tests/test_cli.cpp` checks all of this
on a real save in each PC game ("saving a pack changes nothing in the memory
files but its copy").

Save All writes every pack first and then each memory file once, with every
pack it holds a copy of (Undercover's Global holds three). A `.lzc` and its
`.bin` twin both count, so Undercover's `GAMEPLAY.LZC` copy follows its saved
`.lzc`. attribcli's `--out` writes the updated memory files into the output
folder beside the packs and leaves the originals alone. `--dry-run` lists the
copies it would update.

Opening a pack logs each memory file holding a copy of it. A copy that differs
from the file is a warning, since the game reads the copy and none of the
file's later changes. With the option off, saving leaves the memory files as
they are and the Log warns for each copy that is now older than its file.

## Not covered

- Memory files of other platforms. None are here. The 360 build reads them
  little-endian, and `ReadMemoryFile` also takes one whose magic reads
  big-endian and writes it back that way, untested.
- A pack added to a memory file that has no copy of it. The editor never adds
  entries, since the game would then stop reading the loose file.
- World's memory files. Its executables load them, but none are here.
