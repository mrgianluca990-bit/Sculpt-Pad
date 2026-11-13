#!/usr/bin/env python3
"""Measure the actual 68-second core render without changing its audio."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import wave

import numpy as np
from scipy.signal import resample_poly


def db(value: float) -> float:
    return float(20 * np.log10(max(float(value), 1e-12)))


def rms(audio: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(audio))))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audio", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with wave.open(str(args.audio), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (2, 2, 44100):
            raise ValueError("Expected stereo PCM16, 44100 Hz")
        rate = wav.getframerate()
        audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").reshape(-1, 2).astype(float) / 32768
    if len(audio) != 68 * rate or not np.isfinite(audio).all():
        raise ValueError("Invalid render length or samples")
    metrics = {
        "audio": args.audio.name,
        "seconds": len(audio) / rate,
        "sampleRate": rate,
        "channels": 2,
        "peakDbFS": db(np.max(np.abs(audio))),
        "truePeak4xDbFS": db(np.max(np.abs(resample_poly(audio, 4, 1, axis=0)))),
        "fullScaleSamples": int(np.count_nonzero(np.abs(audio) >= 32767 / 32768)),
        "nonfiniteSamples": int(np.count_nonzero(~np.isfinite(audio))),
        "anchors": [],
    }
    for index, name in enumerate(["Archi", "Ensemble", "Analog", "Digital"]):
        section = audio[index * 12 * rate:(index + 1) * 12 * rate]
        active = section[rate:7 * rate]
        mono = np.mean(active, axis=1)
        window = np.hanning(len(active))[:, None]
        spectrum = np.sum(np.abs(np.fft.rfft(active * window, axis=0)) ** 2, axis=1)
        hz = np.fft.rfftfreq(len(active), 1 / rate)
        metrics["anchors"].append({
            "name": name,
            "morph": index / 3,
            "startSeconds": index * 12,
            "activeRmsDbFS": db(rms(active)),
            "peakDbFS": db(np.max(np.abs(section))),
            "monoLossDb": db(rms(mono)) - db(rms(active)),
            "stereoCorrelation": float(np.corrcoef(active.T)[0, 1]),
            "spectralCentroidHz": float(np.sum(hz * spectrum) / np.sum(spectrum)),
            "energyAbove2kHzPercent": float(100 * np.sum(spectrum[hz > 2000]) / np.sum(spectrum)),
            "dc": [float(x) for x in np.mean(active, axis=0)],
            "last200msPeakDbFS": db(np.max(np.abs(section[-8820:]))),
            "maximumAdjacentStep": float(np.max(np.abs(np.diff(section, axis=0)))),
        })
    levels = [section["activeRmsDbFS"] for section in metrics["anchors"]]
    metrics["anchorRmsSpreadDb"] = max(levels) - min(levels)
    sweep = audio[48 * rate:]
    held = sweep[rate:15 * rate]
    windows = [rms(held[i:i + rate // 2]) for i in range(0, len(held) - rate // 2 + 1, rate // 2)]
    metrics["heldMorph"] = {
        "startSeconds": 48,
        "morphStartSeconds": 50,
        "morphEndSeconds": 61,
        "pedalReleaseSeconds": 63,
        "minHalfSecondRmsDbFS": db(min(windows)),
        "maxHalfSecondRmsDbFS": db(max(windows)),
        "last200msPeakDbFS": db(np.max(np.abs(sweep[-8820:]))),
        "maximumAdjacentStep": float(np.max(np.abs(np.diff(sweep, axis=0)))),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(metrics, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(metrics, indent=2, ensure_ascii=False))
    if metrics["fullScaleSamples"] or metrics["truePeak4xDbFS"] >= 0:
        raise SystemExit("FAIL: full-scale/true-peak clipping")
    if metrics["anchorRmsSpreadDb"] > 2.0:
        raise SystemExit("FAIL: anchor loudness spread exceeds 2 dB on reference phrase")
    if metrics["heldMorph"]["last200msPeakDbFS"] > -75:
        raise SystemExit("FAIL: release did not settle")
    if metrics["heldMorph"]["minHalfSecondRmsDbFS"] < -50:
        raise SystemExit("FAIL: sustained morph contains a dropout")


if __name__ == "__main__":
    main()
