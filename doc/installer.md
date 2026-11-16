# The mod installer

`AttribulinaModInstaller.exe` installs one mod into a game, for players who don't need the editor. It builds beside `Attribulina.exe` (the `ModInstaller` project) and needs the same `data\` folder next to it. The code is `src/installer/Installer.h` for the window and `src/core/Install.h` for the work, which attribcli's `apply` shares.

## The steps

1. **Mod and Game**, on one page, in either order.
   - **The mod:** a mod script (`.amod`) or an nfsms script (`.nfsms`), with the files that came with it. A path on the command line, a file dropped on the exe or on the window, or Browse gives it. The page shows the mod's title, author, version and description: a mod script's own lines, or an nfsms script's `# amod` block (`doc/nfsms.md`, "The mod's title and version"), else its file name and its `ui_text` lines. A mod script that doesn't read shows its error.
   - **The game:** the folder the game is installed in. Its database is always in its `GLOBAL` folder, in any case, with `attributes.bin` there (`FindGameFolder`). The `GLOBAL` folder itself, or the `attributes.bin` in it, works too. Every pack there loads as attribcli and the editor load a folder. The page names the game and platform, the packs, the memory files that hold copies of them (`doc/memory-files.md`) and how many entries the game's list of installed mods has. When the files don't say for certain which game they are from, it says what it guessed and offers a list to pick another. A folder on the command line or dropped on the window is the game's. The folders chosen before are kept in `mod_installer.ini` in the user's folder, and the last one loads by itself when the program starts.
   - The game needs no mod: with only the game chosen, Installed Mods (below) shows its list and uninstalls from it.
2. **Options.** A mod script's options, drawn as the editor's Apply Script prompt draws them, or an nfsms script's setup, drawn as the editor's nfsms Setup prompt draws it: the pictures, the text, the check boxes and radio buttons (`app/ScriptSetup.h`). A mod without options says so.
3. **Install.** A summary of the mod, the game and the choices, then Install. The script runs on the game's packs, and the packs it changed are saved (`SaveChanges`). The first time a file changes, its original is kept beside it as `<file>.bak`. Each memory file holding a copy of a saved pack gets the pack's new bytes, its own first original kept as `.bak` too. The page then says how it went and lists the files changed. When the mod changes what a mod on the list changed, installing stops before anything is written and asks (below, "Mods that change the same things").

