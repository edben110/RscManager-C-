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

## 1. Vigilante de recursos

Mide RAM, CPU y memoria comprometida con `GlobalMemoryStatusEx` y
`GetSystemTimes`, y refresca un panel en la misma pantalla.

```
build\vigilante.exe --umbral 80 --intervalo 1000 --duracion 0
```

Opciones:

- `--umbral <pct>` porcentaje de RAM que dispara la alerta (80 por defecto)
- `--intervalo <ms>` cada cuánto se muestrea (1000)
- `--duracion <s>` cuánto dura; 0 = hasta Ctrl+C
- `--salida <archivo>` log `.txt` (por defecto `alertas_memoria.txt`)
- `--relog <s>` vuelve a escribir mientras la RAM siga alta; 0 = en cada muestra

Ctrl+C o `Q` detienen el programa. Al escribir en el log se anota la fecha, el
porcentaje de RAM, la RAM libre, el compromiso de memoria y el de CPU.

Sobre el 80 % de RAM en un equipo con muchas pestañas abiertas esto no es raro,
así que para ver la alerta en acción basta con dejar el umbral bajo:

```
build\vigilante.exe --umbral 20 --intervalo 500 --duracion 5
```

## 2. Simulador de caché

Reproduce la jerarquía L1 → L2 → RAM → disco. Los ficheros se trocean en
bloques de 16 KB y el bloque es la unidad que sube por los niveles, como una
línea de caché o una página. Las tablas hash son `std::unordered_map` y la
política de expulsión es LRU, montada con `std::list`.

```
build\cache.exe --archivos 6 --tamano 2048 --iteraciones 5
```

Opciones:

- `--archivos <n>`, `--tamano <kb>` ficheros a generar (6 × 2 MB)
- `--iteraciones <n>` pasadas sobre el conjunto (5)
- `--bloque <kb>` tamaño de bloque (16)
- `--l1 <kb>`, `--l2 <kb>`, `--ram <kb>` capacidades de cada nivel
- `--caliente <pct>` porcentaje de accesos sobre una zona repetida (50)
- `--semilla <n>` semilla del patrón, para reproducir la ejecución
- `--sin-cache-os` leer del disco saltando la caché de archivos de Windows
- `--carpeta <ruta>` dónde deja los datos (por defecto `datos_cache`)

El programa mide tres cosas: leer siempre desde disco, la primera pasada con
las cachés frías, y las pasadas ya calientes. Al final compara cada bloque
guardado en la caché con el que hay en el disco, para comprobar que salen
exactamente los mismos datos.

Dos detalles que conviene mirar:

- Con `--caliente 0` el recorrido es puramente secuencial y L1 y L2 no aciertan
  casi nunca. Es lo que pasa de verdad: un barrido sin repeticiones no aprovecha
  la caché del procesador.
- La ganancia sobre disco sale pequeña porque Windows ya cachea los ficheros
  en RAM. Con `--sin-cache-os` (usando `FILE_FLAG_NO_BUFFERING`) se lee del
  disco de verdad y la diferencia crece.

## 3. Estrés de memoria

Llena un `std::vector<std::string>` con millones de cadenas hasta agotar la RAM
y forzar el uso del archivo de paginación.

Antes de arrancar comprueba que exista archivo de paginación, y avisa si no lo hay.

**Este programa consume memoria de verdad.** Lleva cuatro límites de seguridad y
todos están activos por defecto:

1. `--limite-mb` tope de memoria comprometida por el proceso (4096)
2. `--margen-mb` para si la RAM libre del sistema baja de este valor (1024)
3. `--segundos` tiempo máximo de relleno (60)
4. Ctrl+C detiene en cualquier momento y libera todo

Además, con `--aviso-libre-mb` avisa y ralentiza el relleno antes de llegar al
margen, para dejar margen a Windows.

```
build\estres.exe --limite-mb 2048 --aviso-libre-mb 3072 --margen-mb 2048 --retener 20
```

El programa tiene tres fases: rellena, mantiene la memoria asignada un rato
(`--retener`) para que puedas mirarla en el administrador de tareas, y libera.

Para observar el salto a la memoria virtual, abre el administrador de tareas con
Ctrl+Shift+Esc y ve a **Rendimiento → Memoria**. Mira dos números:

- **En uso**: la RAM física ocupada
- **Enviado**: el compromiso total, RAM más archivo de paginación

Cuando *Enviado* supera el tamaño de la RAM, la diferencia está respaldada por
el archivo de paginación, y eso es la memoria virtual. El apartado *Paginado*
muestra el ritmo de entrada y salida al disco.

El programa recorre las cadenas después de cada tanda para que sus páginas sigan
residentes. Sin eso Windows podría recortar el conjunto de trabajo, la RAM
libera bajaría y no habría paginación real que ver.

Si ya tienes archivo de paginación y quieres ver la diferencia más clara:

```
build\estres.exe --limite-mb 8192 --aviso-libre-mb 2048 --margen-mb 1024 --retener 30
```

## 4. Prioridad de procesos

Ejecuta un cálculo matemático pesado (criba de Eratosthenes y conjunto de
Mandelbrot) cambiando la prioridad del propio proceso con `SetPriorityClass`.

```
build\prioridad.exe --clase baja       --afinidad 0 --etiqueta BAJA
build\prioridad.exe --clase tiempo_real --afinidad 0 --etiqueta REALTIME
```

Clases disponibles, de más baja a más alta: `baja`, `below`, `normal`, `above`,
`alta`, `tiempo_real`.

**Usa `--afinidad` en las dos instancias.** Es lo importante: si las fijas al
mismo núcleo, la única diferencia entre ellas es la prioridad. Sin eso, el
reparto depende del planificador y no se puede concluir nada.

Para arrancar a la vez y que compitan de verdad:

```
build\prioridad.exe --clase baja       --afinidad 0 --esperar 5 --iteraciones 6
build\prioridad.exe --clase tiempo_real --afinidad 0 --esperar 5 --iteraciones 6
```

Cada instancia imprime su PID, la prioridad solicitada, la que el kernel
confirmó con `GetPriorityClass`, el tiempo de reloj y el de CPU. La diferencia
se ve en dos sitios: el tiempo total y el porcentaje de uso del procesador. Con
`baja` el kernel interrumpe el proceso en cuanto hay algo mejor que hacer, así
que su porcentaje de CPU rara vez pasa del 10-15 %; con `tiempo real` se queda
cerca del 100 %.

Dos avisos:

- `tiempo real` requiere ejecutar el programa como administrador. Windows
  deniega esa clase a un proceso normal; el programa lo detecta, explica el
  error y sugiere `alta`, que no necesita permisos especiales.
- Con `tiempo real` el proceso se ejecuta casi sin interrupciones y puede
  colgar el escritorio. Úsalo en pruebas cortas.

El cálculo da exactamente el mismo resultado en cualquier ejecución y en
cualquier compilador, sin `sqrt` ni potencias, para que la comparación de
tiempos sea justa y reproducible.

## Nota sobre las medidas de tiempo

El enunciado menciona `console.time()` (PHP) y `time.time()` (Python). En C++
el equivalente es `std::chrono::steady_clock`, que es monótono: no le afecta un
cambio de hora del sistema. Aquí se usa en los programas 2 y 4.

# Pruebas ejecucion