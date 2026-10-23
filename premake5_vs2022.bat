@echo off
rem Generates build\Attribulina.sln for Visual Studio 2022 (MSVC, v143).
rem Uses premake\premake5.exe, or premake5 from PATH if that copy is missing.
rem For Visual Studio 2026, run: premake\premake5.exe vs2026
setlocal
cd /d "%~dp0"
set PREMAKE=premake\premake5.exe
if not exist "%PREMAKE%" set PREMAKE=premake5
"%PREMAKE%" vs2022 %*
if errorlevel 1 (
    pause
    exit /b 1
)
echo.
echo Generated build\Attribulina.sln
endlocal
