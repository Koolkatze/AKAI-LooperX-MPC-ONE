// Compressor.cpp - LooperX FX: Compressor. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxCompressor : lx::FxPlugin
{
    LooperXFxCompressor() : lx::FxPlugin("Compressor", MPC_FOURCC('L', 'x', 'C', 'o'), new lx::fxdsp::CompressorFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxCompressor)
