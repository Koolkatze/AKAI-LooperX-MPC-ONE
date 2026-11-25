// StereoWidth.cpp - LooperX FX: Stereo Width. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxStereoWidth : lx::FxPlugin
{
    LooperXFxStereoWidth() : lx::FxPlugin("Stereo Width", MPC_FOURCC('L', 'x', 'S', 'w'), new lx::fxdsp::WidenerFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxStereoWidth)
