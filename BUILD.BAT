@echo off
rem Build the two C89 tools with Microsoft Visual C++ 6.0.
rem Output goes to build\ so the original reference EXEs stay untouched.
call "C:\Program Files (x86)\Microsoft Visual Studio\VC98\Bin\VCVARS32.BAT"
if not exist build mkdir build
cl /nologo /W3 /O2 /Fobuild\ /Febuild\EXPAND.EXE expand.c mscompress.c msinflate.c
if errorlevel 1 goto fail
cl /nologo /W3 /O2 /Fobuild\ /Febuild\COMPRESS.EXE compress.c lzencode.c
if errorlevel 1 goto fail
echo Built build\EXPAND.EXE and build\COMPRESS.EXE
goto end
:fail
echo Build failed
:end
