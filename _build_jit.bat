@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cmake --build build -j 8 > C:\Users\lenovo\Desktop\build.txt 2>&1
echo BUILD_EXIT=%ERRORLEVEL% >> C:\Users\lenovo\Desktop\build.txt
