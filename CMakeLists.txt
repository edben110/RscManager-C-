# =====================================================================
#  Sistema Operativo - 4 practicas en C++
#  Compilacion con CMake
# =====================================================================
#
#  Uso:
#    cmake -B build
#    cmake --build build --config Release
#
#  Salida:
#    build/Release/vigilante.exe
#    build/Release/cache.exe
#    build/Release/estres.exe
#    build/Release/prioridad.exe
#
#  El ejecutable 3 enlaza con psapi (GetProcessMemoryInfo). En MSVC la
#  libreria se llama psapi.lib; en MinGW, -lpsapi.
# =====================================================================

cmake_minimum_required(VERSION 3.15)
project(so_practicas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- 1. Vigilante de recursos (RAM y CPU en tiempo real) ---
add_executable(vigilante 01_vigilante_recursos.cpp)
if(MSVC)
  target_compile_options(vigilante PRIVATE /W4)
else()
  target_compile_options(vigilante PRIVATE -Wall -Wextra)
endif()

# --- 2. Simulador de cache L1/L2/RAM ---
add_executable(cache 02_simulador_cache.cpp)
if(MSVC)
  target_compile_options(cache PRIVATE /W4)
else()
  target_compile_options(cache PRIVATE -Wall -Wextra)
endif()

# --- 3. Estres de memoria y salto a la virtual ---
add_executable(estres 03_estres_memoria.cpp)
if(MSVC)
  target_compile_options(estres PRIVATE /W4)
  target_link_libraries(estres PRIVATE psapi)
else()
  target_compile_options(estres PRIVATE -Wall -Wextra)
  find_library(PSAPI_LIB psapi)
  if(PSAPI_LIB)
    target_link_libraries(estres PRIVATE ${PSAPI_LIB})
  endif()
endif()

# --- 4. Prioridad de procesos ---
add_executable(prioridad 04_prioridad_procesos.cpp)
if(MSVC)
  target_compile_options(prioridad PRIVATE /W4)
else()
  target_compile_options(prioridad PRIVATE -Wall -Wextra)
endif()