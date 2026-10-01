/*
 * =====================================================================
 *  3. ESTRES DE MEMORIA Y SALTO A LA VIRTUAL  (paginacion / swap)
 * =====================================================================
 *  Concepto : Memoria virtual y archivo de paginacion.
 *  Objetivo : Forzar al SO a usar la memoria virtual creando objetos
 *             masivos (millones de cadenas) hasta agotar la RAM fisica.
 *
 *  Tarea    : Llenar una lista/array con millones de strings en bucle.
 *  Reto     : Mientras corre, abrir el Administrador de tareas y ver como
 *             sube la memoria virtual al agotarse la RAM fisica.
 *
 *  !! ADVERTENCIA !! Este programa consume RAM de verdad. Lleva LIMITES
 *  DE SEGURIDAD incorporados para no bloquear la maquina:
 *
 *    1. --limite-mb       Tope de memoria comprometida por el proceso.
 *    2. --margen-mb       Para y avisa si la RAM libre del sistema baja de
 *                         este valor (deja margen a Windows).
 *    3. --segundos        Tiempo maximo de relleno.
 *    4. --aviso-libre-mb  Avisa y empieza a frenar cuando la RAM libre
 *                         cae por debajo de este valor.
 *    5. Ctrl+C detiene todo en cualquier momento de forma ordenada.
 *
 *  Para observar el salto a la virtual:
 *    Administrador de tareas (Ctrl+Shift+Esc) > Rendimiento > Memoria
 *      - "En uso"        = RAM fisica ocupada
 *      - "Enviado"       = compromiso de memoria (RAM + archivo de paginacion)
 *      - "Paginado"      = ritmo de entrada/salida al archivo de paginacion
 *    Cuando "Enviado" supera el tamano de la RAM, la diferencia esta
 *    respaldada por el archivo de paginacion: eso es la memoria virtual.
 *
 *  Librerias :
 *    - STL : std::vector<std::string>  -> el objeto masivo
 *    - Win32: GlobalMemoryStatusEx()   -> estado de memoria del sistema
 *            GetProcessMemoryInfo()     -> consumo real de este proceso
 *
 *  Compilar (MinGW/MSYS2 UCRT64):
 *    g++ -std=c++17 -O2 03_estres_memoria.cpp -o estres.exe -lpsapi
 *  Compilar (MSVC):
 *    cl /std:c++17 /EHsc /O2 03_estres_memoria.cpp psapi.lib
 *
 *  Uso:
 *    estres.exe [--limite-mb 4096] [--margen-mb 1024] [--aviso-libre-mb 2048]
 *              [--segundos 60] [--largo 128] [--lote 50000]
 *              [--retener 20] [--tocar 0]
 * =====================================================================
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// GetProcessMemoryInfo necesita Windows 2000 o superior.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif

#include <windows.h>
#include <psapi.h>
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int ANCHO = 78;
constexpr size_t KB = 1024ull;
constexpr size_t MB = 1024ull * 1024ull;
constexpr size_t GB = 1024ull * 1024ull * 1024ull;

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

std::string separador() {
    return "  | " + std::string(static_cast<size_t>(ANCHO) - 5, '-') + "|";
}

std::string formato_bytes(uint64_t bytes) {
    char         txt[64];
    const double v = static_cast<double>(bytes);
    if (v >= static_cast<double>(GB))      std::snprintf(txt, sizeof txt, "%.2f GB", v / GB);
    else if (v >= static_cast<double>(MB)) std::snprintf(txt, sizeof txt, "%.0f MB", v / MB);
    else                                   std::snprintf(txt, sizeof txt, "%.0f KB", v / KB);
    return txt;
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

std::string formato_hora(uint64_t s) {
    char txt[32];
    std::snprintf(txt, sizeof txt, "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>((s % 3600) / 60),
                  static_cast<unsigned long long>(s % 60));
    return txt;
}

void preparar_consola() {
    HANDLE salida = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  modo   = 0;
    if (GetConsoleMode(salida, &modo))
        SetConsoleMode(salida, modo | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void borrar_pantalla() {
    std::fputs("\x1b[H\x1b[J", stdout);
    std::fflush(stdout);
}

// ------------------------------------------------------------------ sistema

struct Sistema {
    uint64_t total_fisica   = 0;
    uint64_t libre_fisica   = 0;
    uint64_t total_paginac  = 0;   // RAM + archivo de paginacion
    uint64_t libre_paginac  = 0;
    double   pct_fisica     = 0.0;
    double   pct_compromiso = 0.0;
    bool     hay_paginac    = false;
};

Sistema leer_sistema() {
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    Sistema s;
    if (!GlobalMemoryStatusEx(&st)) return s;

    s.total_fisica  = st.ullTotalPhys;
    s.libre_fisica  = st.ullAvailPhys;
    s.total_paginac = st.ullTotalPageFile;
    s.libre_paginac = st.ullAvailPageFile;

    if (s.total_fisica)
        s.pct_fisica = 100.0 * static_cast<double>(s.total_fisica - s.libre_fisica) /
                       static_cast<double>(s.total_fisica);
    if (s.total_paginac) {
        s.pct_compromiso =
            100.0 * static_cast<double>(s.total_paginac - s.libre_paginac) /
            static_cast<double>(s.total_paginac);
    }
    // Si el limite de paginacion es igual a la RAM, no hay archivo de paginacion.
    s.hay_paginac = s.total_paginac > s.total_fisica;
    return s;
}

struct Proceso {
    uint64_t conjunto_residente = 0;   // working set: lo que vive en RAM ahora
    uint64_t conjunto_pico     = 0;   // working set maximo alcanzado
    uint64_t privado            = 0;   // paginas privadas comprometidas
    uint64_t privado_pico       = 0;   // maximo comprometido
};

Proceso leer_proceso(HANDLE h) {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    Proceso p;
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
        p.conjunto_residente = pmc.WorkingSetSize;
        p.conjunto_pico      = pmc.PeakWorkingSetSize;
        p.privado            = pmc.PagefileUsage;
        p.privado_pico       = pmc.PeakPagefileUsage;
    }
    return p;
}

// ------------------------------------------------------------------ config

struct Config {
    size_t      limite_mb      = 4096;    // tope de compromiso del proceso
    size_t      margen_mb      = 1024;    // RAM libre minima para seguir
    size_t      aviso_mb       = 2048;    // RAM libre que dispara la advertencia
    int         segundos       = 60;      // tiempo maximo de relleno
    size_t      largo          = 128;     // caracteres por cadena
    size_t      lote           = 50000;   // cadenas por tanda
    int         retener_s      = 20;      // segundos manteniendo la memoria
    bool        tocar          = true;    // recorrer las cadenas para fijarlas
};

void mostrar_ayuda() {
    std::printf(
        "Estres de memoria y salto a la virtual (paginacion)\n\n"
        "  --limite-mb <mb>     Tope de memoria comprometida por el proceso (4096)\n"
        "  --margen-mb <mb>     Parar si la RAM libre del sistema baja de esto (1024)\n"
        "  --aviso-libre-mb <mb>  Avisar y frenar por debajo de esta RAM libre (2048)\n"
        "  --segundos <s>       Tiempo maximo de relleno (60)\n"
        "  --largo <n>          Caracteres por cadena (128). Usa mas de 15 para\n"
        "                       evitar la optimizacion de cadenas cortas (SSO)\n"
        "  --lote <n>           Cadenas anadidas por tanda (50000)\n"
        "  --retener <s>        Segundos manteniendo la memoria para mirar el\n"
        "                       administrador de tareas (20)\n"
        "  --sin-tocar          No recorrer las cadenas al final de cada tanda\n"
        "  --ayuda              Mostrar esta ayuda\n");
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
            if      (a == "--limite-mb"     && pedir_valor(argc, argv, i, "--limite-mb", v))     c.limite_mb = std::stoull(v);
            else if (a == "--margen-mb"     && pedir_valor(argc, argv, i, "--margen-mb", v))     c.margen_mb = std::stoull(v);
            else if (a == "--aviso-libre-mb"&& pedir_valor(argc, argv, i, "--aviso-libre-mb", v))c.aviso_mb  = std::stoull(v);
            else if (a == "--segundos"      && pedir_valor(argc, argv, i, "--segundos", v))      c.segundos  = std::stoi(v);
            else if (a == "--largo"         && pedir_valor(argc, argv, i, "--largo", v))         c.largo     = std::stoull(v);
            else if (a == "--lote"          && pedir_valor(argc, argv, i, "--lote", v))          c.lote      = std::stoull(v);
            else if (a == "--retener"       && pedir_valor(argc, argv, i, "--retener", v))       c.retener_s = std::stoi(v);
            else if (a == "--sin-tocar") c.tocar = false;
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
    if (c.largo == 0 || c.lote == 0) {
        std::fprintf(stderr, "--largo y --lote deben ser > 0.\n");
        return false;
    }
    if (c.largo < 16)
        std::fprintf(stderr,
                     "Aviso: con --largo %zu las cadenas caben en el objeto interno del\n"
                     "       propio std::string (SSO) y casi no se reserva memoria del heap.\n",
                     c.largo);
    return true;
}

// ------------------------------------------------------------------ panel

void dibujar(const Sistema& sis, const Proceso& pro, uint64_t cadenas,
             uint64_t bytes_datos, uint64_t transcurrido,
             const std::string& fase, const std::string& aviso) {
    borrar_pantalla();
    std::printf("%s\n", regla().c_str());
    std::printf("%s\n", fila("ESTRES DE MEMORIA  ::  forzar el salto a la virtual").c_str());
    std::printf("%s\n", regla().c_str());
    std::printf("%s\n", fila("").c_str());
    {
        std::ostringstream os;
        os << "PID " << GetCurrentProcessId() << "   Fase: " << fase
           << "   Tiempo: " << formato_hora(transcurrido);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", fila("").c_str());
std::printf("%s\n", separador().c_str());
    {
        std::ostringstream os;
        os << std::left << std::setw(20) << "Cadenas"
           << std::setw(15) << "Datos utiles" << std::setw(15) << "Residencia"
           << "Compromiso (virtual)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", separador().c_str());
    {
        std::ostringstream os;
        os << std::left << std::setw(20) << numero_separado(cadenas)
           << std::setw(15) << formato_bytes(bytes_datos)
           << std::setw(15) << formato_bytes(pro.conjunto_residente)
           << formato_bytes(pro.privado)
           << " (pico " << formato_bytes(pro.privado_pico) << ")";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", separador().c_str());
    std::printf("%s\n", fila("").c_str());

    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(1)
           << "RAM fisica del sistema : "
           << formato_bytes(sis.total_fisica - sis.libre_fisica) << " de "
           << formato_bytes(sis.total_fisica) << "  (" << sis.pct_fisica
           << "%)   libres: " << formato_bytes(sis.libre_fisica);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        const uint64_t comprometido = sis.total_paginac - sis.libre_paginac;
        std::ostringstream os;
        os << std::fixed << std::setprecision(1)
           << "Compromiso del sistema : " << formato_bytes(comprometido) << " de "
           << formato_bytes(sis.total_paginac) << "  (" << sis.pct_compromiso << "%)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    if (sis.hay_paginac) {
        const uint64_t comprometido = sis.total_paginac - sis.libre_paginac;
        const uint64_t en_disco = (comprometido > sis.total_fisica)
                                      ? comprometido - sis.total_fisica
                                      : 0;
        std::ostringstream os;
        os << ">> Paginado al disco   : " << formato_bytes(en_disco)
           << " respaldado por el archivo de paginacion";
        std::printf("%s\n", fila(os.str()).c_str());
    } else {
        std::ostringstream os;
        os << ">> SIN ARCHIVO DE PAGINACION: no se vera swap. Activelo en";
        std::printf("%s\n", fila(os.str()).c_str());
        os << "   Configuracion > Sistema > Memoria > rendimiento.";
        std::printf("%s\n", fila(os.str()).c_str());
    }

    std::printf("%s\n", fila("").c_str());
    if (!aviso.empty()) {
        std::ostringstream os;
        os << "[ " << aviso << " ]";
        std::printf("%s\n", fila(os.str()).c_str());
    } else {
        std::printf("%s\n",
                    fila("Ctrl+C para detener ahora   Q para detener").c_str());
    }
    std::printf("%s\n", regla().c_str());
}

// Recorre todas las cadenas para forzar que sus paginas sigan residentes.
// Sin esto, Windows puede recortar el conjunto de trabajo y la RAM fisica
// bajaria aunque la memoria este comprometida: no habria paginacion real.
uint64_t tocar_todo(std::vector<std::string>& v) {
    uint64_t suma = 0;
    for (std::string& s : v) {
        // volatile impide que el optimizador elimine la lectura
        volatile char c = s[s.size() / 2];
        suma += static_cast<unsigned char>(c);
    }
    return suma;
}

} // namespace

// =============================================================== MAIN

int main(int argc, char** argv) {
    Config cfg;
    if (!leer_args(argc, argv, cfg))
        return 1;

    preparar_consola();
    SetConsoleCtrlHandler(al_recibir_senal, TRUE);

    HANDLE  proceso = GetCurrentProcess();
    Sistema inicial  = leer_sistema();

    // ----------------------------------------------------------------
    //  Cabecera con el plan y los limites de seguridad
    // ----------------------------------------------------------------
    std::printf("%s\n", regla().c_str());
    std::printf("%s\n", fila("ESTRESS DE MEMORIA Y SALTO A LA VIRTUAL").c_str());
    std::printf("%s\n", regla().c_str());
    {
        std::ostringstream os;
        os << "RAM fisica    : " << formato_bytes(inicial.total_fisica)
           << "   libre: " << formato_bytes(inicial.libre_fisica);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Limite pagin.: " << formato_bytes(inicial.total_paginac)
           << "   (RAM + archivo de paginacion)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    if (!inicial.hay_paginac) {
        std::printf("%s\n",
            fila("AVISO: no hay archivo de paginacion activo. No se vera swap.")
                .c_str());
        std::printf("%s\n",
            fila("      Activalo en Configuracion > Sistema > Memoria > rendimiento.")
                .c_str());
    }
    std::printf("%s\n", fila("").c_str());
    std::printf("%s\n", fila("LIMITES DE SEGURIDAD ACTIVOS").c_str());
    {
        std::ostringstream os;
        os << "1. Tope del proceso      : " << cfg.limite_mb << " MB comprometidos";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "2. Parar si RAM libre <  : " << cfg.margen_mb << " MB";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "3. Avisar si RAM libre < : " << cfg.aviso_mb << " MB (y frenar)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "4. Tiempo maximo         : " << cfg.segundos << " s";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "5. Ctrl+C detiene de inmediato y libera la memoria";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", fila("").c_str());
    {
        std::ostringstream os;
        os << "Plan: cadenas de " << cfg.largo << " caracteres, "
           << numero_separado(cfg.lote) << " por tanda.";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        const double por_cadena =
            sizeof(std::string) + (cfg.largo > 15 ? cfg.largo + 1 : 0);
        os << "      Coste estimado ~" << std::fixed << std::setprecision(0)
           << por_cadena << " B por cadena  ->  el tope de "
           << cfg.limite_mb << " MB equivale a unos "
           << numero_separado(static_cast<uint64_t>(
                  (static_cast<double>(cfg.limite_mb) * MB) / por_cadena))
           << " cadenas";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("\n  Abriendo el Administrador de tareas en 3 segundos para que lo");
    std::printf("\n  tengas listo (Rendimiento > Memoria).\n");
    for (int i = 3; i > 0; --i) {
        std::printf("\r  %d...   ", i);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::printf("\r          \n");

    // ----------------------------------------------------------------
    //  Fase 1: relleno masivo
    // ----------------------------------------------------------------
    std::vector<std::string> datos;
    datos.reserve(cfg.lote);

    const auto     inicio = std::chrono::steady_clock::now();

    // Base de referencia: lo que este proceso tenia comprometido al arrancar,
    // para que --limite-mb cuente solo lo que anade el programa.
    const uint64_t compromiso_0 = leer_proceso(proceso).privado;

    uint64_t cadenas = 0;
    uint64_t bytes_datos = 0;
    uint64_t suma_tocadas = 0;
    unsigned tanda = 0;
    std::string  motivo = "Objetivo alcanzado";
    std::string  aviso;

    while (!g_detener.load(std::memory_order_relaxed)) {
        const auto     ahora = std::chrono::steady_clock::now();
        const uint64_t elapsed = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(ahora - inicio).count());
        const Sistema sis = leer_sistema();

        // El tope se mide sobre el compromiso del PROPIO proceso
        // (PagefileUsage), no sobre el del sistema: asi el limite significa
        // lo mismo este el equipo libre o lleno.
        const Proceso pro_ahora = leer_proceso(proceso);
        const uint64_t comprometido_delta =
            pro_ahora.privado > compromiso_0 ? pro_ahora.privado - compromiso_0 : 0;

        // ---- comprobacion de limites ANTES de seguir reservando ----
        if (comprometido_delta >= cfg.limite_mb * MB) {
            motivo = "Tope de memoria comprometida del proceso alcanzado";
            break;
        }
        if (sis.libre_fisica < cfg.margen_mb * MB) {
            motivo = "RAM libre del sistema por debajo del margen de seguridad";
            break;
        }
        if (cfg.segundos > 0 && elapsed >= static_cast<uint64_t>(cfg.segundos)) {
            motivo = "Tiempo maximo de relleno alcanzado";
            break;
        }

        // ---- avisar y frenar cuando la RAM empieza a escasear ----
        if (sis.libre_fisica < cfg.aviso_mb * MB) {
            static uint64_t t_aviso = 0;
            if (elapsed >= t_aviso + 3) {
                t_aviso = elapsed;
                aviso = "Poca RAM libre: ralentizando el relleno";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        // ---- anadir una tanda de cadenas ----
        try {
            for (size_t i = 0; i < cfg.lote; ++i) {
                if (g_detener.load(std::memory_order_relaxed)) break;
                // Se escribe contenido real: una pagina solo queda comprometida
                // cuando se toca. Relleno con un patron para que se vea claro.
                std::string s(cfg.largo, '\0');
                const char patron = static_cast<char>('A' + ((cadenas + i) % 26));
                for (size_t j = 0; j < s.size(); ++j)
                    s[j] = static_cast<char>(patron + static_cast<char>(j % 13));
                s[0] = static_cast<char>('0' + ((cadenas + i) / 1000000000ull) % 10);
                datos.push_back(std::move(s));
                ++cadenas;
                bytes_datos += cfg.largo;
            }
        } catch (const std::bad_alloc&) {
            motivo = "El sistema no ha podido comprometer mas memoria (bad_alloc)";
            break;
        } catch (const std::length_error&) {
            motivo = "Se ha superado el limite del vector (length_error)";
            break;
        }

        // ---- fijar las paginas en RAM para que haya paginacion real ----
        if (cfg.tocar)
            suma_tocadas += tocar_todo(datos);

        ++tanda;
        if (tanda % 2 == 0)
            aviso.clear();

        // Se vuelve a leer el estado DESPUES de la tanda para que el panel
        // refleje la memoria que acabamos de comprometer.
        const Sistema sis_ahora = leer_sistema();
        const Proceso pro = leer_proceso(proceso);
        dibujar(sis_ahora, pro, cadenas, bytes_datos, elapsed, "1 - RELLENO", aviso);

        if (_kbhit()) {
            const int c = _getch();
            if (c == 'q' || c == 'Q' || c == 27) {
                motivo = "Detenido por el usuario";
                break;
            }
        }
    }

    const auto fin_relleno = std::chrono::steady_clock::now();
    const Sistema sis_fin   = leer_sistema();
    const Proceso pro_fin   = leer_proceso(proceso);

    const double   segundos_relleno =
        std::chrono::duration<double>(fin_relleno - inicio).count();
    const uint64_t comprometido_final = sis_fin.total_paginac - sis_fin.libre_paginac;
    const uint64_t en_disco =
        comprometido_final > sis_fin.total_fisica
            ? comprometido_final - sis_fin.total_fisica
            : 0;

    // ----------------------------------------------------------------
    //  Fase 2: retener la memoria para poder observar el swap
    // ----------------------------------------------------------------
    if (cfg.retener_s > 0) {
        const std::string fase = "2 - RETENIENDO";
        std::printf("\n");
        std::printf("  Ahora la memoria se mantiene asignada. Mira el administrador de");
        std::printf("\n  tareas: memoria virtual / \"Enviado\" frente a RAM fisica.\n");
        for (int s = 1; s <= cfg.retener_s && !g_detener.load(std::memory_order_relaxed); ++s) {
            const Sistema sis = leer_sistema();
            const Proceso pro = leer_proceso(proceso);
            const uint64_t transcurrido =
                static_cast<uint64_t>(segundos_relleno) + static_cast<uint64_t>(s);
            dibujar(sis, pro, cadenas, bytes_datos, transcurrido, fase, "");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (_kbhit()) {
                const int c = _getch();
                if (c == 'q' || c == 'Q' || c == 27) break;
            }
        }
        aviso.clear();
    }

    // ----------------------------------------------------------------
    //  Fase 3: liberar y mostrar el resumen
    // ----------------------------------------------------------------
    datos.clear();
    datos.shrink_to_fit();   // devuelve la memoria al sistema

    const Sistema sis_libre = leer_sistema();
    std::printf("\n%s\n", regla().c_str());
    std::printf("%s\n", fila("RESUMEN DEL ESTRES DE MEMORIA").c_str());
    std::printf("%s\n", regla().c_str());
    {
        std::ostringstream os;
        os << "Motivo de parada       : " << motivo;
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Tiempo de relleno      : " << std::fixed << std::setprecision(2)
           << segundos_relleno << " s en " << tanda << " tanda(s)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Cadenas creadas       : " << numero_separado(cadenas);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Datos utiles           : " << formato_bytes(bytes_datos)
           << "  (veces " << numero_separado(cadenas) << ")";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << std::fixed << "Residencia maxima (RAM) : " << std::setprecision(0)
           << formato_bytes(pro_fin.conjunto_pico)
           << "   [" << std::setprecision(1) << sis_fin.pct_fisica
           << "% de la RAM del sistema]";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Compromiso maximo      : " << formato_bytes(pro_fin.privado_pico);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Residuo en el swap     : " << formato_bytes(en_disco)
           << (en_disco > 0 ? "  <- memoria virtual en uso" : "  <- todavia cabe en RAM");
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "RAM libre tras liberar : " << formato_bytes(sis_libre.libre_fisica)
           << " de " << formato_bytes(sis_libre.total_fisica);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    if (suma_tocadas != 0) {
        std::ostringstream os;
        os << "Huella de verificacion : " << suma_tocadas
           << " (leer las cadenas mantiene sus paginas vivas)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", regla().c_str());
    std::printf("\n  Conclusion: la memoria \"virtual\" no es una RAM mas grande, es el\n");
    std::printf("  conjunto RAM + archivo de paginacion. En cuanto el compromiso total\n");
    std::printf("  supera el tamano de la RAM, el SO empieza a paginar: cada acceso que\n");
    std::printf("  no cabe en RAM genera un fallo de pagina y el dato se trae del disco.\n");
    std::printf("  Por eso la maquina se ralentiza justo cuando la RAM se agota.\n");
    std::printf("\n  Para verlo mejor:Administrator de tareas > Rendimiento > Memoria,\n");
    std::printf("  y compara \"Enviado\" con el total de RAM. Repite con unos limites\n");
    std::printf("  mas agresivos solo si sabes lo que haces:\n");
    std::printf("    %s --limite-mb 12288 --aviso-libre-mb 1024 --margen-mb 512\n",
                "estres.exe");

    return 0;
}