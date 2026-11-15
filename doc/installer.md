# The mod installer

`AttribulinaModInstaller.exe` installs one mod into a game, for players who don't need the editor. It builds beside `Attribulina.exe` (the `ModInstaller` project) and needs the same `data\` folder next to it. The code is `src/installer/Installer.h` for the window and `src/core/Install.h` for the work, which attribcli's `apply` shares.

## The steps

1. **Mod.** A mod script (`.amod`) or an nfsms script (`.nfsms`), with the files that came with it. A path on the command line, a file dropped on the exe or on the window, or Browse gives it. The page shows the mod's title, author, version and description: a mod script's own lines, or an nfsms script's `# amod` block (`doc/nfsms.md`, "The mod's title and version"), else its file name and its `ui_text` lines. A mod script that doesn't read stays here with its error.
2. **Game.** The folder the game is installed in. Its database is always in its `GLOBAL` folder, in any case, with `attributes.bin` there (`FindGameFolder`). The `GLOBAL` folder itself, or the `attributes.bin` in it, works too. Every pack there loads as attribcli and the editor load a folder. The page names the game and platform, the packs, and the memory files that hold copies of them (`doc/memory-files.md`). When the files don't say for certain which game they are from, it says what it guessed and offers a list to pick another. A folder on the command line or dropped on the window is the game's. The folders chosen before are kept in `mod_installer.ini` in the user's folder, and the last one loads by itself once a mod opens.
3. **Options.** A mod script's options, drawn as the editor's Apply Script prompt draws them, or an nfsms script's setup, drawn as the editor's nfsms Setup prompt draws it: the pictures, the text, the check boxes and radio buttons (`app/ScriptSetup.h`). A mod without options says so.
4. **Install.** A summary of the mod, the game and the choices, then Install. The script runs on the game's packs, and the packs it changed are saved (`SaveChanges`). The first time a file changes, its original is kept beside it as `<file>.bak`. Each memory file holding a copy of a saved pack gets the pack's new bytes, its own first original kept as `.bak` too. The page then says how it went and lists the files changed.

