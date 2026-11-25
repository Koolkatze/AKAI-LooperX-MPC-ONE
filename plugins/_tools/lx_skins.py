#!/usr/bin/env python3
"""lx_skins.py - genera los skins (TUI.json + Q-Links.json) de un plugin LooperX con el generador
oficial del SDK (tools/gen_ui.py) y después aplica lo que el generador no sabe hacer:

  * "momentary": botones de acción (LOOP n, STOP n, ...) cuyo texto es el ESTADO en vivo (REC, PLAY...).
    El generador los trataría como interruptores On/Off y taparía ese texto con "Off"/"On".
    Aquí se quedan con el texto del plugin y con un dibujo de pad que no cambia al pulsar.
  * "liveText": bool/choice cuyo texto lo escribe el plugin en vivo (p. ej. "64MB*", "NO LINK").
  * "continuous": parámetros que el simulador del SDK confunde con listas por su texto.
  * "steps": {"Parámetro": nº de pasos} - pasos discretos para la rueda del MPC; el texto de cada
    paso se le pregunta al propio plugin (plugins/_tools/paramtext.cpp), así siempre coincide.

Uso:  python3 plugins/_tools/lx_skins.py plugins/LooperXLooper
      (necesita plugins/<P>/build/params.json: ejecuta antes tools/build.py <plugin> --no-ui)
Vuelve a ejecutarlo si cambias parámetros: build.py copia plugins/<P>/skins/ tal cual.
"""
import copy
import fnmatch
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def matches(name, patterns):
    return any(fnmatch.fnmatchcase(name, p) for p in patterns)


def plugin_lib(pdir):
    nat = os.path.join(pdir, "build", "native")
    libs = [f for f in os.listdir(nat) if f.endswith((".so", ".dylib"))] if os.path.isdir(nat) else []
    if not libs:
        raise SystemExit("lx_skins: no hay librería nativa en %s (ejecuta tools/build.py <plugin> --no-ui)" % nat)
    return os.path.join(nat, libs[0])


def paramtext_tool():
    exe = os.path.join(HERE, "bin", "paramtext")
    src = os.path.join(HERE, "paramtext.cpp")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        subprocess.check_call(["g++", "-std=c++17", "-O1", "-I" + os.path.join(ROOT, "framework"), "-o", exe, src, "-ldl"])
    return exe


def step_strings(pdir, name, count):
    """Texto real del plugin en cada uno de los `count` pasos del parámetro."""
    out = subprocess.check_output([paramtext_tool(), plugin_lib(pdir), name, str(count)], text=True)
    return json.loads(out)


def pad_png(path, accent):
    from PIL import Image, ImageDraw
    w, h = 76, 60
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    a = tuple(int(accent[i:i + 2], 16) for i in (2, 4, 6))  # AARRGGBB
    d.rounded_rectangle([1, 1, w - 2, h - 2], radius=9, fill=(44, 44, 48, 255), outline=a + (255,), width=3)
    d.rounded_rectangle([8, 8, w - 9, h - 9], radius=6, outline=(70, 70, 76, 255), width=1)
    im.save(path)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    pdir = os.path.abspath(sys.argv[1])
    spec_path = os.path.join(pdir, "skin_spec.json")
    params_path = os.path.join(pdir, "build", "params.json")
    with open(spec_path, encoding="utf-8") as f:
        spec = json.load(f)
    with open(params_path, encoding="utf-8") as f:
        data = json.load(f)

    momentary = spec.get("momentary", [])
    live = spec.get("liveText", [])
    continuous = spec.get("continuous", [])
    steps = {k: step_strings(pdir, k, int(v)) for k, v in spec.get("steps", {}).items()}

    strip = set()      # índices sin replacementStrings
    mom_idx = set()
    for p in data["params"]:
        n = p["name"]
        if matches(n, momentary):
            p["kind"], p["choices"] = "bool", ["Off", "On"]
            strip.add(p["index"]); mom_idx.add(p["index"])
        elif matches(n, continuous):
            p["kind"], p["choices"] = "float", []
        elif matches(n, live):
            if p.get("kind") not in ("bool", "choice"):
                p["kind"], p["choices"] = "bool", ["Off", "On"]
            strip.add(p["index"])
        if n in steps:
            p["kind"], p["choices"] = "choice", steps[n]

    with tempfile.TemporaryDirectory() as tmp:
        pj = os.path.join(tmp, "params.json")
        with open(pj, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1)
        out = os.path.join(tmp, "skins")
        cmd = [sys.executable, os.path.join(ROOT, "tools", "gen_ui.py"), "--params", pj,
               "--ui", os.path.join(pdir, "ui.json"), "--out", out, "--kit-cache", os.path.join(pdir, "build")]
        r = subprocess.run(cmd, capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode != 0:
            return r.returncode

        tui_path = os.path.join(out, "TUI.json")
        with open(tui_path, encoding="utf-8") as f:
            tui = json.load(f)
        pd = tui["pageData"]
        handles = {"Parameter %d" % i for i in strip}
        before = len(pd["replacementStrings"])
        pd["replacementStrings"] = [e for e in pd["replacementStrings"] if e["key"] not in handles]
        removed = before - len(pd["replacementStrings"])

        defs = pd["componentDefinitions"]["localComponentDefinitions"]
        retyped = 0
        if mom_idx:
            toggle = next((e["value"] for e in defs if e["key"] == "toggle"), None)
            if toggle is None:
                print("lx_skins: no hay definición 'toggle' (¿ningún botón en ui.json?)", file=sys.stderr)
                return 1
            mom = copy.deepcopy(toggle)
            for c in mom["componentsData"]:
                cd = c["componentData"]
                if cd["type"] == "Button":
                    cd["data"]["onImage"] = "pad.png"
                    cd["data"]["offImage"] = "pad.png"
                    c["bounds"]["bounds"] = "190 24 76 60"
                if cd["type"] == "Label" and cd["data"].get("type") == "Value":
                    c["bounds"]["bounds"] = "15 50 176 46"
            defs.insert(0, {"key": "momentary", "value": mom})
            mom_handles = {"Parameter %d" % i for i in mom_idx}
            for tab in pd["tabs"]:
                for c in tab["componentDefinition"]["componentsData"]:
                    m = c.get("handle remapping", {}).get("map", [])
                    if c["componentData"]["type"] == "toggle" and any(x["value"] in mom_handles for x in m):
                        c["componentData"]["type"] = "momentary"
                        retyped += 1
            accent = (json.load(open(os.path.join(pdir, "ui.json"))).get("kit") or {}).get("accent") or "ffff6a1a"
            pad_png(os.path.join(out, "pad.png"), accent)

        with open(tui_path, "w", encoding="utf-8") as f:
            json.dump(tui, f, indent=2, ensure_ascii=False)

        dest = os.path.join(pdir, "skins")
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        shutil.copytree(out, dest)
    print("lx_skins: %s -> skins/ (%d botones de estado, %d parámetros con texto en vivo, %d listas de pasos)"
          % (os.path.basename(pdir), retyped, removed, len(steps)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
