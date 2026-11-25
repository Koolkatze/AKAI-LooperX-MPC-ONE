#!/usr/bin/env python3
"""Genera los sonidos de fábrica de LooperX Granular (Content/Samples/*.wav).

Todo es síntesis propia (sin grabaciones de terceros): se pueden distribuir libremente con el
plugin (MIT). Mono, 16 bits, 44,1 kHz, ~7 s cada uno para gastar poca RAM en el MPC.
Uso: python3 plugins/LooperXGranular/tools/make_samples.py
"""
import os
import wave

import numpy as np
from scipy.signal import butter, lfilter

SR = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Content", "Samples")
rng = np.random.default_rng(7)


def t_axis(sec):
    return np.arange(int(sec * SR)) / SR


def fade(x, a=0.05, r=0.4):
    n = len(x)
    env = np.ones(n)
    na, nr = int(a * SR), int(r * SR)
    env[:na] = np.linspace(0, 1, na)
    env[-nr:] = np.linspace(1, 0, nr)
    return x * env


def norm(x, peak=0.89):
    return x * (peak / max(1e-9, np.max(np.abs(x))))


def write(name, x):
    os.makedirs(OUT, exist_ok=True)
    x = np.clip(norm(x), -1, 1)
    data = (x * 32767).astype("<i2").tobytes()
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(data)
    print("  %-22s %.1f s" % (name + ".wav", len(x) / SR))


def saw(f, t, phase=0.0):
    return 2 * ((f * t + phase) % 1.0) - 1


def midi(n):
    return 440.0 * 2 ** ((n - 69) / 12)


def glass_pad():
    t = t_axis(8)
    x = np.zeros_like(t)
    for n in (48, 55, 60, 64, 67, 71):        # Cmaj9
        for d in (-0.08, 0.0, 0.07):
            x += saw(midi(n) * 2 ** (d / 12), t, rng.random())
    b, a = butter(2, 1800 / (SR / 2))
    x = lfilter(b, a, x) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.15 * t))
    return fade(x, 1.2, 1.5)


def vox_formant():
    t = t_axis(7)
    f0 = midi(57) * (1 + 0.006 * np.sin(2 * np.pi * 5.2 * t))
    src = saw(f0 * 0 + 1, np.cumsum(f0) / SR)  # pulso glotal (diente de sierra por fase acumulada)
    vowels = [(800, 1150, 2900), (400, 1600, 2700), (350, 600, 2700), (270, 2140, 2950), (500, 700, 2500)]  # a e o i u
    seg = len(t) // len(vowels)
    out = np.zeros_like(t)
    for k, (f1, f2, f3) in enumerate(vowels):
        s = src[k * seg:(k + 1) * seg + 2000]
        y = np.zeros_like(s)
        for f, g in ((f1, 1.0), (f2, 0.5), (f3, 0.25)):
            b, a = butter(2, [f * 0.9 / (SR / 2), f * 1.1 / (SR / 2)], btype="band")
            y += g * lfilter(b, a, s)
        win = np.hanning(len(y))
        end = min(len(out), k * seg + len(y))
        out[k * seg:end] += (y * win)[:end - k * seg]
    return fade(out, 0.3, 0.6)


def kalimba_run():
    t = t_axis(7)
    x = np.zeros_like(t)
    notes = [60, 64, 67, 72, 71, 67, 64, 62, 65, 69, 72, 76, 74, 69, 65, 60]
    step = 0.42
    for i, n in enumerate(notes):
        st = int(i * step * SR)
        tt = t[: len(t) - st]
        f = midi(n)
        env = np.exp(-tt * 3.5)
        y = np.sin(2 * np.pi * f * tt + 1.8 * np.exp(-tt * 9) * np.sin(2 * np.pi * f * 3.01 * tt)) * env
        x[st:] += y * (0.8 + 0.2 * rng.random())
    return fade(x, 0.005, 0.5)


def wind_noise():
    t = t_axis(8)
    n = rng.standard_normal(len(t))
    out = np.zeros_like(t)
    blk = 2048
    zi = None
    for i in range(0, len(t), blk):
        fc = 300 + 2500 * (0.5 + 0.5 * np.sin(2 * np.pi * 0.11 * t[i] + 0.7))
        b, a = butter(2, [fc * 0.7 / (SR / 2), fc * 1.3 / (SR / 2)], btype="band")
        if zi is None or len(zi) != max(len(a), len(b)) - 1:
            zi = np.zeros(max(len(a), len(b)) - 1)
        out[i:i + blk], zi = lfilter(b, a, n[i:i + blk], zi=zi)
    return fade(out * (0.7 + 0.3 * np.sin(2 * np.pi * 0.31 * t)), 1.0, 1.5)


def bell_cloud():
    t = t_axis(8)
    x = np.zeros_like(t)
    ratios = (1.0, 2.76, 5.40, 8.93, 13.34)
    for k in range(9):
        st = int(rng.uniform(0, 5.5) * SR)
        f = midi(rng.choice([67, 72, 74, 79, 84]))
        tt = t[: len(t) - st]
        y = sum(np.sin(2 * np.pi * f * r * tt) * np.exp(-tt * (1.2 + r * 0.6)) / (1 + r) for r in ratios)
        x[st:] += y
    return fade(x, 0.01, 1.0)


def tape_choir():
    t = t_axis(8)
    x = np.zeros_like(t)
    chord = (53, 57, 60, 65, 69)
    for n in chord:
        for v in range(3):
            wob = 1 + 0.004 * np.sin(2 * np.pi * (0.4 + 0.1 * v) * t + rng.random() * 6)
            ph = np.cumsum(midi(n) * wob) / SR
            for h in range(1, 9):
                x += np.sin(2 * np.pi * h * ph) * (1.0 / h ** 1.6) * (1 + 0.5 * np.sin(2 * np.pi * 0.2 * h * t))
    b, a = butter(2, 3000 / (SR / 2))
    x = lfilter(b, a, x)
    return fade(x, 1.5, 1.5)


def drum_loop():
    """2 compases a 120 BPM (4 s): bombo, caja y charles sintetizados."""
    sec = 4.0
    t = t_axis(sec)
    x = np.zeros_like(t)
    beat = 0.5
    for i in range(16):                       # corcheas
        st = int(i * beat / 2 * SR)
        tt = t[: len(t) - st]
        if i % 4 == 0 or i == 10:             # bombo
            f = 50 + 90 * np.exp(-tt * 30)
            x[st:] += 1.0 * np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-tt * 7)
        if i % 8 == 4:                        # caja
            x[st:] += 0.5 * np.sin(2 * np.pi * 190 * tt) * np.exp(-tt * 18)
            x[st:] += 0.45 * rng.standard_normal(len(tt)) * np.exp(-tt * 14)
        hh = rng.standard_normal(len(tt)) * np.exp(-tt * (60 if i % 2 else 35))
        b, a = butter(2, 7000 / (SR / 2), btype="high")
        x[st:] += 0.22 * lfilter(b, a, hh)
    return x


if __name__ == "__main__":
    print("LooperX Granular: sonidos de fábrica ->", os.path.normpath(OUT))
    write("01 Glass Pad", glass_pad())
    write("02 Vox Formant", vox_formant())
    write("03 Kalimba Run", kalimba_run())
    write("04 Wind Noise", wind_noise())
    write("05 Bell Cloud", bell_cloud())
    write("06 Tape Choir", tape_choir())
    write("07 Drum Loop 120", drum_loop())
