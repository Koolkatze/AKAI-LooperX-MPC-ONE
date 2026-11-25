// Stutter.cpp - LooperX FX: Stutter. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxStutter : lx::FxPlugin
{
    LooperXFxStutter() : lx::FxPlugin("Stutter", MPC_FOURCC('L', 'x', 'S', 't'), new lx::fxdsp::StutterFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxStutter)
