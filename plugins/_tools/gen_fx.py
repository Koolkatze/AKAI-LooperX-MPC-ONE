#!/usr/bin/env python3
"""Genera los proyectos de los 19 plugins LooperX FX (plugins/LooperXFx<Nombre>/).
Cada uno: plugin.json, src/<Nombre>.cpp (envoltorio de _shared/lx_fxplugin.h), ui.json y
skin_spec.json. Los uid ('Lx..') y los ids de parámetro son permanentes: no los cambies."""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
PL = os.path.normpath(os.path.join(HERE, ".."))

# (nombre en el MPC, carpeta, clase, args, uid, categoría, tema, descripción)
FX = [
    ("Filter", "Filter", "FilterFx", "", "LxFi", "Filter", "cyan", "Multimode SVF filter (LP/HP/BP/notch) with drive and mix."),
    ("EQ 3-Band", "Eq3", "Eq3Fx", "", "LxEq", "EQ", "cyan", "Three-band EQ: low shelf, mid peak and high shelf with adjustable frequencies."),
    ("Compressor", "Compressor", "CompressorFx", "", "LxCo", "Dynamics", "mono", "Feed-forward compressor with soft knee, makeup gain and parallel mix."),
    ("Limiter", "Limiter", "LimiterFx", "", "LxLi", "Dynamics", "mono", "Brickwall-style peak limiter with ceiling, release and input gain."),
    ("Gate", "Gate", "GateFx", "", "LxGa", "Dynamics", "mono", "Noise gate with attack, hold, release and range."),
    ("Delay", "Delay", "DelayFx", "", "LxDl", "Delay", "cyan", "Stereo delay synced to the MPC tempo (or free), ping-pong and feedback filters."),
    ("Reverb", "Reverb", "ReverbFx", "", "LxRv", "Reverb", "cyan", "Stereo algorithmic reverb with size, damping, width, pre-delay and low cut."),
    ("Chorus", "Chorus", "ModDelayFx", "false", "LxCh", "Modulation", "violet", "Stereo chorus."),
    ("Flanger", "Flanger", "ModDelayFx", "true", "LxFl", "Modulation", "violet", "Stereo flanger with positive/negative feedback."),
    ("Phaser", "Phaser", "PhaserFx", "", "LxPh", "Modulation", "violet", "4/6/8/12-stage phaser."),
    ("Drive", "Drive", "DriveFx", "", "LxDr", "Distortion", "amber", "Soft, hard, wavefold and tube saturation with tone control."),
    ("Bitcrusher", "Bitcrusher", "CrusherFx", "", "LxBc", "Distortion", "amber", "Bit depth and sample-rate reduction."),
    ("Tremolo", "Tremolo", "TremoloFx", "false", "LxTr", "Modulation", "violet", "Tremolo synced to the MPC tempo or free, sine/square/triangle."),
    ("Auto Pan", "AutoPan", "TremoloFx", "true", "LxAp", "Modulation", "violet", "Auto-panner synced to the MPC tempo or free."),
    ("Ring Mod", "RingMod", "RingModFx", "", "LxRm", "Modulation", "violet", "Ring modulator."),
    ("Stereo Width", "StereoWidth", "WidenerFx", "", "LxSw", "Utility", "mono", "Mid/side stereo width with mono bass."),
    ("Stutter", "Stutter", "StutterFx", "", "LxSt", "Creative", "lime", "Beat-repeat stutter synced to the MPC tempo."),
    ("Pitch Shift", "PitchShift", "PitchFx", "", "LxPs", "Pitch", "lime", "Two-head granular pitch shifter, +/-24 semitones."),
    ("Tape", "Tape", "TapeFx", "", "LxTp", "Creative", "lime", "Tape emulation: wow, flutter, saturation, tone and hiss."),
]

SRC = '''// {folder}.cpp - LooperX FX: {name}. GENERADO por plugins/_tools/gen_fx.py.
// SPDX-License-Identifier: MIT
#include "../../_shared/lx_fxplugin.h"

struct LooperXFx{folder} : lx::FxPlugin
{{
    LooperXFx{folder}() : lx::FxPlugin("{name}", MPC_FOURCC('{u0}', '{u1}', '{u2}', '{u3}'), new lx::fxdsp::{cls}({args})) {{}}
}};

MPC_PLUGIN_ENTRY(LooperXFx{folder})
'''


def main():
    for name, folder, cls, args, uid, cat, theme, desc in FX:
        d = os.path.join(PL, "LooperXFx" + folder)
        os.makedirs(os.path.join(d, "src"), exist_ok=True)
        slug = "fx-" + folder.lower()
        open(os.path.join(d, "src", "LooperXFx%s.cpp" % folder), "w").write(
            SRC.format(folder=folder, name=name, cls=cls, args=args, u0=uid[0], u1=uid[1], u2=uid[2], u3=uid[3]))
        pj = {"name": name, "manufacturer": "LooperX", "version": "1.0.0", "format": "vst2", "kind": "effect",
              "uid": uid, "numInputs": 2, "numOutputs": 2, "category": cat,
              "file32": "VST/%s-arm32.so" % slug, "file64": "VST/%s-arm64.so" % slug,
              "description": desc, "author": "Antonio (LooperX)", "url": "", "license": "MIT"}
        json.dump(pj, open(os.path.join(d, "plugin.json"), "w"), indent=2)
    print("gen_fx: %d proyectos" % len(FX))


if __name__ == "__main__" and len(os.sys.argv) == 1:
    main()


def make_ui(folder):
    """ui.json + skin_spec.json a partir de build/params.json (tras tools/build.py --no-ui)."""
    name, _f, cls, args, uid, cat, theme, desc = next(x for x in FX if x[1] == folder)
    d = os.path.join(PL, "LooperXFx" + folder)
    params = json.load(open(os.path.join(d, "build", "params.json")))["params"]
    ctl, steps = [], {}
    fx_params = params[1:]
    for p in fx_params:
        kind, n = p["kind"], p["name"]
        if kind == "bool" or (kind == "choice" and len(p["choices"]) <= 4):
            ctl.append({"param": n, "widget": "switch"})
        elif kind == "choice":
            ctl.append({"param": n, "widget": "knob", "overlay": "menu"})
        else:
            ctl.append({"param": n, "widget": "knob"})
    # Level al final, en su propia fila si cabe
    while len(ctl) % 4 != 3 and len(ctl) < 15:
        ctl.append("empty")
    ctl.append({"param": "Level", "widget": "fader"})
    ui = {"kit": {"theme": theme, "accent": "ffff6a1a"}, "qlinkGroups": "rows",
          "style": {"background": "ff141417"}, "pages": [{"name": "MAIN", "controls": ctl}]}
    json.dump(ui, open(os.path.join(d, "ui.json"), "w"), indent=2)
    json.dump({"steps": steps}, open(os.path.join(d, "skin_spec.json"), "w"), indent=2)


if __name__ == "__main__" and len(os.sys.argv) == 3 and os.sys.argv[1] == "ui":
    make_ui(os.sys.argv[2])
