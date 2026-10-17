"""Behavioral regressions for DC clipping, album peaks, progress and shared help."""
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from investigate_todo import GAIN, PROGRESS, run, write_fixture

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "wavegain"


def samples(path):
    data = path.read_bytes()
    pos = 12
    fmt = payload = None
    while pos + 8 <= len(data):
        tag, size = struct.unpack_from("<4sI", data, pos)
        if tag == b"fmt ": fmt = data[pos + 8:pos + 8 + size]
        if tag == b"data": payload = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    assert fmt and payload is not None
    kind, channels = struct.unpack_from("<HH", fmt)
    bits = struct.unpack_from("<H", fmt, 14)[0]
    if kind == 3:
        values = [v[0] for v in struct.iter_unpack("<f", payload)]
    elif bits == 8:
        values = [(v - 128) / 128 for v in payload]
    else:
        width = bits // 8
        values = [int.from_bytes(payload[i:i + width], "little", signed=True) / (1 << (bits - 1))
                  for i in range(0, len(payload), width)]
    return values, channels, bits


def check_progress(stderr):
    rows = PROGRESS.findall(stderr)
    assert rows, stderr
    for row in rows:
        for token in row:
            if token:
                assert math.isfinite(float(token)) and 0 <= float(token) <= 100, row
    assert rows[-1] == ("100", "100"), rows[-1]


with tempfile.TemporaryDirectory(prefix="wavegain-todo-regressions-") as temp:
    folder = Path(temp)
    for seconds in (0.5, 4.5):
        source = folder / "duration.wav"
        write_fixture(source, 44100, seconds)
        check_progress(run(BINARY, ["-y", source]).stderr)
    for rates in ((8000, 48000), (48000, 8000)):
        paths = [folder / "first.wav", folder / "second.wav"]
        for path, rate in zip(paths, rates): write_fixture(path, rate, 4.5)
        check_progress(run(BINARY, ["-y", *paths]).stderr)
    source = folder / "dc.wav"
    write_fixture(source, 44100, 1, dc_pulses=True)
    original = source.read_bytes()
    input_values, _, _ = samples(source)
    mean = sum(input_values) / len(input_values)
    for bits in range(1, 6):
        for flags in ([], ["-g", "12"], ["-p", "-g", "12"], ["-t", "-g", "12"], ["-d", "1"]):
            source.write_bytes(original)
            result = run(BINARY, ["-y", "-b", bits, *flags, source])
            check_progress(result.stderr)
            output, channels, width = samples(source)
            assert channels == 2 and len(output) == len(input_values)
            recommendation = GAIN.search(result.stderr)
            gain = 10 ** (float(recommendation.group(1)) / 20)
            correction = 0 if "-p" in flags else mean
            tolerance = (2 / 128 if width == 8 else 2 / (1 << (width - 1))) + 0.0006
            for before, after in zip(input_values, output):
                expected = (before - correction) * gain
                assert abs(expected) < 1.001  # cap was selected from corrected extrema
                if "-t" in flags and abs(expected) > 0.5:
                    expected = math.copysign(0.5 + 0.5 * math.tanh((abs(expected) - 0.5) / 0.5), expected)
                assert abs(after - expected) <= tolerance, (bits, flags, after, expected)
            if width >= 16 and "-p" not in flags and "-d" not in flags:
                assert min(output) > -1
    # A real two-track album: differing means change the common-DC peak.
    second = folder / "other.wav"
    source.write_bytes(original)
    import wave
    data = bytearray()
    for i in range(44100):
        value = .8 if i % 22050 == 0 else -.1 + .01 * math.sin(2 * math.pi * 440 * i / 44100)
        pcm = round(value * 32767)
        data.extend(struct.pack("<hh", pcm, pcm))
    with wave.open(str(second), "wb") as out:
        out.setparams((2, 2, 44100, 0, "NONE", "PCM")); out.writeframes(data)
    second_original = second.read_bytes()
    second_input = samples(second)[0]
    for flags in (["-a", "-q"], ["-a"], ["-q"], ["-a", "-p"]):
        source.write_bytes(original); second.write_bytes(second_original)
        result = run(BINARY, ["-y", "-g", "12", *flags, source, second])
        check_progress(result.stderr)
        for path in (source, second):
            values = samples(path)[0]
            assert min(values) > -1 and max(values) < 1, flags
        if "-a" in flags:
            # Album mode must apply one gain to both tracks even with distinct DC.
            applied = [float(line.split("dB")[0].split("of")[-1]) for line in result.stderr.splitlines()
                       if " Applying Gain of " in line]
            assert len(applied) == 2 and applied[0] == applied[1]
    # Already processed tracks are excluded from overall planned work.
    source.write_bytes(original); second.write_bytes(second_original)
    run(BINARY, ["-y", "-w", source])
    check_progress(run(BINARY, ["-y", source, second]).stderr)
    # Undo counts only tracks whose stored gain has not already been undone.
    run(BINARY, ["--undo-gain", source])
    second.write_bytes(second_original); run(BINARY, ["-y", "-w", second])
    check_progress(run(BINARY, ["--undo-gain", second, source]).stderr)
    # Empty WAV remains unchanged and never emits inf/NaN progress.
    empty = folder / "empty.wav"
    write_fixture(empty, 44100, 0)
    before = empty.read_bytes()
    result = subprocess.run([str(BINARY), "-y", str(empty)], capture_output=True, text=True)
    assert result.returncode == 1
    assert empty.read_bytes() == before and not PROGRESS.findall(result.stderr)
    # The limiter has documented semantics: its cap stays enabled unless -n.
    source.write_bytes(original)
    capped = float(GAIN.search(run(BINARY, ["-c", "-t", "-g", "12", source]).stderr).group(1))
    uncapped = float(GAIN.search(run(BINARY, ["-c", "-n", "-t", "-g", "12", source]).stderr).group(1))
    assert uncapped > capped
    # Platform-conditional generated help matches the native build exactly.
    definition = json.loads((ROOT / "doc/help.json").read_text())
    expected = "".join(block["text"] for block in definition["blocks"]
                       if block["condition"] != "_WIN32" or os.name == "nt")
    help_text = run(BINARY, ["-h"]).stdout.split("\n", 1)[1]
    assert help_text == expected
    assert help_text.index("Copyright") > help_text.index("INPUT FILES")
    assert "wavegain '*.wav'" in help_text and "BEFORE limiting" in help_text
print("TODO regressions passed: corrected PCM/float peaks, distinct album DC, progress/rates/skips/undo, empty files, shared help")
