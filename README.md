# AKAI LooperX · MPC ONE

**LooperX** para Akai MPC: 22 plugins externos hechos con el SDK oficial de plugins de Akai/inMusic
([`mpc-plugin-builder-sdk`](https://github.com/inmusic-software/mpc-plugin-builder-sdk), MIT).
Funcionan en la MPC One (32 bits) y en las MPC de 64 bits, con **MPC OS 3.10.1 o posterior**.

| Plugin | Tipo | Qué hace |
|---|---|---|
| **Looper** | efecto (insert) | Looper de pedal de 16 pistas, sincronizado con el tempo y el transporte del MPC. Exporta los loops a WAV. |
| **Pads** | instrumento (sin audio) | Dispara el Looper desde los pads del MPC o desde un controlador MIDI externo. |
| **Granular** | instrumento | Sintetizador granular: WAV de fábrica, de `LooperX/Samples`, de los loops exportados o de un loop vivo del Looper. |
| 19 efectos | efecto (insert) | Filter, EQ 3-Band, Compressor, Limiter, Gate, Delay, Reverb, Chorus, Flanger, Phaser, Drive, Bitcrusher, Tremolo, Auto Pan, Ring Mod, Stereo Width, Stutter, Pitch Shift, Tape. |

## Instalar en la tarjeta SD

1. Descarga [`release/LooperX_MPC_Plugins_1.0.0.zip`](release/LooperX_MPC_Plugins_1.0.0.zip) y descomprímelo.
2. Copia a la **raíz** de la SD (o de un USB, o del disco interno) las carpetas `VST2 Plugins` y `LooperX`.
   Debe quedar `<SD>/VST2 Plugins/LooperX - VST - Looper/plugin.json`, etc.
3. En el MPC: **Preferences → Enhancements (Beta)** → activa **Enable Enhancements** y **External Plugin Support**.
4. Mete la SD (o reinicia). Los plugins aparecen en el navegador en **External Plugins**, fabricante **LooperX**.

Manual completo de uso (montaje recomendado en la MPC One, páginas del Looper, Granular, efectos):
[`release/LEEME_LooperX.txt`](release/LEEME_LooperX.txt).

> Akai/inMusic no da soporte a plugins de terceros: corren dentro del MPC y un fallo puede colgarlo.
> Están probados con el simulador del SDK y con pruebas propias, **todavía no en un MPC real**.
> Guarda a menudo y pruébalos antes de un directo.

## Código fuente

Este repositorio es una copia del SDK de Akai con los proyectos de LooperX en [`plugins/`](plugins/):

- `plugins/LooperXLooper`, `plugins/LooperXPads`, `plugins/LooperXGranular`, `plugins/LooperXFx*` (19 efectos).
- `plugins/_shared/` — motor del looper, enlace entre plugins, motor granular, efectos, escritura WAV.
- `plugins/_tests/` — pruebas propias. `plugins/_tools/` — generadores y empaquetado.
- Detalles técnicos: [`plugins/README_LooperX.md`](plugins/README_LooperX.md). Documentación del SDK: [`README_SDK.md`](README_SDK.md) y [`docs/`](docs/).

```sh
bash plugins/_tests/run_all.sh          # compila los 22 (nativo + ARM32 + ARM64), valida, host_sim y pruebas
python3 plugins/_tools/package_all.py   # dist/LooperX_MPC_Plugins_1.0.0.zip con la carpeta "VST2 Plugins"
```

## Con el FM1 Sampler

[FM1-AKAI-Sampler-MPC-ONE](https://github.com/Koolkatze/FM1-AKAI-Sampler-MPC-ONE) es el FM1 Sampler portado a plugin
para la misma MPC. Lee los loops que exporta el Looper (`LooperX/Loops`) y los WAV de `LooperX/Samples`,
así que lo que grabas con LooperX se puede tocar con los modos del FM1 (slicer, granular, Paulstretch…).

## Licencia

MIT (ver [`LICENSE`](LICENSE)). "VST" es una marca de Steinberg Media Technologies GmbH; el SDK no contiene código del VST SDK.
