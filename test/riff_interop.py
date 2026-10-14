"""Optional RIFF/RF64 checks using FFmpeg: piped WAV, RF64 input and --rf64."""
import pathlib
import struct
import subprocess
import tempfile

binary = str(pathlib.Path(__file__).resolve().parents[1] / "wavegain")
FRAMES = 3 * 44100


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


def source(*options):
    return ["ffmpeg", "-v", "error", "-f", "lavfi",
            "-i", "sine=f=997:r=44100:d=3", "-ac", "2", *options]


def apply(path, *options):
    before = path.read_bytes()
    result = run([binary, "-y", *options, str(path)], text=True)
    assert "Error" not in result.stderr, result.stderr
    assert "Applying Gain" in result.stderr, result.stderr
    assert path.read_bytes() != before, "gain was not applied"


def frames(path):
    decoded = run(["ffmpeg", "-v", "error", "-i", str(path),
                   "-f", "s16le", "-c:a", "pcm_s16le", "-"]).stdout
    return len(decoded) // 4


def chunk_sizes(path):
    data = path.read_bytes()
    return data[:4], struct.unpack_from("<I", data, 4)[0], data[12:16], len(data)


with tempfile.TemporaryDirectory(prefix="wavegain-riff-") as temp:
    folder = pathlib.Path(temp)

    # A WAV written to a pipe keeps 0xFFFFFFFF sizes; it must still be processed.
    piped = folder / "piped.wav"
    piped.write_bytes(run(source("-f", "wav", "-")).stdout)
    assert chunk_sizes(piped)[1] == 0xFFFFFFFF
    apply(piped)
    tag, riff, _, length = chunk_sizes(piped)
    assert tag == b"RIFF" and riff == length - 8, (tag, riff, length)
    assert frames(piped) == FRAMES

    # RF64 input: processed, and written back as RIFF + JUNK while under 4 GiB.
    rf64 = folder / "rf64.wav"
    run(source("-rf64", "always", str(rf64)))
    assert chunk_sizes(rf64)[0] == b"RF64"
    apply(rf64)
    tag, riff, first, length = chunk_sizes(rf64)
    assert tag == b"RIFF" and first == b"JUNK" and riff == length - 8
    assert frames(rf64) == FRAMES

    # --rf64 on small output reserves JUNK but stays a plain RIFF/WAV file.
    plain = folder / "plain.wav"
    run(source(str(plain)))
    apply(plain, "--rf64")
    tag, riff, first, length = chunk_sizes(plain)
    assert tag == b"RIFF" and first == b"JUNK" and riff == length - 8
    assert frames(plain) == FRAMES

print("FFmpeg RIFF interoperability passed: piped 0xFFFFFFFF WAV, RF64 input, --rf64 under 4 GiB")
