// Bitcrusher.cpp - LooperX FX: Bitcrusher. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxBitcrusher : lx::FxPlugin
{
    LooperXFxBitcrusher() : lx::FxPlugin("Bitcrusher", MPC_FOURCC('L', 'x', 'B', 'c'), new lx::fxdsp::CrusherFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxBitcrusher)
