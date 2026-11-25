// Phaser.cpp - LooperX FX: Phaser. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFxPhaser : lx::FxPlugin
{
    LooperXFxPhaser() : lx::FxPlugin("Phaser", MPC_FOURCC('L', 'x', 'P', 'h'), new lx::fxdsp::PhaserFx()) {}
};

MPC_PLUGIN_ENTRY(LooperXFxPhaser)
