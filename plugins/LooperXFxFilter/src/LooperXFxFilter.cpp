// Filter.cpp - LooperX FX: Filter. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxFilter : lx::FxPlugin
{
    LooperXFxFilter() : lx::FxPlugin("Filter", MPC_FOURCC('L', 'x', 'F', 'i'), new lx::fxdsp::FilterFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxFilter)
