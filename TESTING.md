## Tests (any platform)

```bash
premake5 gmake && make -C build config=release_native -j4
./bin/Release/attribtests
```

The tests read the retail databases under `attrib/databases/`, round-trip them byte for byte, and run the editor UI headless.

The database tree is expected to look like this:

```
attrib/databases
├───360
│   ├───carbon-latedemo
│   ├───mw-beta
│   ├───prostreet-vanilla
│   └───undercover-vanilla
├───gamecube
│   ├───carbon-vanilla
│   └───mw-vanilla
├───pc
│   ├───carbon-vanilla
│   ├───carbon-vlted
│   ├───mw-vanilla
│   ├───prostreet-vanilla
│   ├───prostreet-vlted
│   ├───undercover-vanilla
│   ├───world-2009-beta
│   │   └───gc.vaults
│   ├───world-2010-beta
│   │   └───gc.vaults
│   └───world-final
│       └───gc.vaults
│           └───Team Escape Opponents
│               ├───1 Opponent
│               ├───2 Opponents
│               └───3 Opponents
├───ps2
│   ├───carbon-demo
│   ├───carbon-vanilla
│   ├───mw-beta
│   ├───mw-early-opm-betademo
│   └───prostreet-vanilla
├───ps3
│   ├───carbon-latebeta
│   └───carbon-vanilla
├───wii
│   ├───carbon-vanilla
│   ├───nitro-vanilla
│   ├───prostreet-vanilla
│   └───undercover-vanilla
└───xbox
    └───mw-early-oxm-betademo
```

With the database files inside them (from GLOBAL, copy Attributes.bin, FE_ATTRIB.BIN, cars_vault.bin, Gameplay.bin, Gameplay.lzc, gc.vaults, etc.).

`doc/vault-format.md` describes the file format, `doc/gameplay.md` the gameplay database, `doc/lua.md` the compiled scripts, `doc/lua-api.md` the functions they can call, `doc/scripts.md` mod scripts and attribcli, and `doc/nfsms.md` the nfsms support.
