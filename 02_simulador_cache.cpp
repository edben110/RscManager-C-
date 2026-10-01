/*
 * =====================================================================
 *  2. SIMULADOR DE MEMORIA CACHE  -  L1 / L2 vs RAM vs Disco
 * =====================================================================
 *  Concepto : Jerarquia de memoria y optimizacion.
 *  Objetivo : Implementar caches en memoria (Map / Diccionario) para ver
 *             como el SO evita ir al disco (memoria secundaria) una y
 *             otra vez por los mismos datos.
 *
 *  Modelo implementado (3 niveles + disco, con politica LRU):
 *
 *      L1  256 KB  <- cache "de CPU",   latencia ~1 ciclo
 *      L2    8 MB  <- cache "L2",       latencia ~12 ciclos
 *      RAM  512 MB <- cache del proceso (un Map), latencia ~100 ciclos
 *      DISCO       <- fichero del SO,   latencia ~10^7 ciclos
 *
 *    Cada fichero se trocea en bloques de 16 KB. El bloque es la unidad
 *    que viaja entre niveles (como una linea de cache o una pagina).
 *    La clave del Map es "archivo#bloque".
 *    Como un fichero NO cabe entero en L1 ni en L2, se aprecia la
 *    diferencia entre acierto y fallo, y el efecto de la eviccion.
 *
 *  Librerias :
 *    - STL : std::unordered_map + std::list -> tabla hash e indice LRU
 *    - STL : std::chrono::steady_clock       -> equivalente C++ de
 *            console.time() (PHP) / time.time() (Python)
 *    - Win32: CreateFileA + FILE_FLAG_NO_BUFFERING (opcional) para
 *             saltar la cache de archivos de Windows y leer de verdad
 *             desde el disco fisico.
 *
 *  Compilar (MinGW/MSYS2 UCRT64):
 *    g++ -std=c++17 -O2 02_simulador_cache.cpp -o cache.exe
 *  Compilar (MSVC):
 *    cl /std:c++17 /EHsc /O2 02_simulador_cache.cpp
 *
 *  Uso:
 *    cache.exe [--archivos 8] [--tamano 4096] [--iteraciones 6]
 *              [--bloque 16] [--l1 256] [--l2 8192] [--ram 524288]
 *              [--sin-cache-os] [--carpeta datos_cache]
 *    Todos los tamanos van en KB.
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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <list>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int ANCHO = 78;
constexpr size_t KB = 1024ull;
constexpr size_t MB = 1024ull * 1024ull;

using Reloj = std::chrono::steady_clock;

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
    char        txt[64];
    const double v = static_cast<double>(bytes);
    if (v >= static_cast<double>(MB)) std::snprintf(txt, sizeof txt, "%.2f MB", v / MB);
    else std::snprintf(txt, sizeof txt, "%.0f KB", static_cast<double>(bytes) / KB);
    return txt;
}

// =====================================================================
//  Cache LRU sobre tabla hash (unordered_map) + lista doblemente enlazada.
//  La lista mantiene el orden de uso: frente = mas reciente, fondo = LRU.
//  Al superar la capacidad en bytes se expulsa el elemento mas antiguo.
// =====================================================================
template <typename K, typename V>
class CacheLRU {
public:
    CacheLRU(std::string nombre, size_t capacidad_bytes)
        : nombre_(std::move(nombre)), capacidad_(capacidad_bytes) {}

    // true = acierto. "salida" recibe el valor copiado.
    bool obtener(const K& clave, V& salida) {
        ++consultas_;
        auto it = indice_.find(clave);
        if (it == indice_.end()) {
            ++fallos_;
            return false;
        }
        // Promocion a la cabecera. Si ya estaba al frente, splice es no-op.
        lista_.splice(lista_.begin(), lista_, it->second);
        salida = it->second->second.valor;
        ++aciertos_;
        return true;
    }

    void insertar(const K& clave, const V& valor, size_t peso) {
        auto it = indice_.find(clave);
        if (it != indice_.end()) {          // ya estaba: se actualiza
            bytes_ -= it->second->second.peso;
            lista_.erase(it->second);
            indice_.erase(it);
            --entradas_;
        }
        lista_.emplace_front(clave, Entrada{valor, peso});
        indice_[clave] = lista_.begin();
        bytes_ += peso;
        ++entradas_;
        ++inserciones_;

        while (bytes_ > capacidad_ && !lista_.empty()) {
            bytes_ -= lista_.back().second.peso;
            indice_.erase(lista_.back().first);
            lista_.pop_back();
            --entradas_;
            ++evicciones_;
        }
    }

    void vaciar() {
        lista_.clear();
        indice_.clear();
        bytes_    = 0;
        entradas_ = 0;
    }

    void reiniciar_estadisticas() {
        consultas_ = aciertos_ = fallos_ = inserciones_ = evicciones_ = 0;
    }

    const std::string& nombre()       const { return nombre_; }
    size_t             capacidad()   const { return capacidad_; }

    // Consulta de solo lectura: no altera el orden LRU ni las estadisticas.
    // Se usa para verificar que la cache devuelve los datos originales.
    bool buscar(const K& clave, V& salida) const {
        auto it = indice_.find(clave);
        if (it == indice_.end()) return false;
        salida = it->second->second.valor;
        return true;
    }
    size_t             bytes()       const { return bytes_; }
    size_t             entradas()    const { return entradas_; }
    uint64_t           aciertos()    const { return aciertos_; }
    uint64_t           fallos()      const { return fallos_; }
    uint64_t           evicciones()  const { return evicciones_; }
    double             tasa()        const {
        return consultas_ ? 100.0 * static_cast<double>(aciertos_) /
                                static_cast<double>(consultas_)
                          : 0.0;
    }

private:
    struct Entrada { V valor; size_t peso; };
    using Lista = std::list<std::pair<K, Entrada>>;

    std::string  nombre_;
    size_t       capacidad_   = 0;
    size_t       bytes_       = 0;
    size_t       entradas_    = 0;
    Lista        lista_;                       // frente = mas reciente
    std::unordered_map<K, typename Lista::iterator> indice_;

    uint64_t consultas_   = 0;
    uint64_t aciertos_    = 0;
    uint64_t fallos_      = 0;
    uint64_t inserciones_ = 0;
    uint64_t evicciones_  = 0;
};

// =====================================================================
//  Buffer alineado a 4096 bytes. Necesario para
//  FILE_FLAG_NO_BUFFERING, que exige buffer, offset y longitud alineados
//  al tamano de sector del volumen.
// =====================================================================
class BufferAlineado {
public:
    BufferAlineado() = default;
    ~BufferAlineado() { liberar(); }
    BufferAlineado(const BufferAlineado&)            = delete;
    BufferAlineado& operator=(const BufferAlineado&) = delete;

    void reservar(size_t bytes) {
        liberar();
        base_ = static_cast<char*>(std::malloc(bytes + 4096));
        if (!base_) return;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(base_);
        datos_ = reinterpret_cast<char*>((addr + 4095) & ~static_cast<uintptr_t>(4095));
        capacidad_ = bytes;
    }

    void liberar() {
        std::free(base_);
        base_ = datos_ = nullptr;
        capacidad_ = 0;
    }

    char*  datos()     const { return datos_; }
    size_t capacidad() const { return capacidad_; }
    bool   valido()    const { return datos_ != nullptr; }

private:
    char*  base_      = nullptr;
    char*  datos_     = nullptr;
    size_t capacidad_ = 0;
};

// =====================================================================
//  Capa de disco: crea los ficheros de prueba y lee bloques sueltos.
// =====================================================================
class Disco {
public:
    Disco() = default;
    ~Disco() { cerrar(); }
    Disco(const Disco&)            = delete;
    Disco& operator=(const Disco&) = delete;

    void usar_sin_cache_os(bool v) { sin_cache_os_ = v; }

    bool preparar(const std::string& carpeta, size_t archivos, size_t bytes_por_archivo) {
        cerrar();
        crear_carpeta(carpeta);

        rutas_.reserve(archivos);
        streams_.reserve(archivos);
        std::mt19937 rng(20240917);   // semilla fija: datos reproducibles

        // Se escribe en trozos de 1 MB para no Reservar un fichero entero.
        std::vector<char> trozo(1024 * 1024);

        for (size_t i = 0; i < archivos; ++i) {
            char nombre[64];
            std::snprintf(nombre, sizeof nombre, "datos_%03zu.bin", i);
            rutas_.push_back(carpeta + "\\" + nombre);

            if (tamano_de(rutas_.back()) != bytes_por_archivo) {
                std::ofstream out(rutas_.back(), std::ios::binary | std::ios::trunc);
                if (!out) return false;
                size_t escritos = 0;
                while (escritos < bytes_por_archivo) {
                    const size_t n = std::min(trozo.size(), bytes_por_archivo - escritos);
                    for (size_t j = 0; j < n; j += 4) {
                        const uint32_t v = rng();
                        std::memcpy(trozo.data() + j, &v, std::min<size_t>(4, n - j));
                    }
                    out.write(trozo.data(), static_cast<std::streamsize>(n));
                    if (!out) return false;
                    escritos += n;
                }
                out.close();
            }

            streams_.emplace_back(rutas_.back(), std::ios::binary);
            if (!streams_.back()) return false;
        }
        return true;
    }

    // Lee "bytes" en "offset" del fichero indicado.
    bool leer(size_t indice_archivo, size_t offset, char* destino, size_t bytes) {
        if (indice_archivo >= rutas_.size()) return false;
        return sin_cache_os_ ? leer_nativo(indice_archivo, offset, destino, bytes)
                             : leer_flujo(indice_archivo, offset, destino, bytes);
    }

    void cerrar() {
        for (auto& f : streams_) f.close();
        streams_.clear();
        for (HANDLE h : nativos_) {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        nativos_.clear();
        rutas_.clear();
    }

private:
    static void crear_carpeta(const std::string& ruta) {
        if (GetFileAttributesA(ruta.c_str()) == INVALID_FILE_ATTRIBUTES)
            CreateDirectoryA(ruta.c_str(), nullptr);
    }

    static uint64_t tamano_de(const std::string& ruta) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExA(ruta.c_str(), GetFileExInfoStandard, &fad)) return 0;
        return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }

    bool leer_flujo(size_t i, size_t offset, char* destino, size_t bytes) {
        std::ifstream& f = streams_[i];
        f.clear();
        f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!f) return false;
        f.read(destino, static_cast<std::streamsize>(bytes));
        return static_cast<size_t>(f.gcount()) == bytes;
    }

    bool abrir_nativo(size_t i) {
        if (nativos_.size() <= i) nativos_.resize(i + 1, INVALID_HANDLE_VALUE);
        if (nativos_[i] != INVALID_HANDLE_VALUE) return true;

        DWORD flags = FILE_ATTRIBUTE_NORMAL;
        if (sin_cache_os_) flags |= FILE_FLAG_NO_BUFFERING;

        nativos_[i] = CreateFileA(rutas_[i].c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, flags, nullptr);
        return nativos_[i] != INVALID_HANDLE_VALUE;
    }

    bool leer_nativo(size_t i, size_t offset, char* destino, size_t bytes) {
        if (!abrir_nativo(i)) return false;

        // OVERLAPPED en lugar de SetFilePointer: con FILE_FLAG_NO_BUFFERING
        // el desplazamiento debe ir en la propia lectura y ambos deben estar
        // alineados al sector, cosa que no se puede garantizar con un puntero
        // de fichero compartido entre llamadas.
        OVERLAPPED ov{};
        ov.Offset     = static_cast<DWORD>(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

        size_t leidos = 0;
        while (leidos < bytes) {
            DWORD n = 0;
            if (!ReadFile(nativos_[i], destino + leidos,
                          static_cast<DWORD>(bytes - leidos), &n, &ov))
                return false;
            if (n == 0) return false;
            leidos += n;
            ov.Offset += n;   // siguiente trozo contiguo del mismo bloque
        }
        return true;
    }

    std::vector<std::string>  rutas_;
    std::vector<std::ifstream> streams_;
    std::vector<HANDLE>        nativos_;
    bool                       sin_cache_os_ = false;
};

// =====================================================================
//  Cadena de caches: L1 -> L2 -> RAM -> Disco
// =====================================================================
enum class Origen { L1, L2, RAM, DISCO };

// Los tres niveles no guardan lo mismo, igual que en el hardware real:
//
//   L1 y L2 guardan LINEAS de 4 KB, no el bloque entero. Asi L1 puede
//   contener 64 lineas con 256 KB, y un acierto en L2 evita el viaje a RAM
//   aunque el bloque completo no quepa en L1.
//   RAM guarda el bloque completo: es el "objeto" que el programa conserva.
constexpr size_t kBytesLinea = 4096;

class CadenaCache {
public:
    CadenaCache(size_t cap_l1, size_t cap_l2, size_t cap_ram)
        : l1_("L1", cap_l1), l2_("L2", cap_l2), ram_("RAM", cap_ram) {}

    // Busca en el orden real de la jerarquia: L1 -> L2 -> RAM -> disco.
    // Si devuelve DISCO, el llamador debe leer de disco.
    // L1 y L2 responden linea a linea, igual que el hardware. Un nivel
    // sirve el bloque si tiene TODAS sus lineas; si no, se baja al
    // siguiente nivel, que es lo que hace el procesador.
    Origen buscar(const std::string& bloque, std::string& salida) {
        const size_t    lineas = (bloque.size() + kBytesLinea - 1) / kBytesLinea;
        std::string     tmp;

        // 1) L1
        if (intentar_nivel(l1_, nullptr, bloque, lineas, salida)) {
            ++aciertos_l1_;
            return Origen::L1;
        }

        // 2) L2. Al servir desde aqui se promociona a L1, que es el
        //    comportamiento normal: lo recien usado se recalienta en L1.
        if (intentar_nivel(l2_, &l1_, bloque, lineas, salida)) {
            ++aciertos_l2_;
            return Origen::L2;
        }

        // 3) RAM: el "objeto" completo que el programa conserva.
        if (ram_.obtener(bloque, salida)) {
            ++aciertos_ram_;
            promover(bloque, salida);   // RAM -> L2 -> L1, linea a linea
            return Origen::RAM;
        }

        // 4) Disco (memoria secundaria).
        ++fallos_ram_;
        return Origen::DISCO;
    }

    // Un bloque recien leido del disco se guarda en RAM y se promociona.
    void cargar(const std::string& bloque, const std::string& valor) {
        ram_.insertar(bloque, valor, valor.size());
        promover(bloque, valor);
    }

    CacheLRU<std::string, std::string>& l1()  { return l1_; }
    CacheLRU<std::string, std::string>& l2()  { return l2_; }
    CacheLRU<std::string, std::string>& ram() { return ram_; }

    // Consulta de solo lectura, para verificar la integridad sin alterar
    // el orden LRU ni las estadisticas de acierto/fallo.
    bool comprobar(const std::string& bloque, std::string& salida) const {
        return ram_.buscar(bloque, salida);
    }

    uint64_t desde(Origen o) const {
        switch (o) {
            case Origen::L1:    return aciertos_l1_;
            case Origen::L2:    return aciertos_l2_;
            case Origen::RAM:   return aciertos_ram_;
            case Origen::DISCO: return fallos_ram_;
        }
        return 0;
    }

    uint64_t lineas_l1() const { return l1_.aciertos(); }
    uint64_t lineas_l2() const { return l2_.aciertos(); }
    uint64_t lineas_ram() const { return ram_.aciertos(); }

private:
    // Clave de una linea: "archivo#bloque@linea"
    static std::string clave_linea(const std::string& bloque, size_t linea) {
        return bloque + "@" + std::to_string(linea);
    }

    // Intenta servir el bloque entero desde un nivel de lineas.
    // Devuelve false si a ese nivel le falta alguna linea; entonces se
    // baja al siguiente nivel, que es justo lo que hace el procesador.
    // Si "destino" no es nulo, cada linea servida se copia tambien ahi
    // (una promocion L2 -> L1, sin volver a consultar el nivel origen).
    bool intentar_nivel(CacheLRU<std::string, std::string>& nivel,
                        CacheLRU<std::string, std::string>* destino,
                        const std::string& bloque, size_t lineas,
                        std::string& salida) {
        std::vector<std::string> partes;
        partes.reserve(lineas);
        std::string tmp;
        for (size_t i = 0; i < lineas; ++i) {
            const std::string clave = clave_linea(bloque, i);
            if (!nivel.obtener(clave, tmp)) return false;
            partes.push_back(tmp);
            if (destino) destino->insertar(clave, tmp, tmp.size());
        }
        ensamblar(partes, salida);
        return true;
    }

    // Copia las lineas consecutivas en un unico bloque.
    static void ensamblar(const std::vector<std::string>& lineas, std::string& salida) {
        size_t total = 0;
        for (const auto& l : lineas) total += l.size();
        salida.clear();
        salida.reserve(total);
        for (const auto& l : lineas) salida += l;
    }

    // Vuelca un bloque en L2 y luego en L1, linea a linea.
    void promover(const std::string& bloque, const std::string& valor) {
        const size_t lineas = (valor.size() + kBytesLinea - 1) / kBytesLinea;
        for (size_t i = 0; i < lineas; ++i) {
            const size_t ini = i * kBytesLinea;
            if (ini >= valor.size()) break;
            const size_t fin = std::min(ini + kBytesLinea, valor.size());
            const std::string lin = valor.substr(ini, fin - ini);
            l2_.insertar(clave_linea(bloque, i), lin, lin.size());
            l1_.insertar(clave_linea(bloque, i), lin, lin.size());
        }
    }

    CacheLRU<std::string, std::string> l1_, l2_, ram_;
    uint64_t aciertos_l1_ = 0, aciertos_l2_ = 0, aciertos_ram_ = 0, fallos_ram_ = 0;
    uint64_t intentos_l2_ = 0;
};

// Huella barata de un bloque: lee 1 byte de cada 64.
// Sirve para comprobar que los datos del disco y los de la cache coinciden.
uint64_t huella(const char* datos, size_t bytes) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(datos);
    for (size_t j = 0; j < bytes; j += 64)
        h = (h ^ p[j]) * 1099511628211ull;
    return h;
}

// =====================================================================
//  Configuracion y argumentos
// =====================================================================
struct Config {
    size_t      archivos      = 8;
    size_t      tamano_kb     = 4096;
    size_t      iteraciones   = 6;
    size_t      bloque_kb     = 16;
    size_t      l1_kb         = 256;
    size_t      l2_kb         = 8192;
    size_t      ram_kb        = 512ull * 1024;
    // Porcentaje de accesos que caen sobre una zona caliente pequena, como
    // ocurre en un programa real. Es lo que permite que L1 y L2 acierten:
    // un recorrido secuencial puro no repite datos y ninguna cache de
    // processor sirve de nada.
    unsigned    caliente_pct  = 50;
    unsigned    semilla       = 12345;
    bool        sin_cache_os  = false;
    std::string carpeta       = "datos_cache";
};

// Generador congruencial lineal rapido, determinista.
uint32_t siguiente(uint32_t& estado) {
    estado = estado * 1664525u + 1013904223u;
    return estado >> 8;
}

void mostrar_ayuda() {
    std::printf(
        "Simulador de cache L1/L2/RAM vs Disco\n\n"
        "  --archivos <n>    Numero de ficheros a crear (por defecto 8)\n"
        "  --tamano <kb>     Tamano de cada fichero (por defecto 4096 KB)\n"
        "  --iteraciones <n> Veces que se recorre todo el conjunto (por defecto 6)\n"
        "  --bloque <kb>     Tamano de bloque, la unidad que sube por la jerarquia\n"
        "                    (por defecto 16 KB)\n"
        "  --l1 <kb>         Capacidad de L1 (por defecto 256 KB)\n"
        "  --l2 <kb>         Capacidad de L2 (por defecto 8192 KB)\n"
        "  --caliente <pct>  Porcentaje de accesos sobre una zona repetida (50).\n"
        "                    Con 0 el recorrido es puramente secuencial y, como\n"
        "                    en el hardware real, L1 y L2 no aciertan casi nunca\n"
        "  --semilla <n>    Semilla del patron de acceso, para reproducir la\n"
        "                    misma ejecucion\n"
        "  --ram <kb>        Capacidad de la cache en RAM del proceso (por defecto 524288 = 512 MB)\n"
        "  --sin-cache-os    Leer del disco real saltando la cache de archivos de\n"
        "                    Windows con FILE_FLAG_NO_BUFFERING\n"
        "  --carpeta <ruta>  Carpeta de trabajo (por defecto datos_cache)\n"
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
            if      (a == "--archivos" && pedir_valor(argc, argv, i, "--archivos", v)) c.archivos    = std::stoull(v);
            else if (a == "--tamano"   && pedir_valor(argc, argv, i, "--tamano", v))   c.tamano_kb   = std::stoull(v);
            else if (a == "--iteraciones" && pedir_valor(argc, argv, i, "--iteraciones", v)) c.iteraciones = std::stoull(v);
            else if (a == "--bloque"   && pedir_valor(argc, argv, i, "--bloque", v))   c.bloque_kb   = std::stoull(v);
            else if (a == "--l1"       && pedir_valor(argc, argv, i, "--l1", v))       c.l1_kb       = std::stoull(v);
            else if (a == "--l2"       && pedir_valor(argc, argv, i, "--l2", v))       c.l2_kb       = std::stoull(v);
            else if (a == "--ram"      && pedir_valor(argc, argv, i, "--ram", v))      c.ram_kb      = std::stoull(v);
            else if (a == "--caliente" && pedir_valor(argc, argv, i, "--caliente", v)) c.caliente_pct = static_cast<unsigned>(std::stoul(v));
            else if (a == "--semilla" && pedir_valor(argc, argv, i, "--semilla", v)) c.semilla      = static_cast<unsigned>(std::stoul(v));
            else if (a == "--carpeta"  && pedir_valor(argc, argv, i, "--carpeta", v))  c.carpeta     = v;
            else if (a == "--sin-cache-os") c.sin_cache_os = true;
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
    if (c.archivos == 0 || c.iteraciones == 0 || c.bloque_kb == 0) {
        std::fprintf(stderr, "--archivos, --iteraciones y --bloque deben ser > 0.\n");
        return false;
    }
    if (c.caliente_pct > 100) {
        std::fprintf(stderr, "--caliente debe estar entre 0 y 100.\n");
        return false;
    }
    return true;
}

size_t ajustar_a_sector(size_t bytes) {
    constexpr size_t SECTOR = 4096;
    return ((bytes + SECTOR - 1) / SECTOR) * SECTOR;
}

void imprimir_cabecera(const Config& c, size_t bytes_archivo, size_t bloques_archivo) {
    std::printf("%s\n", regla().c_str());
    std::printf("%s\n", fila("SIMULADOR DE CACHE  ::  L1 / L2 / RAM / Disco").c_str());
    std::printf("%s\n", regla().c_str());
    {
        std::ostringstream os;
        os << "Ficheros : " << c.archivos << " x " << formato_bytes(bytes_archivo)
           << " = " << formato_bytes(bytes_archivo * c.archivos) << " en total";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Bloque   : " << c.bloque_kb << " KB -> " << bloques_archivo
           << " bloques/fichero (unidad que sube por la jerarquia)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Capacidad: L1 " << c.l1_kb << " KB | L2 " << c.l2_kb << " KB | RAM "
           << formato_bytes(c.ram_kb * KB) << " | disco sin limite practico";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Recorrido: " << c.iteraciones << " iteracion(es) sobre el conjunto completo";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Disco    : " << (c.sin_cache_os
                                   ? "FILE_FLAG_NO_BUFFERING, lectura fisica real"
                                   : "lectura normal, Windows puede cachear el fichero");
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("\n%s Generando datos de prueba, espera...\n", fila("").c_str());
}

void imprimir_tabla_caches(CadenaCache& cadena) {
    std::printf("%s\n", fila("").c_str());
    {
        std::ostringstream os;
        os << std::left << std::setw(6) << "CACHE" << std::setw(11) << "CAPACIDAD"
           << std::setw(11) << "OCUPADO" << std::setw(9) << "ACIERTOS"
           << std::setw(8) << "FALLOS" << std::setw(11) << "EVICCIONES" << "TASA ACIERTO";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", separador().c_str());

    const CacheLRU<std::string, std::string>* niveles[] = {
        &cadena.l1(), &cadena.l2(), &cadena.ram()};

    for (const auto* n : niveles) {
        std::ostringstream os;
        os << std::left << std::setw(6) << n->nombre()
           << std::setw(11) << formato_bytes(n->capacidad())
           << std::setw(11) << formato_bytes(n->bytes())
           << std::setw(9)  << n->aciertos()
           << std::setw(8)  << n->fallos()
           << std::setw(11) << n->evicciones()
           << std::fixed << std::setprecision(1) << n->tasa() << " %";
        std::printf("%s\n", fila(os.str()).c_str());
    }
}

} // namespace

// =============================================================== MAIN

int main(int argc, char** argv) {
    Config cfg;
    if (!leer_args(argc, argv, cfg))
        return 1;

    const size_t bytes_por_archivo   = ajustar_a_sector(cfg.tamano_kb * KB);
    const size_t bytes_bloque        = ajustar_a_sector(cfg.bloque_kb * KB);
    const size_t bloques_por_archivo = bytes_por_archivo / bytes_bloque;
    const size_t bloques_totales     = bloques_por_archivo * cfg.archivos;

    if (bytes_por_archivo < bytes_bloque) {
        std::fprintf(stderr, "--tamano debe ser >= --bloque.\n");
        return 1;
    }

    imprimir_cabecera(cfg, bytes_por_archivo, bloques_por_archivo);

    Disco disco;
    disco.usar_sin_cache_os(cfg.sin_cache_os);
    if (!disco.preparar(cfg.carpeta, cfg.archivos, bytes_por_archivo)) {
        std::fprintf(stderr, "No se pudieron preparar los ficheros en '%s'.\n",
                     cfg.carpeta.c_str());
        return 1;
    }

    BufferAlineado buffer;
    buffer.reservar(bytes_bloque);
    if (!buffer.valido()) {
        std::fprintf(stderr, "No se pudo reservar el buffer de %zu KB.\n", cfg.bloque_kb);
        return 1;
    }

    // =============================================================
    //  FASE A - referencia: leer SIEMPRE desde disco, ignorando la cache.
    // =============================================================
    const auto    t_disco_ini   = Reloj::now();
    const size_t  lecturas_a    = bloques_totales * cfg.iteraciones;

    for (size_t it = 0; it < cfg.iteraciones; ++it) {
        for (size_t a = 0; a < cfg.archivos; ++a) {
            for (size_t b = 0; b < bloques_por_archivo; ++b) {
                if (!disco.leer(a, b * bytes_bloque, buffer.datos(), bytes_bloque)) {
                    std::fprintf(stderr, "Error de lectura de disco (fichero %zu, bloque %zu).\n", a, b);
                    return 1;
                }
            }
        }
    }
    const double ms_disco = std::chrono::duration<double, std::milli>(
                                Reloj::now() - t_disco_ini).count();

    // =============================================================
    //  FASE B - leer a traves de la cadena L1 -> L2 -> RAM -> Disco.
    //  La primera pasada falla en todos los niveles; las siguientes
    //  aciertan y ya no tocan el disco.
    // =============================================================
    CadenaCache cadena(cfg.l1_kb * KB, cfg.l2_kb * KB, cfg.ram_kb * KB);

    std::vector<double>  ms_por_pasada;
    ms_por_pasada.reserve(cfg.iteraciones);
    uint64_t             lecturas_b     = 0;

    // Zona caliente: un grupo de bloques contiguos que se repite mucho, como
    // los datos que un programa real usa una y otra vez.
    const size_t bloques_calientes = bloques_totales / 4 + 1;

    for (size_t it = 0; it < cfg.iteraciones; ++it) {
        const auto t_ini = Reloj::now();
        uint32_t   rng  = cfg.semilla + static_cast<uint32_t>(it * 7919u);

        for (size_t n = 0; n < bloques_totales; ++n) {
            // Decide si este acceso cae en la zona caliente o en el resto.
            const bool es_caliente = cfg.caliente_pct > 0 &&
                                     (siguiente(rng) % 100u) < cfg.caliente_pct;
            const size_t indice = es_caliente
                ? static_cast<size_t>(siguiente(rng) % bloques_calientes)
                : n;

            const size_t a = indice / bloques_por_archivo;
            const size_t b = indice % bloques_por_archivo;

            const std::string clave = std::to_string(a) + "#" + std::to_string(b);
            std::string     valor;
            const Origen    origen = cadena.buscar(clave, valor);

            if (origen == Origen::DISCO) {
                // Fallo en RAM: toca ir al disco (memoria secundaria).
                if (!disco.leer(a, b * bytes_bloque, buffer.datos(), bytes_bloque)) {
                    std::fprintf(stderr, "Error de lectura de disco (fichero %zu, bloque %zu).\n", a, b);
                    return 1;
                }
                valor.assign(buffer.datos(), bytes_bloque);
                cadena.cargar(clave, valor);   // el objeto queda guardado en RAM
            }

            ++lecturas_b;
        }
        ms_por_pasada.push_back(std::chrono::duration<double, std::milli>(
                                    Reloj::now() - t_ini).count());
    }

    const double ms_primera = ms_por_pasada.empty() ? 0.0 : ms_por_pasada.front();

    // Media de las pasadas ya calientes (de la segunda en adelante).
    double   suma_estable = 0.0;
    size_t   n_estable    = 0;
    for (size_t i = 1; i < ms_por_pasada.size(); ++i) {
        suma_estable += ms_por_pasada[i];
        ++n_estable;
    }
    const double ms_estable = n_estable ? suma_estable / static_cast<double>(n_estable)
                                        : ms_primera;

    // =============================================================
    //  Verificacion de integridad: cada bloque se lee del disco y se compara
    //  con lo que la cadena de caches tiene guardado. Es la prueba de que la
    //  cache devuelve exactamente los mismos datos, no una aproximacion.
    // =============================================================
    size_t bloques_coinciden  = 0;   // en cache y con los datos correctos
    size_t bloques_discrepan = 0;   // en cache pero con datos distintos: ERROR
    size_t bloques_sin_cache = 0;   // no llegaron a tocar la cache
    {
        std::string valor_cache;
        for (size_t a = 0; a < cfg.archivos; ++a) {
            for (size_t b = 0; b < bloques_por_archivo; ++b) {
                const std::string clave = std::to_string(a) + "#" + std::to_string(b);
                if (!cadena.comprobar(clave, valor_cache)) {
                    ++bloques_sin_cache;
                    continue;
                }
                if (!disco.leer(a, b * bytes_bloque, buffer.datos(), bytes_bloque)) continue;
                if (huella(valor_cache.data(), valor_cache.size()) ==
                    huella(buffer.datos(), bytes_bloque))
                    ++bloques_coinciden;
                else
                    ++bloques_discrepan;
            }
        }
    }

    // =============================================================
    //  Informe
    // =============================================================
    std::printf("\n%s\n", regla().c_str());
    std::printf("%s\n", fila("DE DONDE SALIO CADA LECTURA").c_str());
    std::printf("%s\n", regla().c_str());

    struct Fila { const char* nombre; Origen origen; const char* nota; };
    const Fila filas[] = {
        {"L1",    Origen::L1,    "cercana al procesador"},
        {"L2",    Origen::L2,    "sigue siendo memoria, pero mas pequena"},
        {"RAM",   Origen::RAM,   "el Map del proceso, todavia muy rapido"},
        {"disco", Origen::DISCO, "memoria secundaria: el viaje que la cache evita"},
    };
    for (const Fila& f : filas) {
        const uint64_t n = cadena.desde(f.origen);
        std::ostringstream os;
        os << std::left << std::setw(7) << f.nombre << std::setw(8) << n << "lectura(s)"
           << std::setw(8)
           << std::fixed << std::setprecision(1)
           << (lecturas_b ? 100.0 * static_cast<double>(n) /
                             static_cast<double>(lecturas_b)
                          : 0.0)
           << "%   " << f.nota;
        std::printf("%s\n", fila(os.str()).c_str());
    }

    std::printf("%s\n", fila("").c_str());
    {
        std::ostringstream os;
        os << "L1 y L2 guardan lineas de 4 KB (como el hardware); RAM guarda bloques "
           << "de " << cfg.bloque_kb << " KB completos.";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << std::left << std::setw(18) << "Lineas servidas" << std::setw(12) << "L1"
           << std::setw(12) << "L2" << "RAM (bloques)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << std::left << std::setw(18) << "" << std::setw(12) << cadena.lineas_l1()
           << std::setw(12) << cadena.lineas_l2() << cadena.lineas_ram();
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Accesos con locality : " << cfg.caliente_pct << "% sobre una zona caliente de "
           << bloques_calientes << " bloques";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", fila("").c_str());
    imprimir_tabla_caches(cadena);

    std::printf("%s\n", fila("").c_str());
    std::printf("%s\n", fila("TIEMPOS  (std::chrono::steady_clock)").c_str());
    std::printf("%s\n", separador().c_str());

    {
        std::ostringstream os;
        os << "A) Solo disco : " << std::fixed << std::setprecision(2) << ms_disco
           << " ms por " << lecturas_a << " lecturas ("
           << std::setprecision(4) << ms_disco / static_cast<double>(lecturas_a)
           << " ms/lectura)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "B) 1a pasada : " << std::setprecision(2) << ms_primera
           << " ms   (cold: todo falla, se lee del disco y se guarda en RAM)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "C) Caliente   : " << std::setprecision(2) << ms_estable
           << " ms de media por pasada   ("
           << std::setprecision(4)
           << ms_estable / static_cast<double>(bloques_totales) << " ms/lectura)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Ganancia     : " << std::setprecision(1)
           << (ms_estable > 0.0 ? ms_disco / ms_estable : 0.0)
           << "x mas rapido usando la cache que yendo al disco";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "Integridad   : " << (bloques_discrepan == 0 ? "OK" : "ERROR")
           << "   " << bloques_coinciden << " bloques de la cache coinciden con el disco";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    {
        std::ostringstream os;
        os << "   " << bloques_sin_cache
           << " bloques no llegaron a la cache (no los cubrio el recorrido)";
        std::printf("%s\n", fila(os.str()).c_str());
    }
    std::printf("%s\n", regla().c_str());

    std::printf("\n  Conclusion: el SO aplica este mismo principio.\n");
    std::printf("  La cache de la CPU (L1/L2) y la del sistema evitan que cada acceso\n");
    std::printf("  repetido tenga que llegar a la memoria secundaria. Aqui los %zu bloques\n",
                cadena.ram().entradas());
    std::printf("  de %zu KB son los objetos guardados en RAM por el programa.\n",
                cfg.bloque_kb);

    return 0;
}