A mod can't go past the first step when it was written for another game: a mod script's `game` line names another game, or an nfsms script's `game` line doesn't list this one (or its `# amod` block's `game` names another), or the game is Nitro, which NFS-VltEd never read (`ModProblem`). A mod script written for the same game on another platform gets a warning only, since its values go in by their fields' types (`ModWarning`).

A mod the game's list of installed mods has already (below) waits at the first step too. The page says when it was installed, and that installing it again runs its script over its own changes, which can fail: a line that adds a node finds it there. Another version of it says the same of the other version's changes. Install it anyway, a check box under that, lets it go on.

Once the mod is installed, or failed to be, the steps before are locked: the list on the left, Back, a mod or folder dropped on the window, none goes back to them, and the page shows what was installed. Install Another Mod starts over with the game's folder kept, and Close ends.

## How it runs scripts

- A mod script runs as Apply Script does without Strict: it stops at its first error, and then nothing is saved.
- An nfsms script runs in NFS-VltEd mode (`nfsms::RunSettings::vltEd`, `doc/nfsms.md`), as most nfsms mods are made for NFS-VltEd (the user's call), unless Advanced Options turns NFS-VltEd compatible off, for OGVI mode. A line that fails is logged and the next one runs, as in both tools. In NFS-VltEd mode a script NFS-VltEd's installer refuses runs none of its lines, and `file_copy` and `folder_create` write into the game's folder; OGVI mode skips those two lines, as OGVI does.
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
    trace "attribulina_traces\2026-11-14 153012 Shifted 1.2.amod"
end
```

- `mod` takes the mod's own title; without one it stands alone, and the window shows the script's file name.
- `option` is a mod script's option value by id, or an nfsms setup's choice: a check box `on` or `off`, a radio group's button.
- `file` is each file it changed, from the game's folder (the one above `GLOBAL`), with `\` between folders: the packs, the memory files that hold copies of them, files it removed, and what nfsms `file_copy` wrote.
- A text in quotes doubles a quote inside it (`""`), so paths keep their backslashes.
- `trace` names the file holding what it changed in the database, from the list's folder (below). An entry without one was installed before Attribulina kept traces, or its trace couldn't be written: it can't be uninstalled, only taken off the list.
- A line this version doesn't know stays with its mod when the file is written again, so a later Attribulina can add to an entry.
- The file is written through a `.tmp` that then replaces it. One that doesn't read is left as it is: installing says so in the Log and on the Game page, and doesn't add to it.

**Installed Mods...**, the button at the bottom left beside Advanced Options, on every step, opens the list of the game chosen, with or without a mod: a table of the mods with their version, author, time and result, and the one clicked in full below it (its script, SHA-256 with a Copy button, game, program, options, files and what uninstalling it sets back). `certutil -hashfile <script> SHA256` prints the same SHA-256 for the same file on Windows. Uninstall... and Remove from List... act on the mod clicked, each after asking.

**Advanced Options** has two check boxes, both on by default and kept in `mod_installer.ini`:

- Keep a list of installed mods (`TrackMods`). Off, installing neither reads the list nor adds to it, and nothing is traced or checked.
- NFS-VltEd compatible (`NfsmsVltEd`, the editor's name for its own switch). On (the default), NFS-VltEd mode: an nfsms script runs as NFS-VltEd 4.6 runs it, for mods made with NFS-VltEd. Off, OGVI mode: it runs as OGVI and the community Attribulator run it, with their commands, VaultLib's names and value types and OGVI's setup (each radio group starts on its first button), for mods made with OGVI. Switching resets an nfsms mod's choices to the mode's. The Log says which mode each nfsms mod ran in. Mod scripts don't change.

## What a mod changed: its trace

Installing a mod keeps a trace of what it changed in the database (`core/ModTrace.h`): a copy of the database is taken before its script runs, and `script::Diff` between the two gives the mod script that turns the database as the mod left it back into the one before. That script is the trace. It goes in `GLOBAL\attribulina_traces\`, named by the time and the mod, and the entry's `trace` line names it. The game doesn't read the folder.

```
attribulina 2
game prostreet_pc
title "Undo Shifted 1.2"
# The trace of Shifted 1.2, installed 2026-11-14 15:30:12 by Attribulina Mod Installer 0.9.3.
# Applied to the database as the mod left it, it sets back what the mod changed: uninstalling
# the mod applies it. Files the mod copied aren't in it. Each `was` is the value the mod set.
# Changed values
edit audio_revlimiter/default {
    total_frames_short = 3 was 21
}
edit car_list/all_cars {
    remove Cars = pvehicle/shifted
}
# Deleted
delete pvehicle/shifted
```

Only installing makes one: the installer, `attribcli apply` with a script file, and the editor's File > Apply Script and File > Run nfsms Script. Lines run by hand, a script run from the editor's Script or nfsms panel, the console and edits in the inspector are never traced, and never go on the list. Only the database is in a trace. What a mod does to other files (nfsms `file_copy` and `folder_create`) isn't, and stays when it is uninstalled, since putting files back could leave the game's folder broken.

## Uninstalling

Installed Mods... > Uninstall... first works out what uninstalling the mod takes (`PlanUninstall`, on copies of the database: nothing is written). Then it asks. Uninstall applies the trace to the game's database, saves the files it changes (and the memory files' copies of them) as installing does, and takes the mod off the list, its trace file with it.

- A value someone changed after the mod was installed (by hand, or with a mod that isn't on the list) still goes back to what it was before the mod, and the Log says which.
- When a mod installed later changes some of the same things, the later mods are undone first, newest first, then the mod's trace is applied, then the later mods are done again from what they had set, and each gets a new trace. What a later mod set stays, and uninstalling it afterwards goes back to how things were before both.
- **Mods that build on it go with it**, as a package manager removes the packages that depend on one. A mod installed later that builds on what this one made (a value of a car it added, a collection under one it made) can't be done again without it. Working out the uninstall finds each such mod, and the ones building on those, by trying it on copies: each time a later mod can't be done again, that mod joins the ones going and it tries again. The question then lists them in a warning, with what each changes that is gone without it (`User 1 builds on what Maker 1 changed, so it is uninstalled with it: User 1 (no collection pvehicle/dep_car)`), and its button reads Uninstall All of Them. Cancel leaves everything as it is.
- A mod whose changes no later mod touches is just its trace applied.
- Values come back exactly. Uninstalling every mod gives the game's packs and memory files back byte for byte, except where a mod deleted collections: they come back with all their values, but the file may hold them in another order.

Remove from List... takes a mod off the list and changes nothing else: for a mod put back by hand from its `.bak` files, or one without a trace.

## Mods that change the same things

Before a mod is saved, its trace is compared with the traces of the mods on the list (`FindConflicts`). Two mods clash when they change the same value, or a value inside one the other sets whole, or both make, delete or rename the same collection, or one changes a class whose collections the other changes, or both make or delete the same vault. Two mods that each add to the same list (`append`) or take their own values out of it (`remove ... = value`) don't clash. Nor does a mod that changes what is inside a collection or class another made: it builds on that mod, an add-on for it, and uninstalling that mod later takes it along (above).

When it clashes, nothing has been written yet, and the Install page lists the mods and the first few things each shares with it. Each has a check box, ticked to start with:

- Uninstall the Ticked Mods, Then Install uninstalls those (as above), runs the mod again and saves both in one go. The mods left unticked stay, and it goes over them. A ticked mod that others build on takes them along, and its row says which before anything is pressed. A mod that can't be uninstalled (no trace, or a trace that doesn't apply) has its tick greyed out, with the reason.
- Install Anyway installs it over all of them. Where both change the same thing, this mod's value is the one that stays, and uninstalling either of them later keeps the other's changes.

Back, to change the options, forgets the question: installing asks again. An nfsms mod that copies files is first run without copying them, so they are copied only once the user has said to install it. A mod installed again over itself clashes with its own first installation, which makes the page the way to replace one version with another.

## In the editor and attribcli

The editor reads the list and the traces as it opens a database (none when the folder has no file), and View > Installed Mods shows them. File > Apply Script and File > Run nfsms Script trace the mod they apply and add it when Save All saves its changes; undoing the step takes it back off before then. The Apply Script prompt and the Log say when the list has the mod already. A mod that changes what a listed mod changed opens Mod Conflicts once it is applied: Uninstall the Ticked Mods, Keep Them Installed, or Undo it. The panel's Uninstall... works out what goes first, as the installer does, and its Uninstall Mod prompt warns of the mods that build on it (Mod Conflicts' Uninstall the Ticked Mods opens that prompt when there are any). Uninstalling is an undo step like any edit, and Save All writes the files and the list. File > Keep a List of Installed Mods turns adding to the list off (`TrackMods=` in `attribulina.ini`); a mod uninstalled still comes off it.

`attribcli apply` traces each script file it applies (not `-e` statements or standard input), adds it once saved, writes a copy of the list into `--out` when given, warns when the list has the mod already or when it changes what a listed mod changed, and leaves the list out with `--no-mod-list`. `attribcli mods <database>` prints the list, numbered, with what uninstalling each sets back, and `attribcli uninstall <database> <mod>...` uninstalls by number, title or script file name. When other mods build on those, it lists them and saves nothing, unless `--with-dependents` uninstalls them too.

## How it went

| Outcome | When | What the game's files are |
| --- | --- | --- |
| The mod is installed. | the script ran and its changes were saved | changed, originals as `.bak` |
| The mod is installed, but N lines of its script failed. | nfsms lines failed | the other lines' changes saved |
| The mod wasn't installed. | a mod script stopped, NFS-VltEd's checks refused an nfsms script, or saving failed | as they were, or, when saving failed halfway, the files before it saved (the Log names them) |

## The window, the Log and the log file

The steps are listed on the left, and Back and Next move between them. A step the mod or the game isn't ready for is greyed out. Installed Mods... and Advanced Options are at the bottom left on every step. A status line along the bottom says what is going on, coloured for warnings, errors and success. Show the Log opens the Log above it: every line of the run, the script's own messages (`mod.amod:4:1: error: ...`), each file saved, and as the last line how the installation went. Copy All copies it, and Open the Log File opens the file.

Reading the game's files, installing, working out an uninstall and uninstalling run on a thread of their own, so the window keeps drawing. While a mod is being installed, Cancel is greyed out, closing the window is refused, and the status line asks to wait.

Each run writes a log file named by the time it started, `AttribulinaModInstaller_2026-11-14_15-30-12.log`, in the folder the program runs in, else beside the program, else in the user's folder. Each line has its time and level, and the file is written as the lines come, so a crash still leaves it. The Log's first lines say where it is.

## The editor's log file

The editor writes its Log to `attribulina.log` in the user's folder (`core/LogFile.h`; portable mode: beside the program). It is written line by line, and the session before is kept as `attribulina.previous.log`. A second editor window writes `attribulina (2).log` while the first has its file open. On Windows the file is opened so others can read it but not write it; elsewhere no such check is made. The Log's first lines name the file.

## Not covered

- Putting back the files a mod copied. Uninstalling sets back the database only; the `.bak` files hold each file's first original for anything else.
- Uninstalling a mod installed before Attribulina kept traces: it has none, so it can only be taken off the list.
- A mod made of several scripts to run one after another. An nfsms script's `script` lines and a mod script's `include` statements cover that within one mod.
- Other platforms. The window is ImGui through `app::Host`, so another platform needs a window and a renderer for it, as the editor does, and the installer's code stays as it is.
