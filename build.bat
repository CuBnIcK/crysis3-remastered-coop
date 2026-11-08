@echo off
rem Builds build\c3coop.dll with Visual Studio 2022 (x64, static CRT).
setlocal
set ROOT=%~dp0
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%build\obj" mkdir "%ROOT%build\obj"
set MH=%ROOT%third_party\minhook
cl /nologo /c /O2 /MT /W3 /Fo"%ROOT%build\obj\\" /I"%MH%\include" ^
  "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" || exit /b 1
cl /nologo /LD /O2 /MT /W3 /EHsc /std:c++20 /Zi /Fo"%ROOT%build\obj\\" /Fd"%ROOT%build\obj\\" /I"%MH%\include" ^
  "%ROOT%src\c3coop\dllmain.cpp" "%ROOT%src\c3coop\hud.cpp" "%ROOT%src\c3coop\anim.cpp" "%ROOT%src\c3coop\coop.cpp" ^
  "%ROOT%src\c3coop\steam.cpp" "%ROOT%src\c3coop\sp.cpp" "%ROOT%src\c3coop\fgmirror.cpp" ^
  "%ROOT%build\obj\buffer.obj" "%ROOT%build\obj\hook.obj" ^
  "%ROOT%build\obj\trampoline.obj" "%ROOT%build\obj\hde64.obj" ^
  /link /DEBUG /OUT:"%ROOT%build\c3coop.dll" /PDB:"%ROOT%build\c3coop.pdb" /PDBALTPATH:c3coop.pdb || exit /b 1
echo built %ROOT%build\c3coop.dll
rem Steam launch option wrapper: "<repo>\build\c3coop_launch.exe" %command%
cl /nologo /O2 /MT /W3 /EHsc /std:c++20 /Fo"%ROOT%build\obj\\" "%ROOT%src\launcher\launcher.cpp" ^
  /link /OUT:"%ROOT%build\c3coop_launch.exe" || exit /b 1
echo built %ROOT%build\c3coop_launch.exe
