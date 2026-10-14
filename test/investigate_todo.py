# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate evidence for the remaining TODOs without changing real audio files.

Run after building: python3 test/investigate_todo.py --output /tmp/todo.json
This is a diagnostic probe, not a passing regression test: it records current
behavior so later fixes can be compared using --binary.
"""
import argparse
import json
import math
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
PROGRESS = re.compile(r"This file\s+(\S+)% done\s*(?:All files\s+(\S+)% done)?")
GAIN = re.compile(r"([+-]?[\d.]+) dB\s*\|\s*([\d.]+)\s*\|\s*([\d.]+)\s*\|\s*([\d.]+)")


def write_fixture(path, rate, seconds, dc_pulses=False):
    data = bytearray()
    for i in range(round(rate * seconds)):
        if dc_pulses:
            value = -0.55 if i % (rate // 2) == 0 else 0.3 + 0.01 * math.sin(2 * math.pi * 440 * i / rate)
        else:
            value = 0.2 * math.sin(2 * math.pi * 440 * i / rate)
        pcm = round(32767 * value)
        data.extend(struct.pack("<hh", pcm, pcm))
    with wave.open(str(path), "wb") as out:
        out.setparams((2, 2, rate, 0, "NONE", "PCM"))
        out.writeframes(data)


def run(binary, args):
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result


def progress_summary(stderr):
    rows = PROGRESS.findall(stderr)
    def summarize(column):
        values = [row[column] for row in rows if row[column]]
        finite = [float(value) for value in values if math.isfinite(float(value))]
        return {
            "last": values[-1] if values else None,
            "max_finite": max(finite) if finite else None,
            "nonfinite": sorted(set(value for value in values if not math.isfinite(float(value)))),
        }
    return {"file": summarize(0), "all_files": summarize(1), "reports": len(rows)}


def waveform_summary(path):
    with wave.open(str(path), "rb") as source:
        count = source.getnframes() * source.getnchannels()
        samples = struct.unpack("<" + "h" * count, source.readframes(source.getnframes()))
    return {"min": min(samples), "max": max(samples),
            "rail_samples": sum(value in (-32768, 32767) for value in samples)}


def gain_summary(stderr):
    match = GAIN.search(stderr)
    if not match:
        raise RuntimeError("No gain recommendation found: " + stderr)
    return dict(zip(("gain_db", "input_peak", "displayed_scale", "displayed_new_peak"),
                    map(float, match.groups())))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "wavegain")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    report = {"platform": platform.system(), "architecture": platform.machine(),
              "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "note": "Generated fixtures only; rounded display values are not exact internal scales.",
              "progress": {}, "gain_dc": {}}
    with tempfile.TemporaryDirectory(prefix="wavegain-todo-") as temp:
        folder = Path(temp)
        for seconds in (0.5, 4.5):
            path = folder / "sine.wav"
            write_fixture(path, 44100, seconds)
            result = run(binary, ["-y", path])
            report["progress"][f"{seconds}s_44100Hz"] = progress_summary(result.stderr)
        paths = [folder / "8k.wav", folder / "48k.wav"]
        for path, rate in zip(paths, (8000, 48000)):
            write_fixture(path, rate, 4.5)
        report["progress"]["mixed_8000_48000Hz_4.5s_each"] = progress_summary(run(binary, ["-y", *paths]).stderr)
        path = folder / "dc.wav"
        write_fixture(path, 44100, 1, dc_pulses=True)
        original = path.read_bytes()
        with wave.open(str(path), "rb") as source:
            input_samples = struct.unpack("<" + "h" * 88200, source.readframes(44100))[::2]
        dc_mean = sum(input_samples) / len(input_samples)
        centered_peak = max(abs(value - dc_mean) for value in input_samples) / 32768
        raw_peak = max(map(abs, input_samples)) / 32768
        report["gain_dc"]["fixture"] = {
            "rate": 44100, "frames": 44100, "channels": 2,
            "sample_bits": 16, "baseline": 0.3, "sine_amplitude": 0.01,
            "negative_pulse": -0.55, "pulse_frames": [0, 22050],
            "mean_pcm": dc_mean, "raw_peak_normalized": raw_peak,
            "post_dc_peak_normalized": centered_peak,
        }
        for name, flags in [
            ("default", []), ("gain12", ["-g", "12"]),
            ("gain12_no_offset", ["-g", "12", "-p"]),
            ("gain12_noclip", ["-g", "12", "-n"]),
            ("gain12_limiter", ["-g", "12", "-t"]),
            ("gain12_noclip_limiter", ["-g", "12", "-n", "-t"]),
            ("album_gain12_adc", ["-a", "-q", "-g", "12"]),
        ]:
            path.write_bytes(original)
            result = run(binary, ["-y", *flags, path])
            recommendation = gain_summary(result.stderr)
            expected_peak = (raw_peak if "-p" in flags else centered_peak) * 10 ** (recommendation["gain_db"] / 20)
            report["gain_dc"][name] = {"flags": flags, "recommendation": recommendation,
                                       "pre_limiter_peak_estimate_from_displayed_gain": expected_peak,
                                       "output": waveform_summary(path)}
        # Manual gain is added once; without a cap a 6 dB request adds 6 dB.
        path = folder / "manual.wav"
        write_fixture(path, 44100, 1)
        a = gain_summary(run(binary, ["-c", "-n", "-p", path]).stderr)
        b = gain_summary(run(binary, ["-c", "-n", "-p", "-g", "6", path]).stderr)
        report["manual_gain_without_cap"] = {"base_db": a["gain_db"], "plus6_db": b["gain_db"],
                                               "delta_db": round(b["gain_db"] - a["gain_db"], 2)}
        # Pass a literal glob as one argv argument: no shell expansion occurs.
        glob = run(binary, ["-c", str(folder / "manual*.wav")])
        report["help_wildcards"] = {"literal_quoted_glob_finds_file": bool(GAIN.search(glob.stderr)),
                                    "copyright_is_before_usage": "Copyright" in run(binary, ["-h"]).stdout.split(" Usage:")[0],
                                    "WAVE64_in_help": "Sony Wave64" in run(binary, ["-h"]).stdout,
                                    "WAVE64_in_man": "Wave64" in (ROOT / "debian/wavegain.1").read_text(),
                                    "WAVE64_in_html": "Wave64" in (ROOT / "doc/manual/index.html").read_text()}
    output = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(output)
    else:
        print(output, end="")


if __name__ == "__main__":
    main()
