// Prueba del enlace entre plugins LooperX cargados en el MISMO proceso, como en el MPC:
// LooperX Pads dispara los loops de LooperX Looper con notas y CC, y LooperX Granular granula un
// loop vivo del Looper. También: LEARN de Pads, dos Loopers (el segundo queda "BUSY" y hereda el
// enlace cuando se borra el primero) y Granular con un WAV.
#include <chrono>
#include <thread>
#include "vsthost.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("  (l.%d)\n", __LINE__); ++fails; } \
  else { std::printf("ok   " __VA_ARGS__); std::printf("\n"); } } while (0)

static const char* kLooper = "plugins/LooperXLooper/build/native/looper.so";
static const char* kPads = "plugins/LooperXPads/build/native/pads.so";
static const char* kGran = "plugins/LooperXGranular/build/native/granular.so";

struct World {
    th::Plug looper{kLooper}, pads{kPads}, gran{kGran};
    std::vector<th::Plug*> extra;
    int block = 128;
    float ph = 0;
    double granRms = 0; int64_t granN = 0;
    World() { looper.prepare(44100, block); pads.prepare(44100, block); gran.prepare(44100, block); }
    void run(double secs, float amp = 0.3f) {
        std::vector<float> L(block), R(block), gl(block), gr(block), pl(block), pr(block);
        for (int64_t d = 0; d < (int64_t)(secs * 44100); d += block) {
            for (int i = 0; i < block; ++i) { ph += 220.f / 44100.f; if (ph > 1) ph -= 1; L[i] = R[i] = amp * std::sin(6.2831853f * ph); }
            std::fill(pl.begin(), pl.end(), 0.f); std::fill(pr.begin(), pr.end(), 0.f);
            pads.process(pl.data(), pr.data(), block);
            looper.process(L.data(), R.data(), block);
            for (auto* x : extra) { std::vector<float> a(L), b(R); x->process(a.data(), b.data(), block); }
            std::fill(gl.begin(), gl.end(), 0.f); std::fill(gr.begin(), gr.end(), 0.f);
            gran.process(gl.data(), gr.data(), block);
            for (int i = 0; i < block; ++i) { granRms += (double)gl[i] * gl[i]; ++granN; }
            th::g_tr.samplePos += block;
            if (th::g_tr.playing) th::g_tr.ppq += block * th::g_tr.bpm / 60.0 / 44100.0;
        }
    }
    double grms() { double r = granN ? std::sqrt(granRms / granN) : 0; granRms = 0; granN = 0; return r; }
    std::string L(int s) { return looper.text(looper.find("LOOP " + std::to_string(s))); }
    std::string P(int s) { return pads.text(pads.find("LOOP " + std::to_string(s))); }
    void wait(int ms) { for (int k = 0; k < ms / 10; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); run(0.003, 0.f); } }
};

