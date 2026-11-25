#!/usr/bin/env python3
"""Extrae los 19 efectos del proyecto LooperX original (src/core/effects.*) a un único
header para los plugins del MPC: plugins/_shared/lx_effects.h (sin LADSPA ni registro)."""
import re, sys
src_dir = sys.argv[1]   # .../looperx/src/core
out = sys.argv[2]
h = open(src_dir + "/effects.h").read()
c = open(src_dir + "/effects.cpp").read().split("\n")

# Tipos y clases base del header (sin registro)
start = h.index("struct FxContext")
end = h.index("// Registro de tipos de efecto")
base = h[start:end]

# Métodos de ParamHost (en effects.cpp, antes de "Bloques DSP")
cpp = "\n".join(c)
m0 = cpp.index("void ParamHost::addParam")
m1 = cpp.index("// ---------------------------------------------------------------------------\n// Bloques DSP")
methods = cpp[m0:m1]
methods = re.sub(r"^(void|float|std::string) ParamHost::", r"inline \1 ParamHost::", methods, flags=re.M)

# DSP + 19 efectos (namespace anónimo → namespace fxdsp)
d0 = cpp.index("namespace {\n\nstruct OnePole")
d1 = cpp.index("// ---------------------------------------------------------------------------\n// Host LADSPA")
dsp = cpp[d0:d1].replace("namespace {", "namespace fxdsp {", 1)

hdr = '''// lx_effects.h - los 19 efectos de LooperX para los plugins LooperX FX del MPC.
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

'''
ftr = '''}  // namespace fxdsp
}  // namespace lx

#endif
'''
dsp = dsp.rstrip()
open(out, "w").write(hdr + base + "\n" + methods + "\n" + dsp + "\n\n" + ftr)
print("lx_effects.h:", out)
