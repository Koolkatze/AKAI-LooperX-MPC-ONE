// lx_granular.h - motor granular polifónico de LooperX (sin memoria dinámica en el audio).
//
// SPDX-License-Identifier: MIT
//
// Viene del sinte granular del proyecto LooperX original, adaptado al MPC:
//   * la fuente puede ser un WAV en memoria o un slot vivo del LooperX Looper (vía lx_link.h),
//   * se toca por MIDI (pads del MPC, teclado o controlador externo): nota, velocidad,
//     aftertouch polifónico/de canal, pitch bend, rueda de modulación y pedal de sustain,
//   * modo "POSITION": cada nota elige una zona del sonido (16 pads = 16 zonas) en vez del tono.
#ifndef LX_GRANULAR_H
#define LX_GRANULAR_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "lx_link.h"

namespace lx {
namespace gr {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.f * kPi;
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float dbToGain(float db) { return db <= -96.f ? 0.f : std::pow(10.f, db * 0.05f); }
inline void panGains(float pan, float& gl, float& gr)
{
    float a = (clampf(pan, -1.f, 1.f) + 1.f) * 0.25f * kPi;
    gl = std::min(1.f, std::cos(a) * 1.41421356f);
    gr = std::min(1.f, std::sin(a) * 1.41421356f);
}
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float uni() { return (float)(next() >> 8) * (1.f / 16777216.f); }
    inline float bi() { return uni() * 2.f - 1.f; }
};

}  // namespace gr

// De dónde leen los granos. Se rellena una vez por bloque de audio.
struct GrainSrc
{
    enum Type { kNone, kBuffer, kLink };
    int type = kNone;
    int64_t len = 0;                       // frames
    const float* buf = nullptr;            // kBuffer: estéreo intercalado
    const LinkSlot* slot = nullptr;        // kLink: slot del Looper
    const PoolReadGuard* guard = nullptr;

    inline void frame(int64_t i, float& l, float& r) const
    {
        if (type == kBuffer) { const float* p = buf + i * 2; l = p[0]; r = p[1]; return; }
        const uint32_t c = slot->chunk[i >> kChunkShift].load(std::memory_order_relaxed);
        const float* base = guard->chunkPtr(c);
        if (!base) { l = r = 0.f; return; }
        const float* p = base + (size_t)(i & kChunkMask) * 2;
        l = p[0]; r = p[1];
    }
    bool usable() const { return type != kNone && len > 2048; }
};

struct GrainParams
{
    float position = 0.3f, scan = 0, sprayMs = 40, sizeMs = 90, density = 24;
    float pitch = 0, fine = 0, pitchRand = 0, spread = 0.6f, reverse = 0;
    int window = 0;
    float attackMs = 30, decayMs = 300, sustain = 0.8f, releaseMs = 800;
    float cutoff = 12000, reso = 0.15f, gainDb = -4;
    bool freeze = false;
    float lfoRate = 0.2f, lfoPos = 0;
    int voices = 8, rootKey = 60, bendRange = 2;
    int atTarget = 1;     // 0 OFF, 1 DENSITY, 2 SIZE, 3 CUTOFF
    int modTarget = 1;    // 0 OFF, 1 POSITION, 2 SPRAY, 3 SIZE, 4 CUTOFF, 5 LFO>POS
    int keyMode = 0;      // 0 PITCH, 1 POSITION
    float velAmount = 1.f;
};

class GranularEngine
{
public:
    static constexpr int kMaxVoices = 16;
    static constexpr int kMaxGrains = 128;
    static constexpr int kMaxBlock = 512;

    void prepare(double sr) { sr_ = sr; reset(); }
    void reset()
    {
        for (auto& v : voices_) v = Voice();
        for (auto& g : grains_) g = Grain();
        fz_[0][0] = fz_[0][1] = fz_[1][0] = fz_[1][1] = 0;
        sustain_ = false; bend_ = 0; mod_ = 0; grainCount_ = 0;
    }

