# SPDX-License-Identifier: LGPL-2.1-or-later
"""Optional independent Wave64 checks using FFmpeg and ffprobe."""
import json
import pathlib
import subprocess
import tempfile

binary = str(pathlib.Path(__file__).resolve().parents[1] / "wavegain")


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


def verify(path, codec, channels):
    info = json.loads(run([
        "ffprobe", "-v", "error", "-show_entries",
        "format=format_name:stream=codec_name,channels,sample_rate,duration",
        "-of", "json", str(path)
    ]).stdout)
    stream = info["streams"][0]
    assert info["format"]["format_name"] == "w64"
    assert stream["codec_name"] == codec, stream
    assert stream["channels"] == channels
    assert stream["sample_rate"] == "44100"
    assert abs(float(stream["duration"]) - 44096 / 44100) < 0.000001
    decoded = run(["ffmpeg", "-v", "error", "-i", str(path),
                   "-f", "f32le", "-c:a", "pcm_f32le", "-"]).stdout
    assert len(decoded) == 44096 * channels * 4


with tempfile.TemporaryDirectory(prefix="wavegain-w64-interop-") as temp:
    folder = pathlib.Path(temp)
    codecs = ["pcm_u8", "pcm_s16le", "pcm_s24le", "pcm_s32le", "pcm_f32le"]
    for channels in [1, 2]:
        for codec in codecs:
            source = folder / "source.w64"
            run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                 "sine=frequency=440:duration=1", "-af", "atrim=end_sample=44096", "-ac", str(channels),
                 "-c:a", codec, "-f", "w64", str(source)])
            original = source.read_bytes()
            run([binary, "-y", "-w", str(source)])
            verify(source, codec, channels)
            run([binary, "--undo-gain", str(source)])
            verify(source, codec, channels)
            for bits, target_codec in enumerate(codecs, 1):
                source.write_bytes(original)
                run([binary, "-y", "-b", str(bits), str(source)])
                verify(source, target_codec, channels)
            # stdout must also contain accurate sizes and readable gain metadata.
            source.write_bytes(original)
            output = folder / "stdout.w64"
            output.write_bytes(run([binary, "-y", "-w", "-o", str(source)]).stdout)
            verify(output, codec, channels)
            assert source.read_bytes() == original
            # The format detector must also work on a pipe rather than a file.
            run([binary, "-c", "-"], input=original)
    print("FFmpeg Wave64 interoperability passed: mono/stereo, 5 sample formats, conversions, gain/undo, stdout/stdin")
