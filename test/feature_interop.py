"""Independent FFmpeg LUFS/true-peak checks for the new measurement modes."""
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'wavegain'


def command(args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True)


def meter(path):
    r = command(['ffmpeg', '-hide_banner', '-nostats', '-i', path, '-af', 'apad=pad_len=128,ebur128=peak=true', '-f', 'null', '-'])
    summary = r.stderr.rsplit('Summary:', 1)[1]
    loudness = float(re.search(r'I:\s*([+-]?[\d.]+) LUFS', summary).group(1))
    peak = float(re.search(r'Peak:\s*([+-]?[\d.]+) dBFS', summary).group(1))
    return loudness, peak


with tempfile.TemporaryDirectory(prefix='wavegain-feature-interop-') as tmp:
    folder = Path(tmp)
    for rate in (44100, 48000, 96000, 176400, 192000):
        for channels in (1, 2):
            source = folder / 'source.wav'
            n = rate * 2 + 37
            values = []
            for i in range(n):
                for ch in range(channels):
                    values.append(round(32767 * (.12 * math.sin(2 * math.pi * (1000 + 500 * ch) * i / rate)
                                                 + .025 * math.cos(2 * math.pi * 300 * i / rate))))
            with wave.open(str(source), 'wb') as f:
                f.setparams((channels, 2, rate, 0, 'NONE', 'PCM'))
                f.writeframes(struct.pack('<' + 'h' * len(values), *values))
            expected_lufs, expected_peak = meter(source)
            result = command([BIN, '-p', '--lufs', '--true-peak', source]).stderr
            lufs = float(re.search(r'Integrated loudness:\s*([+-]?[\d.]+)', result).group(1))
            peak = float(re.search(r'Input true peak after DC correction:\s*([+-]?[\d.]+)', result).group(1))
            assert abs(lufs - expected_lufs) <= .11, (rate, channels, lufs, expected_lufs)
            # FIR designs and oversampling rates differ, especially at file edges.
            assert abs(peak - expected_peak) <= .3, (rate, channels, peak, expected_peak)
            output = folder / f'output-{rate}-{channels}.wav'
            command([BIN, '-y', '-p', '--target-lufs', '-23', '--true-peak', '--output', output, source])
            output_lufs, output_peak = meter(output)
            assert abs(output_lufs + 23) <= .11, (rate, channels, output_lufs)
            assert output_peak <= -1., output_peak
            with wave.open(str(output)) as f:
                assert f.getframerate() == rate and f.getnframes() == n
    # Independent inter-sample overshoot and ceiling checks, including rate dither.
    source = folder / 'overshoot.wav'
    with wave.open(str(source), 'wb') as f:
        f.setparams((1, 2, 48000, 0, 'NONE', 'PCM'))
        f.writeframes(struct.pack('<' + 'h' * 48000, *[26214 if i % 4 < 2 else -26214 for i in range(48000)]))
    assert meter(source)[1] > 0.
    for bits in (1, 2, 3, 4, 5):
        output = folder / f'limited-{bits}.wav'
        command([BIN, '-y', '-p', '-n', '--target-lufs', '-5', '--peak-ceiling', '-2',
                 '-b', bits, '--output', output, source])
        assert meter(output)[1] <= -2., (bits, meter(output))
    for dither in (1, 2, 3, 4):
        output = folder / f'dither-{dither}.wav'
        command([BIN, '-y', '-p', '-n', '--target-lufs', '-5', '--peak-ceiling', '-2',
                 '-d', dither, '--dither-profile', 'rate', '--output', output, source])
        assert meter(output)[1] <= -2., (dither, meter(output))
print('FFmpeg feature interop passed: LUFS/true peak at 44.1..192 kHz, preserved frames/rates, inter-sample overshoot and quantized output ceilings')
