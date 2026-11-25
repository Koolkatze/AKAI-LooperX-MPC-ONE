// Prueba del núcleo LooperCore (sin el framework del SDK).
#include <cmath>
#include <cstdio>
#include <vector>
#include "../_shared/lx_looper.h"

using namespace lx;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("  (l.%d)\n", __LINE__); ++fails; } \
  else { std::printf("ok   " __VA_ARGS__); std::printf("\n"); } } while (0)

struct Rig {
    ChunkPool pool;
    LinkBlock* link = newPrivateLink();
    LooperCore core;
    LooperSettings set;
    HostTime ht;
    double sr = 44100;
    int block = 128;
    int64_t t = 0;          // frames procesados
    double ppq = 0;         // posición del MPC (si playing)
    std::vector<float> outL, outR;
    float phaseOsc = 0;
    Rig(size_t mb = 64) {
        pool.create(mb << 20);
        core.attach(link, &pool);
        core.useSettings(&set);
        core.prepare(sr);
        ht.valid = true; ht.tempoValid = true; ht.ppqValid = true; ht.bpm = 120; ht.num = 4; ht.den = 4;
    }
    ~Rig() { freePrivateLink(link); }
    void run(double secs, float amp = 0.3f) {
        int64_t frames = (int64_t)(secs * sr);
        std::vector<float> L(block), R(block);
        for (int64_t d = 0; d < frames; d += block) {
            for (int i = 0; i < block; ++i) { phaseOsc += 220.f / (float)sr; if (phaseOsc > 1) phaseOsc -= 1; L[i] = R[i] = amp * std::sin(6.2831853f * phaseOsc); }
            if (ht.playing) { ht.ppq = ppq; ht.barStartPpq = std::floor(ppq / 4.0) * 4.0; }
            core.process(L.data(), R.data(), L.data(), R.data(), block, ht, set);
            outL.insert(outL.end(), L.begin(), L.end());
            if (ht.playing) ppq += block * ht.bpm / 60.0 / sr;
            t += block;
        }
    }
    void runUntil(double sec) { run(sec - (double)t / sr); }
};

