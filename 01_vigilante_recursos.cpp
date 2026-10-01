/*
 * =====================================================================
 *  1. VIGILANTE DE RECURSOS  -  Monitorizacion de RAM y CPU
 * =====================================================================
 *  Concepto : Gestor de memoria y E/S.
 *  Objetivo : Panel de control del SO en tiempo real (RAM, CPU y
 *             compromiso de memoria virtual) con alerta al superar
 *             un umbral y escritura en un archivo de log .txt
 *
 *  Librerias :
 *    - Win32 API : GlobalMemoryStatusEx()  -> RAM fisica y compromiso
 *                 GetSystemTimes()        -> % de CPU del sistema
 *    - STL       : <fstream> para el log, <chrono> para el muestreo
 *
 *  Nota sobre "console.time()" / "time.time()": en C++ el equivalente
 *  es std::chrono::steady_clock (monotonico, no affected por ajustes
 *  del reloj del sistema). Se usa para medir cada intervalo.
 *
 *  Compilar (MinGW/MSYS2 UCRT64):
 *    g++ -std=c++17 -O2 01_vigilante_recursos.cpp -o vigilante.exe
 *  Compilar (MSVC):
 *    cl /std:c++17 /EHsc /O2 01_vigilante_recursos.cpp
 *
 *  Uso:
 *    vigilante.exe [--umbral 80] [--intervalo 1000] [--duracion 0]
 *                 [--salida alertas_memoria.txt] [--relog 30]
 * =====================================================================
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace {

// ------------------------------------------------------------------ config

struct Config {
    double      umbral_pct   = 80.0;    // % de RAM fisica que dispara alerta
    int         intervalo_ms = 1000;    // periodo de muestreo
    std::string salida       = "alertas_memoria.txt";
    int         duracion_s   = 0;       // 0 = infinito (Ctrl+C para salir)
    int         relog_s      = 30;      // re-registro mientras siga en alerta
};

constexpr int    ANCHO      = 76;      // ancho del panel
constexpr size_t KB         = 1024ull;
constexpr size_t MB         = 1024ull * 1024ull;
constexpr size_t GB         = 1024ull * 1024ull * 1024ull;

std::atomic<bool> g_detener{false};

// ------------------------------------------------------------- senales

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

// ------------------------------------------------------------- formateo

std::string formato_bytes(uint64_t bytes) {
    char         txt[64];
    const double v = static_cast<double>(bytes);
    // 128 TB es el tope de espacio de direcciones de usuario en x64: mas alla
    // de eso "Espacio virtual" no dice nada util, asi que se muestra en TB.
    if (v >= static_cast<double>(GB) * 1024.0)
        std::snprintf(txt, sizeof txt, "%.2f TB", v / (static_cast<double>(GB) * 1024.0));
    else if (v >= static_cast<double>(GB))      std::snprintf(txt, sizeof txt, "%.2f GB", v / GB);
    else if (v >= static_cast<double>(MB))      std::snprintf(txt, sizeof txt, "%.0f MB", v / MB);
    else                                        std::snprintf(txt, sizeof txt, "%.0f KB", v / KB);
    return txt;
}

std::string formato_hora(uint64_t segundos) {
    const uint64_t h = segundos / 3600;
    const uint64_t m = (segundos % 3600) / 60;
    const uint64_t s = segundos % 60;
    char txt[32];
    std::snprintf(txt, sizeof txt, "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(h),
                  static_cast<unsigned long long>(m),
                  static_cast<unsigned long long>(s));
    return txt;
}

std::string ahora_iso() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char txt[32];
    std::snprintf(txt, sizeof txt, "%04u-%02u-%02u %02u:%02u:%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return txt;
}

std::string regla() {
    return "+" + std::string(static_cast<size_t>(ANCHO) - 2, '-') + "+";
}

// Linea de contenido: "  | texto.....|"
std::string fila(const std::string& texto) {
    std::string s = "  | " + texto;
    const int    destino = ANCHO - 1;
    if (static_cast<int>(s.size()) < destino)
        s.append(static_cast<size_t>(destino - static_cast<int>(s.size())), ' ');
    return s + "|";
}

// Barra de progreso ASCII: [##########......................]
std::string barra(double pct, int ancho = 30) {
    pct = std::max(0.0, std::min(100.0, pct));
    int llenos = static_cast<int>(pct / 100.0 * ancho + 0.5);
    llenos = std::max(0, std::min(ancho, llenos));
    std::string s(static_cast<size_t>(llenos), '#');
    s.append(static_cast<size_t>(ancho - llenos), '.');
    return s;
}

// "  RAM fisica : [####...]  62.3%   6.1 / 15.9 GB"
std::string fila_grafica(const std::string& etiqueta, double pct,
                         const std::string& extra) {
    std::ostringstream os;
    os << std::left << std::setw(13) << etiqueta << ": " << barra(pct) << " "
       << std::fixed << std::setprecision(1) << std::setw(6) << pct << "%  " << extra;
    return fila(os.str());
}

// ------------------------------------------------------- lectura de RAM

struct Memoria {
    uint64_t total_fisica    = 0;
    uint64_t libre_fisica    = 0;
    uint64_t total_pagefile  = 0;
    uint64_t libre_pagefile  = 0;
    uint64_t total_virtual   = 0;
    uint64_t libre_virtual   = 0;
    double   pct_fisica      = 0.0;   // (total - libre) / total
    double   pct_compromiso  = 0.0;   // commit charge -> "Enviado" del admin
};

Memoria leer_memoria() {
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    Memoria m;
    if (!GlobalMemoryStatusEx(&st))
        return m;

    m.total_fisica   = st.ullTotalPhys;
    m.libre_fisica   = st.ullAvailPhys;
    m.total_pagefile = st.ullTotalPageFile;
    m.libre_pagefile = st.ullAvailPageFile;
    m.total_virtual  = st.ullTotalVirtual;
    m.libre_virtual  = st.ullAvailVirtual;

    if (m.total_fisica)
        m.pct_fisica = 100.0 * static_cast<double>(m.total_fisica - m.libre_fisica)
                             / static_cast<double>(m.total_fisica);

    if (m.total_pagefile) {
        const uint64_t comprometido = m.total_pagefile - m.libre_pagefile;
        m.pct_compromiso = 100.0 * static_cast<double>(comprometido)
                           / static_cast<double>(m.total_pagefile);
    }
    return m;
}

// ------------------------------------------------------- lectura de CPU

struct TiemposCpu {
    uint64_t idle   = 0;
    uint64_t kernel = 0;   // incluye el tiempo idle
    uint64_t user   = 0;
};

uint64_t a_u64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

TiemposCpu leer_tiempos_cpu() {
    FILETIME fIdle{}, fKernel{}, fUser{};
    TiemposCpu t;
    if (!GetSystemTimes(&fIdle, &fKernel, &fUser))
        return t;
    t.idle   = a_u64(fIdle);
    t.kernel = a_u64(fKernel);
    t.user   = a_u64(fUser);
    return t;
}

// % de uso = (tiempo ocupado) / (tiempo total) entre dos muestras.
// "kernel" ya incluye "idle", por eso el trabajo total = kernel + user.
double calcular_pct_cpu(const TiemposCpu& ahora, const TiemposCpu& previa) {
    const uint64_t total_ahora = ahora.kernel + ahora.user;
    const uint64_t total_previa = previa.kernel + previa.user;
    if (total_ahora <= total_previa)
        return 0.0;

    const uint64_t d_total = total_ahora - total_previa;
    const uint64_t d_ocupado = (total_ahora - ahora.idle) - (total_previa - previa.idle);
    return 100.0 * static_cast<double>(d_ocupado) / static_cast<double>(d_total);
}

// ----------------------------------------------------------------- args

bool pedir_valor(int argc, char** argv, int& i, const char* opcion, std::string& salida) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s requiere un valor.\n", opcion);
        return false;
    }
    salida = argv[++i];
    return true;
}

void mostrar_ayuda() {
    std::printf(
        "Vigilante de recursos - monitor de RAM y CPU\n\n"
        "  --umbral <pct>     Umbral de RAM que dispara la alerta (por defecto 80)\n"
        "  --intervalo <ms>   Periodo de muestreo (por defecto 1000)\n"
        "  --duracion <s>     Duracion en segundos; 0 = hasta Ctrl+C (por defecto 0)\n"
        "  --salida <archivo> Archivo .txt del log (por defecto alertas_memoria.txt)\n"
        "  --relog <s>        Re-registar cada N s si la RAM sigue alta; 0 = en cada\n"
        "                     muestra mientras supere el umbral (por defecto 30)\n"
        "  --ayuda            Mostrar esta ayuda\n");
}

bool leer_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        std::string v;
        try {
            if (a == "--umbral"     && pedir_valor(argc, argv, i, "--umbral", v))     cfg.umbral_pct   = std::stod(v);
            else if (a == "--intervalo" && pedir_valor(argc, argv, i, "--intervalo", v)) cfg.intervalo_ms = std::stoi(v);
            else if (a == "--duracion"  && pedir_valor(argc, argv, i, "--duracion", v))  cfg.duracion_s   = std::stoi(v);
            else if (a == "--relog"     && pedir_valor(argc, argv, i, "--relog", v))     cfg.relog_s      = std::stoi(v);
            else if (a == "--salida"    && pedir_valor(argc, argv, i, "--salida", v))    cfg.salida       = v;
            else if (a == "--ayuda" || a == "-h" || a == "/?") { mostrar_ayuda(); return false; }
            else { std::fprintf(stderr, "Opcion desconocida: %s\n", a.c_str()); mostrar_ayuda(); return false; }
        } catch (const std::exception&) {
            std::fprintf(stderr, "Valor invalido en la opcion: %s\n", a.c_str());
            return false;
        }
    }
    if (cfg.umbral_pct < 1.0 || cfg.umbral_pct > 100.0) {
        std::fprintf(stderr, "--umbral debe estar entre 1 y 100.\n");
        return false;
    }
    if (cfg.intervalo_ms < 100) {
        std::fprintf(stderr, "--intervalo minimo: 100 ms.\n");
        return false;
    }
    return true;
}

// ------------------------------------------------------------- consola

void preparar_consola() {
    HANDLE salida = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  modo   = 0;
    if (GetConsoleMode(salida, &modo))
        SetConsoleMode(salida, modo | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void borrar_pantalla() {
    // "\x1b[H" cursor a inicio, "\x1b[J" borra desde el cursor.
    std::fputs("\x1b[H\x1b[J", stdout);
    std::fflush(stdout);
}

} // namespace

// =============================================================== MAIN

int main(int argc, char** argv) {
    Config cfg;
    if (!leer_args(argc, argv, cfg))
        return 1;

    preparar_consola();
    SetConsoleCtrlHandler(al_recibir_senal, TRUE);

    std::ofstream log(cfg.salida, std::ios::out | std::ios::app);
    if (!log) {
        std::fprintf(stderr, "No se pudo abrir el log '%s' para escritura.\n", cfg.salida.c_str());
        return 1;
    }

    log << "\n============================================================\n"
        << " Nueva sesion: " << ahora_iso() << "\n"
        << " Umbral de RAM : " << cfg.umbral_pct << "%\n"
        << " Intervalo     : " << cfg.intervalo_ms << " ms\n"
        << "============================================================\n";
    log.flush();

    const auto        inicio    = std::chrono::steady_clock::now();
    TiemposCpu        t_previa  = leer_tiempos_cpu();

    double    pct_ram_max   = 0.0;
    uint64_t  ram_libre_min = UINT64_MAX;
    double    pct_cpu_max   = 0.0;
    unsigned  alertas       = 0;
    unsigned  muestras      = 0;
    bool      en_alerta     = false;
    int64_t   ultima_alerta_s = -1;

    while (!g_detener.load(std::memory_order_relaxed)) {
        // ---- esperar el intervalo y tomar la segunda muestra ----
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.intervalo_ms));

        const auto        ahora_clock = std::chrono::steady_clock::now();
        const uint64_t    transcurrido =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                      ahora_clock - inicio).count());

        const TiemposCpu t_ahora = leer_tiempos_cpu();
        const double     pct_cpu = calcular_pct_cpu(t_ahora, t_previa);
        t_previa = t_ahora;

        const Memoria    mem = leer_memoria();
        ++muestras;

        pct_ram_max = std::max(pct_ram_max, mem.pct_fisica);
        pct_cpu_max = std::max(pct_cpu_max, pct_cpu);
        ram_libre_min = std::min(ram_libre_min, mem.libre_fisica);

        const std::string extra_ram = formato_bytes(mem.libre_fisica) + " libres de " +
                                      formato_bytes(mem.total_fisica);
        const std::string extra_commit = formato_bytes(mem.total_pagefile - mem.libre_pagefile) +
                                         " de " + formato_bytes(mem.total_pagefile);

        // ---- log: flanco de subida + latido mientras siga en alerta ----
        const bool supera = mem.pct_fisica > cfg.umbral_pct;
        const bool limite_tiempo = cfg.duracion_s > 0 &&
                                   static_cast<int>(transcurrido) >= cfg.duracion_s;

        // Se escribe al cruzar el umbral y, si sigue arriba, cada relog_s.
        const bool toca_registrar =
            supera && (!en_alerta || ultima_alerta_s < 0 ||
                       transcurrido - static_cast<uint64_t>(ultima_alerta_s) >=
                           static_cast<uint64_t>(cfg.relog_s));

        if (toca_registrar) {
            ++alertas;
            ultima_alerta_s = static_cast<int64_t>(transcurrido);
            log << "[" << ahora_iso() << "] "
                << (en_alerta ? "PERSISTE" : "ALERTA ")
                << " | RAM " << std::fixed << std::setprecision(1) << mem.pct_fisica << "%"
                << " (umbral " << cfg.umbral_pct << "%)"
                << " | RAM libre " << formato_bytes(mem.libre_fisica)
                << " | commit " << std::setprecision(1) << mem.pct_compromiso << "%"
                << " | CPU " << std::setprecision(1) << pct_cpu << "%"
                << " | muestra #" << muestras
                << " | t+" << transcurrido << "s"
                << "\n";
            log.flush();
        }
        en_alerta = supera;

        // ------------------------------------------------------------
        //  Panel en pantalla
        // ------------------------------------------------------------
        borrar_pantalla();
        std::printf("%s\n", regla().c_str());
        std::printf("%s\n", fila("  VIGILANTE DE RECURSOS  ::  RAM / CPU en tiempo real").c_str());
        std::printf("%s\n", regla().c_str());
        std::printf("%s\n", fila("").c_str());

        {
            std::ostringstream os;
            os << "Muestra #" << muestras << "   Tiempo: " << formato_hora(transcurrido)
               << "   Intervalo: " << cfg.intervalo_ms << " ms";
            std::printf("%s\n", fila(os.str()).c_str());
        }

        std::printf("%s\n", fila("").c_str());
        std::printf("%s\n", fila_grafica("RAM fisica", mem.pct_fisica, extra_ram).c_str());
        std::printf("%s\n", fila_grafica("CPU total", pct_cpu, std::string("del sistema")).c_str());
        std::printf("%s\n", fila_grafica("Compromiso", mem.pct_compromiso, extra_commit).c_str());

        std::printf("%s\n", fila("").c_str());

        {
            std::ostringstream os;
            os << "Umbral de alerta : " << std::fixed << std::setprecision(1) << cfg.umbral_pct
               << "%    ->    " << (mem.pct_fisica > cfg.umbral_pct ? "SUPERADO" : "normal");
            std::printf("%s\n", fila(os.str()).c_str());
        }
        {
            std::ostringstream os;
            os << "Estado           : " << (mem.pct_fisica > cfg.umbral_pct
                                             ? "[ ALERTA ] RAM alta, hay presion de paginacion"
                                             : "[ OK     ] sin presion de memoria");
            std::printf("%s\n", fila(os.str()).c_str());
        }
        {
            std::ostringstream os;
            os << "Alertas          : " << alertas << " registro(s) en " << cfg.salida;
            std::printf("%s\n", fila(os.str()).c_str());
        }

        std::printf("%s\n", fila("").c_str());
        {
            std::ostringstream os;
            os << "Espacio virtual  : " << formato_bytes(mem.total_virtual)
               << "   libre: " << formato_bytes(mem.libre_virtual);
            std::printf("%s\n", fila(os.str()).c_str());
        }

        std::printf("%s\n", regla().c_str());
        std::printf("  Ctrl+C para detener    Q para detener\n");

        // ---- salir sin bloquear si el usuario pulsa una tecla ----
        if (_kbhit()) {
            const int c = _getch();
            if (c == 'q' || c == 'Q' || c == 27)
                break;
        }

        if (limite_tiempo)
            break;
    }

    // ------------------------------------------------------------
    //  Resumen final
    // ------------------------------------------------------------
    const uint64_t duracion =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - inicio).count());

    log << "------------------------------------------------------------\n"
        << " Fin de sesion: " << ahora_iso() << " | duracion " << duracion
        << " s | muestras " << muestras << " | alertas " << alertas
        << " | RAM max " << std::fixed << std::setprecision(1) << pct_ram_max << "%"
        << " | RAM libre minima " << formato_bytes(ram_libre_min) << "\n\n";
    log.flush();
    log.close();

    std::printf("\n");
    std::printf("%s\n", regla().c_str());
    std::printf("%s\n", fila("  RESUMEN DE LA SESION").c_str());
    std::printf("%s\n", regla().c_str());
    {
        std::ostringstream os;
        os << "Duracion            : " << duracion << " s en " << muestras << " muestra(s)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "RAM maxima          : " << std::fixed << std::setprecision(1) << pct_ram_max
           << "%   (umbral " << cfg.umbral_pct << "%)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "RAM libre minima    : " << formato_bytes(ram_libre_min);
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(1) << "CPU maxima          : "
           << pct_cpu_max << "%";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Alertas registradas : " << alertas << "  ->  " << cfg.salida;
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", regla().c_str());
    return 0;
}