int main() {
    World w;
    CHECK(w.looper.ok() && w.pads.ok() && w.gran.ok(), "los tres plugins cargan en el mismo proceso");
    w.run(0.05);
    CHECK(w.looper.text(w.looper.find("Link Pads")) == "LINKED", "Looper: Link Pads = LINKED");
    CHECK(w.pads.text(w.pads.find("Status")) == "LINKED", "Pads ve al Looper (\"%s\")", w.pads.text(w.pads.find("Status")).c_str());
    w.looper.set(w.looper.find("Monitor"), 0.f);

    // Pad A01 (nota 36) = LOOP 1
    w.pads.midi({{0x90, 36, 100}}); w.run(0.01);
    CHECK(w.L(1).rfind("REC", 0) == 0, "pad A01 (nota 36) → Looper LOOP 1 graba (\"%s\")", w.L(1).c_str());
    CHECK(w.P(1) == "REC", "la pantalla de Pads muestra REC en LOOP 1 (\"%s\")", w.P(1).c_str());
    w.pads.midi({{0x80, 36, 0}});
    w.run(2.0);
    w.pads.midi({{0x90, 36, 100}}); w.run(0.1);
    CHECK(w.L(1) == "PLAY 1b" && w.P(1) == "PLAY 1b", "segundo golpe cierra el loop (Looper \"%s\", Pads \"%s\")", w.L(1).c_str(), w.P(1).c_str());

    // Banco B: STOP 1 (nota 36+16)
    w.pads.midi({{0x90, 52, 100}}); w.run(2.1);
    CHECK(w.L(1) == "STOP 1b", "pad B01 (nota 52) → STOP 1 (\"%s\")", w.L(1).c_str());
    w.pads.midi({{0x90, 52, 100}}); w.run(2.1);
    CHECK(w.L(1) == "PLAY 1b", "otro golpe en B01 lo vuelve a arrancar");

    // Granular: fuente LOOPER, slot 1
    w.gran.set(w.gran.find("Source"), 1.f);
    w.gran.set(w.gran.find("Slot"), 0.f);
    w.run(0.05, 0.f);
    CHECK(w.gran.text(w.gran.find("Slot")) == "L1 PLAY", "Granular ve el slot 1 del Looper (\"%s\")", w.gran.text(w.gran.find("Slot")).c_str());
    w.grms();
    w.gran.midi({{0x90, 60, 110}}); w.run(1.0, 0.f);
    double r1 = w.grms();
    CHECK(r1 > 0.01, "Granular suena granulando el loop vivo (rms %.3f)", r1);
    w.gran.midi({{0x80, 60, 0}}); w.run(4.0, 0.f); w.grms(); w.run(0.3, 0.f);
    CHECK(w.grms() < 1e-4, "y se calla al soltar la nota");
    // Slot vacío → silencio
    w.gran.set(w.gran.find("Slot"), 5.f / 15.f);
    w.run(0.01, 0.f); w.grms();
    w.gran.midi({{0x90, 60, 110}}); w.run(0.5, 0.f);
    CHECK(w.grms() < 1e-6, "con un slot vacío no suena (\"%s\")", w.gran.text(w.gran.find("Slot")).c_str());
    w.gran.midi({{0xB0, 123, 0}}); w.run(4.0, 0.f);

    // Granular con WAV de fábrica, pads con aftertouch y pitch bend
    w.gran.set(w.gran.find("Source"), 0.f);
    w.gran.set(w.gran.find("File"), 0.f);
    w.wait(200);
    CHECK(w.gran.text(w.gran.find("File")) == "01 Glass Pad", "File muestra el WAV de fábrica (\"%s\")", w.gran.text(w.gran.find("File")).c_str());
    w.grms();
    w.gran.midi({{0x90, 48, 100}, {0x90, 55, 100}, {0xA0, 48, 127}, {0xE0, 0, 96}}); w.run(1.0, 0.f);
    CHECK(w.grms() > 0.01, "acorde con aftertouch polifónico y pitch bend: suena");
    w.gran.midi({{0xB0, 64, 127}, {0x80, 48, 0}, {0x80, 55, 0}}); w.run(4.0, 0.f); w.grms(); w.run(0.3, 0.f);
    CHECK(w.grms() > 0.005, "con el sustain pisado sigue sonando tras soltar");
    w.gran.midi({{0xB0, 64, 0}}); w.run(4.0, 0.f); w.grms(); w.run(0.3, 0.f);
    CHECK(w.grms() < 1e-4, "al soltar el sustain se apaga");

    // CC de pedalera: CC 103 = LOOP 2 (graba), CC 118 = STOP ALL
    w.pads.midi({{0xB0, 103, 127}}); w.run(0.01);
    CHECK(w.L(2) == "WAIT", "CC103 → LOOP 2 espera al compás (\"%s\")", w.L(2).c_str());
    w.pads.midi({{0xB0, 103, 0}}); w.run(2.1);
    w.pads.midi({{0xB0, 103, 127}}); w.run(2.1);
    CHECK(w.L(2) == "PLAY 1b", "segundo CC103 cierra LOOP 2 (\"%s\")", w.L(2).c_str());
    w.pads.midi({{0xB0, 118, 127}}); w.run(2.1);
    CHECK(w.L(1) == "STOP 1b" && w.L(2) == "STOP 1b", "CC118 → STOP ALL (\"%s\" \"%s\")", w.L(1).c_str(), w.L(2).c_str());

    // Banco C: CLEAR 2 con dos golpes
    w.pads.midi({{0x90, 36 + 33, 100}}); w.run(0.01);
    w.pads.midi({{0x90, 36 + 33, 100}}); w.run(0.01);
    CHECK(w.L(2) == "EMPTY", "dos golpes en C02 borran LOOP 2");

    // Botones de la pantalla de Pads
    w.pads.press(w.pads.find("LOOP 1")); w.run(2.1);
    CHECK(w.L(1) == "PLAY 1b", "el botón LOOP 1 de la pantalla de Pads arranca el loop (\"%s\")", w.L(1).c_str());

    // LEARN: el próximo pad pasa a ser LOOP 1
    w.pads.press(w.pads.find("Learn")); w.run(0.01);
    CHECK(w.pads.text(w.pads.find("Learn")) == "HIT PAD", "LEARN espera un pad");
    w.pads.midi({{0x90, 37, 100}}); w.run(0.01);
    w.wait(100);
    CHECK(w.pads.text(w.pads.find("Base Note")) == "C#1", "LEARN: Base Note = C#1 (\"%s\")", w.pads.text(w.pads.find("Base Note")).c_str());

    // Canal: solo CH 10
    w.pads.set(w.pads.find("Channel"), 10.f / 16.f);
    w.pads.midi({{0x90, 37 + 2, 100}}); w.run(0.01);
    CHECK(w.L(3) == "EMPTY", "con Channel = CH 10 se ignora el canal 1");
    w.pads.midi({{0x99, 37 + 2, 100}}); w.run(0.01);
    CHECK(w.L(3) == "WAIT", "y responde al canal 10 (\"%s\")", w.L(3).c_str());

    // Segundo Looper: BUSY; al borrar el primero hereda el enlace
    {
        th::Plug* second = new th::Plug(kLooper);
        second->prepare(44100, 128);
        w.extra.push_back(second);
        w.run(0.05);
        CHECK(second->text(second->find("Link Pads")) == "BUSY", "un segundo Looper queda BUSY (\"%s\")", second->text(second->find("Link Pads")).c_str());
        w.looper.close();
        // el primer Looper ya no procesa: sustituimos su proceso por el del segundo
        std::vector<float> L(128), R(128), a(128), b(128);
        for (int k = 0; k < 40; ++k) { w.pads.process(a.data(), b.data(), 128); second->process(L.data(), R.data(), 128); }
        CHECK(second->text(second->find("Link Pads")) == "LINKED", "al borrar el primero, el segundo toma el enlace");
        w.pads.midi({{0x99, 37, 100}});
        for (int k = 0; k < 2; ++k) { w.pads.process(a.data(), b.data(), 128); second->process(L.data(), R.data(), 128); }
        CHECK(second->text(second->find("LOOP 1")).rfind("REC", 0) == 0, "y Pads ya dispara el segundo Looper");
        w.extra.clear();
        delete second;
    }
    std::printf("\n%s (%d fallos)\n", fails ? "LINK: FALLOS" : "LINK: TODO OK", fails);
    return fails ? 1 : 0;
}
