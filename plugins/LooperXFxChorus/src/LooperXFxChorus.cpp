// Chorus.cpp - LooperX FX: Chorus. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxChorus : lx::FxPlugin
{
    LooperXFxChorus() : lx::FxPlugin("Chorus", MPC_FOURCC('L', 'x', 'C', 'h'), new lx::fxdsp::ModDelayFx(false)) {}
};

MPC_PLUGIN_ENTRY(LooperXFxChorus)
