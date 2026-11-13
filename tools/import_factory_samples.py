#!/usr/bin/env python3
"""Rebuild Sculpt Pad's recorded string bank from pinned VSCO 2 CE sources.

Requires Python 3, NumPy and SciPy. The plugin build uses the prepared files and
does not need Python or a network request for these samples. No loop crossfade is
baked into the WAV: PadEngine performs the overlap at runtime.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import urllib.parse
import urllib.request

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfilt


def rms(audio: np.ndarray) -> float:
    return float(np.sqrt(np.mean(audio * audio)))


def choose_loop(audio: np.ndarray, rate: int, crossfade: int) -> tuple[int, int]:
    """Match sustained energy across a long overlap; retain recorded bow attack."""
    best: tuple[float, int, int] | None = None
    for start_sec in np.arange(1.8, 2.701, 0.05):
        start = round(float(start_sec) * rate)
        head = audio[start:start + crossfade]
        head_rms = rms(head)
        for end_sec in np.arange(5.0, 6.101, 0.05):
            end = round(float(end_sec) * rate)
            if end > len(audio) or end - start < 3 * crossfade:
                continue
            tail = audio[end-crossfade:end]
            tail_rms = rms(tail)
            mismatch = abs(np.log((head_rms + 1e-12) / (tail_rms + 1e-12)))
            # Prefer sustained, similar energy and at least 3 seconds of material.
            body_rms = rms(audio[start:end])
            edge_ratio = abs(np.log(((head_rms + tail_rms) * 0.5 + 1e-12)
                                    / (body_rms + 1e-12)))
            duration_penalty = max(0.0, 3.0 - (end-start)/rate) * 0.5
            score = mismatch + 0.4 * edge_ratio + duration_penalty
            candidate = (float(score), start, end)
            if best is None or candidate[0] < best[0]:
                best = candidate
    if best is None:
        raise ValueError("No valid sustained loop found")
    return best[1], best[2]


def tuning_estimate(audio: np.ndarray, rate: int, root_note: int) -> float:
    """Harmonic energy centroid, a broad tuning check (not pitch correction)."""
    signal = audio[2*rate:min(6*rate, len(audio))].mean(axis=1)
    n = 2 ** int(np.ceil(np.log2(len(signal) * 2)))
    spectrum = np.abs(np.fft.rfft(signal * np.hanning(len(signal)), n)) ** 2
    freq = np.fft.rfftfreq(n, 1/rate)
    fundamental = 440 * 2 ** ((root_note-69)/12)
    cents, weights = [], []
    for harmonic in range(1, min(7, int(9000/fundamental))):
        expected = fundamental * harmonic
        mask = (freq > expected * 2 ** (-35/1200)) & (freq < expected * 2 ** (35/1200))
        power = float(spectrum[mask].sum())
        if power < 1e-20:
            continue
        measured = float(np.sum(freq[mask] * spectrum[mask]) / power)
        cents.append(1200 * np.log2(measured/expected))
        weights.append(power)
    return float(np.average(cents, weights=weights))


def main() -> None:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=project / ".sample-cache" / "VSCO2")
    parser.add_argument("--output-dir", type=Path, default=project / "Samples")
    args = parser.parse_args()
    config = json.loads((project / "tools/factory_sample_sources.json").read_text())
    args.cache_dir.mkdir(parents=True, exist_ok=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    rate = int(config["sampleRate"])
    crossfade = round(float(config["loopCrossfadeSeconds"]) * rate)
    base = f'https://raw.githubusercontent.com/sgossner/VSCO-2-CE/{config["commit"]}/'
    entries, provenance = [], []
    # Gentle conditioning removes subsonic drift and ultrasonic bow noise.
    filters = np.concatenate((butter(2, 20, "highpass", fs=rate, output="sos"),
                              butter(4, 9500, "lowpass", fs=rate, output="sos")))
    for item in config["files"]:
        path = args.cache_dir / Path(item["path"]).name
        if not path.exists():
            path.write_bytes(urllib.request.urlopen(base + urllib.parse.quote(item["path"]),
                                                   timeout=60).read())
        original_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        if original_hash != item["sha256"]:
            raise ValueError(f"Source hash mismatch: {path.name}")
        original_rate, raw = wavfile.read(path)
        if original_rate != rate or raw.ndim != 2 or raw.shape[1] != 2:
            raise ValueError(f"Expected {rate} Hz stereo: {path.name}")
        if not np.issubdtype(raw.dtype, np.integer):
            raise ValueError(f"Unexpected source format: {path.name}")
        audio = raw.astype(np.float64) / 2 ** (np.iinfo(raw.dtype).bits - 1)
        estimated_cents = tuning_estimate(audio, rate, item["rootNote"])
        audio = sosfilt(filters, audio, axis=0)
        start, end = choose_loop(audio, rate, crossfade)
        audio = audio[:end].copy()
        source_rms = rms(audio[start:end])
        gain = min(float(config["targetSustainRms"]) / source_rms,
                   float(config["peakCeiling"]) / float(np.max(np.abs(audio))))
        audio *= gain
        # Tiny leading fade protects any non-zero first sample; bow attack stays.
        fade = min(round(rate * 0.003), len(audio))
        audio[:fade] *= np.linspace(0.0, 1.0, fade)[:, None]
        random = np.random.default_rng(20261113 + int(item["rootNote"]))
        dither = random.random(audio.shape) - random.random(audio.shape)
        pcm = np.clip(np.rint(audio * 32768 + dither), -32768, 32767).astype(np.int16)
        filename = f'pad_strings_{item["rootNote"]:03d}_rr0.wav'
        destination = args.output_dir / filename
        wavfile.write(destination, rate, pcm)
        decoded = pcm.astype(np.float64)/32768.0
        entry = dict(file=filename, rootNote=item["rootNote"], roundRobin=0,
                     loopStart=start, loopEnd=end, loopCrossfade=crossfade)
        entries.append(entry)
        head, tail = decoded[start:start+crossfade], decoded[end-crossfade:end]
        u = np.linspace(0, 1, crossfade, endpoint=False)[:, None]
        overlap = tail*np.cos(u*np.pi/2) + head*np.sin(u*np.pi/2)
        record = dict(**entry, instrument=item["instrument"], sourcePath=item["path"],
                      sourceUrl=base+urllib.parse.quote(item["path"]),
                      originalSha256=original_hash,
                      preparedSha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
                      durationSeconds=end/rate, importedSustainRms=rms(decoded[start:end]),
                      importedPeak=float(np.max(np.abs(decoded))),
                      previewCrossfadePeak=float(np.max(np.abs(overlap))),
                      gainDb=float(20*np.log10(gain)),
                      estimatedSourceTuningCents=estimated_cents,
                      loopHeadTailRmsDifferenceDb=float(20*np.log10(rms(head)/rms(tail))))
        provenance.append(record)
        print(f'{filename}: RMS {record["importedSustainRms"]:.6f}, '
              f'peak {record["importedPeak"]:.6f}, loop {start}:{end}:{crossfade}')
    manifest = dict(schemaVersion=1, sampleRate=rate, recordedTakesPerRoot=1,
                    modelledRoundRobinCount=4, sampleFiles=entries)
    info = dict(instrument="Sculpt Pad", sourceLibrary="VSCO 2 Community Edition",
                repository=config["repository"], sourceCommit=config["commit"],
                authors="Versilian Studios; recordings by Sam Gossner and Simon Dalzell; sample cutting by Elan Hickler / Soundemote",
                license="CC0-1.0", officialSource="https://versilian-studios.com/vsco-community/",
                recordedTakesPerRoot=1, modelledRoundRobinCount=4,
                processing=["Selected soft cello and violin section sustains",
                            "Original filename octaves mapped to verified concert MIDI (+12)",
                            "20 Hz high-pass and 9.5 kHz low-pass",
                            "Sustain RMS matching with 0.8 peak ceiling; no pitch correction",
                            "Retained recorded attack, trimmed after selected loop end",
                            "44.1 kHz stereo PCM16 with deterministic TPDF dither",
                            "Loop overlap is performed by PadEngine, not baked into WAV"],
                loopContract="At loopEnd-loopCrossfade overlap tail with loopStart using equal-power gains. At exclusive loopEnd wrap to loopStart+loopCrossfade.",
                samples=provenance)
    (args.output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    (args.output_dir / "source-info.json").write_text(json.dumps(info, indent=2)+"\n")
    (args.output_dir / "preview-bank.tsv").write_text("".join(
        "\t".join(str(e[k]) for k in ("file", "rootNote", "roundRobin", "loopStart",
                                      "loopEnd", "loopCrossfade"))+"\n" for e in entries))


if __name__ == "__main__":
    main()
