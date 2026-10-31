#!/usr/bin/env python3
"""Render the official site's Web Audio pad preview as chromatic WAV loops.

Requires numpy and scipy. The audio graph mirrors public/app.js: six detuned
sine/triangle voices, the 650 Hz low-pass with 280 Hz LFO, and a stereo noise
convolution reverb. The note frequencies are quantized by less than 0.01 Hz so
oscillators land on a seamless 50-second loop boundary.
"""
from pathlib import Path
import wave

import numpy as np
from scipy.signal import fftconvolve, lfilter


ROOT = Path(__file__).resolve().parents[1]
PADS = ROOT / "Pads" / "Soft"
RATE = 44_100
LOOP_SECONDS = 50
FRAMES = round(RATE * LOOP_SECONDS / 128) * 128  # align period to Web Audio quanta
LOOP_DURATION = FRAMES / RATE
RENDER_FRAMES = FRAMES * 2  # one full loop to settle the filter/reverb, one to save
NOTES = "C C# D D# E F F# G G# A A# B".split()
PARTIALS = ((1, "sine", 0), (2, "triangle", -7), (2, "triangle", 7),
            (3, "sine", 4), (4, "triangle", -5), (6, "sine", 6))
VOICE_GAINS = (0.7, 0.3, 0.3, 0.18, 0.12, 0.07)


def block_filter(signal: np.ndarray, note_index: int) -> np.ndarray:
    """Web Audio style RBJ low-pass, recalculated at each 128-frame render quantum."""
    output = np.empty_like(signal)
    state = np.zeros(2, dtype=np.float64)
    quantum = 128
    lfo_hz = round(0.12 * LOOP_DURATION) / LOOP_DURATION
    for start in range(0, signal.size, quantum):
        end = min(signal.size, start + quantum)
        time = start / RATE
        cutoff = 650.0 + 280.0 * np.sin(2 * np.pi * lfo_hz * time)
        w0 = 2 * np.pi * cutoff / RATE
        cosine = np.cos(w0)
        alpha = np.sin(w0) / (2 * 0.7)
        a0 = 1 + alpha
        b = np.array([(1 - cosine) / (2 * a0), (1 - cosine) / a0,
                      (1 - cosine) / (2 * a0)])
        a = np.array([1.0, -2 * cosine / a0, (1 - alpha) / a0])
        output[start:end], state = lfilter(b, a, signal[start:end], zi=state)
    return output


def impulse_response(channel: int) -> np.ndarray:
    rng = np.random.default_rng(0x535447 + channel)
    t = np.arange(RATE * 5, dtype=np.float64) / (RATE * 5)
    envelope = (1.0 - t) ** 3
    impulse = rng.uniform(-1, 1, RATE * 5) * envelope
    # ConvolverNode normalizes its impulse response. Keep the two channels
    # independent, as in the page's two-channel random convolver buffer.
    impulse /= max(np.sqrt(np.sum(impulse * impulse)), 1e-12)
    return impulse


def make_note(note_index: int) -> np.ndarray:
    time = np.arange(RENDER_FRAMES, dtype=np.float64) / RATE
    root = 65.41 * 2 ** (note_index / 12)
    signal = np.zeros(RENDER_FRAMES, dtype=np.float64)
    for (multiple, shape, detune), gain in zip(PARTIALS, VOICE_GAINS):
        frequency = root * multiple * 2 ** (detune / 1200)
        frequency = round(frequency * LOOP_DURATION) / LOOP_DURATION
        phase = 2 * np.pi * frequency * time
        if shape == "sine":
            voice = np.sin(phase)
        else:
            voice = (2 / np.pi) * np.arcsin(np.sin(phase))
        signal += voice * gain

    # Web Audio graph has a 0.5 pad gain, then parallel 0.35 dry and 1.1 wet
    # paths. Keep that mix, then normalize only to a safe sample peak for StageTracks.
    filtered = block_filter(signal * 0.5, note_index)
    dry = filtered * 0.35
    left = dry.copy()
    right = dry.copy()
    for ch, out in enumerate((left, right)):
        convolved = fftconvolve(filtered, impulse_response(ch), mode="full")[:RENDER_FRAMES]
        out += convolved * 1.1

    audio = np.column_stack((left[FRAMES:], right[FRAMES:]))
    # Leave 6 dB headroom, mirroring the site's default master output level.
    peak = float(np.max(np.abs(audio)))
    if peak > 0:
        audio *= 0.5 / peak
    return np.clip(audio, -1.0, 1.0)


def write_wave(path: Path, samples: np.ndarray) -> None:
    pcm = np.round(samples * 32767).astype("<i2")
    with wave.open(str(path), "wb") as dst:
        dst.setnchannels(2)
        dst.setsampwidth(2)
        dst.setframerate(RATE)
        dst.writeframes(pcm.tobytes())


def main() -> None:
    PADS.mkdir(exist_ok=True)
    for i, note in enumerate(NOTES):
        output = PADS / f"Pads Worship {note}.wav"
        write_wave(output, make_note(i))
        print(f"Rendered {output.name}")
if __name__ == "__main__":
    main()
