"""Independent FFmpeg decoding and mono-energy checks for surround output."""
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from audio_fixtures import GUID, wav, w64
from test_support import parse_binary, run_cli

BINARY = parse_binary(__doc__)


def command(*args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, timeout=30)


def levels(path, channels):
    result = []
    for ch in range(channels):
        text = command('ffmpeg', '-hide_banner', '-nostats', '-i', path, '-af',
                       f'pan=mono|c0=c{ch},apad=pad_len=128,ebur128=peak=true',
                       '-f', 'null', '-').stderr.decode().rsplit('Summary:', 1)[1]
        level = float(re.search(r'I:\s*(\S+) LUFS', text)[1])
        peak = float(re.search(r'Peak:\s*(\S+) dBFS', text)[1])
        result.append((level, peak))
    return result


with tempfile.TemporaryDirectory(prefix='wavegain-surround-interop-') as tmp:
    folder = Path(tmp)
    for channels, mask, layout in ((6, 0x3f, '5.1'), (6, 0x60f, '5.1(side)'), (8, 0x63f, '7.1')):
        rate, frames = 48000, 96037
        pcm = bytearray()
        for i in range(frames):
            edge = min(1., i / 1000., (frames - 1 - i) / 1000.)
            for ch in range(channels):
                value = edge * (.05 + ch * .008) * math.sin(2 * math.pi * (300 + ch * 150) * i / rate)
                pcm += struct.pack('<f', value)
        fmt = struct.pack('<HHIIHHHHI', 0xfffe, channels, rate, rate * channels * 4,
                          channels * 4, 32, 22, 32, mask) + b'\x03' + GUID[1:]
        source = folder / f'source-{mask}.wav'
        source.write_bytes(wav(pcm, fmt))
        per_channel = levels(source, channels)
        # Use independently measured mono energies with normative weights.
        # Some FFmpeg versions weight 7.1 back speakers like 5.1 surrounds;
        # their direct 7.1 reading cannot be the normative reference here.
        energy = 0.
        for ch, (level, _) in enumerate(per_channel):
            if ch == 3:
                continue
            weight = 1. if ch < 3 or (channels == 8 and ch < 6) else 1.41
            energy += weight * 10 ** (level / 10)
        expected = 10 * math.log10(energy)
        measured = run_cli(BINARY, ['--lufs', '--true-peak', '-p', source], text=True).stderr
        actual = float(re.search(r'Integrated loudness:\s*(\S+) LUFS', measured)[1])
        peak = float(re.search(r'Input true peak after DC correction:\s*(\S+) dBTP', measured)[1])
        assert abs(actual - expected) <= .12, (layout, actual, expected)
        assert abs(peak - max(p for _, p in per_channel)) <= .15
        for extension in ('wav', 'w64'):
            if extension == 'w64':
                source = folder / f'source-{mask}.w64'
                source.write_bytes(w64(pcm, format_bytes=fmt))
            target = folder / f'output-{mask}.{extension}'
            run_cli(BINARY, ['-y', '-p', '--target-lufs', '-23', '--true-peak', '-b', '3',
                             '--output', target, source])
            stream = json.loads(command('ffprobe', '-v', 'error', '-show_entries',
                                        'stream=channels,channel_layout,sample_rate', '-of', 'json', target).stdout)['streams'][0]
            assert stream['channels'] == channels and stream['channel_layout'] == layout, stream
            assert int(stream['sample_rate']) == rate
            decoded = command('ffmpeg', '-v', 'error', '-i', target, '-f', 'f32le', '-').stdout
            assert len(decoded) == frames * channels * 4
            values = struct.unpack('<' + 'f' * (len(decoded) // 4), decoded)
            # Decode with FFmpeg, then verify the same scale and order on every speaker.
            original = struct.unpack('<' + 'f' * (len(pcm) // 4), pcm)
            index = max(range(len(original)), key=lambda i: abs(original[i]))
            factor = values[index] / original[index]
            assert max(abs(a - b * factor) for a, b in zip(values, original)) < 4 / (1 << 23)
    print('FFmpeg surround layout, decoding, per-channel energy and peak interop passed')
