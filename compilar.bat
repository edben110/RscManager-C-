@echo off
setlocal

rem ===================================================================
rem  Compila las 4 practicas.
rem  Usa g++ (MinGW / MSYS2 UCRT64) o cl (MSVC), segun este disponible.
rem
rem  Uso:  compilar.bat
rem ===================================================================

where g++ >nul 2>&1
if errorlevel 1 goto :msvc
goto :gcc

rem --------------------------------------------------------------- GCC
:gcc
if not exist build mkdir build

echo [1/4] Compilando 01_vigilante_recursos.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra 01_vigilante_recursos.cpp -o build/vigilante.exe
if errorlevel 1 goto :fallo

echo [2/4] Compilando 02_simulador_cache.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra 02_simulador_cache.cpp -o build/cache.exe
if errorlevel 1 goto :fallo

echo [3/4] Compilando 03_estres_memoria.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra 03_estres_memoria.cpp -o build/estres.exe -lpsapi
if errorlevel 1 goto :fallo

echo [4/4] Compilando 04_prioridad_procesos.cpp ...
g++ -std=c++17 -O2 -Wall -Wextra 04_prioridad_procesos.cpp -o build/prioridad.exe
if errorlevel 1 goto :fallo

echo.
echo Listo. Ejecutables en la carpeta build\ :
dir /b build\*.exe
exit /b 0

rem --------------------------------------------------------------- MSVC
:msvc
where cl >nul 2>&1
if errorlevel 1 goto :sin_compilador

echo Detectado MSVC. Compilando con cl ...
if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /O2 /W4 01_vigilante_recursos.cpp  /Fe:build\vigilante.exe
if errorlevel 1 goto :fallo
cl /nologo /std:c++17 /EHsc /O2 /W4 02_simulador_cache.cpp     /Fe:build\cache.exe
if errorlevel 1 goto :fallo
cl /nologo /std:c++17 /EHsc /O2 /W4 03_estres_memoria.cpp      /Fe:build\estres.exe /link psapi.lib
if errorlevel 1 goto :fallo
cl /nologo /std:c++17 /EHsc /O2 /W4 04_prioridad_procesos.cpp  /Fe:build\prioridad.exe
if errorlevel 1 goto :fallo

echo.
echo Listo. Ejecutables en la carpeta build\ :
dir /b build\*.exe
exit /b 0

rem --------------------------------------------------------------- Ninguno
:sin_compilador
echo.
echo  No se encontro g++ ni cl.
echo.
echo  Opcion A - MSYS2 UCRT64 (recomendado):
echo    1. Abre "MSYS2 UCRT64" desde el menu Inicio.
echo    2. pacman -S --needed mingw-w64-ucrt-x86_64-gcc cmake
echo    3. cd a esta carpeta y ejecuta  compilar.bat
echo.
echo  Opcion B - Visual Studio:
echo    1. Abre "Developer Command Prompt for VS".
echo    2. cd a esta carpeta y ejecuta  cmake -B build ^&^& cmake --build build --config Release
echo.
exit /b 1

:fallo
echo.
echo COMPILACION FALLIDA. Revisa los errores de arriba.
exit /b 1