int main() {
    const int BAR = 88200;  // 120 BPM 4/4 a 44,1 kHz
    // 1) Primer loop con el MPC parado: empieza ya; cierre tardío (40 ms) se pega al compás
    {
        Rig r;
        r.core.press(0);
        CHECK(r.core.state(0) == kRecording, "primer REC con el MPC parado empieza al instante");
        r.run(2.04);
        r.core.press(0);
        r.run(0.05);
        CHECK(r.core.state(0) == kPlaying, "A1 suena tras cerrar (%s)", slotStateName(r.core.state(0)));
        CHECK(r.core.lengthFrames(0) == BAR, "A1 = 1 compás exacto pese a pulsar 40 ms tarde (%d)", r.core.lengthFrames(0));
        // 2) Segundo loop cuantizado: pulsa a 2,5 s -> empieza a 4,0 s; cierra a 7,9 -> 2 compases
        r.runUntil(2.5);
        r.core.press(1);
        CHECK(r.core.state(1) == kArmed, "A2 espera al siguiente compás (%s)", slotStateName(r.core.state(1)));
        r.runUntil(4.05);
        CHECK(r.core.state(1) == kRecording, "A2 graba desde el compás 3");
        r.runUntil(7.9);
        r.core.press(1);
        r.runUntil(8.1);
        CHECK(r.core.lengthFrames(1) == 2 * BAR, "A2 = 2 compases (%d)", r.core.lengthFrames(1));
        // 3) Overdub + undo/redo
        auto energy = [&](int s) { double e = 0; int n = r.core.lengthFrames(s); for (int i = 0; i < n; i += 7) { const float* f = r.core.frame(s, i); e += (double)f[0] * f[0]; } return e; };
        double e0 = energy(0);
        r.core.press(0); r.run(0.01);
        CHECK(r.core.state(0) == kOverdub, "A1 en overdub");
        r.run(2.0);
        r.core.press(0); r.run(0.01);
        double e1 = energy(0);
        CHECK(e1 > e0 * 1.2, "overdub añade audio (%.1f -> %.1f)", e0, e1);
        r.core.undoSlot(0);
        CHECK(std::fabs(energy(0) - e0) < 1e-6 * e0, "UNDO deja el loop como antes del overdub");
        r.core.undoSlot(0);
        CHECK(std::fabs(energy(0) - e1) < 1e-6 * e1, "REDO lo recupera");
        // 4) STOP cuantizado y vuelta a PLAY
        r.core.stopSlot(1); r.run(0.05);
        CHECK(r.core.pending(1) == LooperCore::kStopPlay, "STOP de A2 espera al compás");
        r.run(2.1);
        CHECK(r.core.state(1) == kStopped, "A2 parado en la raya del compás");
        r.core.press(1); r.run(2.1);
        CHECK(r.core.state(1) == kPlaying, "A2 vuelve a sonar en el siguiente compás");
        // 5) Borrar con confirmación
        r.core.requestClear(1);
        CHECK(r.core.state(1) == kPlaying && r.core.clearArmed(1), "1ª pulsación de CLEAR solo arma");
        r.core.requestClear(1);
        CHECK(r.core.state(1) == kEmpty, "2ª pulsación borra");
    }
    // 6) Largo fijo 4/16 (16 tiempos = 4 compases) y 4/24 (6 compases)
    {
        Rig r;
        r.set.lengthBeats = 16;
        r.core.press(3);
        r.run(8.05);
        CHECK(r.core.state(3) == kPlaying && r.core.lengthFrames(3) == 4 * BAR, "LENGTH 4/16 cierra solo a 4 compases (%d)", r.core.lengthFrames(3));
        r.set.lengthBeats = 24;
        r.core.press(4);
        r.run(14.5);
        CHECK(r.core.lengthFrames(4) == 6 * BAR, "LENGTH 4/24 cierra solo a 6 compases (%d)", r.core.lengthFrames(4));
        r.core.press(5); r.run(2.1); r.core.press(5); r.run(4.1);
        CHECK(r.core.lengthFrames(5) > 0 && r.core.lengthFrames(5) % BAR == 0 && r.core.lengthFrames(5) < 6 * BAR,
              "con largo fijo, pulsar antes cierra antes y en compás (%d)", r.core.lengthFrames(5));
    }
    // 7) Sincronía con el MPC: PLAY reinicia todo desde arriba; STOP del MPC para los loops
    {
        Rig r;
        r.core.press(0); r.run(2.0); r.core.press(0); r.run(0.7);   // loop de 1 compás, fase ~0,35
        CHECK(r.core.state(0) == kPlaying, "loop grabado con el MPC parado");
        r.ht.playing = true; r.ppq = 0;
        r.run(0.003);
        float ph = r.link->slot[0].phase.load();
        CHECK(ph < 0.01f, "al dar PLAY en el MPC el loop arranca desde el principio (fase %.3f)", ph);
        r.run(1.0);
        r.ht.playing = false;
        r.run(0.1);
        CHECK(r.core.state(0) == kStopped, "al parar el MPC el loop se para");
        r.ht.playing = true; r.ppq = 0;
        r.run(0.05);
        CHECK(r.core.state(0) == kPlaying, "al volver a PLAY el loop vuelve");
        // 8) La secuencia del MPC da la vuelta (ppq salta atrás): la fase del loop sigue continua
        r.run(1.0);
        float before = r.link->slot[0].phase.load();
        r.ppq = 0.0;   // salto a compás 1
        r.run(0.003);
        float after = r.link->slot[0].phase.load();
        float expected = before + (float)(0.003 * 44100 / BAR);
        CHECK(std::fabs(after - expected) < 0.01f, "salto de la secuencia del MPC: el loop no salta (%.3f -> %.3f)", before, after);
    }
    // 9) Con el MPC en marcha, el primer loop espera al compás del MPC; pulsación tardía recupera audio
    {
        Rig r;
        r.ht.playing = true; r.ppq = 0;
        r.run(2.05);                      // 50 ms después del compás 2
        r.core.press(0);
        CHECK(r.core.state(0) == kRecording, "pulsación 50 ms tarde: graba desde el compás (sin esperar)");
        CHECK(r.core.recFrames(0) >= (int)(0.045 * 44100), "y recupera del historial lo que faltaba (%d frames)", r.core.recFrames(0));
        r.run(1.95); r.core.press(0); r.run(0.2);
        CHECK(r.core.lengthFrames(0) == BAR, "el loop tardío mide justo 1 compás (%d)", r.core.lengthFrames(0));
    }
    // 10) Cuantización libre y cambio de tempo (varispeed sin cortes)
    {
        Rig r;
        r.set.quantize = 2;
        r.core.press(0); r.run(1.234); r.core.press(0); r.run(0.1);
        int len = r.core.lengthFrames(0);
        CHECK(std::abs(len - (int)(1.234 * 44100)) < 256, "modo libre: el loop mide lo que pulsaste (%d)", len);
        r.ht.bpm = 100;
        r.run(1.0);
        bool finite = true;
        for (float v : r.outL) if (!std::isfinite(v)) finite = false;
        CHECK(finite && r.core.state(0) == kPlaying, "cambio de tempo del MPC: sigue sonando sin NaN");
    }
    // 11) Memoria llena: cierre automático sin romper nada
    {
        Rig r;
        r.pool.create(5 * 131072);   // 5 bloques de 128 KiB ≈ 1,9 s de audio
        r.set.quantize = 2;
        r.core.press(0);
        r.run(5.0);
        CHECK(r.core.state(0) == kPlaying && r.core.memoryFullFlag(), "sin memoria: el loop se cierra solo y avisa");
    }
    // 12) STOP ALL y CLEAR ALL con confirmación
    {
        Rig r;
        r.core.press(0); r.run(2.0); r.core.press(0); r.run(0.1);
        r.core.press(1); r.run(2.0); r.core.press(1); r.run(2.1);
        r.core.stopAll(); r.run(2.1);
        CHECK(r.core.state(0) == kStopped && r.core.state(1) == kStopped, "STOP ALL para todo en el compás");
        r.core.requestClearAll();
        CHECK(r.core.state(0) == kStopped, "CLEAR ALL pide confirmación");
        r.core.requestClearAll();
        CHECK(r.core.allEmpty() && r.pool.freeChunks() == r.pool.numChunks(), "CLEAR ALL borra y devuelve toda la memoria");
    }
    std::printf("\n%s (%d fallos)\n", fails ? "LOOPER CORE: FALLOS" : "LOOPER CORE: TODO OK", fails);
    return fails ? 1 : 0;
}
