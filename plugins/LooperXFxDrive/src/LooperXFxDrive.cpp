// Drive.cpp - LooperX FX: Drive. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxDrive : lx::FxPlugin
{
    LooperXFxDrive() : lx::FxPlugin("Drive", MPC_FOURCC('L', 'x', 'D', 'r'), new lx::fxdsp::DriveFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxDrive)
