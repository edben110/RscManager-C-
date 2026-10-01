/*
 * =====================================================================
 *  4. PRIORIDAD DE PROCESOS  -  Planificacion del kernel
 * =====================================================================
 *  Concepto : Planificacion de procesos del kernel.
 *  Objetivo : Ver que no todos los procesos son iguales para el SO y que
 *             se puede influir en su "importancia" desde el propio
 *             programa.
 *
 *  Tarea    : Usar la API de Windows para cambiar la prioridad de un
 *             proceso que este haciendo un calculo matematico pesado.
 *  Reto     : Ejecutar dos instancias del mismo script, una con prioridad
 *             "Baja" y otra con "Tiempo Real", y comparar cual termina
 *             el calculo antes.
 *
 *  Libreria : Win32 SetPriorityClass() / GetPriorityClass()   -> clase del proceso
 *             Win32 SetProcessAffinityMask()                  -> nucleo de CPU
 *             Win32 GetProcessTimes()                         -> tiempos de CPU
 *             Win32 ProcessTimesToInputString() / FileTimeToSystemTime()
 *
 *  Para una ejecucion pareja hay que aislar una sola variable: la
 *  prioridad. Por eso las dos instancias se lanzan con --afinidad para
 *  fijarlas al MISMO nucleo. Si no se hace eso, el reparto depende del
 *  planificador y no se puede concluir nada.
 *
 *  Clases de prioridad de Windows, de mas baja a mas alta:
 *    IDLE_PRIORITY_CLASS    "Baja"        se ejecuta solo cuando el SO no
 *                                        tiene nada mejor que darle
 *    BELOW_NORMAL           "Below normal"
 *    NORMAL_PRIORITY_CLASS  "Normal"
 *    ABOVE_NORMAL           "Above normal"
 *    HIGH_PRIORITY_CLASS    "Alta"
 *    REALTIME_PRIORITY_CLASS"Tiempo real" se adelanta a casi todo y puede
 *                                        colgar el SO; requiere
 *                                        administrador
 *
 *  Compilar (MinGW/MSYS2 UCRT64):
 *    g++ -std=c++17 -O2 04_prioridad_procesos.cpp -o prioridad.exe
 *  Compilar (MSVC):
 *    cl /std:c++17 /EHsc /O2 04_prioridad_procesos.cpp
 *
 *  Uso:
 *    prioridad.exe [--clase baja|below|normal|above|alta|tiempo_real]
 *                  [--iteraciones 40] [--afinidad 0] [--etiqueta A]
 *                  [--esperar 5] [--sin-prioridad]
 *
 *  Comparar (una terminal, o dos):
 *    prioridad.exe --clase baja       --afinidad 0 --etiqueta BAJA
 *    prioridad.exe --clase tiempo_real --afinidad 0 --etiqueta REALTIME
 * =====================================================================
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int ANCHO = 78;

std::atomic<bool> g_detener{false};

BOOL WINAPI al_recibir_senal(DWORD tipo) {
    switch (tipo) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_detener.store(true, std::memory_order_relaxed);
            return TRUE;
        default:
            return FALSE;
    }
}

// ------------------------------------------------------------------ formato

std::string regla() {
    return "+" + std::string(static_cast<size_t>(ANCHO) - 2, '-') + "+";
}

std::string fila(const std::string& texto) {
    std::string s = "  | " + texto;
    const int    destino = ANCHO - 1;
    if (static_cast<int>(s.size()) < destino)
        s.append(static_cast<size_t>(destino - static_cast<int>(s.size())), ' ');
    return s + "|";
}



std::string numero_separado(uint64_t v) {
    std::string s = std::to_string(v), r;
    int cuenta = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i) {
        r.push_back(s[static_cast<size_t>(i)]);
        if (++cuenta % 3 == 0 && i > 0) r.push_back('.');
    }
    std::reverse(r.begin(), r.end());
    return r;
}



// ------------------------------------------------------------------ prioridad

struct Clase {
    DWORD         valor;
    const char*   nombre;
    const char*   descripcion;
};

const Clase kClases[] = {
    {IDLE_PRIORITY_CLASS,     "Baja",        "solo cuando el SO no tiene nada mejor que darle"},
    {BELOW_NORMAL_PRIORITY_CLASS, "Below", "por debajo de la normal"},
    {NORMAL_PRIORITY_CLASS,   "Normal",      "la que usan la mayoria de las aplicaciones"},
    {ABOVE_NORMAL_PRIORITY_CLASS, "Above",  "por encima de la normal"},
    {HIGH_PRIORITY_CLASS,     "Alta",        "se adelanta a casi todo el software"},
    {REALTIME_PRIORITY_CLASS, "Tiempo real", "se adelanta a TODO, incluido el SO. Requiere admin"},
};

// Traduce el nombre de la linea de ordenes al valor de la API.
bool buscar_clase(const std::string& nombre, DWORD& salida) {
    std::string n = nombre;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (n == "baja" || n == "idle" || n == "low")            { salida = IDLE_PRIORITY_CLASS;     return true; }
    if (n == "below" || n == "below-normal" || n == "baja-media") { salida = BELOW_NORMAL_PRIORITY_CLASS; return true; }
    if (n == "normal" || n == "media")                        { salida = NORMAL_PRIORITY_CLASS;   return true; }
    if (n == "above" || n == "above-normal" || n == "media-alta") { salida = ABOVE_NORMAL_PRIORITY_CLASS; return true; }
    if (n == "alta" || n == "high")                           { salida = HIGH_PRIORITY_CLASS;     return true; }
    if (n == "tiempo_real" || n == "tiemporeal" || n == "realtime" || n == "real") {
        salida = REALTIME_PRIORITY_CLASS;
        return true;
    }
    return false;
}

const char* nombre_clase(DWORD c) {
    for (const Clase& k : kClases)
        if (k.valor == c) return k.nombre;
    return "Desconocida";
}

const char* descripcion_clase(DWORD c) {
    for (const Clase& k : kClases)
        if (k.valor == c) return k.descripcion;
    return "";
}

// =====================================================================
//  CALCULO PESADO
//  Se eligen dos clculos que no dependen de la libreria matematica para
//  que el resultado sea exactamente igual en cualquier ejecucion y en
//  cualquier compilador: solo asi la comparacion de tiempos es justa.
//
//   1. Criva de Eratostenes para contar primos.
//   2. Conjunto de Mandelbrot: se recorre punto por punto y se cuentan
//      los que estan dentro del conjunto. Cada punto exige iterar hasta
//      que el modulo se dispara, y el coste depende del punto: es trabajo
//      real de CPU sin atajos.
//  El resultado se acumula en un checksum para que el optimizador no
//  pueda eliminar el calculo.
// =====================================================================

uint64_t criba_de_eratostenes(size_t limite) {
    std::vector<bool> primo(limite + 1, true);
    primo[0] = primo[1] = false;
    for (size_t i = 2; i * i <= limite; ++i) {
        if (!primo[i]) continue;
        for (size_t j = i * i; j <= limite; j += i) primo[j] = false;
    }
    uint64_t cuenta = 0;
    for (size_t i = 2; i <= limite; ++i)
        if (primo[i]) ++cuenta;
    return cuenta;
}

uint64_t mandelbrot(double cx, double cy, int max_iter, uint64_t& acumulado) {
    double zx = 0.0, zy = 0.0;
    int    i  = 0;
    for (; i < max_iter; ++i) {
        const double zx2 = zx * zx;
        const double zy2 = zy * zy;
        if (zx2 + zy2 > 4.0) break;
        zy = 2.0 * zx * zy + cy;
        zx = zx2 - zy2 + cx;
    }
    acumulado += static_cast<uint64_t>(i);
    return static_cast<uint64_t>(i);
}

// ------------------------------------------------------------------ config

struct Config {
    std::string clase = "normal";
    int         iteraciones  = 40;
    int         afinidad     = -1;   // -1 = libre
    std::string etiqueta     = "A";
    int         esperar_s    = 0;
    bool        sin_prioridad = false;
};

void mostrar_ayuda() {
    std::printf(
        "Prioridad de procesos\n\n"
        "  --clase <nombre>  baja | below | normal | above | alta | tiempo_real\n"
        "  --iteraciones <n> Veces que se repite el calculo (por defecto 40)\n"
        "  --afinidad <n>    Fija el proceso al nucleo n. Imprescindible para una\n"
        "                    comparacion justa: ambas instancias en el mismo nucleo\n"
        "  --etiqueta <txt>  Nombre que aparece en la salida (por defecto A)\n"
        "  --esperar <s>     Espera antes de empezar, para lanzar las dos a la vez\n"
        "  --sin-prioridad   No cambia la prioridad (solo mide)\n"
        "  --ayuda           Mostrar esta ayuda\n");
}

bool pedir_valor(int argc, char** argv, int& i, const char* op, std::string& v) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s requiere un valor.\n", op);
        return false;
    }
    v = argv[++i];
    return true;
}

bool leer_args(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        std::string v;
        try {
            if      (a == "--clase"       && pedir_valor(argc, argv, i, "--clase", v))       c.clase     = v;
            else if (a == "--iteraciones" && pedir_valor(argc, argv, i, "--iteraciones", v)) c.iteraciones = std::stoi(v);
            else if (a == "--afinidad"    && pedir_valor(argc, argv, i, "--afinidad", v))    c.afinidad = std::stoi(v);
            else if (a == "--etiqueta"    && pedir_valor(argc, argv, i, "--etiqueta", v))    c.etiqueta  = v;
            else if (a == "--esperar"     && pedir_valor(argc, argv, i, "--esperar", v))     c.esperar_s = std::stoi(v);
            else if (a == "--sin-prioridad") c.sin_prioridad = true;
            else if (a == "--ayuda" || a == "-h") { mostrar_ayuda(); return false; }
            else {
                std::fprintf(stderr, "Opcion desconocida: %s\n", a.c_str());
                mostrar_ayuda();
                return false;
            }
        } catch (const std::exception&) {
            std::fprintf(stderr, "Valor invalido en la opcion: %s\n", a.c_str());
            return false;
        }
    }
    if (c.iteraciones <= 0) {
        std::fprintf(stderr, "--iteraciones debe ser > 0.\n");
        return false;
    }
    DWORD tmp = 0;
    if (!buscar_clase(c.clase, tmp)) {
        std::fprintf(stderr, "Clase de prioridad no valida: %s\n", c.clase.c_str());
        return false;
    }
    return true;
}

// Tiempo de CPU consumido por el proceso (usuario + kernel).
uint64_t cpu_por_segundo_de_proceso(HANDLE h) {
    FILETIME creacion, salida_, kernel, usuario;
    if (!GetProcessTimes(h, &creacion, &salida_, &kernel, &usuario)) return 0;
    const uint64_t total =
        ((static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
        ((static_cast<uint64_t>(usuario.dwHighDateTime) << 32) | usuario.dwLowDateTime);
    return total / 10000ull;   // 100 ns -> ms
}

} // namespace

// =============================================================== MAIN

int main(int argc, char** argv) {
    Config cfg;
    if (!leer_args(argc, argv, cfg))
        return 1;

    SetConsoleCtrlHandler(al_recibir_senal, TRUE);

    HANDLE  proceso = GetCurrentProcess();
    DWORD   pid     = GetProcessId(proceso);
    DWORD   valor_clase = NORMAL_PRIORITY_CLASS;
    buscar_clase(cfg.clase, valor_clase);

    // ----------------------------------------------------------------
    //  Cabecera
    // ----------------------------------------------------------------
    std::printf("%s\n", regla().c_str());
    {
        std::ostringstream os;
        os << "PRIORIDAD DE PROCESOS  ::  instancia " << cfg.etiqueta;
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", regla().c_str());
    {
        std::ostringstream os;
        os << "PID                   : " << pid;
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        const DWORD ahora = GetPriorityClass(proceso);
        os << "Prioridad inicial    : " << nombre_clase(ahora) << "  (" << ahora << ")";
        std::printf("%s\n", fila(os.str()).c_str());
    }

    // ----------------------------------------------------------------
    //  PASO 1: fijar la afinidad de CPU (debe ir antes de la prioridad)
    //  Asi las dos instancias compiten solo por el mismo nucleo y la
    //  unica diferencia entre ellas es la prioridad.
    // ----------------------------------------------------------------
    if (cfg.afinidad >= 0) {
        const WORD      nucleos = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        const int       n       = cfg.afinidad;
        if (n < 0 || n >= nucleos) {
            std::fprintf(stderr, "--afinidad fuera de rango: hay %u nucleos.\n",
                         static_cast<unsigned>(nucleos));
            return 1;
        }
        const DWORD mascara = 1ul << n;
        if (SetProcessAffinityMask(proceso, mascara)) {
            std::ostringstream os;
            os << "Afinidad de CPU      : nucleo " << n << " de " << nucleos
               << "   (mascara 0x" << std::hex << mascara << std::dec << ")";
            std::printf("%s\n", fila(os.str()).c_str());
        } else {
            std::fprintf(stderr, "No se pudo fijar la afinidad (error %lu).\n",
                         GetLastError());
        }
    } else {
        std::printf("%s\n",
            fila("Afinidad de CPU      : libre (el SO decide)").c_str());
        std::printf("%s\n",
            fila("   Para comparar dos instancias, usa --afinidad <n> en las dos")
                .c_str());
    }
    std::printf("%s\n", fila("").c_str());

    // ----------------------------------------------------------------
    //  PASO 2: cambiar la prioridad del proceso
    // ----------------------------------------------------------------
    DWORD prioridad_original = NORMAL_PRIORITY_CLASS;
    if (cfg.sin_prioridad) {
        std::printf("%s\n",
            fila("Prioridad            : NO cambiada (--sin-prioridad)").c_str());
    } else {
        prioridad_original = GetPriorityClass(proceso);

        if (SetPriorityClass(proceso, valor_clase)) {
            // Se vuelve a leer para confirmar lo que aplico el kernel de verdad.
            const DWORD efectiva = GetPriorityClass(proceso);
            std::printf("%s\n", fila("Prioridad APLICADA con exito:").c_str());
            {
                std::ostringstream os;
                os << "   Solicitada       : " << cfg.clase
                   << "   -> clase " << valor_clase;
                std::printf("%s\n", fila(os.str()).c_str());
            }
            {
                std::ostringstream os;
                os << "   Confirmada       : " << nombre_clase(efectiva)
                   << "  (" << efectiva << ")";
                std::printf("%s\n", fila(os.str()).c_str());
            }
            {
                std::ostringstream os;
                os << "   Significado      : " << descripcion_clase(efectiva);
                std::printf("%s\n", fila(os.str()).c_str());
            }
        } else {
            const DWORD err = GetLastError();
            std::printf("%s\n",
                fila("No se pudo cambiar la prioridad.").c_str());
            std::fprintf(stderr,
                "\n  Error %lu. La clase \"tiempo real\" (REALTIME_PRIORITY_CLASS)\n"
                "  exige ejecutar el programa como ADMINISTRADOR: abre una terminal\n"
                "  de Windows con permisos de administrador y repitelo.\n"
                "  Para ver el efecto sin admin usa \"alta\" (HIGH_PRIORITY_CLASS).\n\n",
                err);
            return 1;
        }
    }
    std::printf("%s\n", fila("").c_str());

    // ----------------------------------------------------------------
    //  PASO 3: el calculo pesado
    // ----------------------------------------------------------------
    {
        std::ostringstream os;
        os << "Calculo: " << cfg.iteraciones << " vuelta(s) a la criba de Eratostenes"
           << " + Mandelbrot";
        std::printf("%s\n", fila(os.str()).c_str());
    }

    if (cfg.esperar_s > 0) {
        std::printf("  Esperando %d s para que las dos instancias arranquen a la vez...\n",
                    cfg.esperar_s);
        for (int i = cfg.esperar_s; i > 0; --i) {
            if (g_detener.load(std::memory_order_relaxed)) {
                std::printf("\n  Detenido antes de empezar.\n");
                return 0;
            }
            std::printf("\r  %2d...   ", i);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::printf("\r          \n");
    }

    const uint64_t cpu_inicial = cpu_por_segundo_de_proceso(proceso);
    const auto     reloj_ini   = std::chrono::steady_clock::now();

    uint64_t primos  = 0;
    uint64_t puntos = 0;
    uint64_t puntos_dentro = 0;
    bool     interrumpido = false;

    for (int vuelta = 0; vuelta < cfg.iteraciones && !interrumpido; ++vuelta) {
        // Un salto de 4 reparte mejor el trabajo que uno de 2 y mantiene la
        // duracion de cada vuelta en pocos segundos, que es lo que hace
        // comparables las dos instancias.
        for (size_t p = 2; p < 60000; p += 4) {
            primos += criba_de_eratostenes(p);
            if (g_detener.load(std::memory_order_relaxed)) {
                interrumpido = true;
                break;
            }
        }

        // Mandelbrot sobre una rejilla: coste alto y muy variable por punto.
        constexpr int    REJILLA = 260;
        constexpr int    MAX_ITER = 600;
        const double     paso = 3.0 / static_cast<double>(REJILLA);
        double           cx_ini = -2.0, cy_ini = -1.2;

        for (int y = 0; y < REJILLA && !interrumpido; ++y) {
            for (int x = 0; x < REJILLA; ++x) {
                const uint64_t it = mandelbrot(cx_ini + x * paso, cy_ini + y * paso,
                                              MAX_ITER, puntos);
                ++puntos;
                if (it == MAX_ITER) ++puntos_dentro;
            }
            if ((y % 50) == 0 && g_detener.load(std::memory_order_relaxed)) {
                interrumpido = true;
                break;
            }
        }

        std::printf("\r  Vuelta %d / %d ...   ", vuelta + 1, cfg.iteraciones);
        std::fflush(stdout);
    }

    const auto reloj_fin = std::chrono::steady_clock::now();
    const uint64_t cpu_final = cpu_por_segundo_de_proceso(proceso);

    std::printf("\r  Vuelta %d / %d completada.            \n",
                cfg.iteraciones, cfg.iteraciones);

    const double segundos =
        std::chrono::duration<double>(reloj_fin - reloj_ini).count();
    const uint64_t cpu_ms = cpu_final - cpu_inicial;
    const double   uso_cpu = segundos > 0.0 ? 100.0 * (cpu_ms / 1000.0) / segundos : 0.0;

    // Se apunta la clase que se estuvo usando ANTES de restaurar, para que el
    // informe diga la verdad aunque despues se devuelva la original.
    const DWORD   clase_usada  = GetPriorityClass(proceso);
    if (!cfg.sin_prioridad && prioridad_original != valor_clase)
        SetPriorityClass(proceso, prioridad_original);

    std::printf("\n%s\n", regla().c_str());
    {
        std::ostringstream os;
        os << "RESULTADO  [" << cfg.etiqueta << "]";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", regla().c_str());
    {
        std::ostringstream os;
        os << "Prioridad usada      : " << nombre_clase(clase_usada)
           << "  -> restaurada a " << nombre_clase(GetPriorityClass(proceso));
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Tiempo de reloj      : " << std::fixed << std::setprecision(3)
           << segundos << " s";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << std::fixed << "Tiempo de CPU        : " << std::setprecision(3)
           << (cpu_ms / 1000.0) << " s   (" << std::setprecision(1) << uso_cpu
           << "% de uso del processor)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    if (cfg.afinidad >= 0) {
        std::ostringstream os;
        os << "Nucleo               : " << cfg.afinidad << " (fijado con afinidad)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Primos encontrados   : " << numero_separado(primos);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Puntos de Mandelbrot : " << numero_separado(puntos)
           << "   dentro del conjunto: " << numero_separado(puntos_dentro);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Estado               : " << (interrumpido ? "INTERRUMPIDO por el usuario"
                                                         : "calculo completo");
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", regla().c_str());

    std::printf("\n  COMO COMPARAR DOS PRIORIDADES\n");
    std::printf("  Abre dos terminales y ejecuta, con el MISMO numero de nucleo:\n\n");
    std::printf("    prioridad.exe --clase baja       --afinidad 0 --etiqueta BAJA\n");
    std::printf("    prioridad.exe --clase tiempo_real --afinidad 0 --etiqueta REALTIME\n\n");
    std::printf("  Si el kernel no deja aplicar \"tiempo real\" por falta de permisos,\n");
    std::printf("  ejecuta desde una terminal de administrador, o compara \"baja\" con\n");
    std::printf("  \"alta\", que no necesita permisos especiales.\n\n");
    std::printf("  Que se espera: con \"tiempo real\" el proceso se ejecuta casi sin\n");
    std::printf("  interrupciones, el SO le cede la CPU siempre que puede y termina\n");
    std::printf("  antes. Con \"baja\" el kernel lo interrumpe en cuanto hay cualquier\n");
    std::printf("  otra cosa que hacer, asi que tarda mucho mas aunque reciba CPU.\n");
    std::printf("  La diferencia se ve mejor si las dos corren a la vez con --esperar 5.\n");
    std::printf("  El porcentaje de uso de CPU tambien lo delata: la baja apenas pasa\n");
    std::printf("  del %% que le toca, la de tiempo real se acerca al 100%%.\n");

    return 0;
}