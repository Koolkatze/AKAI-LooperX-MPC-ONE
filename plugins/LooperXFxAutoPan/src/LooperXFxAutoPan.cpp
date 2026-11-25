// AutoPan.cpp - LooperX FX: Auto Pan. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxAutoPan : lx::FxPlugin
{
    LooperXFxAutoPan() : lx::FxPlugin("Auto Pan", MPC_FOURCC('L', 'x', 'A', 'p'), new lx::fxdsp::TremoloFx(true)) {}
};

MPC_PLUGIN_ENTRY(LooperXFxAutoPan)
