// Tremolo.cpp - LooperX FX: Tremolo. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxTremolo : lx::FxPlugin
{
    LooperXFxTremolo() : lx::FxPlugin("Tremolo", MPC_FOURCC('L', 'x', 'T', 'r'), new lx::fxdsp::TremoloFx(false)) {}
};

MPC_PLUGIN_ENTRY(LooperXFxTremolo)
