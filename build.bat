@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul || call :vcvars || exit /b 1
if not exist build mkdir build
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /LD src\emifix.c /Fobuild\ /Fe:build\dinput.dll /link /DEF:src\emifix.def opengl32.lib gdi32.lib user32.lib || exit /b 1
cl /nologo /W3 /MT /D_CRT_SECURE_NO_WARNINGS tests\test.c /Fobuild\ /Fe:build\test.exe user32.lib || exit /b 1
exit /b 0

:vcvars
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
where cl >nul 2>nul
