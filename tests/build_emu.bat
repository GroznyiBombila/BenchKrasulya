@echo off
rem Test build with hardware emulation (-DBK_EMU): tests\BenchKrasulya_emu.exe
setlocal
cd /d "%~dp0\.."
if exist C:\mingw64\bin set PATH=C:\mingw64\bin;%PATH%
set FLAGS=-O2 -std=c++20 -DUNICODE -D_UNICODE -municode -fno-tree-loop-distribute-patterns -Wall -DBK_EMU
set O=build\emu
if not exist %O% mkdir %O%
if not exist build\nomanifest mkdir build\nomanifest
echo.> build\nomanifest\empty.c
gcc -c build\nomanifest\empty.c -o build\nomanifest\default-manifest.o || goto :err
windres -i res\app.rc -o %O%\app.o -O coff --include-dir res || goto :err
for %%f in (sysinfo bench chart main kernels_sse2) do g++ %FLAGS% -c src\%%f.cpp -o %O%\%%f.o || goto :err
g++ %FLAGS% -mavx -c src\kernels_avx.cpp -o %O%\kernels_avx.o || goto :err
g++ %FLAGS% -mavx512f -c src\kernels_avx512.cpp -o %O%\kernels_avx512.o || goto :err
g++ %FLAGS% -c tests\emu.cpp -o %O%\emu.o || goto :err
g++ %FLAGS% -Bbuild/nomanifest/ -mwindows -static -s -o tests\BenchKrasulya_emu.exe %O%\*.o -lcomctl32 -lgdiplus -lgdi32 -luser32 -ladvapi32 -lcomdlg32 -lole32 -lshell32 -lpowrprof || goto :err
echo OK: tests\BenchKrasulya_emu.exe
exit /b 0
:err
echo BUILD FAILED
exit /b 1
