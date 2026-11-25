// Limiter.cpp - LooperX FX: Limiter. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxLimiter : lx::FxPlugin
{
    LooperXFxLimiter() : lx::FxPlugin("Limiter", MPC_FOURCC('L', 'x', 'L', 'i'), new lx::fxdsp::LimiterFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxLimiter)
