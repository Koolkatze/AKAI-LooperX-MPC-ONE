// Eq3.cpp - LooperX FX: EQ 3-Band. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxEq3 : lx::FxPlugin
{
    LooperXFxEq3() : lx::FxPlugin("EQ 3-Band", MPC_FOURCC('L', 'x', 'E', 'q'), new lx::fxdsp::Eq3Fx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxEq3)
