// Flanger.cpp - LooperX FX: Flanger. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxFlanger : lx::FxPlugin
{
    LooperXFxFlanger() : lx::FxPlugin("Flanger", MPC_FOURCC('L', 'x', 'F', 'l'), new lx::fxdsp::ModDelayFx(true)) {}
};

MPC_PLUGIN_ENTRY(LooperXFxFlanger)
