// Gate.cpp - LooperX FX: Gate. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxGate : lx::FxPlugin
{
    LooperXFxGate() : lx::FxPlugin("Gate", MPC_FOURCC('L', 'x', 'G', 'a'), new lx::fxdsp::GateFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxGate)
