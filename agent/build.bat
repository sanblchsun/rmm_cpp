@echo off
setlocal

REM ==== Вариант 1: MSVC (Visual Studio / Build Tools) ====
REM Даёт самодостаточный .exe (VC runtime есть в любой современной Windows).
where cl >nul 2>nul
if %errorlevel%==0 (
    echo [build] using MSVC
    cl /nologo /EHsc /O2 /std:c++17 /MT main.cpp /Fe:agent.exe /link ws2_32.lib
    if errorlevel 1 exit /b 1
    del /q main.obj 2>nul
    echo [build] done: agent.exe  ^(standalone^)
    goto :eof
)

REM ==== Вариант 2: MinGW-w64 g++ (СТАТИЧЕСКАЯ сборка) ====
where g++ >nul 2>nul
if %errorlevel%==0 (
    echo [build] using g++ ^(static^)
    g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ ^
        main.cpp -o agent.exe -lws2_32
    if errorlevel 1 exit /b 1
    echo [build] done: agent.exe  ^(standalone^)
    goto :eof
)

echo ERROR: ни cl, ни g++ не найдены.
echo   - MSVC: запустите build.bat из "x64 Native Tools Command Prompt for VS"
echo   - MinGW: установите msys2/mingw-w64 и добавьте g++ в PATH
exit /b 1