    // ---- MIDI (hilo de audio) ----
    void noteOn(int key, float vel, const GrainParams& p)
    {
        int maxV = std::clamp(p.voices, 1, kMaxVoices);
        int slot = -1;
        for (int i = 0; i < maxV; ++i) if (voices_[i].active && voices_[i].key == key) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < maxV; ++i) if (!voices_[i].active) { slot = i; break; }
        if (slot < 0) { uint32_t oldest = ~0u; for (int i = 0; i < maxV; ++i) if (voices_[i].age < oldest) { oldest = voices_[i].age; slot = i; } }
        Voice& v = voices_[slot];
        v.active = true; v.key = key; v.held = false; v.press = 0;
        float vv = gr::clampf(vel, 0.f, 1.f);
        v.vel = 1.f - p.velAmount + p.velAmount * std::max(0.05f, vv);
        if (p.keyMode == 1) {   // POSITION: la nota elige zona, el tono es el de Pitch
            v.ratio = 1.f;
            int z = ((key - p.rootKey) % 16 + 16) % 16;
            v.posOffset = (float)z / 16.f;
        } else {
            v.ratio = std::pow(2.f, (float)(key - p.rootKey) / 12.f);
            v.posOffset = 0;
        }
        v.timer = 0; v.age = ++clock_;
        v.env.stage = 1;
    }
    void noteOff(int key)
    {
        for (auto& v : voices_)
            if (v.active && v.key == key && v.env.stage != 4) { if (sustain_) v.held = true; else v.env.stage = 4; }
    }
    void setSustain(bool on)
    {
        sustain_ = on;
        if (!on) for (auto& v : voices_) if (v.active && v.held) { v.held = false; v.env.stage = 4; }
    }
    void polyPressure(int key, float a) { for (auto& v : voices_) if (v.active && v.key == key) v.press = gr::clampf(a, 0, 1); }
    void channelPressure(float a) { for (auto& v : voices_) if (v.active) v.press = gr::clampf(a, 0, 1); }
    void setBend(float norm) { bend_ = gr::clampf(norm, -1.f, 1.f); }   // -1..1
    void setMod(float m) { mod_ = gr::clampf(m, 0.f, 1.f); }
    void allNotesOff() { sustain_ = false; for (auto& v : voices_) if (v.active) { v.held = false; v.env.stage = 4; } }
    void allSoundOff() { for (auto& v : voices_) v.active = false; for (auto& g : grains_) g.active = false; }

    int activeVoices() const { int c = 0; for (auto& v : voices_) c += v.active ? 1 : 0; return c; }
    int activeGrains() const { return grainCount_; }
    float playhead() const { return playhead_; }

    // Escribe (sobrescribe) n <= kMaxBlock frames estéreo.
    void render(float* outL, float* outR, int n, const GrainParams& p, const GrainSrc& src)
    {
        std::memset(outL, 0, sizeof(float) * (size_t)n);
        std::memset(outR, 0, sizeof(float) * (size_t)n);
        const bool hasSrc = src.usable();
        const float sr = (float)sr_;

        if (!p.freeze && hasSrc) { scan_ += p.scan * (float)n / (float)src.len; scan_ -= std::floor(scan_); }
        lfoPh_ += p.lfoRate * (float)n / sr; lfoPh_ -= std::floor(lfoPh_);
        float lfoAmt = p.lfoPos;
        float center = p.position + scan_;
        if (p.modTarget == 1) center += mod_;
        if (p.modTarget == 5) lfoAmt = std::max(lfoAmt, mod_);
        center += std::sin(gr::kTwoPi * lfoPh_) * lfoAmt * 0.5f;
        center -= std::floor(center);
        playhead_ = center;

        for (int vi = 0; vi < kMaxVoices; ++vi) {
            Voice& v = voices_[vi];
            if (!v.active) continue;
            advanceEnv(v.env, envBuf_[vi], n, p);
            if (v.env.stage == 0) { v.active = false; continue; }
            if (!hasSrc) continue;
            float dens = p.density;
            if (p.atTarget == 1) dens *= 1.f + v.press * 4.f;
            const float interval = sr / std::max(0.5f, dens);
            for (int i = 0; i < n; ++i) {
                v.timer -= 1.f;
                if (v.timer <= 0.f) {
                    float c = center + v.posOffset; c -= std::floor(c);
                    spawn(vi, i, c, p, src);
                    v.timer += interval * (0.75f + 0.5f * rng_.uni());
                }
            }
        }

        int count = 0;
        if (hasSrc) {
            const double len = (double)src.len;
            const int64_t ilen = src.len;
            for (auto& g : grains_) {
                if (!g.active) continue;
                const Voice& v = voices_[g.voice];
                if (!v.active) { g.active = false; continue; }
                const float* env = envBuf_[g.voice];
                const float invLen = 1.f / (float)g.len;
                int i = g.start;
                g.start = 0;
                for (; i < n && g.age < g.len; ++i, ++g.age) {
                    double pos = g.pos;
                    if (pos < 0) pos += len; else if (pos >= len) pos -= len;
                    if (pos < 0 || pos >= len) pos = 0;
                    g.pos = pos + g.inc;
                    int64_t i0 = (int64_t)pos;
                    float fr = (float)(pos - (double)i0);
                    int64_t i1 = i0 + 1 >= ilen ? 0 : i0 + 1;
                    float al, ar, bl, br;
                    src.frame(i0, al, ar);
                    src.frame(i1, bl, br);
                    float w = window(p.window, (float)g.age * invLen) * g.amp * env[i];
                    outL[i] += (al + (bl - al) * fr) * w * g.gl;
                    outR[i] += (ar + (br - ar) * fr) * w * g.gr;
                }
                if (g.age >= g.len) g.active = false; else ++count;
            }
        } else {
            for (auto& g : grains_) g.active = false;
        }
        grainCount_ = count;

        // Filtro SVF paso bajo + ganancia
        float fc = p.cutoff;
        if (p.atTarget == 3) { float mp = 0; for (auto& v : voices_) if (v.active) mp = std::max(mp, v.press); fc *= 1.f + mp * 3.f; }
        if (p.modTarget == 4) fc *= 1.f + mod_ * 7.f;
        fc = gr::clampf(fc, 20.f, sr * 0.45f);
        const float g = std::tan(gr::kPi * fc / sr), k = 2.f - 2.f * gr::clampf(p.reso, 0.f, 0.97f);
        const float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const float gain = gr::dbToGain(p.gainDb);
        float* ch[2] = {outL, outR};
        for (int c = 0; c < 2; ++c) {
            float ic1 = fz_[c][0], ic2 = fz_[c][1];
            for (int i = 0; i < n; ++i) {
                float x = ch[c][i];
                float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
                ch[c][i] = v2 * gain;
            }
            if (!std::isfinite(ic1) || std::fabs(ic1) < 1e-20f) ic1 = 0;
            if (!std::isfinite(ic2) || std::fabs(ic2) < 1e-20f) ic2 = 0;
            fz_[c][0] = ic1; fz_[c][1] = ic2;
        }
    }

