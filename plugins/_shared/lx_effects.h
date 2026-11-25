// lx_effects.h - los 19 efectos de LooperX para los plugins LooperX FX del MPC.
// GENERADO por plugins/_tools/extract_effects.py desde el proyecto LooperX original
// (src/core/effects.h/.cpp). SPDX-License-Identifier: MIT
//
// Reglas de tiempo real: los efectos solo reservan memoria en prepare() (onPrepare); process()
// no reserva, no bloquea y no toca ficheros.
#ifndef LX_EFFECTS_H
#define LX_EFFECTS_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lx {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.f * kPi;
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float dbToGain(float db) { return db <= -96.f ? 0.f : std::pow(10.f, db * 0.05f); }
inline float gainToDb(float g) { return g <= 1e-6f ? -120.f : 20.f * std::log10(g); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline void panGains(float pan, float& gl, float& gr) {
  float a = (clampf(pan, -1.f, 1.f) + 1.f) * 0.25f * kPi;
  gl = std::min(1.f, std::cos(a) * 1.41421356f);
  gr = std::min(1.f, std::sin(a) * 1.41421356f);
}
struct Rng {
  uint32_t s = 0x9E3779B9u;
  explicit Rng(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
  inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  inline float uni() { return (next() >> 8) * (1.f / 16777216.f); }
  inline float bi() { return uni() * 2.f - 1.f; }
};

inline const std::vector<std::string> kDivNames = {"1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8",
                                                   "1/8D", "1/4T",  "1/4",  "1/4D",  "1/2",  "1/1"};
inline double divisionBeats(int idx) {
  static const double b[] = {0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5,
                             0.75,  2.0 / 3.0, 1.0,  1.5,   2.0,       4.0};
  return b[std::clamp(idx, 0, 11)];
}

struct FxContext {
  double sr = 44100.0;
  double bpm = 120.0;
  bool running = false;
  int64_t pos = 0;  // posición del transporte (frames)
};

enum class ParamKind : uint8_t { Linear, Log, Choice, Toggle, Int };

struct ParamInfo {
  std::string name;
  float min = 0.f, max = 1.f, def = 0.f;
  ParamKind kind = ParamKind::Linear;
  std::vector<std::string> choices;  // para Choice
  std::string unit;
};

// Conjunto de parámetros editable desde la UI (encoder, Q-Links, táctil).
// Los valores se escriben desde el hilo de UI y se leen en el de audio; el flag
// dirty_ avisa al audio para recalcular coeficientes al inicio del siguiente bloque.
class ParamHost {
 public:
  static constexpr int kMaxParams = 32;
  virtual ~ParamHost() = default;
  virtual int numParams() const { return int(params_.size()); }
  const ParamInfo& info(int i) const { return params_[size_t(i)]; }
  float get(int i) const { return p_[i]; }
  void set(int i, float v);
  float getNormalized(int i) const;
  void setNormalized(int i, float n);
  void nudge(int i, int steps);  // un paso de encoder (64 pasos por recorrido)
  std::string valueText(int i) const;

 protected:
  void addParam(const char* n, float mn, float mx, float def, ParamKind k = ParamKind::Linear,
                const char* unit = "", std::vector<std::string> ch = {});
  std::vector<ParamInfo> params_;
  float p_[kMaxParams] = {};
  std::atomic<bool> dirty_{true};
};

class Effect : public ParamHost {
 public:
  virtual const char* name() const = 0;

  void prepare(double sr) { sr_ = sr; dirty_ = true; onPrepare(); }
  virtual void reset() {}
  // Procesa en el sitio (estéreo). Llamado solo desde el hilo de audio.
  void run(float* L, float* R, int n, const FxContext& c) {
    if (dirty_.exchange(false, std::memory_order_acquire)) update(c);
    if (!bypass) process(L, R, n, c);
  }

  std::atomic<bool> bypass{false};
  int typeIndex = 0;  // índice en FxRegistry

 protected:
  virtual void onPrepare() {}
  virtual void update(const FxContext&) {}
  virtual void process(float* L, float* R, int n, const FxContext& c) = 0;
  double sr_ = 44100.0;
};


inline void ParamHost::addParam(const char* n, float mn, float mx, float def, ParamKind k, const char* unit,
                      std::vector<std::string> ch) {
  ParamInfo pi;
  pi.name = n; pi.min = mn; pi.max = mx; pi.def = def; pi.kind = k; pi.unit = unit;
  pi.choices = std::move(ch);
  if (k == ParamKind::Choice) { pi.min = 0; pi.max = float(pi.choices.size() - 1); }
  if (k == ParamKind::Toggle) { pi.min = 0; pi.max = 1; }
  p_[params_.size()] = def;
  params_.push_back(std::move(pi));
}

inline void ParamHost::set(int i, float v) {
  if (i < 0 || i >= numParams()) return;
  const ParamInfo& pi = params_[size_t(i)];
  v = clampf(v, pi.min, pi.max);
  if (pi.kind == ParamKind::Choice || pi.kind == ParamKind::Toggle || pi.kind == ParamKind::Int)
    v = std::round(v);
  p_[i] = v;
  dirty_.store(true, std::memory_order_release);
}

inline float ParamHost::getNormalized(int i) const {
  const ParamInfo& pi = params_[size_t(i)];
  if (pi.max <= pi.min) return 0.f;
  if (pi.kind == ParamKind::Log && pi.min > 0)
    return std::log(p_[i] / pi.min) / std::log(pi.max / pi.min);
  return (p_[i] - pi.min) / (pi.max - pi.min);
}

inline void ParamHost::setNormalized(int i, float n) {
  const ParamInfo& pi = params_[size_t(i)];
  n = clampf(n, 0.f, 1.f);
  if (pi.kind == ParamKind::Log && pi.min > 0) set(i, pi.min * std::pow(pi.max / pi.min, n));
  else set(i, pi.min + n * (pi.max - pi.min));
}

inline void ParamHost::nudge(int i, int steps) {
  const ParamInfo& pi = params_[size_t(i)];
  if (pi.kind == ParamKind::Choice || pi.kind == ParamKind::Toggle || pi.kind == ParamKind::Int) {
    float v = p_[i] + float(steps);
    if (pi.kind == ParamKind::Toggle) v = steps != 0 ? 1.f - p_[i] : p_[i];
    set(i, v);
  } else {
    setNormalized(i, getNormalized(i) + float(steps) / 64.f);
  }
}

inline std::string ParamHost::valueText(int i) const {
  const ParamInfo& pi = params_[size_t(i)];
  char b[48];
  float v = p_[i];
  switch (pi.kind) {
    case ParamKind::Choice: return pi.choices[size_t(std::clamp(int(v), 0, int(pi.choices.size()) - 1))];
    case ParamKind::Toggle: return v > 0.5f ? "ON" : "OFF";
    case ParamKind::Int: std::snprintf(b, sizeof b, "%d%s", int(v), pi.unit.c_str()); return b;
    default:
      if (std::fabs(v) >= 1000.f) std::snprintf(b, sizeof b, "%.1fk%s", v / 1000.f, pi.unit.c_str());
      else if (std::fabs(v) >= 100.f) std::snprintf(b, sizeof b, "%.0f%s", v, pi.unit.c_str());
      else if (std::fabs(v) >= 10.f) std::snprintf(b, sizeof b, "%.1f%s", v, pi.unit.c_str());
      else std::snprintf(b, sizeof b, "%.2f%s", v, pi.unit.c_str());
      return b;
  }
}


namespace fxdsp {

struct OnePole {  // paso bajo de un polo
  float a = 0.f, z = 0.f;
  void setHz(float hz, double sr) { a = std::exp(-kTwoPi * hz / float(sr)); }
  inline float lp(float x) { z = x + a * (z - x); return z; }
  inline float hp(float x) { return x - lp(x); }
};

struct Svf {  // filtro de estado variable TPT (Zavalishin)
  float a1 = 0, a2 = 0, a3 = 0, k = 1, ic1 = 0, ic2 = 0;
  void set(float fc, float res, double sr) {
    fc = clampf(fc, 10.f, float(sr) * 0.49f);
    float g = std::tan(kPi * fc / float(sr));
    k = 2.f - 2.f * clampf(res, 0.f, 0.985f);
    a1 = 1.f / (1.f + g * (g + k)); a2 = g * a1; a3 = g * a2;
  }
  inline void tick(float x, float& lo, float& bp, float& hi) {
    float v3 = x - ic2;
    float v1 = a1 * ic1 + a2 * v3;
    float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
    lo = v2; bp = v1; hi = x - k * v1 - v2;
  }
  void reset() { ic1 = ic2 = 0; }
};

struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  inline float tick(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void norm(float a0) { b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0; }
  void lowShelf(float f, float db, double sr) {
    float A = std::pow(10.f, db / 40.f), w = kTwoPi * f / float(sr), c = std::cos(w), s = std::sin(w);
    float al = s / 2.f * std::sqrt(2.f), sa = 2.f * std::sqrt(A) * al;
    b0 = A * ((A + 1) - (A - 1) * c + sa); b1 = 2 * A * ((A - 1) - (A + 1) * c);
    b2 = A * ((A + 1) - (A - 1) * c - sa); float a0 = (A + 1) + (A - 1) * c + sa;
    a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sa; norm(a0);
  }
  void highShelf(float f, float db, double sr) {
    float A = std::pow(10.f, db / 40.f), w = kTwoPi * f / float(sr), c = std::cos(w), s = std::sin(w);
    float al = s / 2.f * std::sqrt(2.f), sa = 2.f * std::sqrt(A) * al;
    b0 = A * ((A + 1) + (A - 1) * c + sa); b1 = -2 * A * ((A - 1) + (A + 1) * c);
    b2 = A * ((A + 1) + (A - 1) * c - sa); float a0 = (A + 1) - (A - 1) * c + sa;
    a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sa; norm(a0);
  }
  void peak(float f, float db, float q, double sr) {
    float A = std::pow(10.f, db / 40.f), w = kTwoPi * f / float(sr), al = std::sin(w) / (2 * q), c = std::cos(w);
    b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; float a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A; norm(a0);
  }
};

// Línea de retardo con lectura interpolada.
struct DelayLine {
  std::vector<float> buf;
  int mask = 0, w = 0;
  void init(int minSize) {
    int n = 1; while (n < minSize) n <<= 1;
    buf.assign(size_t(n), 0.f); mask = n - 1; w = 0;
  }
  inline void push(float x) { buf[size_t(w)] = x; w = (w + 1) & mask; }
  inline float read(float d) const {  // d en frames (>=1)
    float rp = float(w) - d;
    while (rp < 0) rp += float(mask + 1);
    int i0 = int(rp); float fr = rp - float(i0);
    float a = buf[size_t(i0 & mask)], b = buf[size_t((i0 + 1) & mask)];
    return a + (b - a) * fr;
  }
  void clear() { std::fill(buf.begin(), buf.end(), 0.f); }
};

inline float lfoShape(float ph, int shape) {  // ph 0..1
  switch (shape) {
    case 1: return ph < 0.5f ? 1.f : -1.f;                       // cuadrada
    case 2: return 1.f - 4.f * std::fabs(ph - 0.5f);             // triangular
    default: return std::sin(kTwoPi * ph);                       // seno
  }
}

inline float syncPhase(const FxContext& c, double beats, float& freePh, float hz, int n) {
  if (c.running && beats > 0) {
    double spb = c.sr * 60.0 / c.bpm;
    double ph = std::fmod(double(c.pos) / (spb * beats), 1.0);
    return float(ph < 0 ? ph + 1 : ph);
  }
  float ph = freePh;
  freePh = std::fmod(freePh + hz * float(n) / float(c.sr), 1.f);
  return ph;
}

// ---------------------------------------------------------------------------
// 1. Filtro
// ---------------------------------------------------------------------------
class FilterFx : public Effect {
 public:
  FilterFx() {
    addParam("Type", 0, 3, 0, ParamKind::Choice, "", {"LOWPASS", "HIGHPASS", "BANDPASS", "NOTCH"});
    addParam("Cutoff", 20, 20000, 2000, ParamKind::Log, "Hz");
    addParam("Reso", 0, 1, 0.2f);
    addParam("Drive", 0, 1, 0.f);
    addParam("Mix", 0, 1, 1.f);
  }
  const char* name() const override { return "Filter"; }
  void reset() override { f_[0].reset(); f_[1].reset(); }
  void update(const FxContext&) override { target_ = p_[1]; }
  void process(float* L, float* R, int n, const FxContext&) override {
    cur_ = cur_ <= 0 ? target_ : cur_ + (target_ - cur_) * 0.3f;
    f_[0].set(cur_, p_[2], sr_);
    f_[1].set(cur_, p_[2], sr_);
    int type = int(p_[0]); float drive = 1.f + p_[3] * 8.f, mix = p_[4];
    float* ch[2] = {L, R};
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < n; ++i) {
        float x = ch[c][i], lo, bp, hi;
        float xin = p_[3] > 0 ? std::tanh(x * drive) / std::tanh(drive) : x;
        f_[c].tick(xin, lo, bp, hi);
        float y = type == 0 ? lo : type == 1 ? hi : type == 2 ? bp : lo + hi;
        ch[c][i] = x + (y - x) * mix;
      }
  }
 private:
  Svf f_[2];
  float target_ = 2000, cur_ = 0;
};

// ---------------------------------------------------------------------------
// 2. EQ de 3 bandas
// ---------------------------------------------------------------------------
class Eq3Fx : public Effect {
 public:
  Eq3Fx() {
    addParam("Low", -15, 15, 0, ParamKind::Linear, "dB");
    addParam("LowFreq", 40, 800, 200, ParamKind::Log, "Hz");
    addParam("Mid", -15, 15, 0, ParamKind::Linear, "dB");
    addParam("MidFreq", 200, 8000, 1000, ParamKind::Log, "Hz");
    addParam("High", -15, 15, 0, ParamKind::Linear, "dB");
    addParam("HighFreq", 1500, 16000, 5000, ParamKind::Log, "Hz");
  }
  const char* name() const override { return "EQ 3-Band"; }
  void update(const FxContext&) override {
    for (int c = 0; c < 2; ++c) {
      Biquad* b = bq_[c];
      Biquad nb[3] = {b[0], b[1], b[2]};
      nb[0].lowShelf(p_[1], p_[0], sr_); nb[1].peak(p_[3], p_[2], 0.8f, sr_); nb[2].highShelf(p_[5], p_[4], sr_);
      for (int k = 0; k < 3; ++k) { nb[k].z1 = b[k].z1; nb[k].z2 = b[k].z2; b[k] = nb[k]; }
    }
  }
  void process(float* L, float* R, int n, const FxContext&) override {
    float* ch[2] = {L, R};
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < n; ++i) ch[c][i] = bq_[c][2].tick(bq_[c][1].tick(bq_[c][0].tick(ch[c][i])));
  }
 private:
  Biquad bq_[2][3];
};

// ---------------------------------------------------------------------------
// 3. Compresor
// ---------------------------------------------------------------------------
class CompressorFx : public Effect {
 public:
  CompressorFx() {
    addParam("Thresh", -60, 0, -18, ParamKind::Linear, "dB");
    addParam("Ratio", 1, 20, 4, ParamKind::Log, ":1");
    addParam("Attack", 0.1f, 100, 10, ParamKind::Log, "ms");
    addParam("Release", 10, 1000, 120, ParamKind::Log, "ms");
    addParam("Knee", 0, 12, 6, ParamKind::Linear, "dB");
    addParam("Makeup", 0, 24, 0, ParamKind::Linear, "dB");
    addParam("Mix", 0, 1, 1);
  }
  const char* name() const override { return "Compressor"; }
  void update(const FxContext&) override {
    att_ = std::exp(-1.f / (p_[2] * 0.001f * float(sr_)));
    rel_ = std::exp(-1.f / (p_[3] * 0.001f * float(sr_)));
    makeup_ = dbToGain(p_[5]);
  }
  void process(float* L, float* R, int n, const FxContext&) override {
    float th = p_[0], ratio = p_[1], knee = p_[4], mix = p_[6];
    for (int i = 0; i < n; ++i) {
      float x = std::max(std::fabs(L[i]), std::fabs(R[i]));
      float xdb = gainToDb(x), over = xdb - th, gr;
      if (knee > 0 && 2 * std::fabs(over) <= knee) gr = (1.f / ratio - 1.f) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
      else gr = over > 0 ? over * (1.f / ratio - 1.f) : 0.f;
      env_ = gr < env_ ? att_ * env_ + (1 - att_) * gr : rel_ * env_ + (1 - rel_) * gr;
      float g = dbToGain(env_) * makeup_;
      L[i] = L[i] * (1 - mix + mix * g);
      R[i] = R[i] * (1 - mix + mix * g);
    }
  }
  float reduction() const { return env_; }
 private:
  float att_ = 0, rel_ = 0, env_ = 0, makeup_ = 1;
};

// ---------------------------------------------------------------------------
// 4. Limitador
// ---------------------------------------------------------------------------
class LimiterFx : public Effect {
 public:
  LimiterFx() {
    addParam("Ceiling", -12, 0, -0.3f, ParamKind::Linear, "dB");
    addParam("Release", 5, 500, 60, ParamKind::Log, "ms");
    addParam("Gain", 0, 18, 0, ParamKind::Linear, "dB");
  }
  const char* name() const override { return "Limiter"; }
  void update(const FxContext&) override {
    ceil_ = dbToGain(p_[0]); rel_ = std::exp(-1.f / (p_[1] * 0.001f * float(sr_))); in_ = dbToGain(p_[2]);
  }
  void process(float* L, float* R, int n, const FxContext&) override {
    for (int i = 0; i < n; ++i) {
      float l = L[i] * in_, r = R[i] * in_;
      float pk = std::max(std::fabs(l), std::fabs(r));
      float want = pk > ceil_ ? ceil_ / pk : 1.f;
      g_ = want < g_ ? want : rel_ * g_ + (1 - rel_) * want;
      L[i] = l * g_; R[i] = r * g_;
    }
  }
 private:
  float ceil_ = 1, rel_ = 0.99f, g_ = 1, in_ = 1;
};

// ---------------------------------------------------------------------------
// 5. Puerta de ruido
// ---------------------------------------------------------------------------
class GateFx : public Effect {
 public:
  GateFx() {
    addParam("Thresh", -80, 0, -45, ParamKind::Linear, "dB");
    addParam("Attack", 0.1f, 50, 1, ParamKind::Log, "ms");
    addParam("Hold", 0, 500, 40, ParamKind::Linear, "ms");
    addParam("Release", 5, 1000, 120, ParamKind::Log, "ms");
    addParam("Range", -80, 0, -80, ParamKind::Linear, "dB");
  }
  const char* name() const override { return "Gate"; }
  void update(const FxContext&) override {
    th_ = dbToGain(p_[0]); att_ = 1.f - std::exp(-1.f / (p_[1] * 0.001f * float(sr_)));
    rel_ = 1.f - std::exp(-1.f / (p_[3] * 0.001f * float(sr_)));
    hold_ = int(p_[2] * 0.001f * float(sr_)); floor_ = dbToGain(p_[4]);
  }
  void process(float* L, float* R, int n, const FxContext&) override {
    for (int i = 0; i < n; ++i) {
      float pk = std::max(std::fabs(L[i]), std::fabs(R[i]));
      if (pk > th_) holdCnt_ = hold_;
      float target = holdCnt_ > 0 ? 1.f : floor_;
      if (holdCnt_ > 0) --holdCnt_;
      g_ += (target - g_) * (target > g_ ? att_ : rel_);
      L[i] *= g_; R[i] *= g_;
    }
  }
 private:
  float th_ = 0, att_ = 0, rel_ = 0, floor_ = 0, g_ = 0;
  int hold_ = 0, holdCnt_ = 0;
};

// ---------------------------------------------------------------------------
// 6. Delay sincronizable con ping-pong
// ---------------------------------------------------------------------------
class DelayFx : public Effect {
 public:
  DelayFx() {
    addParam("Sync", 0, 1, 1, ParamKind::Toggle);
    addParam("Division", 0, 11, 5, ParamKind::Choice, "", kDivNames);
    addParam("Time", 1, 2000, 375, ParamKind::Log, "ms");
    addParam("Feedback", 0, 0.98f, 0.45f);
    addParam("PingPong", 0, 1, 0, ParamKind::Toggle);
    addParam("LowCut", 20, 2000, 120, ParamKind::Log, "Hz");
    addParam("HighCut", 500, 20000, 7000, ParamKind::Log, "Hz");
    addParam("Mix", 0, 1, 0.35f);
  }
  const char* name() const override { return "Delay"; }
  void onPrepare() override { for (auto& d : d_) d.init(int(sr_ * 4.2)); }
  void reset() override { for (auto& d : d_) d.clear(); }
  void update(const FxContext& c) override {
    for (int k = 0; k < 2; ++k) { lp_[k].setHz(p_[6], sr_); hp_[k].setHz(p_[5], sr_); }
    (void)c;
  }
  void process(float* L, float* R, int n, const FxContext& c) override {
    double secs = p_[0] > 0.5f ? divisionBeats(int(p_[1])) * 60.0 / c.bpm : p_[2] * 0.001;
    float target = float(std::min(secs * sr_, sr_ * 4.0));
    float fb = p_[3], mix = p_[7]; bool pp = p_[4] > 0.5f;
    for (int i = 0; i < n; ++i) {
      time_ = time_ <= 0 ? target : time_ + (target - time_) * 0.0005f;
      float dl = d_[0].read(time_), dr = d_[1].read(time_);
      float wl = lp_[0].lp(hp_[0].hp(dl)), wr = lp_[1].lp(hp_[1].hp(dr));
      float il = L[i], ir = R[i];
      if (pp) { d_[0].push((il + ir) * 0.5f + wr * fb); d_[1].push(wl * fb); }
      else { d_[0].push(il + wl * fb); d_[1].push(ir + wr * fb); }
      // Mezcla dry/wet que conserva el nivel seco hasta mix=0.5
      L[i] = il * (1.f - mix * 0.5f) + dl * mix;
      R[i] = ir * (1.f - mix * 0.5f) + dr * mix;
    }
  }
 private:
  DelayLine d_[2];
  OnePole lp_[2], hp_[2];
  float time_ = 0;
};

// ---------------------------------------------------------------------------
// 7. Reverb (Freeverb)
// ---------------------------------------------------------------------------
class ReverbFx : public Effect {
  struct Comb {
    std::vector<float> b; int i = 0; float st = 0;
    inline float tick(float x, float fb, float damp) {
      float o = b[size_t(i)];
      st = o * (1 - damp) + st * damp;
      b[size_t(i)] = x + st * fb;
      if (++i >= int(b.size())) i = 0;
      return o;
    }
  };
  struct Allpass {
    std::vector<float> b; int i = 0;
    inline float tick(float x) {
      float o = b[size_t(i)];
      b[size_t(i)] = x + o * 0.5f;
      if (++i >= int(b.size())) i = 0;
      return o - x;
    }
  };
 public:
  ReverbFx() {
    addParam("Size", 0, 1, 0.75f);
    addParam("Damp", 0, 1, 0.4f);
    addParam("Width", 0, 1, 1.f);
    addParam("PreDelay", 0, 200, 15, ParamKind::Linear, "ms");
    addParam("LowCut", 20, 1000, 150, ParamKind::Log, "Hz");
    addParam("Mix", 0, 1, 0.3f);
  }
  const char* name() const override { return "Reverb"; }
  void onPrepare() override {
    static const int ct[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int at[4] = {556, 441, 341, 225};
    float s = float(sr_ / 44100.0);
    for (int c = 0; c < 2; ++c) {
      for (int k = 0; k < 8; ++k) comb_[c][k].b.assign(size_t((ct[k] + c * 23) * s) + 1, 0.f);
      for (int k = 0; k < 4; ++k) ap_[c][k].b.assign(size_t((at[k] + c * 23) * s) + 1, 0.f);
    }
    pre_.init(int(sr_ * 0.25));
  }
  void reset() override {
    for (auto& cc : comb_) for (auto& c : cc) std::fill(c.b.begin(), c.b.end(), 0.f);
    for (auto& aa : ap_) for (auto& a : aa) std::fill(a.b.begin(), a.b.end(), 0.f);
    pre_.clear();
  }
  void update(const FxContext&) override { hp_[0].setHz(p_[4], sr_); hp_[1].setHz(p_[4], sr_); }
  void process(float* L, float* R, int n, const FxContext&) override {
    float fb = 0.7f + p_[0] * 0.28f, damp = p_[1] * 0.4f, w = p_[2], mix = p_[5];
    float w1 = w / 2 + 0.5f, w2 = (1 - w) / 2;
    float pd = std::max(1.f, p_[3] * 0.001f * float(sr_));
    for (int i = 0; i < n; ++i) {
      float in = (L[i] + R[i]) * 0.015f;
      pre_.push(in);
      float x = pre_.read(pd);
      float ol = 0, orr = 0;
      for (int k = 0; k < 8; ++k) { ol += comb_[0][k].tick(x, fb, damp); orr += comb_[1][k].tick(x, fb, damp); }
      for (int k = 0; k < 4; ++k) { ol = ap_[0][k].tick(ol); orr = ap_[1][k].tick(orr); }
      ol = hp_[0].hp(ol); orr = hp_[1].hp(orr);
      float wl = ol * w1 + orr * w2, wr = orr * w1 + ol * w2;
      L[i] = L[i] * (1 - mix * 0.5f) + wl * mix * 3.f;
      R[i] = R[i] * (1 - mix * 0.5f) + wr * mix * 3.f;
    }
  }
 private:
  Comb comb_[2][8];
  Allpass ap_[2][4];
  DelayLine pre_;
  OnePole hp_[2];
};

// ---------------------------------------------------------------------------
// 8/9. Chorus y Flanger (línea de retardo modulada)
// ---------------------------------------------------------------------------
class ModDelayFx : public Effect {
 public:
  explicit ModDelayFx(bool flanger) : flanger_(flanger) {
    addParam("Rate", 0.02f, 8, flanger ? 0.25f : 0.8f, ParamKind::Log, "Hz");
    addParam("Depth", 0, 1, flanger ? 0.7f : 0.5f);
    addParam("Delay", flanger ? 0.3f : 4.f, flanger ? 10.f : 30.f, flanger ? 2.f : 12.f, ParamKind::Linear, "ms");
    addParam("Feedback", flanger ? -0.95f : 0.f, flanger ? 0.95f : 0.6f, flanger ? 0.6f : 0.1f);
    addParam("Mix", 0, 1, 0.5f);
  }
  const char* name() const override { return flanger_ ? "Flanger" : "Chorus"; }
  void onPrepare() override { d_[0].init(int(sr_ * 0.08)); d_[1].init(int(sr_ * 0.08)); }
  void reset() override { d_[0].clear(); d_[1].clear(); }
  void process(float* L, float* R, int n, const FxContext&) override {
    float base = p_[2] * 0.001f * float(sr_);
    float depth = p_[1] * (flanger_ ? base * 0.95f : base * 0.6f);
    float inc = p_[0] / float(sr_), fb = p_[3], mix = p_[4];
    float* ch[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      ph_ += inc; if (ph_ >= 1) ph_ -= 1;
      for (int c = 0; c < 2; ++c) {
        float lfo = std::sin(kTwoPi * (ph_ + c * 0.25f));
        float d = std::max(1.f, base + depth * lfo);
        float y = d_[c].read(d);
        d_[c].push(ch[c][i] + y * fb);
        ch[c][i] = ch[c][i] * (1 - mix * 0.5f) + y * mix;
      }
    }
  }
 private:
  bool flanger_;
  DelayLine d_[2];
  float ph_ = 0;
};

// ---------------------------------------------------------------------------
// 10. Phaser
// ---------------------------------------------------------------------------
class PhaserFx : public Effect {
 public:
  PhaserFx() {
    addParam("Rate", 0.02f, 8, 0.4f, ParamKind::Log, "Hz");
    addParam("Depth", 0, 1, 0.8f);
    addParam("Feedback", 0, 0.95f, 0.5f);
    addParam("Stages", 0, 3, 1, ParamKind::Choice, "", {"4", "6", "8", "12"});
    addParam("Center", 200, 3000, 800, ParamKind::Log, "Hz");
    addParam("Mix", 0, 1, 0.5f);
  }
  const char* name() const override { return "Phaser"; }
  void reset() override { std::memset(z_, 0, sizeof z_); std::memset(fbz_, 0, sizeof fbz_); }
  void process(float* L, float* R, int n, const FxContext&) override {
    static const int st[4] = {4, 6, 8, 12};
    int stages = st[std::clamp(int(p_[3]), 0, 3)];
    float inc = p_[0] / float(sr_), mix = p_[5], fb = p_[2];
    float* ch[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      ph_ += inc; if (ph_ >= 1) ph_ -= 1;
      for (int c = 0; c < 2; ++c) {
        float lfo = std::sin(kTwoPi * (ph_ + c * 0.25f));
        float f = p_[4] * std::pow(4.f, lfo * p_[1]);
        float t = std::tan(kPi * clampf(f, 20.f, float(sr_) * 0.45f) / float(sr_));
        float a = (t - 1) / (t + 1);
        float x = ch[c][i] + fbz_[c] * fb;
        for (int s = 0; s < stages; ++s) {
          float y = a * x + z_[c][s];
          z_[c][s] = x - a * y;
          x = y;
        }
        fbz_[c] = x;
        ch[c][i] = ch[c][i] * (1 - mix * 0.5f) + x * mix;
      }
    }
  }
 private:
  float z_[2][12] = {}, fbz_[2] = {}, ph_ = 0;
};

// ---------------------------------------------------------------------------
// 11. Saturación / distorsión
// ---------------------------------------------------------------------------
class DriveFx : public Effect {
 public:
  DriveFx() {
    addParam("Mode", 0, 3, 0, ParamKind::Choice, "", {"SOFT", "HARD", "FOLD", "TUBE"});
    addParam("Drive", 0, 36, 12, ParamKind::Linear, "dB");
    addParam("Tone", 500, 20000, 8000, ParamKind::Log, "Hz");
    addParam("Output", -24, 6, -6, ParamKind::Linear, "dB");
    addParam("Mix", 0, 1, 1);
  }
  const char* name() const override { return "Drive"; }
  void update(const FxContext&) override { lp_[0].setHz(p_[2], sr_); lp_[1].setHz(p_[2], sr_); }
  void process(float* L, float* R, int n, const FxContext&) override {
    float g = dbToGain(p_[1]), out = dbToGain(p_[3]), mix = p_[4];
    int mode = int(p_[0]);
    float* ch[2] = {L, R};
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < n; ++i) {
        float x = ch[c][i], v = x * g, y;
        switch (mode) {
          case 1: y = clampf(v, -1.f, 1.f); break;
          case 2: y = std::sin(v); break;
          case 3: y = v >= 0 ? 1.f - std::exp(-v) : -0.7f * (1.f - std::exp(v * 1.4f)); break;
          default: y = std::tanh(v);
        }
        y = lp_[c].lp(y) * out;
        ch[c][i] = x + (y - x) * mix;
      }
  }
 private:
  OnePole lp_[2];
};

// ---------------------------------------------------------------------------
// 12. Bitcrusher
// ---------------------------------------------------------------------------
class CrusherFx : public Effect {
 public:
  CrusherFx() {
    addParam("Bits", 1, 16, 8, ParamKind::Int);
    addParam("Downsample", 1, 64, 4, ParamKind::Int, "x");
    addParam("Mix", 0, 1, 1);
  }
  const char* name() const override { return "Bitcrusher"; }
  void process(float* L, float* R, int n, const FxContext&) override {
    float q = std::pow(2.f, p_[0] - 1.f);
    int ds = int(p_[1]); float mix = p_[2];
    for (int i = 0; i < n; ++i) {
      if (++cnt_ >= ds) { cnt_ = 0; hl_ = std::round(L[i] * q) / q; hr_ = std::round(R[i] * q) / q; }
      L[i] += (hl_ - L[i]) * mix; R[i] += (hr_ - R[i]) * mix;
    }
  }
 private:
  int cnt_ = 0; float hl_ = 0, hr_ = 0;
};

// ---------------------------------------------------------------------------
// 13/14. Trémolo y Auto-pan
// ---------------------------------------------------------------------------
class TremoloFx : public Effect {
 public:
  explicit TremoloFx(bool pan) : pan_(pan) {
    addParam("Sync", 0, 1, 1, ParamKind::Toggle);
    addParam("Division", 0, 11, 5, ParamKind::Choice, "", kDivNames);
    addParam("Rate", 0.05f, 20, 4, ParamKind::Log, "Hz");
    addParam("Depth", 0, 1, 0.7f);
    addParam("Shape", 0, 2, 0, ParamKind::Choice, "", {"SINE", "SQUARE", "TRIANGLE"});
  }
  const char* name() const override { return pan_ ? "Auto Pan" : "Tremolo"; }
  void process(float* L, float* R, int n, const FxContext& c) override {
    double beats = p_[0] > 0.5f ? divisionBeats(int(p_[1])) : 0.0;
    float ph0 = syncPhase(c, beats, free_, p_[2], n);
    float inc = beats > 0 ? float(60.0 / (c.bpm * beats) / c.sr) : p_[2] / float(c.sr);
    int shape = int(p_[4]); float depth = p_[3];
    for (int i = 0; i < n; ++i) {
      float ph = std::fmod(ph0 + inc * float(i), 1.f);
      float l = lfoShape(ph, shape);
      if (pan_) {
        float gl, gr; panGains(l * depth, gl, gr);
        L[i] *= gl; R[i] *= gr;
      } else {
        float g = 1.f - depth * (0.5f - 0.5f * l);
        L[i] *= g; R[i] *= g;
      }
    }
  }
 private:
  bool pan_;
  float free_ = 0;
};

// ---------------------------------------------------------------------------
// 15. Modulador en anillo
// ---------------------------------------------------------------------------
class RingModFx : public Effect {
 public:
  RingModFx() {
    addParam("Freq", 20, 5000, 440, ParamKind::Log, "Hz");
    addParam("Mix", 0, 1, 0.5f);
  }
  const char* name() const override { return "Ring Mod"; }
  void process(float* L, float* R, int n, const FxContext&) override {
    float inc = p_[0] / float(sr_), mix = p_[1];
    for (int i = 0; i < n; ++i) {
      ph_ += inc; if (ph_ >= 1) ph_ -= 1;
      float m = std::sin(kTwoPi * ph_);
      L[i] += (L[i] * m - L[i]) * mix; R[i] += (R[i] * m - R[i]) * mix;
    }
  }
 private:
  float ph_ = 0;
};

// ---------------------------------------------------------------------------
// 16. Ensanchador estéreo
// ---------------------------------------------------------------------------
class WidenerFx : public Effect {
 public:
  WidenerFx() {
    addParam("Width", 0, 2, 1.4f);
    addParam("BassMono", 20, 500, 120, ParamKind::Log, "Hz");
    addParam("Output", -12, 6, 0, ParamKind::Linear, "dB");
  }
  const char* name() const override { return "Stereo Width"; }
  void update(const FxContext&) override { lp_.setHz(p_[1], sr_); }
  void process(float* L, float* R, int n, const FxContext&) override {
    float w = p_[0], out = dbToGain(p_[2]);
    for (int i = 0; i < n; ++i) {
      float m = (L[i] + R[i]) * 0.5f, s = (L[i] - R[i]) * 0.5f;
      float sLow = lp_.lp(s);
      s = sLow * std::min(w, 1.f) + (s - sLow) * w;
      L[i] = (m + s) * out; R[i] = (m - s) * out;
    }
  }
 private:
  OnePole lp_;
};

// ---------------------------------------------------------------------------
// 17. Stutter / Beat repeat sincronizado
// ---------------------------------------------------------------------------
class StutterFx : public Effect {
 public:
  StutterFx() {
    addParam("Active", 0, 1, 0, ParamKind::Toggle);
    addParam("Division", 0, 11, 2, ParamKind::Choice, "", kDivNames);
    addParam("Decay", 0, 1, 0);
    addParam("Mix", 0, 1, 1);
  }
  const char* name() const override { return "Stutter"; }
  void onPrepare() override { buf_[0].assign(size_t(sr_ * 4), 0.f); buf_[1].assign(size_t(sr_ * 4), 0.f); }
  void process(float* L, float* R, int n, const FxContext& c) override {
    bool on = p_[0] > 0.5f;
    int size = int(buf_[0].size());
    int len = std::clamp(int(divisionBeats(int(p_[1])) * 60.0 / c.bpm * c.sr), 64, size / 2);
    if (on && !active_) { active_ = true; start_ = (w_ - len + size) % size; rp_ = 0; gain_ = 1; slice_ = len; }
    if (!on) active_ = false;
    float mix = p_[3];
    for (int i = 0; i < n; ++i) {
      if (!active_) {
        buf_[0][size_t(w_)] = L[i]; buf_[1][size_t(w_)] = R[i];
        w_ = (w_ + 1) % size;
        continue;
      }
      int idx = (start_ + rp_) % size;
      float env = std::min(1.f, std::min(rp_, slice_ - rp_) / 64.f);
      float yl = buf_[0][size_t(idx)] * env * gain_, yr = buf_[1][size_t(idx)] * env * gain_;
      if (++rp_ >= slice_) { rp_ = 0; gain_ *= 1.f - p_[2] * 0.5f; }
      L[i] += (yl - L[i]) * mix; R[i] += (yr - R[i]) * mix;
    }
  }
 private:
  std::vector<float> buf_[2];
  int w_ = 0, start_ = 0, rp_ = 0, slice_ = 1;
  bool active_ = false;
  float gain_ = 1;
};

// ---------------------------------------------------------------------------
// 18. Pitch shifter (dos cabezales con ventana)
// ---------------------------------------------------------------------------
class PitchFx : public Effect {
 public:
  PitchFx() {
    addParam("Semitones", -24, 24, 7, ParamKind::Linear, "st");
    addParam("Window", 20, 200, 70, ParamKind::Linear, "ms");
    addParam("Mix", 0, 1, 0.5f);
  }
  const char* name() const override { return "Pitch Shift"; }
  void onPrepare() override { d_[0].init(int(sr_ * 0.5)); d_[1].init(int(sr_ * 0.5)); }
  void process(float* L, float* R, int n, const FxContext&) override {
    float ratio = std::pow(2.f, p_[0] / 12.f);
    float win = p_[1] * 0.001f * float(sr_), mix = p_[2];
    float rate = (1.f - ratio) / win;
    float* ch[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      ph_ += rate; if (ph_ >= 1) ph_ -= 1; if (ph_ < 0) ph_ += 1;
      float p2 = std::fmod(ph_ + 0.5f, 1.f);
      float g1 = std::sin(kPi * ph_), g2 = std::sin(kPi * p2);
      for (int c = 0; c < 2; ++c) {
        d_[c].push(ch[c][i]);
        float y = d_[c].read(1.f + ph_ * win) * g1 + d_[c].read(1.f + p2 * win) * g2;
        ch[c][i] += (y - ch[c][i]) * mix;
      }
    }
  }
 private:
  DelayLine d_[2];
  float ph_ = 0;
};

// ---------------------------------------------------------------------------
// 19. Tape (wow, flutter, saturación, tono)
// ---------------------------------------------------------------------------
class TapeFx : public Effect {
 public:
  TapeFx() {
    addParam("Wow", 0, 1, 0.3f);
    addParam("Flutter", 0, 1, 0.2f);
    addParam("Saturation", 0, 1, 0.4f);
    addParam("Tone", 1000, 20000, 9000, ParamKind::Log, "Hz");
    addParam("Hiss", 0, 1, 0.05f);
    addParam("Mix", 0, 1, 1);
  }
  const char* name() const override { return "Tape"; }
  void onPrepare() override {
    d_[0].init(int(sr_ * 0.1)); d_[1].init(int(sr_ * 0.1));
    hissRel_ = std::exp(-1.f / (0.06f * float(sr_)));  // el siseo se apaga en ~60 ms sin señal
    hissEnv_ = 0;
  }
  void reset() override { hissEnv_ = 0; d_[0].clear(); d_[1].clear(); }
  void update(const FxContext&) override { lp_[0].setHz(p_[3], sr_); lp_[1].setHz(p_[3], sr_); }
  void process(float* L, float* R, int n, const FxContext&) override {
    float sr = float(sr_), mix = p_[5], sat = 1.f + p_[2] * 6.f;
    // Ganancia unidad a -10 dBFS: la saturación añade armónicos y aplasta picos, sin subir volumen.
    const float satNorm = 0.3f / std::tanh(0.3f * sat);
    float* ch[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      wph_ += 0.55f / sr; if (wph_ >= 1) wph_ -= 1;
      fph_ += 6.8f / sr; if (fph_ >= 1) fph_ -= 1;
      drift_ += (rng_.bi() * 0.5f - drift_) * 0.0005f;
      float d = 0.012f * sr + (std::sin(kTwoPi * wph_) + drift_) * p_[0] * 0.006f * sr +
                std::sin(kTwoPi * fph_) * p_[1] * 0.0006f * sr;
      // El siseo sigue a la señal (como una cinta con reductor de ruido): en silencio, silencio.
      float in = std::max(std::fabs(L[i]), std::fabs(R[i]));
      hissEnv_ = in > hissEnv_ ? in : hissEnv_ * hissRel_;
      float hiss = p_[4] * 0.004f * std::min(1.f, hissEnv_ * 30.f);
      for (int c = 0; c < 2; ++c) {
        d_[c].push(ch[c][i]);
        float y = d_[c].read(d);
        y = std::tanh(y * sat) * satNorm;
        y = lp_[c].lp(y) + rng_.bi() * hiss;
        ch[c][i] += (y - ch[c][i]) * mix;
      }
    }
  }
 private:
  DelayLine d_[2];
  OnePole lp_[2];
  float wph_ = 0, fph_ = 0, drift_ = 0, hissEnv_ = 0, hissRel_ = 0.9996f;
  Rng rng_{1234};
};

}  // namespace fxdsp
}  // namespace lx

#endif
