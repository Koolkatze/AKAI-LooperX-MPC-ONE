// Prueba funcional del plugin "LooperX Looper" cargado como lo carga el MPC (ABI VST2):
// pulsaciones desde la "pantalla", transporte del MPC, largo fijo, exportar WAV, guardar y
// recargar el proyecto con los loops dentro, y cambio de memoria.
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <thread>
#include "vsthost.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("  (l.%d)\n", __LINE__); ++fails; } \
  else { std::printf("ok   " __VA_ARGS__); std::printf("\n"); } } while (0)

static const char* kLib = "plugins/LooperXLooper/build/native/looper.so";

struct Rig {
    th::Plug p{kLib};
    int block = 128;
    double sr = 44100;
    float ph = 0;
    double outRms = 0; int64_t outN = 0;
    Rig() { p.prepare(sr, block); run(0.01); }
    void run(double secs, float amp = 0.3f) {
        std::vector<float> L(block), R(block);
        for (int64_t d = 0; d < (int64_t)(secs * sr); d += block) {
            for (int i = 0; i < block; ++i) { ph += 330.f / (float)sr; if (ph > 1) ph -= 1; L[i] = R[i] = amp * std::sin(6.2831853f * ph); }
            p.process(L.data(), R.data(), block);
            for (int i = 0; i < block; ++i) { outRms += (double)L[i] * L[i]; ++outN; }
            th::g_tr.samplePos += block;
            if (th::g_tr.playing) th::g_tr.ppq += block * th::g_tr.bpm / 60.0 / sr;
        }
    }
    double rms() { double r = outN ? std::sqrt(outRms / outN) : 0; outRms = 0; outN = 0; return r; }
    int P(const char* n) { int i = p.find(n); if (i < 0) std::printf("param '%s' no encontrado\n", n); return i; }
};

static long fileSize(const std::string& f) { struct stat st {}; return stat(f.c_str(), &st) == 0 ? (long)st.st_size : -1; }

