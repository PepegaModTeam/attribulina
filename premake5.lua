-- Attribulina build definition.
--
-- Windows (MSVC):  premake\premake5.exe vs2022   ->  build\Attribulina.sln
-- Linux (tests):   premake5 gmake && make -C build config=release
--
-- The editor (Attribulina) is Windows-only for now: Win32 + Direct3D 11.
-- Everything else (core library, UI code, tests) builds on any platform, and
-- the tests run the UI headless.

-- --outdir=DIR puts build/ and bin/ under DIR instead of the repo root, so
-- two builds of the same tree don't share object files.
newoption {
    trigger = "outdir",
    value = "DIR",
    description = "Directory for build/ and bin/ (default: the repo root)",
}
local outdir = _OPTIONS["outdir"] and path.getabsolute(_OPTIONS["outdir"]) or "."

workspace "Attribulina"
    configurations { "Debug", "Release" }
    -- x64 for the Windows editor; elsewhere, whatever the host compiler targets
    -- (the dev machine is aarch64 Linux).
    if os.target() == "windows" then
        platforms { "x64" }
    else
        platforms { "native" }
    end
    location(outdir .. "/build")
    startproject "Attribulina"

    language "C++"
    cppdialect "C++20"
    targetdir(outdir .. "/bin/%{cfg.buildcfg}")
    objdir(outdir .. "/build/obj/%{cfg.buildcfg}/%{prj.name}")
    staticruntime "On"

    filter "platforms:x64"
        architecture "x86_64"

    filter "configurations:Debug"
        symbols "On"
        optimize "Off"
        defines { "_DEBUG" }

    filter "configurations:Release"
        symbols "On"
        optimize "Speed"
        defines { "NDEBUG" }

    filter "toolset:msc*"
        buildoptions { "/utf-8", "/permissive-", "/Zc:__cplusplus" }
        defines { "_CRT_SECURE_NO_WARNINGS", "NOMINMAX" }
        multiprocessorcompile "On"

    filter {}

project "lzcompress"
    kind "StaticLib"
    warnings "Off"
    files { "src/lzcompress/**.h", "src/lzcompress/**.cpp" }

-- Lua 5.0.1, the games' script compiler, built with lua_Number = float. Each
-- source is compiled as C++ inside namespace lua50 (external/lua50/*.cpp
-- wrap src/*.c), so another Lua version can be linked in beside it.
project "lua50"
    kind "StaticLib"
    warnings "Off"
    files { "external/lua50/*.cpp", "external/lua50/*.h", "external/lua50/src/*.h" }

-- Lua 5.1.3, Undercover's and World's script compiler, built the same way in
-- namespace lua51 (external/lua51/*.cpp wrap src/*.c). The two cores share
-- no symbols, so both link into every program.
project "lua51"
    kind "StaticLib"
    warnings "Off"
    files { "external/lua51/*.cpp", "external/lua51/*.h", "external/lua51/src/*.h" }

project "imgui"
    kind "StaticLib"
    warnings "Off"
    includedirs { "external/imgui" }
    files {
        "external/imgui/*.h", "external/imgui/*.cpp",
        "external/imgui/misc/cpp/imgui_stdlib.*",
    }
    filter "system:windows"
        files {
            "external/imgui/backends/imgui_impl_win32.*",
            "external/imgui/backends/imgui_impl_dx11.*",
        }
    filter {}

project "attribcore"
    kind "StaticLib"
    warnings "Extra"
    includedirs { "src", "include", "external/lua50", "external/lua51" }
    files { "src/core/**.h", "src/core/**.cpp" }
    filter "system:windows"
        prebuildcommands { "@echo off\r\n" .. 
            "cd $(ProjectDir)..\r\n" ..
            "python generate_build_number.py" }
    filter {}
    
-- The editor UI: platform-independent ImGui code driven by a host.
project "attribui"
    kind "StaticLib"
    warnings "Extra"
    includedirs { "src", "include", "external/imgui", "external/imgui/misc/cpp" }
    files { "src/app/**.h", "src/app/**.cpp" }

project "Attribulina"
    kind "WindowedApp"
    warnings "Extra"
    includedirs { "src", "external/imgui", "external/imgui/backends", "external/imgui/misc/cpp" }
    links { "attribui", "attribcore", "lua50", "lua51", "lzcompress", "imgui" }
    filter "system:windows"
        resincludedirs { "res" }
        files { "res/attribulina.rc", "res/attribulina.ico" }
        files { "src/platform/win32/**.h", "src/platform/win32/**.cpp", "src/platform/win32/**.rc" }
        defines { "UNICODE", "_UNICODE" }
        links { "d3d11", "dxgi", "d3dcompiler", "dwmapi", "user32", "gdi32", "ole32", "shell32", "uuid", "comdlg32", "imm32", "windowscodecs" }
        entrypoint "wWinMainCRTStartup"
        -- data/ (key names, type schemas) next to the exe, where the editor
        -- looks for it. Build events run in the project directory (build/).
        postbuildcommands { '{COPYDIR} "../data" "%{cfg.buildtarget.directory}data"'  }
    filter "system:not windows"
        -- Placeholder until the Vulkan/Wayland host exists.
        kind "None"
    filter {}

-- The command line tool: mod scripts and database queries (src/cli/Cli.h).
project "attribcli"
    kind "ConsoleApp"
    warnings "Extra"
    includedirs { "src", "include" }
    files { "src/cli/**.h", "src/cli/**.cpp" }
    filter "system:not windows"
        -- Placeholder until the Vulkan/Wayland host exists.
        kind "None"
    filter {}
    links { "attribcore", "lua50", "lua51", "lzcompress" }
    filter "system:windows"
        -- data/ next to the exe, where it looks for it.
         postbuildcommands { '{COPYDIR} "../data" "%{cfg.buildtarget.directory}data"' }

-- The tests compile ImGui and the UI themselves, with ImGui's test-engine
-- hooks on, so tests/UiProbe.cpp sees every widget (IDs, labels, rects).
project "attribtests"
    kind "ConsoleApp"
    warnings "Extra"
    defines { "IMGUI_ENABLE_TEST_ENGINE" }
    includedirs { "src", "include", "external/imgui", "external/imgui/backends", "external/imgui/misc/cpp" }
    files {
        "tests/**.h", "tests/**.cpp",
        "src/app/**.h", "src/app/**.cpp",
        "src/platform/headless/**.h", "src/platform/headless/**.cpp",
        "src/cli/Cli.h", "src/cli/Cli.cpp",
        "external/imgui/*.h", "external/imgui/*.cpp",
        "external/imgui/misc/cpp/imgui_stdlib.*",
        "external/imgui/backends/imgui_impl_null.*",
    }
    filter "files:external/**.cpp"
        warnings "Off"
    filter {}
    links { "attribcore", "lua50", "lua51", "lzcompress" }
    debugdir "."
