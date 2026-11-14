# The mod installer

`AttribulinaModInstaller.exe` installs one mod into a game, for players who don't need the editor. It builds beside `Attribulina.exe` (the `ModInstaller` project) and needs the same `data\` folder next to it. The code is `src/installer/Installer.h` for the window and `src/core/Install.h` for the work, which attribcli's `apply` shares.

## The steps

1. **Mod.** A mod script (`.amod`) or an nfsms script (`.nfsms`), with the files that came with it. A path on the command line, a file dropped on the exe or on the window, or Browse gives it. The page shows the mod's title, author, version and description (an nfsms script's `ui_text` lines). A mod script that doesn't read stays here with its error.
2. **Game.** The folder the game is installed in. Its database is always in its `GLOBAL` folder, in any case, with `attributes.bin` there (`FindGameFolder`). The `GLOBAL` folder itself, or the `attributes.bin` in it, works too. Every pack there loads as attribcli and the editor load a folder. The page names the game and platform, the packs, and the memory files that hold copies of them (`doc/memory-files.md`). When the files don't say for certain which game they are from, it says what it guessed and offers a list to pick another. A folder on the command line or dropped on the window is the game's. The folders chosen before are kept in `mod_installer.ini` in the user's folder, and the last one loads by itself once a mod opens.
3. **Options.** A mod script's options, drawn as the editor's Apply Script prompt draws them, or an nfsms script's setup, drawn as the editor's nfsms Setup prompt draws it: the pictures, the text, the check boxes and radio buttons (`app/ScriptSetup.h`). A mod without options says so.
4. **Install.** A summary of the mod, the game and the choices, then Install. The script runs on the game's packs, and the packs it changed are saved (`SaveChanges`). The first time a file changes, its original is kept beside it as `<file>.bak`. Each memory file holding a copy of a saved pack gets the pack's new bytes, its own first original kept as `.bak` too. The page then says how it went and lists the files changed.

A mod can't go past the Game step when it was written for another game: a mod script's `game` line names another game, or an nfsms script's `game` line doesn't list this one, or the game is Nitro, which NFS-VltEd never read (`ModProblem`). A mod script written for the same game on another platform gets a warning only, since its values go in by their fields' types (`ModWarning`).

## How it runs scripts

- A mod script runs as Apply Script does without Strict: it stops at its first error, and then nothing is saved.
- An nfsms script always runs NFS-VltEd compatible (`nfsms::RunSettings::vltEd`, `doc/nfsms.md`), as nfsms mods are made for NFS-VltEd (the user's call). A line that fails is logged and the next one runs, as in NFS-VltEd. A script NFS-VltEd's installer refuses runs none of its lines. `file_copy` and `folder_create` write into the game's folder.
- Names a script gives new collections go to `user_keys.txt` in the user's folder, as attribcli adds them, so the editor shows them.

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

- Undoing an installation. The `.bak` files hold each file's first original, so copying them back over the files restores the game as it was before the first save.
- A mod made of several scripts to run one after another. An nfsms script's `script` lines and a mod script's `include` statements cover that within one mod.
- Other platforms. The window is ImGui through `app::Host`, so another platform needs a window and a renderer for it, as the editor does, and the installer's code stays as it is.
