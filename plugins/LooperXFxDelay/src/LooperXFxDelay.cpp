// Delay.cpp - LooperX FX: Delay. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxDelay : lx::FxPlugin
{
    LooperXFxDelay() : lx::FxPlugin("Delay", MPC_FOURCC('L', 'x', 'D', 'l'), new lx::fxdsp::DelayFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxDelay)
