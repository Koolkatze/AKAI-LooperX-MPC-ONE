// Reverb.cpp - LooperX FX: Reverb. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxReverb : lx::FxPlugin
{
    LooperXFxReverb() : lx::FxPlugin("Reverb", MPC_FOURCC('L', 'x', 'R', 'v'), new lx::fxdsp::ReverbFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxReverb)