int main() {
    if (std::system("rm -rf /tmp/lx_export && mkdir -p /tmp/lx_export") != 0) return 2;
    setenv("LOOPERX_EXPORT_ROOT", "/tmp/lx_export", 1);
    std::vector<uint8_t> saved;
    {
        Rig r;
        CHECK(r.p.ok() && r.p.numParams() == 81, "Looper carga como plugin VST2 con 81 parámetros");
        CHECK(r.p.text(r.P("LOOP 1")) == "EMPTY", "LOOP 1 muestra EMPTY");
        r.p.set(r.P("Monitor"), 0.f);                   // solo los loops en la salida
        r.p.press(r.P("LOOP 1"));
        r.run(0.01);
        CHECK(r.p.text(r.P("LOOP 1")).rfind("REC", 0) == 0, "tocar LOOP 1 graba (\"%s\")", r.p.text(r.P("LOOP 1")).c_str());
        r.run(2.0);
        r.p.press(r.P("LOOP 1"));
        r.run(0.1);
        CHECK(r.p.text(r.P("LOOP 1")) == "PLAY 1b", "al volver a tocar se cierra en 1 compás (\"%s\")", r.p.text(r.P("LOOP 1")).c_str());
        r.rms();
        r.run(1.0, 0.f);                                  // sin entrada: solo suena el loop
        double loopRms = r.rms();
        CHECK(loopRms > 0.05, "el loop suena solo, sin entrada (rms %.3f)", loopRms);
        // Segundo loop cuantizado
        r.p.press(r.P("LOOP 2"));
        r.run(0.01);
        CHECK(r.p.text(r.P("LOOP 2")) == "WAIT", "LOOP 2 espera al compás (\"%s\")", r.p.text(r.P("LOOP 2")).c_str());
        r.run(2.0);
        CHECK(r.p.text(r.P("LOOP 2")).rfind("REC", 0) == 0, "LOOP 2 graba en el compás");
        r.run(2.0);                                       // a mitad del 2º compás de grabación
        r.p.press(r.P("LOOP 2"));
        r.run(2.1);
        CHECK(r.p.text(r.P("LOOP 2")) == "PLAY 2b", "LOOP 2 = 2 compases (\"%s\")", r.p.text(r.P("LOOP 2")).c_str());
        // Largo fijo 4/8 (8 tiempos = 2 compases), cierre automático
        int len = r.P("Length");
        r.p.set(len, 8.f / 128.f);
        r.run(0.01);
        CHECK(r.p.text(len) == "4/8", "Length muestra 4/8 (\"%s\")", r.p.text(len).c_str());
        r.p.press(r.P("LOOP 3"));
        r.run(6.2);
        CHECK(r.p.text(r.P("LOOP 3")) == "PLAY 2b", "LOOP 3 se cierra solo a 4/8 (\"%s\")", r.p.text(r.P("LOOP 3")).c_str());
        // STOP con cuantización
        r.p.press(r.P("STOP 2"));
        r.run(0.01);
        CHECK(r.p.text(r.P("LOOP 2")) == ">STOP 2b", "STOP 2 espera al compás (\"%s\")", r.p.text(r.P("LOOP 2")).c_str());
        r.run(2.1);
        CHECK(r.p.text(r.P("LOOP 2")) == "STOP 2b" && r.p.text(r.P("STOP 2")) == "START", "LOOP 2 parado; su STOP ahora dice START");
        // Exportar WAV
        r.p.press(r.P("Export WAV"));
        r.run(0.01);
        for (int k = 0; k < 100 && r.p.text(r.P("Export WAV")) != "DONE"; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); r.run(0.003); }
        CHECK(r.p.text(r.P("Export WAV")) == "DONE", "Export WAV termina (\"%s\")", r.p.text(r.P("Export WAV")).c_str());
        int files = 0; long sz1 = -1;
        {
            FILE* ls = popen("ls /tmp/lx_export/LooperX/Loops/ 2>/dev/null", "r");
            char line[512];
            while (ls && fgets(line, sizeof line, ls)) {
                ++files;
                std::string f = std::string("/tmp/lx_export/LooperX/Loops/") + line;
                f.erase(f.find_last_not_of("\r\n") + 1);
                if (f.find("_L01_") != std::string::npos) sz1 = fileSize(f);
            }
            if (ls) pclose(ls);
        }
        CHECK(files == 3, "se exportan 3 WAV (uno por loop) en LooperX/Loops (%d)", files);
        CHECK(sz1 == 44 + 88200L * 6, "el WAV de L01 es 24 bits estéreo y mide 1 compás exacto (%ld bytes)", sz1);
        // Guardar el "proyecto"
        saved = r.p.chunk();
        CHECK(saved.size() > (size_t)(88200 * 4 * 5), "los loops viajan dentro del proyecto (%zu bytes)", saved.size());
    }
    {
        // Recargar en una instancia nueva (como abrir el proyecto en el MPC)
        Rig r;
        r.p.setChunk(saved);
        r.run(0.05, 0.f);
        CHECK(r.p.text(r.P("LOOP 1")) == "PLAY 1b" && r.p.text(r.P("LOOP 2")) == "STOP 2b" && r.p.text(r.P("LOOP 3")) == "PLAY 2b",
              "al abrir el proyecto vuelven los 3 loops con su estado (\"%s\" \"%s\" \"%s\")",
              r.p.text(r.P("LOOP 1")).c_str(), r.p.text(r.P("LOOP 2")).c_str(), r.p.text(r.P("LOOP 3")).c_str());
        r.rms();
        r.run(1.0, 0.f);
        CHECK(r.rms() > 0.05, "y suenan");
        // CLEAR con doble pulsación
        r.p.press(r.P("CLEAR 1"));
        r.run(0.01);
        CHECK(r.p.text(r.P("LOOP 1")) == "CLEAR?", "1ª pulsación de CLEAR 1 pide confirmación");
        r.p.press(r.P("CLEAR 1"));
        r.run(0.01);
        CHECK(r.p.text(r.P("LOOP 1")) == "EMPTY", "2ª pulsación borra el loop 1");
        // Overdub y undo
        r.p.press(r.P("LOOP 3")); r.run(0.01);
        CHECK(r.p.text(r.P("LOOP 3")) == "DUB 2b", "LOOP 3 en overdub (\"%s\")", r.p.text(r.P("LOOP 3")).c_str());
        r.run(1.0, 0.5f);
        r.p.press(r.P("LOOP 3")); r.run(0.01);
        CHECK(r.p.text(r.P("Undo")) == "UNDO 3", "Undo ofrece deshacer el overdub del 3 (\"%s\")", r.p.text(r.P("Undo")).c_str());
        r.p.press(r.P("Undo")); r.run(0.01);
        CHECK(r.p.text(r.P("LOOP 3")) == "PLAY 2b", "Undo hecho, el loop sigue sonando");
    }
    {
        // Sincronía con el MPC en PLAY
        th::g_tr.playing = true; th::g_tr.ppq = 0;
        Rig r;
        r.p.set(r.P("Monitor"), 0.f);
        r.run(0.3);
        r.p.press(r.P("LOOP 5"));
        r.run(0.01);
        CHECK(r.p.text(r.P("LOOP 5")) == "WAIT", "con el MPC en PLAY, el primer loop espera al compás del MPC");
        r.run(1.8);
        CHECK(r.p.text(r.P("LOOP 5")).rfind("REC", 0) == 0, "graba en el compás del MPC");
        r.run(1.0); r.p.press(r.P("LOOP 5")); r.run(1.2);
        CHECK(r.p.text(r.P("LOOP 5")) == "PLAY 1b", "loop de 1 compás del MPC (\"%s\")", r.p.text(r.P("LOOP 5")).c_str());
        th::g_tr.playing = false;
        r.run(0.1);
        CHECK(r.p.text(r.P("LOOP 5")) == "STOP 1b", "al parar el MPC se para el loop");
        th::g_tr.playing = true; th::g_tr.ppq = 0;
        r.run(0.1);
        CHECK(r.p.text(r.P("LOOP 5")) == "PLAY 1b", "al darle a PLAY vuelve desde el principio");
        th::g_tr.playing = false;
        // Cambio de memoria con todo vacío
        r.p.press(r.P("CLEAR 5")); r.run(0.01); r.p.press(r.P("CLEAR 5")); r.run(0.01);
        int mem = r.P("Loop Mem");
        r.p.set(mem, 0.5f);   // 64 MB
        for (int k = 0; k < 50; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); r.run(0.003); }
        CHECK(r.p.text(mem).rfind("64MB", 0) == 0 && r.p.text(mem).find('*') == std::string::npos,
              "Loop Mem cambia a 64 MB cuando no hay loops (\"%s\")", r.p.text(mem).c_str());
    }
    std::printf("\n%s (%d fallos)\n", fails ? "LOOPER PLUGIN: FALLOS" : "LOOPER PLUGIN: TODO OK", fails);
    return fails ? 1 : 0;
}
