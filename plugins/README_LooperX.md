# LooperX para MPC — código fuente

22 plugins para el MPC hechos con el SDK oficial de plugins de Akai/inMusic (`mpc-plugin-builder-sdk`, MIT).
Este árbol es una copia del SDK con los proyectos LooperX en `plugins/`.

| Carpeta | Plugin (en el MPC) | Tipo | uid |
|---|---|---|---|
| `LooperXLooper` | LooperX · Looper | efecto 2→2 | `LxLp` |
| `LooperXPads` | LooperX · Pads | instrumento (MIDI → enlace, sin audio) | `LxPd` |
| `LooperXGranular` | LooperX · Granular | instrumento 0→2 | `LxGr` |
| `LooperXFx*` (19) | LooperX · Filter … Tape | efecto 2→2 | `Lx..` (ver `_tools/gen_fx.py`) |

Código común en `plugins/_shared/`:

- `lx_looper.h` — motor del looper (estados de pedal, cuantización, largo fijo, overdub con undo/redo, sincronía con el transporte del MPC, pool de memoria por bloques).
- `lx_link.h` — enlace entre plugins en el mismo proceso del MPC (memoria compartida `/dev/shm/looperx-link-<uid>`): contadores de pulsación, estado de los slots, dirección del pool del Looper y contador de lectores. **No forma parte del contrato documentado del SDK.**
- `lx_granular.h` — motor granular (fuente WAV o slot vivo del Looper).
- `lx_effects.h` — los 19 efectos, **generado** por `_tools/extract_effects.py` desde el proyecto LooperX original.
- `lx_fxplugin.h` — envoltorio común de los plugins FX (parámetro 0 = Level).
- `lx_wavwrite.h` — WAV de 24 bits para Export WAV.

## Compilar y probar

```sh
export ZIG=zig                      # o el que indique docs/building.md
bash plugins/_tests/run_all.sh      # compila los 22 (nativo + ARM32 + ARM64), valida, host_sim y pruebas propias
python3 plugins/_tools/package_all.py   # dist/LooperX_MPC_Plugins_1.0.0.zip (carpeta "VST2 Plugins" lista)
```

Pruebas propias (`plugins/_tests/`): `looper_core_test` (motor, 32 comprobaciones), `looper_plugin_test`
(el plugin cargado por la ABI VST2: pulsaciones, transporte, guardar/abrir proyecto, Export WAV),
`link_test` (Looper + Pads + Granular en un proceso), `fx_test` (los 19 efectos). `vsthost.h` es el host mínimo.

## Skins

`build.py` copia `plugins/<P>/skins/` tal cual. Se generan con:

```sh
python3 tools/build.py plugins/<P> --no-ui      # params.json
python3 plugins/_tools/lx_skins.py plugins/<P>  # gen_ui.py oficial + ajustes de skin_spec.json
```

`skin_spec.json`: `momentary` (botones cuyo texto es un estado en vivo, con dibujo de pad fijo),
`liveText`, `continuous` y `steps` (pasos de la rueda con el texto que devuelve el propio plugin,
vía `_tools/paramtext.cpp`). Vuelve a generar los skins si cambias parámetros.
Los FX: `python3 plugins/_tools/gen_fx.py` (proyectos) y `gen_fx.py ui <Nombre>` (ui.json).

## Reglas que no se pueden romper una vez publicado

- uid, id y orden de los parámetros son permanentes (los proyectos del MPC guardan por índice/id).
- Nada de memoria, locks, ficheros ni logs en `process()`.
- Formato del estado extra: Looper `LXLP` v1 (loops dentro del proyecto), Granular `LXGR` v1 (clave del WAV).
- No instalar en un MPC por red (`tools/mpc_remote.py`) sin permiso explícito del dueño.
