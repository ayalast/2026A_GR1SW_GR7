@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set ROOT=%~dp0..\..
cl /nologo /EHsc /std:c++20 /I"%ROOT%\src" /I"%ROOT%\Dependencias\include" ^
  "%~dp0test_survival_logic.cpp" "%~dp0audio_stubs.cpp" "%ROOT%\src\game_survival.cpp" ^
  /Fe:"%~dp0test_survival_logic.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
"%~dp0test_survival_logic.exe"
exit /b %ERRORLEVEL%
