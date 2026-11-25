// lx_wavwrite.h - LooperX: escritura de WAV estéreo 24 bits (exportar loops para el MPC).
// SPDX-License-Identifier: MIT
// Solo desde un hilo que no sea el de audio.
#ifndef LX_WAVWRITE_H
#define LX_WAVWRITE_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace lx {

inline bool writeWav24(const std::string& path, int sampleRate, int64_t frames,
                       const std::function<bool(int64_t, float&, float&)>& get)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto put16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    auto put32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    const uint32_t dataBytes = (uint32_t)(frames * 6);
    std::fwrite("RIFF", 1, 4, f); put32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); put32(16); put16(1); put16(2); put32((uint32_t)sampleRate);
    put32((uint32_t)sampleRate * 6); put16(6); put16(24);
    std::fwrite("data", 1, 4, f); put32(dataBytes);
    std::vector<uint8_t> buf;
    buf.reserve(60000);
    bool ok = true;
    for (int64_t i = 0; i < frames; ++i) {
        float s[2] = {0.f, 0.f};
        if (!get(i, s[0], s[1])) { s[0] = s[1] = 0.f; }
        for (float v : s) {
            float c = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
            int32_t q = (int32_t)std::lrint(c * 8388607.0f);
            buf.push_back(uint8_t(q)); buf.push_back(uint8_t(q >> 8)); buf.push_back(uint8_t(q >> 16));
        }
        if (buf.size() >= 59994) { if (std::fwrite(buf.data(), 1, buf.size(), f) != buf.size()) ok = false; buf.clear(); }
    }
    if (!buf.empty() && std::fwrite(buf.data(), 1, buf.size(), f) != buf.size()) ok = false;
    if (std::fclose(f) != 0) ok = false;
    return ok;
}

}  // namespace lx

#endif
