@echo off
setlocal

REM === Вариант 1: MSVC (если установлена Visual Studio / Build Tools) ===
where cl >nul 2>nul
if %errorlevel%==0 (
    echo [build] using MSVC
    cl /nologo /EHsc /O2 /std:c++17 main.cpp /Fe:agent.exe ws2_32.lib
    goto done
)

REM === Вариант 2: MinGW-w64 g++ ===
where g++ >nul 2>nul
if %errorlevel%==0 (
    echo [build] using g++
    g++ -O2 -std=c++17 main.cpp -o agent.exe -lws2_32
    goto done
)

echo ERROR: ни cl, ни g++ не найдены. Запустите из "x64 Native Tools Command Prompt" или установите MinGW-w64.
exit /b 1

:done
echo [build] done: agent.exe
