// PitchShift.cpp - LooperX FX: Pitch Shift. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxPitchShift : lx::FxPlugin
{
    LooperXFxPitchShift() : lx::FxPlugin("Pitch Shift", MPC_FOURCC('L', 'x', 'P', 's'), new lx::fxdsp::PitchFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxPitchShift)
