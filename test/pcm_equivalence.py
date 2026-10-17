"""Compare complete output files against an independently built prior CLI.

This is a differential release check, not a replacement for signal-level tests.
Example: python test/pcm_equivalence.py --reference ./wavegain-before --candidate ./wavegain
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def fixture(path, rate, channels, bits, floating):
    rng = random.Random(20261017)
    pcm = bytearray()
    frames = rate // 2 + 17  # Include a partial final block and enough audio for LUFS.
    for i in range(frames):
        for ch in range(channels):
            value = .18 * math.sin(2 * math.pi * (997 + 211 * ch) * i / rate)
            value += rng.uniform(-.07, .07) + .01 * (1 if ch else -1)
            if i % 8191 == 0:
                value = (-1., 0., 1. - 2. ** (1 - bits))[ch % 3]
            if floating:
                pcm.extend(struct.pack('<f', value if i % 997 else -1e-40))
            else:
                sample = max(-(1 << (bits - 1)), min((1 << (bits - 1)) - 1, round(value * (1 << (bits - 1)))))
                pcm.extend(bytes([sample + 128]) if bits == 8 else sample.to_bytes(bits // 8, 'little', signed=True))
    width = bits // 8
    fmt = struct.pack('<HHIIHH', 3 if floating else 1, channels, rate,
                      rate * channels * width, channels * width, bits)
    body = b'WAVEfmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(pcm)) + pcm
    if len(pcm) & 1:
        body += b'\0'
    path.write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, default=ROOT / 'wavegain')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binaries = [args.reference.resolve(), args.candidate.resolve()]
    cases = []
    options = [[], ['-p'], ['-d', '1'], ['-d', '3'], ['-n', '-g', '12', '-t'],
               ['-n', '-g', '12'], ['-g', '-20'], ['-a', '-q'],
               ['--lufs'], ['--true-peak'], ['--lufs', '--true-peak', '-b', '2', '-d', '2', '--dither-profile', 'rate'],
               ['--rf64'], ['-w'], *[['-b', str(b)] for b in range(1, 6)]]
    with tempfile.TemporaryDirectory(prefix='wavegain-pcm-equivalence-') as temp:
        folder = Path(temp)
        source = folder / 'input.wav'
        for rate in (8000, 44100, 48000, 88200, 96000, 176400, 192000):
            for channels in (1, 2):
                for bits, floating in ((8, False), (16, False), (24, False), (32, False), (32, True)):
                    fixture(source, rate, channels, bits, floating)
                    original = source.read_bytes()
                    for flags in options:
                        hashes = []
                        for index, binary in enumerate(binaries):
                            output = folder / f'output-{index}.wav'
                            result = subprocess.run([str(binary), '-y', *flags, '--output', str(output), str(source)],
                                                    capture_output=True, timeout=30)
                            assert result.returncode == 0, (rate, channels, bits, floating, flags, result.stderr)
                            hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
                            output.unlink()
                        assert hashes[0] == hashes[1], (rate, channels, bits, floating, flags, hashes)
                        assert source.read_bytes() == original
                        cases.append({'rate': rate, 'channels': channels, 'bits': bits, 'float': floating,
                                      'options': flags, 'sha256': hashes[0]})
            print(f'{rate} Hz: complete output bytes match across integer/float, mono/stereo and {len(options)} option sets.', flush=True)
    if args.output:
        args.output.write_text(json.dumps({'cases': cases, 'count': len(cases)}, indent=2) + '\n')
    print(f'Prior/candidate complete output equality: {len(cases)} cases passed.', flush=True)


if __name__ == '__main__':
    main()
