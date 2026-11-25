// lx_fxplugin.h - envoltorio común de los plugins "LooperX FX" (un efecto = un plugin del MPC).
//
// SPDX-License-Identifier: MIT
//
// Parámetro 0 = "Level" (salida en dB; el host del MPC "empuja" el parámetro 0 al preparar, así
// que conviene que sea inofensivo). Después van los parámetros del efecto en su orden original,
// con id estable derivado del nombre (no cambiar una vez publicado).
// El tempo, el play/stop y la posición del MPC llegan a los efectos sincronizados
// (Delay, Tremolo, Auto Pan, Stutter).
#ifndef LX_FXPLUGIN_H
#define LX_FXPLUGIN_H

#include "mpc_plugin.h"

#include <cctype>
#include <memory>
#include <string>

#include "lx_effects.h"

namespace lx {

class FxPlugin : public mpc::Plugin
{
public:
    FxPlugin(const char* name, uint32_t uid, Effect* fx) : mpc::Plugin(makeInfo(name, uid)), fx_(fx)
    {
        using namespace mpc;
        addParam(floatParam("level", "Level", -24.0f, 12.0f, 0.0f, "dB", 1));
        for (int i = 0; i < fx_->numParams(); ++i) {
            const ParamInfo& p = fx_->info(i);
            std::string id;
            for (char ch : p.name) if (std::isalnum((unsigned char)ch)) id += (char)std::tolower((unsigned char)ch);
            switch (p.kind) {
                case ParamKind::Choice: addParam(choiceParam(id.c_str(), p.name.c_str(), p.choices, (int)p.def)); break;
                case ParamKind::Toggle: addParam(boolParam(id.c_str(), p.name.c_str(), p.def > 0.5f)); break;
                case ParamKind::Int: addParam(intParam(id.c_str(), p.name.c_str(), (int)p.min, (int)p.max, (int)p.def, p.unit.c_str())); break;
                case ParamKind::Log:
                    if (p.min > 0) { addParam(logParam(id.c_str(), p.name.c_str(), p.min, p.max, p.def, p.unit.c_str(), 2)); break; }
                    [[fallthrough]];
                default: addParam(floatParam(id.c_str(), p.name.c_str(), p.min, p.max, p.def, p.unit.c_str(), 2)); break;
            }
        }
        std::vector<float> d((size_t)numParams());
        for (int i = 0; i < numParams(); ++i) d[(size_t)i] = param(i).def;
        addProgram("Default", d);
    }

    static mpc::PluginInfo makeInfo(const char* name, uint32_t uid)
    {
        mpc::PluginInfo i;
        i.name = name;
        i.vendor = "LooperX";
        i.uniqueId = uid;
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.tailSamples = 0;
        return i;
    }

    void prepare(double sr, int) override { fx_->prepare(sr); fx_->reset(); level_ = lx::dbToGain(get(0)); }
    void reset() override { fx_->reset(); }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        if (ctx.numInputs > 0 && ctx.inputs) {
            if (ctx.inputs[0] != L) std::memcpy(L, ctx.inputs[0], sizeof(float) * (size_t)n);
            const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
            if (inR != R) std::memcpy(R, inR, sizeof(float) * (size_t)n);
        }
        for (int i = 0; i < fx_->numParams(); ++i) {
            float v = get(i + 1);
            if (v != fx_->get(i)) fx_->set(i, v);
        }
        FxContext c;
        c.sr = ctx.sampleRate;
        c.bpm = ctx.transport.tempoValid && ctx.transport.bpm > 1.0 ? ctx.transport.bpm : 120.0;
        c.running = ctx.transport.playing;
        c.pos = (int64_t)ctx.transport.samplePos;
        fx_->run(L, R, n, c);
        const float target = lx::dbToGain(get(0));
        const float step = (target - level_) / (float)std::max(1, n);
        for (int i = 0; i < n; ++i) {
            level_ += step;
            L[i] *= level_;
            if (R != L) R[i] *= level_;
            // NaN/Inf fuera, y colas diminutas (< -400 dB) a cero exacto.
            if (!std::isfinite(L[i]) || std::fabs(L[i]) < 1e-20f) L[i] = 0.f;
            if (R != L && (!std::isfinite(R[i]) || std::fabs(R[i]) < 1e-20f)) R[i] = 0.f;
        }
        level_ = target;
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index == 0) { char b[24]; std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true; }
        const ParamInfo& p = fx_->info(index - 1);
        char b[48];
        switch (p.kind) {
            case ParamKind::Choice: return false;
            case ParamKind::Toggle: out = plain >= 0.5f ? "ON" : "OFF"; return true;
            case ParamKind::Int: std::snprintf(b, sizeof b, "%d%s%s", (int)std::lround(plain), p.unit.empty() ? "" : " ", p.unit.c_str()); out = b; return true;
            default: break;
        }
        const char* u = p.unit.c_str();
        const char* sp = p.unit.empty() || p.unit[0] == ':' ? "" : " ";
        if (p.unit.empty() && p.min >= -1.0f && p.max <= 1.0f) std::snprintf(b, sizeof b, "%.0f%%", plain * 100.0);
        else if (p.unit == "Hz" && plain >= 1000.f) std::snprintf(b, sizeof b, "%.2f kHz", plain / 1000.0);
        else if (p.unit == "ms" && plain >= 1000.f) std::snprintf(b, sizeof b, "%.2f s", plain / 1000.0);
        else if (std::fabs(plain) >= 100.f) std::snprintf(b, sizeof b, "%.0f%s%s", plain, sp, u);
        else if (std::fabs(plain) >= 10.f) std::snprintf(b, sizeof b, "%.1f%s%s", plain, sp, u);
        else std::snprintf(b, sizeof b, "%.2f%s%s", plain, sp, u);
        out = b;
        return true;
    }

private:
    std::unique_ptr<Effect> fx_;
    float level_ = 1.0f;
};

}  // namespace lx

#endif
