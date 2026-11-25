// RingMod.cpp - LooperX FX: Ring Mod. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxRingMod : lx::FxPlugin
{
    LooperXFxRingMod() : lx::FxPlugin("Ring Mod", MPC_FOURCC('L', 'x', 'R', 'm'), new lx::fxdsp::RingModFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxRingMod)
