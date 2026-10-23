# Building (Windows)

1. Run `premake5_vs2022.bat`. It writes `build\Attribulina.sln` with the bundled premake (`premake\premake5.exe vs2026` makes a Visual Studio 2026 solution instead).
2. Open the solution and build `Attribulina` (x64, Debug or Release) with MSVC.
3. Run `bin\Release\Attribulina.exe`. The build copies `data\` next to it:
   - `attrib_keys.txt` and `bin_keys\` turn StringHash32 and bStringHash values back into names;
   - `schema\` holds the game's struct and enum layouts;
   - `messages\` lists the game messages that gameplay scripts handle.
