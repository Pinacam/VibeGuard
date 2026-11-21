"""
Generates the three VibeGuard mood tracks from scratch (no samples, no
copyrighted material) and saves them as MP3 files.

    python make_music.py

Creates, in this folder:
    0001_calm.mp3      played when the user is mad  (slow, soft, no drums)
    0002_cheerful.mp3  played when the user is sad  (bouncy, major key)
    0003_upbeat.mp3    played when the user is happy (dance beat)

Copy them to the DFPlayer's microSD card as mp3/0001.mp3, 0002.mp3, 0003.mp3.
Needs:  pip install numpy lameenc
"""

from pathlib import Path

import lameenc
import numpy as np

SR = 44100
HERE = Path(__file__).resolve().parent
rng = np.random.default_rng(7)


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12)


def env(n, attack, release):
    """Attack / release envelope in seconds over n samples."""
    e = np.ones(n)
    a = min(n, int(attack * SR))
    r = min(n - a, int(release * SR))
    if a:
        e[:a] = np.linspace(0, 1, a)
    if r:
        e[n - r:] = np.linspace(1, 0, r)
    return e


def pad(midi, dur):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = hz(midi)
    w = sum(np.sin(2 * np.pi * f * d * t) for d in (0.997, 1.0, 1.003)) / 3
    w += 0.25 * np.sin(2 * np.pi * 2 * f * t)
    return w * env(n, dur * 0.4, dur * 0.4)


