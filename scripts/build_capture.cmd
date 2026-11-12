@echo off
rem Builds src\tools\frame_diff_capture.cpp into build\tools\frame_diff_capture.exe (x64, /MT).
setlocal
set "ROOT=%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with C++ x64 tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\tools" mkdir "%ROOT%\build\tools"
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE ^
   /Fo"%ROOT%\build\tools\\" /Fe"%ROOT%\build\tools\frame_diff_capture.exe" ^
   "%ROOT%\src\tools\frame_diff_capture.cpp" /link /NOLOGO || exit /b 1
echo Built %ROOT%\build\tools\frame_diff_capture.exe
