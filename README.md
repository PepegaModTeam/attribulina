# Attribulina

Tool for EA's AttribSys database in Black Box's Need for Speed games.

## Features

- Support for many Black Box NFS games across multiple platforms
- Friendly UI - multiple tabs, browser-like navigation, favorites, hotkeys, search, cross-reference...
- nfsms script execution
- Custom scripting format (.amod)
- State Graph Builder replica - you can decompile, edit and compile new Lua code (supports 5.0.1 and 5.1.3)
- Class editing - you can create and/or remove classes and make your own types (with automatic C++ header generation)
- Vault editing - you can add, remove and rename vaults

## Compatibility

This tool is tested against these games (in game chronological order):

- Need for Speed Most Wanted OPM/OXM Demo - PS2, Xbox
- Need for Speed Most Wanted - PC, PS2, GameCube, Xbox 360
- Need for Speed Carbon Demo - PS2 (OPM)
- Need for Speed Carbon Beta - PS3 (Alpha 56)
- Need for Speed Carbon - PS3
- Need for Speed Carbon - PC, GameCube, Wii, Xbox 360 (XBLA demo)
- Need for Speed ProStreet - PC, PS2, Wii, Xbox 360
- Need for Speed Undercover (Last Gen) - Wii
- Need for Speed Undercover - PC, Xbox 360
- Need for Speed World 2009 Beta - PC
- Need for Speed World 2010 Beta - PC
- Need for Speed Nitro - Wii (partial support)
- Need for Speed World - PC

These games/platforms should be implicitly supported (currently untested, may be misidentified by the tool):

- Need for Speed Most Wanted - Xbox
- Need for Speed ProStreet - PS3
- Need for Speed Undercover (Last Gen) - PS2
- Need for Speed Undercover - PS3

## Hotkeys

| Keys                 | In the Explorer, on the focused item |
| -------------------- | ------------------------------------ |
| Enter, Ctrl+Enter    | open, open in a background tab       |
| Ctrl+C, Ctrl+Shift+C | copy, copy with everything under it  |
| Ctrl+X               | cut (paste to move it)               |
| Ctrl+V, Ctrl+Shift+V | paste next to it, paste as its child |
| Ctrl+D               | duplicate                            |
| Delete               | delete                               |
| F2                   | rename                               |
| Ctrl+N               | new collection under it              |
| Shift+F12            | find references                      |

| Keys                           | Anywhere                                                       |
| ------------------------------ | -------------------------------------------------------------- |
| Ctrl+O                         | open a folder                                                  |
| Ctrl+Shift+O                   | open a file                                                    |
| Ctrl+S                         | save all                                                       |
| Ctrl+Z; Ctrl+Y or Ctrl+Shift+Z | undo, redo (in a text field: its text)                         |
| Ctrl+F                         | filter the Explorer (or the fields, from the collection panel) |
| Ctrl+Shift+F                   | search the database                                            |
| Alt+Left, Alt+Right            | back, forward                                                  |
| Ctrl+T, Ctrl+W                 | new tab, close tab                                             |
| Ctrl+PgDn, Ctrl+PgUp           | next tab, previous tab                                         |
| Ctrl+Shift+N                   | new window                                                     |

## Portable mode

By default, Attribulina runs with per-user specific settings.

If you want to run Attribulina in a portable fashion, simply place/create a "attribulina.ini" file next to the executable.

Please note that attribcli mentioned below is always portable, so you will need to place your keys and data right next to it.

## attribcli

`attribcli.exe` builds next to the editor and does the same with mod scripts from a console or a batch file:

```
attribcli apply GLOBAL mod.amod --set power=1.2      # keeps <file>.bak originals
attribcli apply GLOBAL mod.amod --out build\GLOBAL   # leaves GLOBAL as it is
attribcli diff vanilla\GLOBAL GLOBAL -o mine.amod    # the script that makes your changes
attribcli export GLOBAL pvehicle/911gt3 --children -o 911.amod
attribcli get GLOBAL pvehicle/street HornType
```

`attribcli help` lists every command; `doc/scripts.md` describes them and the script language.

# AI/LLM Usage Disclosure

This project used AI/LLM to assist with:

- code scaffolding
- automated code tests
- debugging
- format research and documentation
- other boilerplate work

No AI generated artwork is used in this project.

# Credits

See [CREDITS.md](./CREDITS.md)