def pluck(midi, dur, bright=1.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = hz(midi)
    w = (np.sin(2 * np.pi * f * t) + 0.4 * bright * np.sin(2 * np.pi * 2 * f * t)
         + 0.15 * bright * np.sin(2 * np.pi * 3 * f * t))
    return w * np.exp(-t * (4.0 / max(dur, 0.05))) * env(n, 0.004, 0.02)


def bass(midi, dur):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = hz(midi)
    w = np.sin(2 * np.pi * f * t) + 0.2 * np.sign(np.sin(2 * np.pi * f * t))
    return w * np.exp(-t * 2.5) * env(n, 0.005, 0.03)


def kick():
    n = int(0.35 * SR)
    t = np.arange(n) / SR
    phase = 2 * np.pi * np.cumsum(45 + 90 * np.exp(-t * 35)) / SR
    return np.sin(phase) * np.exp(-t * 9)


def hat():
    n = int(0.05 * SR)
    x = rng.standard_normal(n)
    x = np.diff(x, prepend=0)          # remove lows: crisp
    return 0.25 * x * np.exp(-np.arange(n) / SR * 70)


def clap():
    n = int(0.18 * SR)
    x = rng.standard_normal(n)
    return 0.5 * x * np.exp(-np.arange(n) / SR * 22)


class Song:
    def __init__(self, bpm, bars):
        self.beat = 60.0 / bpm
        self.buf = np.zeros(int((bars * 4 * self.beat + 3) * SR))

    def add(self, sound, at_beat, gain):
        i = int(at_beat * self.beat * SR)
        end = min(len(self.buf), i + len(sound))
        self.buf[i:end] += gain * sound[: end - i]

    def finish(self, path, reverb=0.25):
        x = self.buf
        wet = np.zeros_like(x)
        for delay, g in ((0.043, 0.6), (0.071, 0.5), (0.113, 0.4), (0.167, 0.3)):
            d = int(delay * SR)
            wet[d:] += g * x[:-d]
        x = x + reverb * wet
        # gentle low-pass to soften harsh edges
        y = np.empty_like(x)
        acc = 0.0
        for i in range(0, len(x), 4096):
            seg = x[i:i + 4096]
            out = np.empty_like(seg)
            for j, v in enumerate(seg):
                acc += 0.55 * (v - acc)
                out[j] = acc
            y[i:i + 4096] = out
        y *= env(len(y), 1.0, 2.5)
        y = 0.89 * y / np.max(np.abs(y))
        pcm = (y * 32767).astype(np.int16)
        enc = lameenc.Encoder()
        enc.set_bit_rate(128)
        enc.set_in_sample_rate(SR)
        enc.set_channels(1)
        enc.set_quality(2)
        data = enc.encode(pcm.tobytes()) + enc.flush()
        path.write_bytes(data)
        print(f"{path.name}: {len(y) / SR:.0f} s, {len(data) / 1024:.0f} KB")


def chord_tones(root, quality):
    third = 3 if quality == "m" else 4
    return [root, root + third, root + 7, root + 12]


def calm():
    """Mad -> calm down. 64 BPM, soft pads and a slow arpeggio, no drums."""
    s = Song(64, 16)
    prog = [(53, "M"), (48, "M"), (57, "m"), (55, "M")]        # F  C  Am G
    for bar in range(16):
        root, q = prog[bar % 4]
        tones = chord_tones(root, q)
        for m in tones[:3]:
            s.add(pad(m, 4 * s.beat + 1.5), bar * 4, 0.16)
        s.add(pad(root - 12, 4 * s.beat + 1.5), bar * 4, 0.14)
        for k, idx in enumerate([0, 1, 2, 3, 2, 1, 2, 1]):
            s.add(pluck(tones[idx] + 12, 1.6, 0.4), bar * 4 + k * 0.5, 0.07)
    s.finish(HERE / "0001_calm.mp3", reverb=0.45)


def cheerful():
    """Sad -> cheer up. 112 BPM, bouncy major-key melody."""
    s = Song(112, 24)
    prog = [(55, "M"), (50, "M"), (52, "m"), (48, "M")]        # G  D  Em C
    motif = [0, 2, 3, 2, 1, None, 2, 1]
    for bar in range(24):
        root, q = prog[bar % 4]
        tones = chord_tones(root, q)
        b = bar * 4
        for k in range(4):                                    # bass bounce
            s.add(bass(root - 12 + (12 if k % 2 else 0), s.beat * 0.9), b + k, 0.35)
        for beat in (1, 3):                                   # chord stabs
            for m in tones[:3]:
                s.add(pluck(m, 0.35, 0.8), b + beat, 0.09)
        if bar >= 4:                                          # melody
            for k, idx in enumerate(motif):
                if idx is not None:
                    up = 12 if (bar // 4) % 2 else 0
                    s.add(pluck(tones[idx] + 12 + up, 0.3, 1.0), b + k * 0.5, 0.16)
        for k in (0, 2):
            s.add(kick(), b + k, 0.5)
        for k in (1, 3):
            s.add(clap(), b + k, 0.25)
        for k in range(8):
            s.add(hat(), b + k * 0.5, 0.6)
    s.finish(HERE / "0002_cheerful.mp3", reverb=0.2)


def upbeat():
    """Happy -> match the vibe. 124 BPM dance beat."""
    s = Song(124, 32)
    prog = [(57, "M"), (54, "m"), (50, "M"), (52, "M")]        # A  F#m D  E
    motif = [3, None, 2, 3, None, 1, 2, 0]
    for bar in range(32):
        root, q = prog[bar % 4]
        tones = chord_tones(root, q)
        b = bar * 4
        for k in range(4):
            s.add(kick(), b + k, 0.6)                         # four on the floor
            s.add(bass(root - 12, s.beat * 0.45), b + k + 0.5, 0.4)
            s.add(hat(), b + k + 0.5, 0.9)
        for k in (1, 3):
            s.add(clap(), b + k, 0.3)
        for k in (0.5, 1.5, 2.5, 3.5):                        # offbeat stabs
            for m in tones[:3]:
                s.add(pluck(m + 12, 0.18, 0.9), b + k, 0.07)
        if bar >= 8 and bar % 16 < 12:                        # lead
            for k, idx in enumerate(motif):
                if idx is not None:
                    s.add(pluck(tones[idx] + 12, 0.25, 1.0), b + k * 0.5, 0.15)
    s.finish(HERE / "0003_upbeat.mp3", reverb=0.15)


if __name__ == "__main__":
    calm()
    cheerful()
    upbeat()
