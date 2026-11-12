@echo off
rem Builds src\probe\kh2vr_probe.cpp into build\probe\kh2vr_probe.dll (x64, /MT, no runtime DLL deps).
rem Uses the newest Visual Studio with the C++ x64 tools (found via vswhere).
setlocal
set "ROOT=%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with C++ x64 tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\probe" mkdir "%ROOT%\build\probe"
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /LD /utf-8 /I"%ROOT%\third_party\openxr\include" ^
   /Fo"%ROOT%\build\probe\\" /Fe"%ROOT%\build\probe\kh2vr_probe.dll" ^
   "%ROOT%\src\probe\kh2vr_probe.cpp" /link /NOLOGO /MAP:"%ROOT%\build\probe\kh2vr_probe.map" user32.lib advapi32.lib || exit /b 1
echo Built %ROOT%\build\probe\kh2vr_probe.dll