A mod can't go past the Game step when it was written for another game: a mod script's `game` line names another game, or an nfsms script's `game` line doesn't list this one (or its `# amod` block's `game` names another), or the game is Nitro, which NFS-VltEd never read (`ModProblem`). A mod script written for the same game on another platform gets a warning only, since its values go in by their fields' types (`ModWarning`).

A mod the game's list of installed mods has already (below) waits at the Game step too. The page says when it was installed, and that installing it again runs its script over its own changes, which can fail: a line that adds a node finds it there. Another version of it says the same of the other version's changes. Install it anyway, a check box under that, lets it go on.

Once the mod is installed, or failed to be, the steps before are locked: the list on the left, Back, a mod or folder dropped on the window, none goes back to them, and the page shows what was installed. Install Another Mod starts over with the game's folder kept, and Close ends.

## How it runs scripts

- A mod script runs as Apply Script does without Strict: it stops at its first error, and then nothing is saved.
- An nfsms script always runs NFS-VltEd compatible (`nfsms::RunSettings::vltEd`, `doc/nfsms.md`), as nfsms mods are made for NFS-VltEd (the user's call). A line that fails is logged and the next one runs, as in NFS-VltEd. A script NFS-VltEd's installer refuses runs none of its lines. `file_copy` and `folder_create` write into the game's folder.
- Names a script gives new collections go to `user_keys.txt` in the user's folder, as attribcli adds them, so the editor shows them.

## The list of installed mods

The installer keeps the mods it installs in `attribulina_mods.txt`, in the game's `GLOBAL` folder beside the database (`core/ModList.h`). It isn't in the packs, so the game's files don't grow by anything an unpatched game would have to read, and the game never reads the file. Nothing needs it either: a game without one has no mods listed, and deleting it forgets the list.

A mod is told apart by the title and version it gives itself: a mod script's `title` and `version` lines, an nfsms script's `# amod` block. Titles match in any case. A mod without a title is its script file, by the file's SHA-256, so the same file is the same mod under any name, and an edited one is another (`FindInstalled`). The Log says which: `it names no title, so its file's SHA-256 (3a7bd3e2360a...) tells it apart`.

Each installation adds an entry once its changes are saved, oldest first: a mod installed twice is there twice. One whose script stopped saved nothing and isn't listed; one whose saving failed halfway is, with the files saved before it.

```
attribulina mods 1

mod "Shifted"
    version "1.2"
    author "someone"
    script "D:\Mods\Shifted\Shifted.nfsms"
    nfsms
    sha256 3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b
    game prostreet_pc
    installed "2026-11-14 15:30:12"
    by "Attribulina Mod Installer 0.9.3"
    result "installed, 2 lines failed"
    option "Loud" "on"
    file "GLOBAL\ATTRIBUTES.BIN"
    file "GLOBAL\GlobalMemoryFile.bin"
    file "CARS\SHIFTED\GEOMETRY.BIN"
end
```

- `mod` takes the mod's own title; without one it stands alone, and the window shows the script's file name.
- `option` is a mod script's option value by id, or an nfsms setup's choice: a check box `on` or `off`, a radio group's button.
- `file` is each file it changed, from the game's folder (the one above `GLOBAL`), with `\` between folders: the packs, the memory files that hold copies of them, files it removed, and what nfsms `file_copy` wrote.
- A text in quotes doubles a quote inside it (`""`), so paths keep their backslashes.
- A line this version doesn't know stays with its mod when the file is written again, so a later Attribulina can add to an entry.
- The file is written through a `.tmp` that then replaces it. One that doesn't read is left as it is: installing says so in the Log and on the Game page, and doesn't add to it.

**Advanced Options**, the button at the bottom left, has a check box for the list, Keep a list of installed mods (on by default, `TrackMods` in `mod_installer.ini`). Off, installing neither reads the list nor adds to it. Its Installed Mods... button opens the list of the game chosen: a table of the mods with their version, author, time and result, and the one clicked in full below it (its script, SHA-256 with a Copy button, game, program, options and files). `certutil -hashfile <script> SHA256` prints the same SHA-256 for the same file on Windows.

The editor reads the list too, as it opens a database (none when the folder has no file), and View > Installed Mods shows it. File > Apply Script and File > Run nfsms Script add the mod they apply when Save All saves its changes, and undoing the step takes it back off before then. The Apply Script prompt and the Log say when the list has the mod already. The Script panels' own Apply and Run don't add to it: those are scripts being written. File > Keep a List of Installed Mods turns this off (`TrackMods=` in `attribulina.ini`). `attribcli apply` adds each script file it applies (not `-e` statements or standard input) once saved, writes a copy of the list into `--out` when given, warns when the list has the mod already, and leaves the list out with `--no-mod-list`.

## How it went

| Outcome | When | What the game's files are |
| --- | --- | --- |
| The mod is installed. | the script ran and its changes were saved | changed, originals as `.bak` |
| The mod is installed, but N lines of its script failed. | nfsms lines failed | the other lines' changes saved |
| The mod wasn't installed. | a mod script stopped, NFS-VltEd's checks refused an nfsms script, or saving failed | as they were, or, when saving failed halfway, the files before it saved (the Log names them) |

## The window, the Log and the log file

The steps are listed on the left, and Back and Next move between them. A step the mod or the game isn't ready for is greyed out. A status line along the bottom says what is going on, coloured for warnings, errors and success. Show the Log opens the Log above it: every line of the run, the script's own messages (`mod.amod:4:1: error: ...`), each file saved, and as the last line how the installation went. Copy All copies it, and Open the Log File opens the file.

Reading the game's files and installing run on a thread of their own, so the window keeps drawing. While a mod is being installed, Cancel is greyed out, closing the window is refused, and the status line asks to wait.

Each run writes a log file named by the time it started, `AttribulinaModInstaller_2026-11-14_15-30-12.log`, in the folder the program runs in, else beside the program, else in the user's folder. Each line has its time and level, and the file is written as the lines come, so a crash still leaves it. The Log's first lines say where it is.

## The editor's log file

The editor writes its Log to `attribulina.log` in the user's folder (`core/LogFile.h`; portable mode: beside the program). It is written line by line, and the session before is kept as `attribulina.previous.log`. A second editor window writes `attribulina (2).log` while the first has its file open. On Windows the file is opened so others can read it but not write it; elsewhere no such check is made. The Log's first lines name the file.

## Not covered

- Undoing an installation. The `.bak` files hold each file's first original, so copying them back over the files restores the game as it was before the first save, and the list of installed mods should then go too. An uninstaller and a check for mods that change the same things are planned, from the list and its files.
- A mod made of several scripts to run one after another. An nfsms script's `script` lines and a mod script's `include` statements cover that within one mod.
- Other platforms. The window is ImGui through `app::Host`, so another platform needs a window and a renderer for it, as the editor does, and the installer's code stays as it is.
