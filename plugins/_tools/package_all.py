#!/usr/bin/env python3
"""Empaqueta los 22 plugins LooperX en un solo zip listo para la SD/USB del MPC, y el código fuente.
Uso: python3 plugins/_tools/package_all.py [--out dist]   (después de compilar con run_all.sh)"""
import json, os, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PL = os.path.join(ROOT, "plugins")


def add_dir(zf, src, arc):
    for r, _d, fs in os.walk(src):
        for f in sorted(fs):
            full = os.path.join(r, f)
            zf.write(full, os.path.join(arc, os.path.relpath(full, src)))


def main():
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(ROOT, "dist")
    os.makedirs(out, exist_ok=True)
    projects = sorted(d for d in os.listdir(PL) if d.startswith("LooperX") and os.path.isfile(os.path.join(PL, d, "plugin.json")))
    bin_zip = os.path.join(out, "LooperX_MPC_Plugins_1.0.0.zip")
    n = 0
    with zipfile.ZipFile(bin_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for p in projects:
            m = json.load(open(os.path.join(PL, p, "plugin.json")))
            folder = "%s - VST - %s" % (m["manufacturer"], m["name"])
            built = os.path.join(PL, p, "build", folder)
            if not os.path.isfile(os.path.join(built, "VST", os.path.basename(m["file32"]))):
                raise SystemExit("falta compilar %s (%s)" % (p, built))
            add_dir(zf, built, os.path.join("VST2 Plugins", folder))
            n += 1
        zf.writestr("LooperX/Samples/LEEME.txt", "Pon aquí tus WAV (mono o estéreo, 16/24/32 bits) para LooperX Granular.\n"
                    "Se admiten subcarpetas (2 niveles). Pulsa Rescan en el Granular tras copiar archivos.\n")
        zf.writestr("LooperX/Loops/LEEME.txt", "Aquí guarda LooperX Looper los loops exportados (Export WAV).\n"
                    "LooperX Granular también los puede usar como fuente.\n")
        zf.write(os.path.join(HERE, "LEEME_LooperX.txt"), "LEEME_LooperX.txt")
    src_zip = os.path.join(out, "LooperX_MPC_source.zip")
    skip_dirs = {"build", "bin", "__pycache__", ".git", "dist", ".sdk-tools"}
    with zipfile.ZipFile(src_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for r, ds, fs in os.walk(ROOT):
            ds[:] = sorted(d for d in ds if d not in skip_dirs)
            for f in sorted(fs):
                full = os.path.join(r, f)
                zf.write(full, os.path.join("looperx-mpc", os.path.relpath(full, ROOT)))
    for z in (bin_zip, src_zip):
        print("%-60s %.1f MB" % (z, os.path.getsize(z) / 1e6))
    print("plugins empaquetados:", n)


if __name__ == "__main__":
    main()
