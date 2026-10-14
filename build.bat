@echo off
rem Build BenchKrasulya with MinGW-w64 g++ x86_64 (C:\mingw64\bin)
setlocal
cd /d "%~dp0"
if exist C:\mingw64\bin set PATH=C:\mingw64\bin;%PATH%
set FLAGS=-O2 -std=c++20 -DUNICODE -D_UNICODE -municode -fno-tree-loop-distribute-patterns -Wall
if not exist build\nomanifest mkdir build\nomanifest
rem empty default-manifest.o: stop MinGW from adding its own manifest (we embed res\app.manifest)
echo.> build\nomanifest\empty.c
gcc -c build\nomanifest\empty.c -o build\nomanifest\default-manifest.o || goto :err
windres -i res\app.rc -o build\app.o -O coff --include-dir res || goto :err
g++ %FLAGS% -c src\sysinfo.cpp -o build\sysinfo.o || goto :err
g++ %FLAGS% -c src\bench.cpp -o build\bench.o || goto :err
g++ %FLAGS% -c src\chart.cpp -o build\chart.o || goto :err
g++ %FLAGS% -c src\main.cpp -o build\main.o || goto :err
g++ %FLAGS% -c src\kernels_sse2.cpp -o build\kernels_sse2.o || goto :err
g++ %FLAGS% -mavx -c src\kernels_avx.cpp -o build\kernels_avx.o || goto :err
g++ %FLAGS% -mavx512f -c src\kernels_avx512.cpp -o build\kernels_avx512.o || goto :err
g++ %FLAGS% -Bbuild/nomanifest/ -mwindows -static -s -o BenchKrasulya.exe build\*.o -lcomctl32 -lgdiplus -lgdi32 -luser32 -ladvapi32 -lcomdlg32 -lole32 -lshell32 -lpowrprof || goto :err
echo OK: BenchKrasulya.exe
exit /b 0
:err
echo BUILD FAILED
exit /b 1