private:
    struct Env { int stage = 0; float level = 0; };
    struct Voice {
        bool active = false, held = false;
        int key = -1;
        float vel = 0, press = 0, ratio = 1, timer = 0, posOffset = 0;
        Env env;
        uint32_t age = 0;
    };
    struct Grain {
        bool active = false;
        int voice = 0, start = 0, len = 0, age = 0;
        double pos = 0, inc = 1;
        float gl = 1, gr = 1, amp = 1;
    };

    static float window(int type, float x)
    {
        switch (type) {
            case 1: return 1.f - std::fabs(2.f * x - 1.f);
            case 2:
                if (x < 0.125f) return 0.5f - 0.5f * std::cos(gr::kPi * x / 0.125f);
                if (x > 0.875f) return 0.5f - 0.5f * std::cos(gr::kPi * (1.f - x) / 0.125f);
                return 1.f;
            case 3: return std::min(1.f, x * 50.f) * std::exp(-5.f * x);
            case 4: return std::exp(-5.f * (1.f - x)) * std::min(1.f, (1.f - x) * 50.f);
            default: return 0.5f - 0.5f * std::cos(gr::kTwoPi * x);
        }
    }

    void advanceEnv(Env& e, float* out, int n, const GrainParams& p)
    {
        const float sr = (float)sr_;
        const float aInc = 1.f / std::max(1.f, p.attackMs * 0.001f * sr);
        const float dCoef = std::exp(-1.f / std::max(1.f, p.decayMs * 0.001f * sr / 4.f));
        const float rCoef = std::exp(-1.f / std::max(1.f, p.releaseMs * 0.001f * sr / 4.f));
        const float sus = p.sustain;
        for (int i = 0; i < n; ++i) {
            switch (e.stage) {
                case 1: e.level += aInc; if (e.level >= 1.f) { e.level = 1.f; e.stage = 2; } break;
                case 2: e.level = sus + (e.level - sus) * dCoef; if (std::fabs(e.level - sus) < 1e-3f) e.stage = 3; break;
                case 3: e.level = sus; break;
                case 4: e.level *= rCoef; if (e.level < 1e-4f) { e.level = 0; e.stage = 0; } break;
                default: e.level = 0;
            }
            out[i] = e.level;
        }
    }

    void spawn(int vi, int offset, float center, const GrainParams& p, const GrainSrc& src)
    {
        int slot = -1;
        for (int i = 0; i < kMaxGrains; ++i) if (!grains_[i].active) { slot = i; break; }
        if (slot < 0) return;
        Voice& v = voices_[vi];
        Grain& g = grains_[slot];
        const double len = (double)src.len;
        const float sr = (float)sr_;
        float sizeMs = p.sizeMs;
        if (p.atTarget == 2) sizeMs *= 1.f + v.press * 3.f;
        if (p.modTarget == 3) sizeMs *= 1.f + mod_ * 3.f;
        float spray = p.sprayMs;
        if (p.modTarget == 2) spray *= 1.f + mod_ * 9.f;
        double pos = (double)center * len + (double)(rng_.bi() * spray * 0.001f * sr);
        pos = std::fmod(pos, len); if (pos < 0) pos += len;
        float semis = p.pitch + p.fine * 0.01f + rng_.bi() * p.pitchRand + bend_ * (float)p.bendRange;
        double inc = (double)(v.ratio * std::pow(2.f, semis / 12.f));
        if (rng_.uni() < p.reverse) inc = -inc;
        g.active = true; g.voice = vi; g.start = offset; g.age = 0;
        g.len = std::max(16, (int)(sizeMs * 0.001f * sr));
        g.pos = pos; g.inc = inc;
        gr::panGains(rng_.bi() * p.spread, g.gl, g.gr);
        const float overlap = std::max(1.f, p.density * sizeMs * 0.001f);
        g.amp = v.vel / std::sqrt(overlap);
    }

    double sr_ = 44100.0;
    Voice voices_[kMaxVoices];
    Grain grains_[kMaxGrains];
    float envBuf_[kMaxVoices][kMaxBlock];
    float scan_ = 0, lfoPh_ = 0, playhead_ = 0, bend_ = 0, mod_ = 0;
    bool sustain_ = false;
    int grainCount_ = 0;
    float fz_[2][2] = {};
    gr::Rng rng_{0xC0FFEEu};
    uint32_t clock_ = 0;
};

}  // namespace lx

#endif
