# Prácticas de Sistemas Operativos en C++

Cuatro programas, cada uno corresponde a un tema del sistema operativo.
Todo lo que se ve por pantalla es texto plano en una consola de Windows.

| Fichero | Tema | Qué demuestra |
|---|---|---|
| `01_vigilante_recursos.cpp` | Gestión de memoria y E/S | Consumo de RAM y CPU en tiempo real, con alerta y log al pasar del 80 % |
| `02_simulador_cache.cpp` | Jerarquía de memoria | Cachés L1 / L2 / RAM sobre `unordered_map`, y el coste real de leer del disco |
| `03_estres_memoria.cpp` | Memoria virtual y paginación | Llenar la memoria hasta forzar el salto al archivo de paginación |
| `04_prioridad_procesos.cpp` | Planificación de procesos | Cambiar la prioridad del propio proceso y comparar tiempos |

## Compilar

Con el compilador de MSYS2 (recomendado):

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc cmake
```

Y después, en esta carpeta:

```
compilar.bat
```

O con CMake:

```
cmake -B build
cmake --build build --config Release
```

Los ejecutables quedan en `build\`. El script detecta solo si tienes `g++` o
`cl`, así que funciona igual con MinGW y con Visual Studio.

El programa 3 necesita `psapi` (`GetProcessMemoryInfo`); el script ya lo añade
en ambos compiladores.

# Pruebas ejecucion

## Vigilante recursos:
<img width="852" height="341" alt="image" src="https://github.com/user-attachments/assets/b1ea06bb-4b54-48ee-8fd1-cdabcd8ceeb8" />

## cache:
<img width="1101" height="216" alt="Captura de pantalla 2026-10-01 183353" src="https://github.com/user-attachments/assets/2436af76-5436-4ccf-85a1-efc7cccb2fa9" />

<img width="897" height="547" alt="image" src="https://github.com/user-attachments/assets/0e9882d5-b9ef-4ca2-a1e4-571b136d38c8" />

## estres:
<img width="782" height="367" alt="image" src="https://github.com/user-attachments/assets/14c10e17-49ad-4ff0-a2b4-efb93137b291" />

## prioridad:
<img width="757" height="302" alt="image" src="https://github.com/user-attachments/assets/05a2df94-bdaa-4a3a-a4b1-aa70f2b5846f" />


