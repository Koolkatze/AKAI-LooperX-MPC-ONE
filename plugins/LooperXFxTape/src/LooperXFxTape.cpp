// Tape.cpp - LooperX FX: Tape. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxTape : lx::FxPlugin
{
    LooperXFxTape() : lx::FxPlugin("Tape", MPC_FOURCC('L', 'x', 'T', 'p'), new lx::fxdsp::TapeFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxTape)
