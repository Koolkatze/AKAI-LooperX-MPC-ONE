// Prueba de los 19 plugins LooperX FX cargados como en el MPC: suenan, no rompen la señal, el
// parámetro 0 (Level) hace lo que dice, y el Delay sincronizado cae en la corchea del tempo.
#include <dirent.h>
#include <algorithm>
#include "vsthost.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("  (l.%d)\n", __LINE__); ++fails; } \
  else { std::printf("ok   " __VA_ARGS__); std::printf("\n"); } } while (0)

static double render(th::Plug& p, float levelDb, std::vector<float>* keep = nullptr, int burstOnly = 0) {
    p.prepare(44100, 128);
    p.set(0, levelDb);   // set() recibe valor normalizado: lo convertimos abajo
    std::vector<float> L(128), R(128);
    double acc = 0; int64_t n = 0; float ph = 0;
    for (int b = 0; b < 344; ++b) {   // 1 s
        for (int i = 0; i < 128; ++i) {
            int64_t t = (int64_t)b * 128 + i;
            ph += 440.f / 44100.f; if (ph > 1) ph -= 1;
            float x = 0.3f * std::sin(6.2831853f * ph);
            if (burstOnly && t >= burstOnly) x = 0;
            L[i] = x; R[i] = 0.8f * x;
        }
        p.process(L.data(), R.data(), 128);
        for (int i = 0; i < 128; ++i) { acc += (double)L[i] * L[i] + (double)R[i] * R[i]; ++n; if (keep) keep->push_back(L[i]); }
        th::g_tr.samplePos += 128;
    }
    return std::sqrt(acc / (double)n);
}

int main() {
    std::vector<std::string> libs;
    DIR* d = opendir("plugins");
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.rfind("LooperXFx", 0) != 0) continue;
        std::string nat = "plugins/" + n + "/build/native/";
        DIR* dd = opendir(nat.c_str());
        if (!dd) continue;
        while (dirent* f = readdir(dd)) { std::string s = f->d_name; if (s.size() > 3 && s.substr(s.size() - 3) == ".so") libs.push_back(nat + s); }
        closedir(dd);
    }
    closedir(d);
    std::sort(libs.begin(), libs.end());
    CHECK(libs.size() == 19, "hay 19 plugins de efecto compilados (%zu)", libs.size());
    for (const auto& lib : libs) {
        th::Plug a(lib), b(lib);
        if (!a.ok() || !b.ok()) { CHECK(false, "%s carga", lib.c_str()); continue; }
        char nm[64] = {}; a.disp(effGetEffectName, 0, 0, nm);
        // Level: normalizado 0..1 sobre -24..+12 dB → 0 dB = 24/36, -12 dB = 12/36
        double r0 = render(a, 24.f / 36.f), r12 = render(b, 12.f / 36.f);
        double diff = 20.0 * std::log10(r12 / std::max(1e-12, r0));
        bool sane = std::isfinite(r0) && r0 > 1e-4 && r0 < 2.0;
        CHECK(sane && std::fabs(diff + 12.0) < 0.6, "%-13s suena (rms %.3f) y Level -12 dB baja %.1f dB", nm, r0, diff);
        CHECK(a.text(0) == "+0.0 dB" && a.name(0) == "Level", "%-13s parámetro 0 = Level (\"%s\")", nm, a.text(0).c_str());
    }
    // Delay sincronizado: 1/8 a 120 BPM = 250 ms = 11025 muestras
    {
        th::g_tr.bpm = 120; th::g_tr.playing = true;
        th::Plug p("plugins/LooperXFxDelay/build/native/delay.so");
        p.prepare(44100, 128);
        p.set(p.find("Mix"), 1.f);              // solo eco
        p.set(p.find("Feedback"), 0.f);
        std::vector<float> out;
        // impulso
        std::vector<float> L(128), R(128);
        for (int b = 0; b < 120; ++b) {
            std::fill(L.begin(), L.end(), 0.f); std::fill(R.begin(), R.end(), 0.f);
            if (b == 0) { L[0] = 1.f; R[0] = 1.f; }
            p.process(L.data(), R.data(), 128);
            out.insert(out.end(), L.begin(), L.end());
        }
        int at = (int)(std::max_element(out.begin(), out.end(), [](float x, float y) { return std::fabs(x) < std::fabs(y); }) - out.begin());
        CHECK(std::abs(at - 11025) < 64, "Delay sync 1/8 a 120 BPM: eco a %d muestras (esperado 11025)", at);
        th::g_tr.playing = false;
    }
    std::printf("\n%s (%d fallos)\n", fails ? "FX: FALLOS" : "FX: TODO OK", fails);
    return fails ? 1 : 0;
}
